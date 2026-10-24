#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/supervisor.h"
#include "internal/services/lease.h"

#include "third_party/cjson/cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef JW_TEST_SERVICE_FIXTURES_ROOT
#define JW_TEST_SERVICE_FIXTURES_ROOT "build/service-fixtures"
#endif

static int g_failures = 0;
static const char *g_program_path = NULL;

#define CHECK(condition)                                                   \
    do {                                                                   \
        if (!(condition)) {                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,      \
                    #condition);                                           \
            g_failures++;                                                  \
        }                                                                  \
    } while (0)

typedef struct {
    char root[PATH_MAX];
    char runtime[PATH_MAX];
    char logs[PATH_MAX];
    char state[PATH_MAX];
    char userdata[PATH_MAX];
    const char *scan_roots[2];
} test_env;

static void join_path(char *out, size_t out_size, const char *left,
                      const char *right) {
    int written = snprintf(out, out_size, "%s/%s", left, right);
    if (written < 0 || (size_t)written >= out_size) {
        fprintf(stderr, "path too long: %s/%s\n", left, right);
        exit(2);
    }
}

static void make_dir(const char *path) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir %s: %s\n", path, strerror(errno));
        exit(2);
    }
}

static void env_setup(test_env *env, const char *scan_root,
                      const char *userdata_override) {
    static const char root_template[] = "/tmp/jw-fixture-paks-XXXXXX";
    memset(env, 0, sizeof(*env));
    memcpy(env->root, root_template, sizeof(root_template));
    if (!mkdtemp(env->root)) {
        fprintf(stderr, "mkdtemp: %s\n", strerror(errno));
        exit(2);
    }
    join_path(env->runtime, sizeof(env->runtime), env->root, "runtime");
    join_path(env->logs, sizeof(env->logs), env->root, "logs");
    join_path(env->state, sizeof(env->state), env->root, "state");
    join_path(env->userdata, sizeof(env->userdata), env->root, "userdata");
    make_dir(env->runtime);
    make_dir(env->logs);
    make_dir(env->state);
    make_dir(env->userdata);
    env->scan_roots[0] = scan_root;
    env->scan_roots[1] = NULL;
    if (userdata_override) {
        snprintf(env->userdata, sizeof(env->userdata), "%s",
                 userdata_override);
    }
}

static void env_from_existing(test_env *env, const char *root,
                              const char *scan_root) {
    memset(env, 0, sizeof(*env));
    snprintf(env->root, sizeof(env->root), "%s", root);
    join_path(env->runtime, sizeof(env->runtime), env->root, "runtime");
    join_path(env->logs, sizeof(env->logs), env->root, "logs");
    join_path(env->state, sizeof(env->state), env->root, "state");
    join_path(env->userdata, sizeof(env->userdata), env->root, "userdata");
    env->scan_roots[0] = scan_root;
    env->scan_roots[1] = NULL;
}

static void env_teardown(const test_env *env) {
    char command[PATH_MAX + 32];
    int written = snprintf(command, sizeof(command), "rm -rf '%s'", env->root);
    if (written < 0 || (size_t)written >= sizeof(command) ||
        system(command) != 0) {
        fprintf(stderr, "warning: could not remove %s\n", env->root);
    }
}

static jw_svc_supervisor *open_supervisor(test_env *env) {
    char reason[JW_SVC_REASON_BUF] = {0};
    jw_svc_supervisor *sup = jw_svc_supervisor_open(
        env->runtime, env->logs, env->state, env->scan_roots, NULL,
        env->userdata, reason, sizeof(reason));
    if (!sup) {
        fprintf(stderr, "open supervisor: %s\n", reason);
        g_failures++;
    }
    return sup;
}

static bool wait_for_state(jw_svc_supervisor *sup, const char *service_id,
                           jw_svc_effective_state wanted, int timeout_ms) {
    for (int waited = 0; waited <= timeout_ms; waited += 20) {
        jw_svc_supervisor_tick(sup);
        const jw_svc_supervised *entry =
            jw_svc_supervisor_find(sup, service_id);
        if (entry && entry->state == wanted) {
            return true;
        }
        usleep(20000);
    }
    return false;
}

static bool wait_for_released(jw_svc_supervisor *sup, const char *service_id,
                              int timeout_ms) {
    for (int waited = 0; waited <= timeout_ms; waited += 20) {
        jw_svc_supervisor_tick(sup);
        const jw_svc_supervised *entry =
            jw_svc_supervisor_find(sup, service_id);
        if (entry && entry->pgid <= 0) {
            return true;
        }
        usleep(20000);
    }
    return false;
}

static char *read_text(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        return NULL;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return NULL;
    }
    long size = ftell(file);
    if (size < 0 || fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    char *text = malloc((size_t)size + 1u);
    if (!text) {
        fclose(file);
        return NULL;
    }
    size_t got = fread(text, 1, (size_t)size, file);
    fclose(file);
    if (got != (size_t)size) {
        free(text);
        return NULL;
    }
    text[got] = '\0';
    return text;
}

static void test_named_behavior_paks(void) {
    char apps[PATH_MAX];
    join_path(apps, sizeof(apps), JW_TEST_SERVICE_FIXTURES_ROOT,
              "valid/Apps");
    test_env env;
    env_setup(&env, apps, NULL);
    jw_svc_supervisor *sup = open_supervisor(&env);
    if (!sup) {
        env_teardown(&env);
        return;
    }
    CHECK(jw_svc_supervisor_scan(sup) == 5);
    for (int i = 0; i < jw_svc_supervisor_count(sup); i++) {
        const jw_svc_supervised *entry = jw_svc_supervisor_at(sup, i);
        if (entry && !entry->manifest_valid) {
            fprintf(stderr, "invalid behavior fixture %s: %s\n",
                    entry->service_id, entry->reject_reason);
        }
        CHECK(entry != NULL && entry->manifest_valid);
    }

    char reason[JW_SVC_REASON_BUF] = {0};
    const char *normal = "org.umrk.fixture.normalexit";
    CHECK(jw_svc_supervisor_run(sup, normal, reason, sizeof(reason)));
    CHECK(wait_for_released(sup, normal, 3000));
    const jw_svc_supervised *entry = jw_svc_supervisor_find(sup, normal);
    CHECK(entry && entry->control.has_last_exit);
    CHECK(entry && entry->control.last_exit_code == 0);

    const char *ignorer = "org.umrk.fixture.ignoreterm";
    CHECK(jw_svc_supervisor_run(sup, ignorer, reason, sizeof(reason)));
    CHECK(wait_for_state(sup, ignorer, JW_SVC_STATE_RUNNING, 3000));
    struct timespec start;
    struct timespec finish;
    clock_gettime(CLOCK_MONOTONIC, &start);
    /* The CTL-1 call returns at once; tick drives TERM -> grace -> KILL ->
     * verified absence against a service that ignores SIGTERM. */
    CHECK(jw_svc_supervisor_stop(sup, ignorer, reason, sizeof(reason)));
    clock_gettime(CLOCK_MONOTONIC, &finish);
    long long call_ms = (long long)(finish.tv_sec - start.tv_sec) * 1000LL +
                        (long long)(finish.tv_nsec - start.tv_nsec) / 1000000LL;
    CHECK(call_ms < 250);
    CHECK(wait_for_released(sup, ignorer, 4000));
    clock_gettime(CLOCK_MONOTONIC, &finish);
    long long elapsed_ms = (long long)(finish.tv_sec - start.tv_sec) * 1000LL +
                           (long long)(finish.tv_nsec - start.tv_nsec) /
                               1000000LL;
    printf("service-fixture timing ignore-term-request-ms=%lld "
           "ignore-term-verified-absent-ms=%lld\n",
           call_ms, elapsed_ms);
    /* Still inside the contract's stop_grace_ms + 2000 ms worst case. */
    CHECK(elapsed_ms <= 2450);
    entry = jw_svc_supervisor_find(sup, ignorer);
    CHECK(entry && entry->pgid <= 0);

    const char *orphan = "org.umrk.fixture.orphandescendant";
    CHECK(jw_svc_supervisor_run(sup, orphan, reason, sizeof(reason)));
    pid_t orphan_leader = -1;
    entry = jw_svc_supervisor_find(sup, orphan);
    if (entry) {
        orphan_leader = entry->pgid;
    }
    bool reservation_observed = false;
    for (int waited = 0; waited <= 2000 && !reservation_observed;
         waited += 20) {
        jw_svc_supervisor_tick(sup);
        entry = jw_svc_supervisor_find(sup, orphan);
        reservation_observed = entry && entry->reap_pending && entry->pgid > 0;
        if (!reservation_observed) {
            usleep(20000);
        }
    }
    CHECK(reservation_observed);
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    CHECK(waitid(P_PID, (id_t)orphan_leader, &info,
                 WEXITED | WNOHANG | WNOWAIT) == 0);
    CHECK(info.si_pid == orphan_leader);
    CHECK(wait_for_released(sup, orphan, 4000));

    const char *daemonizer = "org.umrk.fixture.daemonizes";
    CHECK(jw_svc_supervisor_run(sup, daemonizer, reason, sizeof(reason)));
    CHECK(wait_for_released(sup, daemonizer, 4000));
    entry = jw_svc_supervisor_find(sup, daemonizer);
    CHECK(entry != NULL);
    CHECK(entry && strcmp(entry->control.last_transition_reason,
                          "foreground-contract-violation") == 0);

    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);
    env_teardown(&env);
}

static void test_canonical_invalid_paks(void) {
    char invalid_root[PATH_MAX];
    join_path(invalid_root, sizeof(invalid_root), JW_TEST_SERVICE_FIXTURES_ROOT,
              "invalid");
    DIR *dir = opendir(invalid_root);
    CHECK(dir != NULL);
    if (!dir) {
        return;
    }
    int checked = 0;
    struct dirent *item;
    while ((item = readdir(dir)) != NULL) {
        if (item->d_name[0] == '.') {
            continue;
        }
        char case_root[PATH_MAX];
        char expect_path[PATH_MAX];
        char apps[PATH_MAX];
        char userdata[PATH_MAX];
        join_path(case_root, sizeof(case_root), invalid_root, item->d_name);
        join_path(expect_path, sizeof(expect_path), case_root, "expect.json");
        join_path(apps, sizeof(apps), case_root, "Apps");
        join_path(userdata, sizeof(userdata), case_root, "Userdata");
        char *expect_text = read_text(expect_path);
        if (!expect_text) {
            continue;
        }
        cJSON *expect = cJSON_ParseWithOpts(expect_text, NULL, true);
        free(expect_text);
        const cJSON *reason_json = expect
            ? cJSON_GetObjectItemCaseSensitive(expect, "reason")
            : NULL;
        CHECK(reason_json && cJSON_IsString(reason_json) &&
              reason_json->valuestring);
        if (!reason_json || !cJSON_IsString(reason_json) ||
            !reason_json->valuestring) {
            cJSON_Delete(expect);
            continue;
        }

        test_env env;
        env_setup(&env, apps, userdata);
        jw_svc_supervisor *sup = open_supervisor(&env);
        if (sup) {
            CHECK(jw_svc_supervisor_scan(sup) == 1);
            bool matched = false;
            for (int i = 0; i < jw_svc_supervisor_count(sup); i++) {
                const jw_svc_supervised *entry =
                    jw_svc_supervisor_at(sup, i);
                if (entry && !entry->manifest_valid &&
                    strcmp(entry->reject_reason, reason_json->valuestring) ==
                        0) {
                    matched = true;
                    CHECK(entry->state == JW_SVC_STATE_UNAVAILABLE);
                }
            }
            if (!matched) {
                fprintf(stderr, "FAIL invalid fixture %s did not report %s\n",
                        item->d_name, reason_json->valuestring);
                g_failures++;
            }
            jw_svc_supervisor_close(sup);
        }
        env_teardown(&env);
        cJSON_Delete(expect);
        checked++;
    }
    closedir(dir);
    CHECK(checked >= 27);
}

static int count_lines(const char *path) {
    FILE *file = fopen(path, "r");
    if (!file) {
        return -1;
    }
    int count = 0;
    int ch;
    while ((ch = fgetc(file)) != EOF) {
        if (ch == '\n') {
            count++;
        }
    }
    fclose(file);
    return count;
}

static int run_death_worker(const char *root, const char *apps) {
    g_failures = 0;
    test_env env;
    env_from_existing(&env, root, apps);
    jw_svc_supervisor *sup = open_supervisor(&env);
    if (!sup) {
        return 2;
    }
    char reason[JW_SVC_REASON_BUF] = {0};
    if (jw_svc_supervisor_scan(sup) != 5 ||
        !jw_svc_supervisor_enable(sup,
                                  "org.umrk.fixture.supervisordeath",
                                  reason, sizeof(reason)) ||
        !jw_svc_supervisor_run(sup, "org.umrk.fixture.supervisordeath",
                               reason, sizeof(reason))) {
        return 2;
    }
    for (;;) {
        jw_svc_supervisor_tick(sup);
        usleep(20000);
    }
}

static void restore_death_hold_env(char *old_hold) {
    if (old_hold) {
        CHECK(setenv("UMRK_FIXTURE_DEATH_HOLD_MS", old_hold, 1) == 0);
    } else {
        CHECK(unsetenv("UMRK_FIXTURE_DEATH_HOLD_MS") == 0);
    }
    free(old_hold);
}

static void test_supervisor_death_no_overlap(void) {
    char apps[PATH_MAX];
    join_path(apps, sizeof(apps), JW_TEST_SERVICE_FIXTURES_ROOT,
              "valid/Apps");
    test_env env;
    env_setup(&env, apps, NULL);

    /* Give the test an observable window after the leader exits but while its
     * descendant is still alive. A missing descendant lease would allow the
     * replacement to overlap precisely inside this interval. */
    const char *old_hold_env = getenv("UMRK_FIXTURE_DEATH_HOLD_MS");
    char *old_hold = old_hold_env ? strdup(old_hold_env) : NULL;
    CHECK(!old_hold_env || old_hold != NULL);
    CHECK(setenv("UMRK_FIXTURE_DEATH_HOLD_MS", "1200", 1) == 0);

    pid_t worker = fork();
    CHECK(worker >= 0);
    if (worker < 0) {
        restore_death_hold_env(old_hold);
        env_teardown(&env);
        return;
    }
    if (worker == 0) {
        execl(g_program_path, g_program_path, "--death-worker", env.root,
              apps, (char *)NULL);
        _exit(127);
    }

    char service_runtime[PATH_MAX];
    char ready[PATH_MAX];
    char descendant_parent_death[PATH_MAX];
    char generations[PATH_MAX];
    join_path(service_runtime, sizeof(service_runtime), env.runtime,
              "services/org.umrk.fixture.supervisordeath");
    join_path(ready, sizeof(ready), service_runtime, "ready");
    join_path(descendant_parent_death, sizeof(descendant_parent_death),
              service_runtime, "descendant-parent-death");
    join_path(generations, sizeof(generations), service_runtime,
              "generations.log");
    bool first_ready = false;
    for (int waited = 0; waited <= 5000 && !first_ready; waited += 10) {
        first_ready = access(ready, F_OK) == 0 && count_lines(generations) == 1;
        if (!first_ready) {
            usleep(10000);
        }
    }
    CHECK(first_ready);
    if (!first_ready) {
        kill(worker, SIGKILL);
        waitpid(worker, NULL, 0);
        restore_death_hold_env(old_hold);
        env_teardown(&env);
        return;
    }

    CHECK(kill(worker, SIGKILL) == 0);
    int worker_status = 0;
    CHECK(waitpid(worker, &worker_status, 0) == worker);
    CHECK(WIFSIGNALED(worker_status) && WTERMSIG(worker_status) == SIGKILL);

    jw_svc_supervisor *sup = open_supervisor(&env);
    if (!sup) {
        restore_death_hold_env(old_hold);
        env_teardown(&env);
        return;
    }
    CHECK(jw_svc_supervisor_scan(sup) == 5);
    const jw_svc_supervised *entry = jw_svc_supervisor_find(
        sup, "org.umrk.fixture.supervisordeath");
    CHECK(entry && entry->desired_enabled);
    CHECK(entry && entry->state == JW_SVC_STATE_STALE_GENERATION);
    CHECK(entry && entry->pgid <= 0);

    bool descendant_in_hold = false;
    for (int waited = 0; waited <= 5000 && !descendant_in_hold;
         waited += 10) {
        descendant_in_hold = access(descendant_parent_death, F_OK) == 0;
        if (!descendant_in_hold) {
            jw_svc_supervisor_tick(sup);
            usleep(10000);
        }
    }
    CHECK(descendant_in_hold);
    /* The leader is gone now. Descriptor 3 in the delayed descendant must be
     * the thing still excluding a replacement. Probe the real lease and keep
     * ticking through part of the descendant-only hold window. */
    char lease_reason[JW_SVC_REASON_BUF] = {0};
    int lease_probe = jw_svc_lease_acquire(
        env.runtime, "org.umrk.fixture.supervisordeath", lease_reason,
        sizeof(lease_reason));
    CHECK(lease_probe < 0);
    CHECK(strcmp(lease_reason, "stale-generation") == 0);
    if (lease_probe >= 0) close(lease_probe);
    for (int elapsed = 0; elapsed < 400; elapsed += 20) {
        jw_svc_supervisor_tick(sup);
        entry = jw_svc_supervisor_find(
            sup, "org.umrk.fixture.supervisordeath");
        CHECK(entry && entry->state == JW_SVC_STATE_STALE_GENERATION);
        CHECK(count_lines(generations) == 1);
        usleep(20000);
    }

    CHECK(wait_for_state(sup, "org.umrk.fixture.supervisordeath",
                         JW_SVC_STATE_RUNNING, 7000));
    CHECK(count_lines(generations) == 2);
    for (int elapsed = 0; elapsed < 1200; elapsed += 20) {
        jw_svc_supervisor_tick(sup);
        usleep(20000);
    }
    CHECK(count_lines(generations) == 2);
    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);
    restore_death_hold_env(old_hold);
    env_teardown(&env);
}

int main(int argc, char **argv) {
    g_program_path = argv[0];
    if (argc == 4 && strcmp(argv[1], "--death-worker") == 0) {
        return run_death_worker(argv[2], argv[3]);
    }
    test_named_behavior_paks();
    test_canonical_invalid_paks();
    test_supervisor_death_no_overlap();
    if (g_failures == 0) {
        puts("PASS service-fixture-test (5 behavior + 27 invalid paks)");
        return 0;
    }
    fprintf(stderr, "FAIL service-fixture-test (%d failures)\n", g_failures);
    return 1;
}
