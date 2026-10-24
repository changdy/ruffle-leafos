/* Integration tests for internal/services/supervisor.c: real fixture paks
 * on disk, real forked processes, real leases, real stop sequences. This is
 * the first suite in the series that runs a supervised service END TO END,
 * not an isolated primitive.
 *
 * flock()/kill()/usleep()/mkdtemp()/PATH_MAX need a broader feature-test
 * macro than bare -std=c11 on glibc. Must precede every #include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/supervisor.h"
#include "internal/services/control_state.h"
#include "internal/services/lease.h"
#include "internal/services/reservation.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;
static const char *g_program_path = NULL;
extern char **environ;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

typedef struct {
    char root[PATH_MAX];
    char runtime[PATH_MAX];
    char logs[PATH_MAX];
    char state[PATH_MAX];
    char userdata[PATH_MAX];
    char apps_primary[PATH_MAX];
    char apps_shared[PATH_MAX];     /* second dir of the SAME Primary source */
    char apps_secondary[PATH_MAX];  /* a genuinely Secondary storage source */
    const char *scan_roots[3];
    const char *secondary_scan_roots[2];
} fixture;

static void jw__mkdir(const char *path) {
    if (mkdir(path, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir %s: %s\n", path, strerror(errno));
        exit(2);
    }
}

/* gcc -Wformat-truncation rejects snprintf(dst, sizeof dst, "%s/...", root)
 * because root is an unbounded PATH_MAX array; join with explicit bounds. */
static void jw__join(char *out, size_t out_size, const char *dir,
                     const char *leaf) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    strncat(out, dir ? dir : "", out_size - 1u);
    size_t len = strlen(out);
    if (len < out_size - 1u) {
        strncat(out, "/", out_size - 1u - len);
    }
    len = strlen(out);
    if (len < out_size - 1u) {
        strncat(out, leaf ? leaf : "", out_size - 1u - len);
    }
}

/* Three-component variant: dir/sub/leaf. */
static void jw__join3(char *out, size_t out_size, const char *dir,
                      const char *sub, const char *leaf) {
    char mid[PATH_MAX];
    jw__join(mid, sizeof(mid), dir, sub);
    jw__join(out, out_size, mid, leaf);
}

static void fixture_setup(fixture *f) {
    snprintf(f->root, sizeof(f->root), "/tmp/jw-supv-test-XXXXXX");
    if (!mkdtemp(f->root)) {
        fprintf(stderr, "mkdtemp: %s\n", strerror(errno));
        exit(2);
    }
    jw__join(f->runtime, sizeof(f->runtime), f->root, "runtime");
    jw__join(f->logs, sizeof(f->logs), f->root, "logs");
    jw__join(f->state, sizeof(f->state), f->root, "state");
    jw__join(f->userdata, sizeof(f->userdata), f->root, "userdata");
    jw__join(f->apps_primary, sizeof(f->apps_primary), f->root, "apps");
    jw__join(f->apps_shared, sizeof(f->apps_shared), f->root, "apps-shared");
    jw__join(f->apps_secondary, sizeof(f->apps_secondary), f->root,
             "apps-secondary");
    jw__mkdir(f->runtime);
    jw__mkdir(f->logs);
    jw__mkdir(f->state);
    jw__mkdir(f->userdata);
    jw__mkdir(f->apps_primary);
    jw__mkdir(f->apps_shared);
    jw__mkdir(f->apps_secondary);
    /* Apps/<platform> and Apps/shared are two directories of ONE Primary
     * source; only apps_secondary stands in for a Secondary source. */
    f->scan_roots[0] = f->apps_primary;
    f->scan_roots[1] = f->apps_shared;
    f->scan_roots[2] = NULL;
    f->secondary_scan_roots[0] = NULL;
    f->secondary_scan_roots[1] = NULL;
}

static void fixture_teardown(fixture *f) {
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", f->root);
    if (system(cmd) != 0) {
        fprintf(stderr, "warning: could not remove %s\n", f->root);
    }
}

static void fixture_write_pak_in(fixture *f, const char *root,
                                 const char *pak_dir_name,
                                 const char *service_id,
                                 const char *script_body,
                                 const char *extra_manifest_fields) {
    char pak[PATH_MAX];
    jw__join(pak, sizeof(pak), root, pak_dir_name);
    jw__mkdir(pak);
    char bin[PATH_MAX];
    jw__join(bin, sizeof(bin), pak, "bin");
    jw__mkdir(bin);

    char run[PATH_MAX];
    jw__join3(run, sizeof(run), pak, "bin", "run.sh");
    FILE *fp = fopen(run, "w");
    if (!fp) {
        fprintf(stderr, "fopen %s: %s\n", run, strerror(errno));
        exit(2);
    }
    fprintf(fp, "#!/bin/sh\n%s\n", script_body);
    fclose(fp);
    if (chmod(run, 0700) != 0) {
        fprintf(stderr, "chmod %s: %s\n", run, strerror(errno));
        exit(2);
    }

    char pj[PATH_MAX];
    jw__join(pj, sizeof(pj), pak, "pak.json");
    fp = fopen(pj, "w");
    if (!fp) {
        fprintf(stderr, "fopen %s: %s\n", pj, strerror(errno));
        exit(2);
    }
    fprintf(fp,
            "{\"id\":\"%s\",\"name\":\"%s\",\"platform\":\"%s\","
            "\"pak_version\":\"1.2.3\","
            "\"service\":{\"schema\":1,\"id\":\"%s\","
            "\"run\":{\"path\":\"bin/run.sh\",\"args\":[]},"
            "\"default_enabled\":false,"
            "\"stop_grace_ms\":300%s}}\n",
            service_id, pak_dir_name,
            getenv("PLATFORM") ? getenv("PLATFORM") : "mac",
            service_id, extra_manifest_fields ? extra_manifest_fields : "");
    fclose(fp);
}

static void fixture_write_pak(fixture *f, const char *pak_dir_name,
                              const char *service_id, const char *script_body,
                              const char *extra_manifest_fields) {
    fixture_write_pak_in(f, f->apps_primary, pak_dir_name, service_id,
                         script_body, extra_manifest_fields);
}

static void fixture_write_floor(fixture *f, const char *pak_dir_name,
                                const char *package_id) {
    char pak[PATH_MAX];
    jw__join(pak, sizeof(pak), f->apps_primary, pak_dir_name);
    jw__mkdir(pak);
    char manifest[PATH_MAX];
    jw__join(manifest, sizeof(manifest), pak, "pak.json");
    FILE *fp = fopen(manifest, "w");
    if (!fp) {
        fprintf(stderr, "fopen %s: %s\n", manifest, strerror(errno));
        exit(2);
    }
    fprintf(fp,
            "{\"id\":\"%s\",\"name\":\"Floor\","
            "\"platform\":\"mac\",\"pak_version\":\"0.0.1\"}\n",
            package_id);
    fclose(fp);
    char launch[PATH_MAX];
    jw__join(launch, sizeof(launch), pak, "launch.sh");
    fp = fopen(launch, "w");
    if (!fp) {
        fprintf(stderr, "fopen %s: %s\n", launch, strerror(errno));
        exit(2);
    }
    fputs("#!/bin/sh\nexit 0\n", fp);
    fclose(fp);
    if (chmod(launch, 0700) != 0) {
        fprintf(stderr, "chmod %s: %s\n", launch, strerror(errno));
        exit(2);
    }
}

static jw_svc_supervisor *fixture_open(fixture *f, char *reason) {
    return jw_svc_supervisor_open(f->runtime, f->logs, f->state,
                                  f->scan_roots, f->secondary_scan_roots,
                                  f->userdata, reason, JW_SVC_REASON_BUF);
}

/* Installs/removes a deterministic SQLite write fault without reaching into
 * the supervisor's opaque store. jw__persist() uses INSERT ... ON CONFLICT,
 * so a BEFORE INSERT trigger covers both new and existing control rows. */
static bool fixture_reject_control_writes(fixture *f, bool reject) {
    char db_path[PATH_MAX];
    jw__join(db_path, sizeof(db_path), f->state, "services-control.db");
    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return false;
    }
    const char *sql = reject
        ? "CREATE TRIGGER supervisor_test_reject_write "
          "BEFORE INSERT ON service_control_state "
          "BEGIN SELECT RAISE(ABORT, 'injected write failure'); END;"
        : "DROP TRIGGER IF EXISTS supervisor_test_reject_write;";
    char *error = NULL;
    bool ok = sqlite3_exec(db, sql, NULL, NULL, &error) == SQLITE_OK;
    sqlite3_free(error);
    if (sqlite3_close(db) != SQLITE_OK) {
        ok = false;
    }
    return ok;
}

static bool wait_for_state(jw_svc_supervisor *sup, const char *id,
                           jw_svc_effective_state want, int timeout_ms) {
    int waited = 0;
    const int step = 20;
    while (waited <= timeout_ms) {
        jw_svc_supervisor_tick(sup);
        const jw_svc_supervised *e = jw_svc_supervisor_find(sup, id);
        if (e && e->state == want) {
            return true;
        }
        usleep((useconds_t)step * 1000u);
        waited += step;
    }
    return false;
}

/* Ticks until the entry owns no process group (leader reaped, lease released). */
static bool wait_for_released(jw_svc_supervisor *sup, const char *id,
                              int timeout_ms) {
    int waited = 0;
    const int step = 20;
    while (waited <= timeout_ms) {
        jw_svc_supervisor_tick(sup);
        const jw_svc_supervised *e = jw_svc_supervisor_find(sup, id);
        if (e && e->pgid <= 0 && !e->stop_requested && !e->reap_pending) {
            return true;
        }
        usleep((useconds_t)step * 1000u);
        waited += step;
    }
    return false;
}

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static void test_clean_exit(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "clean.pak", "org.umrk.test.clean", "exit 0", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);

    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.clean");
    CHECK(e != NULL);
    CHECK(e->manifest_valid);
    CHECK(e->state == JW_SVC_STATE_DISABLED);

    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.clean", reason, sizeof(reason)));
    e = jw_svc_supervisor_find(sup, "org.umrk.test.clean");
    CHECK(e->pgid > 0);

    CHECK(wait_for_state(sup, "org.umrk.test.clean",
                         JW_SVC_STATE_DISABLED, 3000));
    e = jw_svc_supervisor_find(sup, "org.umrk.test.clean");
    CHECK(e->pgid <= 0);
    CHECK(e->lease_fd < 0);
    CHECK(e->control.has_last_exit);
    CHECK(e->control.last_exit_code == 0);
    CHECK(e->backoff.count == 0);
    CHECK(!e->backoff.breaker_open);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_stop_sequence(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "sleeper.pak", "org.umrk.test.sleeper",
                      "while :; do sleep 1; done", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.sleeper", reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.sleeper", JW_SVC_STATE_RUNNING, 3000));

    /* Stop is non-blocking: it reports `stopping` immediately and tick drives
     * the sequence to verified absence. */
    CHECK(jw_svc_supervisor_stop(sup, "org.umrk.test.sleeper", reason, sizeof(reason)));
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.sleeper");
    CHECK(e->state == JW_SVC_STATE_STOPPING);
    CHECK(!e->session_run); /* session intent withdrawn at once */

    CHECK(wait_for_released(sup, "org.umrk.test.sleeper", 4000));
    e = jw_svc_supervisor_find(sup, "org.umrk.test.sleeper");
    CHECK(e->pgid <= 0);
    CHECK(e->state == JW_SVC_STATE_STOPPED || e->state == JW_SVC_STATE_DISABLED);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_ignore_term_escalates(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "ignorer.pak", "org.umrk.test.ignorer",
                      "trap '' TERM; while :; do sleep 1; done", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.ignorer", reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.ignorer", JW_SVC_STATE_RUNNING, 3000));

    /* The CTL-1 call itself must not block the daemon loop for the stop
     * sequence: it returns in ~no time even against a service that ignores
     * SIGTERM and can only be removed by the KILL escalation. */
    long long t0 = monotonic_ms();
    CHECK(jw_svc_supervisor_stop(sup, "org.umrk.test.ignorer", reason, sizeof(reason)));
    long long call_ms = monotonic_ms() - t0;
    CHECK(call_ms < 250);

    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.ignorer");
    CHECK(e->pgid > 0); /* still owned: TERM sent, group not yet gone */
    CHECK(e->state == JW_SVC_STATE_STOPPING);

    /* Tick still completes the sequence inside the contract's worst case of
     * stop_grace_ms + 2000 ms. */
    CHECK(wait_for_released(sup, "org.umrk.test.ignorer", 4000));
    long long total_ms = monotonic_ms() - t0;
    e = jw_svc_supervisor_find(sup, "org.umrk.test.ignorer");
    CHECK(e->pgid <= 0);
    CHECK(total_ms <= 300 + 2000 + 400);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_orphan_descendant(void) {
    fixture f;
    fixture_setup(&f);
    /* The leader forks a descendant that IGNORES SIGTERM and exits itself.
     * restart is "no" so the supervisor does not start a replacement over
     * the still-live group: this isolates the reservation rule -- the
     * zombie leader holds the pgid, the orphan is a live member, and the
     * group must stay not-absent until the orphan is KILLed and reaped. */
    fixture_write_pak(&f, "orphan.pak", "org.umrk.test.orphan",
                      "( trap '' TERM; sleep 300 ) & exit 0",
                      ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.orphan", reason, sizeof(reason)));
    const jw_svc_supervised *started =
        jw_svc_supervisor_find(sup, "org.umrk.test.orphan");
    pid_t leader = started ? started->pgid : -1;

    bool noticed = false;
    for (int i = 0; i < 200 && !noticed; i++) {
        jw_svc_supervisor_tick(sup);
        const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.orphan");
        if (e && e->state == JW_SVC_STATE_STOPPING && e->reap_pending) {
            noticed = true;
        }
        usleep(20000);
    }
    CHECK(noticed);

    /* Observing the exit must leave the leader waitable: waitpid(WNOHANG)
     * would have consumed it and freed the pgid before the descendant was
     * verified absent. WNOWAIT confirms the zombie reservation still exists. */
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    errno = 0;
    int wait_rc = waitid(P_PID, (id_t)leader, &info,
                         WEXITED | WNOHANG | WNOWAIT);
    CHECK(wait_rc == 0);
    CHECK(info.si_pid == leader);

    bool reaped = false;
    for (int i = 0; i < 400 && !reaped; i++) {
        jw_svc_supervisor_tick(sup);
        const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.orphan");
        if (e && e->pgid <= 0) {
            reaped = true;
        }
        usleep(20000);
    }
    CHECK(reaped);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_persistent_vs_session_across_restart(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "sleeper.pak", "org.umrk.test.persist",
                      "while :; do sleep 1; done", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_enable(sup, "org.umrk.test.persist", reason, sizeof(reason)));
    for (int i = 0; i < 75; i++) {
        jw_svc_supervisor_tick(sup);
        usleep(20000);
    }
    const jw_svc_supervised *enabled =
        jw_svc_supervisor_find(sup, "org.umrk.test.persist");
    CHECK(enabled != NULL && enabled->pgid <= 0);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.persist", reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.persist", JW_SVC_STATE_RUNNING, 3000));
    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);

    jw_svc_supervisor *sup2 = fixture_open(&f, reason);
    CHECK(sup2 != NULL);
    CHECK(jw_svc_supervisor_scan(sup2) == 1);
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup2, "org.umrk.test.persist");
    CHECK(e != NULL);
    CHECK(e->desired_enabled);
    CHECK(!e->session_run);

    CHECK(wait_for_state(sup2, "org.umrk.test.persist", JW_SVC_STATE_RUNNING, 5000));
    CHECK(jw_svc_supervisor_stop_all(sup2) == 0);
    jw_svc_supervisor_close(sup2);
    fixture_teardown(&f);
}

static void test_invalid_manifest_isolated(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "good.pak", "org.umrk.test.good",
                      "while :; do sleep 1; done", ",\"restart\":\"on-failure\"");
    char pak[PATH_MAX];
    jw__join(pak, sizeof(pak), f.apps_primary, "bad.pak");
    jw__mkdir(pak);
    /* A real executable so run.path passes and the ONLY rejection is the
     * top-level-id vs service.id mismatch. */
    char bin[PATH_MAX];
    jw__join(bin, sizeof(bin), pak, "bin");
    jw__mkdir(bin);
    char run[PATH_MAX];
    jw__join3(run, sizeof(run), pak, "bin", "run.sh");
    FILE *rf = fopen(run, "w");
    fprintf(rf, "#!/bin/sh\nexit 0\n");
    fclose(rf);
    chmod(run, 0700);
    char pj[PATH_MAX];
    jw__join(pj, sizeof(pj), pak, "pak.json");
    FILE *fp = fopen(pj, "w");
    fprintf(fp, "{\"id\":\"org.umrk.test.bad\",\"platform\":\"mac\","
                "\"service\":{\"schema\":1,\"id\":\"org.umrk.other\","
                "\"run\":{\"path\":\"bin/x\",\"args\":[]}}}\n");
    fclose(fp);

    /* A malformed manifest using the valid service's exact id must not win
     * reconciliation based on readdir() order. */
    fixture_write_pak(&f, "attacker.pak", "org.umrk.test.attacker",
                      "exit 0", NULL);
    jw__join(pak, sizeof(pak), f.apps_primary, "attacker.pak");
    jw__join(pj, sizeof(pj), pak, "pak.json");
    fp = fopen(pj, "w");
    CHECK(fp != NULL);
    if (fp) {
        fprintf(fp,
                "{\"id\":\"org.umrk.test.attacker\",\"platform\":\"mac\","
                "\"service\":{\"schema\":1,\"id\":\"org.umrk.test.good\","
                "\"run\":{\"path\":\"bin/run.sh\",\"args\":[]}}}\n");
        fclose(fp);
    }

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 2);
    const jw_svc_supervised *good = jw_svc_supervisor_find(sup, "org.umrk.test.good");
    /* An invalid service is tracked under its declared service.id and
     * reported unavailable, keyed to the specific rejection reason. */
    const jw_svc_supervised *bad = jw_svc_supervisor_find(sup, "org.umrk.other");
    CHECK(good != NULL && good->manifest_valid);
    CHECK(bad != NULL && !bad->manifest_valid);
    CHECK(bad != NULL && bad->state == JW_SVC_STATE_UNAVAILABLE);
    CHECK(strcmp(bad->reject_reason, "id-mismatch") == 0);
    /* A canonical id keeps this malformed discovery representable on CTL-1,
     * so clients can reach the specific rejection reason. */
    CHECK(jw_svc_supervisor_entry_is_listable(bad));

    CHECK(!jw_svc_supervisor_run(sup, "org.umrk.other", reason, sizeof(reason)));
    CHECK(strcmp(reason, "unavailable") == 0);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.good", reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.good", JW_SVC_STATE_RUNNING, 3000));
    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_duplicate_ids_both_unavailable(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "one.pak", "org.umrk.test.dup", "while :; do sleep 1; done", ",\"restart\":\"on-failure\"");
    fixture_write_pak(&f, "two.pak", "org.umrk.test.dup", "while :; do sleep 1; done", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    /* Both duplicate paks carry the same service.id, so they reconcile to a
     * single tracked entry flagged as a duplicate (unavailable). */
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.dup");
    CHECK(e != NULL);
    CHECK(!e->manifest_valid);
    CHECK(strcmp(e->reject_reason, "duplicate-service-id") == 0);
    CHECK(!jw_svc_supervisor_run(sup, "org.umrk.test.dup", reason, sizeof(reason)));

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_stale_generation_blocks_second_daemon(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "leaseholder.pak", "org.umrk.test.lease",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"on-failure\","
                      "\"lifecycle\":{\"game\":\"stop\","
                      "\"stop_on_storage_change\":true,"
                      "\"stop_on_suspend\":true}");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *first = fixture_open(&f, reason);
    CHECK(first != NULL);
    CHECK(jw_svc_supervisor_scan(first) == 1);
    CHECK(jw_svc_supervisor_run(first, "org.umrk.test.lease", reason, sizeof(reason)));
    CHECK(wait_for_state(first, "org.umrk.test.lease", JW_SVC_STATE_RUNNING, 3000));

    jw_svc_supervisor *second = fixture_open(&f, reason);
    CHECK(second != NULL);
    CHECK(jw_svc_supervisor_scan(second) == 1);
    const jw_svc_supervised *e2 =
        jw_svc_supervisor_find(second, "org.umrk.test.lease");
    CHECK(e2 != NULL && e2->state == JW_SVC_STATE_STALE_GENERATION);
    CHECK(e2 != NULL && !e2->desired_enabled && !e2->session_run);
    char stuck[JW_SVC_SUPERVISOR_ID_BUF];
    CHECK(jw_svc_supervisor_suspend_begin(second, stuck,
                                           sizeof(stuck)) == 0);
    CHECK(strcmp(stuck, "org.umrk.test.lease") == 0);
    CHECK(jw_svc_supervisor_storage_change_begin(second, stuck,
                                                  sizeof(stuck)) == 0);
    CHECK(strcmp(stuck, "org.umrk.test.lease") == 0);
    CHECK(jw_svc_supervisor_game_launch_begin(second, stuck,
                                               sizeof(stuck)) == 0);
    CHECK(strcmp(stuck, "org.umrk.test.lease") == 0);
    CHECK(jw_svc_supervisor_stop_all(second) == 1);
    /* The rootfs power supervisor proves absence through the lease; a stale
     * generation has no pgid this daemon may signal. */
    char described[1024];
    CHECK(jw_svc_supervisor_describe_unverified(second, described, sizeof(described)) == 1);
    CHECK(strncmp(described, "service=org.umrk.test.lease pgid=0 lease=", 41) == 0);
    CHECK(strstr(described, "/services/org.umrk.test.lease/generation.lease\n") != NULL);
    CHECK(jw_svc_supervisor_describe_unverified(first, described, sizeof(described)) == 0);
    CHECK(described[0] == '\0');

    /* This test used game_launch_begin only to exercise the stale-generation
     * decision row. Release the synthetic active launch before exercising the
     * ordinary stale Run/lease-retry behavior below. */
    jw_svc_supervisor_game_finish(second);
    CHECK(!jw_svc_supervisor_run(second, "org.umrk.test.lease",
                                 reason, sizeof(reason)));
    CHECK(strcmp(reason, "stale-generation") == 0);
    bool went_stale = false;
    for (int i = 0; i < 200 && !went_stale; i++) {
        jw_svc_supervisor_tick(second);
        const jw_svc_supervised *e = jw_svc_supervisor_find(second, "org.umrk.test.lease");
        if (e && e->state == JW_SVC_STATE_STALE_GENERATION) {
            went_stale = true;
        }
        usleep(20000);
    }
    CHECK(went_stale);
    e2 = jw_svc_supervisor_find(second, "org.umrk.test.lease");
    CHECK(e2->pgid <= 0);

    CHECK(jw_svc_supervisor_stop_all(first) == 0);
    jw_svc_supervisor_close(first);
    bool started = wait_for_state(second, "org.umrk.test.lease", JW_SVC_STATE_RUNNING, 5000) ||
                   wait_for_state(second, "org.umrk.test.lease", JW_SVC_STATE_STARTING, 2000);
    CHECK(started);
    CHECK(jw_svc_supervisor_stop_all(second) == 0);
    jw_svc_supervisor_close(second);
    fixture_teardown(&f);
}

static int run_descriptor_zero_worker(const char *root) {
    g_failures = 0;

    fixture f;
    memset(&f, 0, sizeof(f));
    snprintf(f.root, sizeof(f.root), "%s", root);
    jw__join(f.runtime, sizeof(f.runtime), f.root, "runtime");
    jw__join(f.logs, sizeof(f.logs), f.root, "logs");
    jw__join(f.state, sizeof(f.state), f.root, "state");
    jw__join(f.userdata, sizeof(f.userdata), f.root, "userdata");
    jw__join(f.apps_primary, sizeof(f.apps_primary), f.root, "apps");
    f.scan_roots[0] = f.apps_primary;
    f.scan_roots[1] = NULL;
    f.secondary_scan_roots[0] = NULL;

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);

    CHECK(close(STDIN_FILENO) == 0);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.fdzero",
                                reason, sizeof(reason)));
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.fdzero");
    CHECK(e != NULL && e->lease_fd == STDIN_FILENO);
    CHECK(jw_svc_supervisor_stop(sup, "org.umrk.test.fdzero",
                                 reason, sizeof(reason)));
    /* Stop is asynchronous now; the lease is released only once tick has
     * verified the group absent and reaped the leader. Drive it here so the
     * parent's probe below tests descriptor-0 release, not stop timing. */
    CHECK(wait_for_released(sup, "org.umrk.test.fdzero", 4000));
    char ready[PATH_MAX];
    jw__join(ready, sizeof(ready), f.root, "fdzero-ready");
    FILE *ready_fp = fopen(ready, "w");
    CHECK(ready_fp != NULL);
    if (ready_fp) fclose(ready_fp);

    /* Stay alive with the supervisor open so the parent can distinguish a
     * genuinely closed descriptor 0 from a lock released only by process
     * exit. */
    sleep(3);

    jw_svc_supervisor_close(sup);
    return g_failures == 0 ? 0 : 1;
}

static void test_descriptor_zero_lease_is_released(void) {
    /* Spawn with an independent /dev/null stdin. Closing the suite runner's
     * harness pipe itself can be interpreted by some runners as completion,
     * which would hide the lease assertion behind an external SIGKILL. */
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "fdzero.pak", "org.umrk.test.fdzero",
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");

    posix_spawn_file_actions_t actions;
    CHECK(posix_spawn_file_actions_init(&actions) == 0);
    CHECK(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                           O_RDONLY, (mode_t)0) == 0);
    char *const argv[] = {(char *)g_program_path,
                          (char *)"--fd-zero-worker", f.root, NULL};
    pid_t worker = -1;
    int spawn_rc = posix_spawn(&worker, g_program_path, &actions, NULL,
                               argv, environ);
    CHECK(posix_spawn_file_actions_destroy(&actions) == 0);
    CHECK(spawn_rc == 0);
    if (spawn_rc != 0) {
        fixture_teardown(&f);
        return;
    }
    char ready[PATH_MAX];
    jw__join(ready, sizeof(ready), f.root, "fdzero-ready");
    bool worker_ready = false;
    for (int i = 0; i < 300 && !worker_ready; i++) {
        worker_ready = access(ready, F_OK) == 0;
        if (!worker_ready) usleep(10000);
    }
    CHECK(worker_ready);
    char reason[JW_SVC_REASON_BUF];
    int probe = jw_svc_lease_acquire(f.runtime, "org.umrk.test.fdzero",
                                     reason, sizeof(reason));
    CHECK(probe >= 0);
    if (probe >= 0) close(probe);
    int status = 0;
    CHECK(waitpid(worker, &status, 0) == worker);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    fixture_teardown(&f);
}

static void test_enabled_stop_is_not_immediately_undone(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "stopenabled.pak", "org.umrk.test.stopenabled",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_enable(sup, "org.umrk.test.stopenabled",
                                   reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.stopenabled",
                                reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.stopenabled",
                         JW_SVC_STATE_RUNNING, 3000));
    CHECK(jw_svc_supervisor_stop(sup, "org.umrk.test.stopenabled",
                                 reason, sizeof(reason)));
    for (int i = 0; i < 100; i++) {
        jw_svc_supervisor_tick(sup);
        usleep(20000);
    }
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.stopenabled");
    CHECK(e != NULL && e->desired_enabled);
    CHECK(e != NULL && e->pgid <= 0);
    CHECK(e != NULL && e->state == JW_SVC_STATE_STOPPED);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_running_policy_survives_rescan(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "policy.pak", "org.umrk.test.policy",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\","
                      "\"lifecycle\":{\"game\":\"stop\"}");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_enable(sup, "org.umrk.test.policy",
                                   reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.policy",
                                reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.policy",
                         JW_SVC_STATE_RUNNING, 3000));

    /* A package rescan may install a new policy for the NEXT generation,
     * but the owned group's reserved launch-time policy remains "stop". */
    fixture_write_pak(&f, "policy.pak", "org.umrk.test.policy",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\","
                      "\"lifecycle\":{\"game\":\"ignore\"}");
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.policy");
    CHECK(e != NULL &&
          e->manifest.lifecycle_game == JW_SVC_LIFECYCLE_GAME_IGNORE);
    CHECK(e != NULL &&
          e->active_lifecycle_game == JW_SVC_LIFECYCLE_GAME_STOP);

    char stuck[JW_SVC_SUPERVISOR_ID_BUF];
    CHECK(jw_svc_supervisor_game_launch_begin(sup, stuck, sizeof(stuck)) == 1);
    CHECK(stuck[0] == '\0');
    for (int i = 0; i < 100; i++) {
        jw_svc_supervisor_tick(sup);
        usleep(20000);
    }
    e = jw_svc_supervisor_find(sup, "org.umrk.test.policy");
    CHECK(e != NULL && e->pgid <= 0);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_suspend_and_storage_lifecycle_stops(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "suspend.pak", "org.umrk.test.suspendpolicy",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\","
                      "\"lifecycle\":{\"stop_on_suspend\":true}");
    fixture_write_pak(&f, "storage.pak", "org.umrk.test.storagepolicy",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\","
                      "\"lifecycle\":{\"stop_on_storage_change\":true}");
    fixture_write_pak(&f, "ignore.pak", "org.umrk.test.lifecycleignore",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 3);
    const char *suspend_id = "org.umrk.test.suspendpolicy";
    const char *storage_id = "org.umrk.test.storagepolicy";
    const char *ignore_id = "org.umrk.test.lifecycleignore";
    CHECK(jw_svc_supervisor_run(sup, suspend_id, reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, storage_id, reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, ignore_id, reason, sizeof(reason)));
    CHECK(wait_for_state(sup, suspend_id, JW_SVC_STATE_RUNNING, 3000));
    CHECK(wait_for_state(sup, storage_id, JW_SVC_STATE_RUNNING, 3000));
    CHECK(wait_for_state(sup, ignore_id, JW_SVC_STATE_RUNNING, 3000));

    const jw_svc_supervised *suspend_entry =
        jw_svc_supervisor_find(sup, suspend_id);
    pid_t first_suspend_pgid = suspend_entry ? suspend_entry->pgid : -1;
    char stuck[JW_SVC_SUPERVISOR_ID_BUF];

    /* A lifecycle edge must not hijack a stop that another caller already
     * owns, nor turn crash cleanup into an automatic post-transition resume. */
    jw_svc_supervised *pending = (jw_svc_supervised *)suspend_entry;
    bool old_autostart_pending = pending && pending->autostart_pending;
    if (pending) {
        pending->pending_stop_reason = JW_SVC_STOP_INTENTIONAL;
    }
    CHECK(jw_svc_supervisor_suspend_begin(sup, stuck, sizeof(stuck)) == 0);
    suspend_entry = jw_svc_supervisor_find(sup, suspend_id);
    CHECK(suspend_entry && suspend_entry->pgid == first_suspend_pgid);
    CHECK(suspend_entry &&
          suspend_entry->pending_stop_reason == JW_SVC_STOP_INTENTIONAL);
    CHECK(suspend_entry && !suspend_entry->lifecycle_restart_pending);
    CHECK(suspend_entry &&
          suspend_entry->autostart_pending == old_autostart_pending);
    pending = (jw_svc_supervised *)suspend_entry;
    if (pending) {
        pending->pending_stop_reason = JW_SVC_STOP_LEADER_EXITED;
    }
    CHECK(jw_svc_supervisor_suspend_begin(sup, stuck, sizeof(stuck)) == 0);
    suspend_entry = jw_svc_supervisor_find(sup, suspend_id);
    CHECK(suspend_entry && suspend_entry->pgid == first_suspend_pgid);
    CHECK(suspend_entry &&
          suspend_entry->pending_stop_reason == JW_SVC_STOP_LEADER_EXITED);
    CHECK(suspend_entry && !suspend_entry->lifecycle_restart_pending);
    pending = (jw_svc_supervised *)suspend_entry;
    if (pending) {
        pending->pending_stop_reason = JW_SVC_STOP_NONE;
    }

    CHECK(jw_svc_supervisor_suspend_begin(sup, stuck, sizeof(stuck)) == 1);
    CHECK(stuck[0] == '\0');
    suspend_entry = jw_svc_supervisor_find(sup, suspend_id);
    const jw_svc_supervised *storage_entry =
        jw_svc_supervisor_find(sup, storage_id);
    const jw_svc_supervised *ignore_entry =
        jw_svc_supervisor_find(sup, ignore_id);
    CHECK(suspend_entry && suspend_entry->pgid <= 0);
    CHECK(suspend_entry && suspend_entry->lifecycle_restart_pending);
    CHECK(storage_entry && storage_entry->pgid > 0);
    CHECK(ignore_entry && ignore_entry->pgid > 0);

    /* The daemon loop does not tick while the platform sleep call blocks.
     * Its first post-resume ticks consume the remembered restart once. */
    bool suspend_restarted =
        wait_for_state(sup, suspend_id, JW_SVC_STATE_RUNNING, 3000);
    CHECK(suspend_restarted);
    suspend_entry = jw_svc_supervisor_find(sup, suspend_id);
    if (!suspend_restarted && suspend_entry) {
        fprintf(stderr,
                "suspend restart diagnostic: state=%s pgid=%ld pending=%d "
                "reason=%s\n",
                jw_svc_effective_state_name(suspend_entry->state),
                (long)suspend_entry->pgid,
                suspend_entry->lifecycle_restart_pending,
                suspend_entry->control.last_transition_reason);
    }
    CHECK(suspend_entry && suspend_entry->pgid != first_suspend_pgid);
    CHECK(suspend_entry && !suspend_entry->lifecycle_restart_pending);

    CHECK(jw_svc_supervisor_storage_change_begin(
              sup, stuck, sizeof(stuck)) == 1);
    CHECK(stuck[0] == '\0');
    storage_entry = jw_svc_supervisor_find(sup, storage_id);
    CHECK(storage_entry && storage_entry->pgid <= 0);
    CHECK(storage_entry && storage_entry->lifecycle_restart_pending);
    /* An explicit Disable while a lifecycle restart is pending must win. */
    CHECK(jw_svc_supervisor_disable(sup, storage_id, reason,
                                    sizeof(reason)));
    for (int i = 0; i < 100; i++) {
        jw_svc_supervisor_tick(sup);
        usleep(20000);
    }
    storage_entry = jw_svc_supervisor_find(sup, storage_id);
    CHECK(storage_entry && storage_entry->pgid <= 0);
    CHECK(storage_entry && !storage_entry->lifecycle_restart_pending);
    ignore_entry = jw_svc_supervisor_find(sup, ignore_id);
    CHECK(ignore_entry && ignore_entry->pgid > 0);

    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

typedef enum {
    TEST_ASYNC_STOP = 0,
    TEST_ASYNC_DISABLE,
    TEST_ASYNC_RESTART,
} test_async_operation;

/* Stop/Disable/Restart are now asynchronous. A storage/suspend edge during
 * their grace window must report the still-live group without stealing its
 * pending reason or post-stop action. */
static void test_async_control_stop_blocks_lifecycle(void) {
    for (int operation = TEST_ASYNC_STOP;
         operation <= TEST_ASYNC_RESTART; operation++) {
        fixture f;
        fixture_setup(&f);
        const char *id = "org.umrk.test.asyncpolicy";
        fixture_write_pak(
            &f, "asyncpolicy.pak", id,
            "trap '' TERM; while :; do sleep 1; done",
            ",\"restart\":\"no\","
            "\"lifecycle\":{\"stop_on_storage_change\":true,"
            "\"stop_on_suspend\":true}");

        char reason[JW_SVC_REASON_BUF];
        jw_svc_supervisor *sup = fixture_open(&f, reason);
        CHECK(sup != NULL);
        CHECK(jw_svc_supervisor_scan(sup) == 1);
        CHECK(jw_svc_supervisor_enable(sup, id, reason, sizeof(reason)));
        CHECK(jw_svc_supervisor_run(sup, id, reason, sizeof(reason)));
        CHECK(wait_for_state(sup, id, JW_SVC_STATE_RUNNING, 3000));

        bool accepted = operation == TEST_ASYNC_DISABLE
            ? jw_svc_supervisor_disable(sup, id, reason, sizeof(reason))
            : operation == TEST_ASYNC_RESTART
                  ? jw_svc_supervisor_restart(sup, id, reason, sizeof(reason))
                  : jw_svc_supervisor_stop(sup, id, reason, sizeof(reason));
        CHECK(accepted);
        const jw_svc_supervised *e = jw_svc_supervisor_find(sup, id);
        CHECK(e && e->pgid > 0);
        CHECK(e && e->state == JW_SVC_STATE_STOPPING);
        CHECK(e && e->pending_stop_reason == JW_SVC_STOP_INTENTIONAL);
        jw_svc_post_stop_action expected_post =
            operation == TEST_ASYNC_RESTART ? JW_SVC_POST_STOP_RESTART
                                            : JW_SVC_POST_STOP_NONE;
        CHECK(e && e->post_stop == expected_post);
        CHECK(e && !e->lifecycle_restart_pending);

        char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
        CHECK(jw_svc_supervisor_storage_change_blocked(
            sup, stuck, sizeof(stuck)));
        CHECK(strcmp(stuck, id) == 0);
        CHECK(jw_svc_supervisor_storage_change_begin(
                  sup, stuck, sizeof(stuck)) == 0);
        CHECK(strcmp(stuck, id) == 0);
        e = jw_svc_supervisor_find(sup, id);
        CHECK(e && e->pending_stop_reason == JW_SVC_STOP_INTENTIONAL);
        CHECK(e && e->post_stop == expected_post);
        CHECK(e && !e->lifecycle_restart_pending);

        CHECK(jw_svc_supervisor_suspend_begin(
                  sup, stuck, sizeof(stuck)) == 0);
        CHECK(strcmp(stuck, id) == 0);
        e = jw_svc_supervisor_find(sup, id);
        CHECK(e && e->pending_stop_reason == JW_SVC_STOP_INTENTIONAL);
        CHECK(e && e->post_stop == expected_post);
        CHECK(e && !e->lifecycle_restart_pending);

        /* Cancel a queued Restart for cleanup, then prove normal release. */
        CHECK(jw_svc_supervisor_stop(sup, id, reason, sizeof(reason)));
        CHECK(wait_for_released(sup, id, 5000));
        jw_svc_supervisor_close(sup);
        fixture_teardown(&f);
    }
}

static void test_safe_unmount_followup_latch_matrix(void) {
    CHECK(!jw_svc_storage_should_suppress_followup_tick(0, false));
    CHECK(!jw_svc_storage_should_suppress_followup_tick(1, false));
    CHECK(!jw_svc_storage_should_suppress_followup_tick(0, true));
    CHECK(!jw_svc_storage_should_suppress_followup_tick(-1, true));
    CHECK(jw_svc_storage_should_suppress_followup_tick(1, true));
    CHECK(jw_svc_storage_should_suppress_followup_tick(3, true));
}

/* stop_unverified_logged belongs to one reserved generation. Once that group
 * disappears, a replacement must not inherit a false safe-unmount blocker. */
static void test_unverified_latch_resets_for_replacement(void) {
    fixture f;
    fixture_setup(&f);
    const char *id = "org.umrk.test.latchreset";
    fixture_write_pak(
        &f, "latchreset.pak", id,
        "trap '' TERM; while :; do sleep 1; done",
        ",\"restart\":\"no\","
        "\"lifecycle\":{\"stop_on_storage_change\":true}");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_run(sup, id, reason, sizeof(reason)));
    CHECK(wait_for_state(sup, id, JW_SVC_STATE_RUNNING, 3000));
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, id);
    pid_t old_pgid = e ? e->pgid : -1;
    CHECK(old_pgid > 0);
    CHECK(jw_svc_supervisor_restart(sup, id, reason, sizeof(reason)));
    jw_svc_supervised *mutable_entry =
        (jw_svc_supervised *)jw_svc_supervisor_find(sup, id);
    if (mutable_entry) {
        mutable_entry->stop_unverified_logged = true;
    }
    char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
    CHECK(jw_svc_supervisor_storage_change_blocked(
        sup, stuck, sizeof(stuck)));
    CHECK(strcmp(stuck, id) == 0);

    CHECK(wait_for_state(sup, id, JW_SVC_STATE_RUNNING, 5000));
    e = jw_svc_supervisor_find(sup, id);
    CHECK(e && e->pgid > 0 && e->pgid != old_pgid);
    CHECK(e && !e->stop_unverified_logged);
    CHECK(!jw_svc_supervisor_storage_change_blocked(
        sup, stuck, sizeof(stuck)));
    CHECK(stuck[0] == '\0');

    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

/* A locked old generation remains a safety fact after persistent intent is
 * disabled. The state may clear only after a successful lease probe. */
static void test_disable_preserves_stale_generation(void) {
    fixture f;
    fixture_setup(&f);
    const char *id = "org.umrk.test.staledisable";
    fixture_write_pak(
        &f, "staledisable.pak", id,
        "while :; do sleep 1; done",
        ",\"restart\":\"no\","
        "\"lifecycle\":{\"stop_on_storage_change\":true}");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *owner = fixture_open(&f, reason);
    CHECK(owner != NULL);
    CHECK(jw_svc_supervisor_scan(owner) == 1);
    CHECK(jw_svc_supervisor_run(owner, id, reason, sizeof(reason)));
    CHECK(wait_for_state(owner, id, JW_SVC_STATE_RUNNING, 3000));

    jw_svc_supervisor *observer = fixture_open(&f, reason);
    CHECK(observer != NULL);
    CHECK(jw_svc_supervisor_scan(observer) == 1);
    const jw_svc_supervised *e = jw_svc_supervisor_find(observer, id);
    CHECK(e && e->state == JW_SVC_STATE_STALE_GENERATION);
    CHECK(jw_svc_supervisor_disable(observer, id, reason, sizeof(reason)));
    e = jw_svc_supervisor_find(observer, id);
    CHECK(e && !e->desired_enabled && !e->session_run);
    CHECK(e && e->state == JW_SVC_STATE_STALE_GENERATION);
    char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
    CHECK(jw_svc_supervisor_storage_change_blocked(
        observer, stuck, sizeof(stuck)));
    CHECK(strcmp(stuck, id) == 0);

    CHECK(jw_svc_supervisor_stop_all(owner) == 0);
    jw_svc_supervisor_close(owner);
    CHECK(wait_for_state(observer, id, JW_SVC_STATE_DISABLED, 5000));
    e = jw_svc_supervisor_find(observer, id);
    CHECK(e && e->pgid <= 0 && !e->session_run && !e->desired_enabled);
    CHECK(!jw_svc_supervisor_storage_change_blocked(
        observer, stuck, sizeof(stuck)));
    jw_svc_supervisor_close(observer);
    fixture_teardown(&f);
}

/* A control operation that reports store-failed must not have signalled the
 * old group or queued a replacement behind the caller's back. */
static void test_store_failure_precedes_stop_and_restart(void) {
    fixture f;
    fixture_setup(&f);
    const char *id = "org.umrk.test.storeordering";
    fixture_write_pak(&f, "storeordering.pak", id,
                      "trap '' TERM; while :; do sleep 1; done",
                      ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_run(sup, id, reason, sizeof(reason)));
    CHECK(wait_for_state(sup, id, JW_SVC_STATE_RUNNING, 3000));
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, id);
    pid_t pgid = e ? e->pgid : -1;
    CHECK(pgid > 0);
    CHECK(fixture_reject_control_writes(&f, true));

    CHECK(!jw_svc_supervisor_stop(sup, id, reason, sizeof(reason)));
    CHECK(strcmp(reason, "store-failed") == 0);
    e = jw_svc_supervisor_find(sup, id);
    CHECK(e && e->pgid == pgid && e->state == JW_SVC_STATE_RUNNING);
    CHECK(e && !e->stop_requested && e->post_stop == JW_SVC_POST_STOP_NONE);
    CHECK(e && e->pending_stop_reason == JW_SVC_STOP_NONE);
    CHECK(e && e->session_run);

    CHECK(!jw_svc_supervisor_restart(sup, id, reason, sizeof(reason)));
    CHECK(strcmp(reason, "store-failed") == 0);
    e = jw_svc_supervisor_find(sup, id);
    CHECK(e && e->pgid == pgid && e->state == JW_SVC_STATE_RUNNING);
    CHECK(e && !e->stop_requested && e->post_stop == JW_SVC_POST_STOP_NONE);
    CHECK(e && e->pending_stop_reason == JW_SVC_STOP_NONE);

    CHECK(fixture_reject_control_writes(&f, false));
    CHECK(jw_svc_supervisor_stop(sup, id, reason, sizeof(reason)));
    CHECK(wait_for_released(sup, id, 5000));
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

/* SVC-1's "reported and not startable" rule is about a Secondary storage
 * SOURCE (PATH-2: SDCARD_PATHS index 0 is Primary), not about which Apps/
 * subdirectory a pak sits in. Apps/<platform> and Apps/shared are two
 * directories of ONE source, so a service pak in either must be startable.
 * A previous implementation flagged every root past index 0 as Secondary,
 * which made every shared service pak permanently `unavailable`. */
static void test_shared_root_is_primary_secondary_root_is_not(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak_in(&f, f.apps_shared, "shared.pak",
                         "org.umrk.test.shared",
                         "while :; do sleep 1; done", ",\"restart\":\"no\"");
    fixture_write_pak_in(&f, f.apps_secondary, "secondary.pak",
                         "org.umrk.test.secondary",
                         "while :; do sleep 1; done", ",\"restart\":\"no\"");
    f.secondary_scan_roots[0] = f.apps_secondary;
    f.secondary_scan_roots[1] = NULL;

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 2);

    /* Apps/shared: Primary, therefore startable. */
    const jw_svc_supervised *shared =
        jw_svc_supervisor_find(sup, "org.umrk.test.shared");
    CHECK(shared != NULL);
    CHECK(shared && shared->manifest_valid);
    CHECK(shared && !shared->on_secondary_root);
    CHECK(shared && shared->state != JW_SVC_STATE_UNAVAILABLE);
    CHECK(jw_svc_supervisor_enable(sup, "org.umrk.test.shared",
                                   reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.shared",
                                reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.shared",
                         JW_SVC_STATE_RUNNING, 3000));

    /* A genuine Secondary source: reported, but never startable. */
    const jw_svc_supervised *secondary =
        jw_svc_supervisor_find(sup, "org.umrk.test.secondary");
    CHECK(secondary != NULL);
    CHECK(secondary && secondary->on_secondary_root);
    CHECK(secondary && secondary->state == JW_SVC_STATE_UNAVAILABLE);
    CHECK(!jw_svc_supervisor_run(sup, "org.umrk.test.secondary",
                                 reason, sizeof(reason)));
    CHECK(strcmp(reason, "unavailable") == 0);
    CHECK(!jw_svc_supervisor_enable(sup, "org.umrk.test.secondary",
                                    reason, sizeof(reason)));
    CHECK(strcmp(reason, "unavailable") == 0);

    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

/* CTL-1 must distinguish a lifecycle-policy stop from an ordinary user Stop,
 * and must name which policy fired: "whether a lifecycle policy stop is in
 * force (reason: game, storage, suspend, package)". */
static void test_lifecycle_stop_reasons_are_distinct(void) {
    CHECK(strcmp(jw_svc_stop_reason_lifecycle_slug(
                     JW_SVC_STOP_LIFECYCLE_GAME), "game") == 0);
    CHECK(strcmp(jw_svc_stop_reason_lifecycle_slug(
                     JW_SVC_STOP_LIFECYCLE_SUSPEND), "suspend") == 0);
    CHECK(strcmp(jw_svc_stop_reason_lifecycle_slug(
                     JW_SVC_STOP_LIFECYCLE_STORAGE), "storage") == 0);
    CHECK(jw_svc_stop_reason_lifecycle_slug(JW_SVC_STOP_INTENTIONAL) == NULL);
    CHECK(jw_svc_stop_reason_lifecycle_slug(JW_SVC_STOP_NONE) == NULL);
    CHECK(jw_svc_stop_reason_lifecycle_slug(
              JW_SVC_STOP_LEADER_EXITED) == NULL);

    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "suspendy.pak", "org.umrk.test.suspendy",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"on-failure\""
                      ",\"lifecycle\":{\"stop_on_suspend\":true}");
    fixture_write_pak(&f, "storagey.pak", "org.umrk.test.storagey",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"on-failure\""
                      ",\"lifecycle\":{\"stop_on_storage_change\":true}");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 2);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.suspendy",
                                reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.storagey",
                                reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.suspendy",
                         JW_SVC_STATE_RUNNING, 3000));
    CHECK(wait_for_state(sup, "org.umrk.test.storagey",
                         JW_SVC_STATE_RUNNING, 3000));

    char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
    CHECK(jw_svc_supervisor_suspend_begin(sup, stuck, sizeof(stuck)) == 1);
    CHECK(stuck[0] == '\0');
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.suspendy");
    /* Recorded as a suspend policy stop, NOT as a generic intentional stop --
     * otherwise CTL-1 cannot tell the user why their service went down. */
    CHECK(e && e->pending_stop_reason == JW_SVC_STOP_LIFECYCLE_SUSPEND);
    CHECK(e && strcmp(jw_svc_stop_reason_lifecycle_slug(
                          e->pending_stop_reason), "suspend") == 0);
    /* Still excluded from the on-failure restart policy. */
    CHECK(e && !e->backoff.breaker_open && e->backoff.count == 0);
    CHECK(e && e->lifecycle_restart_pending);

    /* Resume starts exactly one replacement generation. */
    CHECK(wait_for_state(sup, "org.umrk.test.suspendy",
                         JW_SVC_STATE_RUNNING, 3000));
    e = jw_svc_supervisor_find(sup, "org.umrk.test.suspendy");
    CHECK(e && !e->lifecycle_restart_pending);
    CHECK(e && e->pending_stop_reason == JW_SVC_STOP_NONE);

    CHECK(jw_svc_supervisor_storage_change_begin(
              sup, stuck, sizeof(stuck)) == 1);
    CHECK(stuck[0] == '\0');
    e = jw_svc_supervisor_find(sup, "org.umrk.test.storagey");
    CHECK(e && e->pending_stop_reason == JW_SVC_STOP_LIFECYCLE_STORAGE);
    CHECK(e && strcmp(jw_svc_stop_reason_lifecycle_slug(
                          e->pending_stop_reason), "storage") == 0);
    CHECK(e && !e->backoff.breaker_open && e->backoff.count == 0);
    CHECK(e && e->lifecycle_restart_pending);
    /* A stopped storage-sensitive service stays down while the source is
     * absent; ordinary daemon ticks are not a remount signal. */
    for (int i = 0; i < 20; i++) {
        jw_svc_supervisor_tick(sup);
        usleep(20000);
    }
    e = jw_svc_supervisor_find(sup, "org.umrk.test.storagey");
    CHECK(e && e->pgid <= 0 && e->lifecycle_restart_pending);
    jw_svc_supervisor_storage_change_resume(sup);
    CHECK(wait_for_state(sup, "org.umrk.test.storagey",
                         JW_SVC_STATE_RUNNING, 3000));
    e = jw_svc_supervisor_find(sup, "org.umrk.test.storagey");
    CHECK(e && !e->lifecycle_restart_pending);
    CHECK(e && e->pending_stop_reason == JW_SVC_STOP_NONE);

    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

/* CTL-1 Restart must not block the daemon loop either: it stops, returns, and
 * tick starts exactly one replacement once the old group is proven absent. */
static void test_restart_is_async_and_starts_one_generation(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "restarty.pak", "org.umrk.test.restarty",
                      "trap '' TERM; while :; do sleep 1; done",
                      ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.restarty",
                                reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.restarty",
                         JW_SVC_STATE_RUNNING, 3000));
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.restarty");
    pid_t first_pgid = e ? e->pgid : -1;
    CHECK(first_pgid > 0);

    long long t0 = monotonic_ms();
    CHECK(jw_svc_supervisor_restart(sup, "org.umrk.test.restarty",
                                    reason, sizeof(reason)));
    CHECK(monotonic_ms() - t0 < 250);
    e = jw_svc_supervisor_find(sup, "org.umrk.test.restarty");
    CHECK(e && e->state == JW_SVC_STATE_STOPPING);
    CHECK(e && e->pgid == first_pgid); /* old group still reserved */

    /* Run must be refused while the old group is still owned: starting now
     * would overlap the reservation SVC-1 forbids overlapping. */
    CHECK(!jw_svc_supervisor_run(sup, "org.umrk.test.restarty",
                                 reason, sizeof(reason)));
    CHECK(strcmp(reason, "stopping") == 0);

    CHECK(wait_for_state(sup, "org.umrk.test.restarty",
                         JW_SVC_STATE_RUNNING, 5000));
    e = jw_svc_supervisor_find(sup, "org.umrk.test.restarty");
    CHECK(e && e->pgid > 0);
    CHECK(e && e->pgid != first_pgid); /* exactly one NEW generation */
    CHECK(e && e->session_run);

    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

/* Shutdown must discard an in-flight CTL-1 Restart. Otherwise the post-stop
 * action fires from inside stop_all()'s synchronous stop and launches a fresh
 * generation with no daemon left to supervise it. */
static void test_shutdown_discards_pending_restart(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "shutdowny.pak", "org.umrk.test.shutdowny",
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.shutdowny",
                                reason, sizeof(reason)));
    CHECK(wait_for_state(sup, "org.umrk.test.shutdowny",
                         JW_SVC_STATE_RUNNING, 3000));

    /* Restart requested, then shutdown before tick can complete it. */
    CHECK(jw_svc_supervisor_restart(sup, "org.umrk.test.shutdowny",
                                    reason, sizeof(reason)));
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.shutdowny");
    CHECK(e && e->post_stop == JW_SVC_POST_STOP_RESTART);

    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    e = jw_svc_supervisor_find(sup, "org.umrk.test.shutdowny");
    CHECK(e && e->pgid <= 0);   /* nothing relaunched */
    CHECK(e && e->post_stop == JW_SVC_POST_STOP_NONE);
    /* And no stray child survived the shutdown. */
    errno = 0;
    CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

/* A record with nothing left to retain -- package gone, not enabled, no
 * session intent, no live or stale generation -- must be shed from both the
 * control store and the canonical list, or Settings -> Services can never
 * return to "absent on a clean system" after an uninstall. */
static void test_spent_record_is_shed(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "spent.pak", "org.umrk.test.spent",
                      "exit 0", ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    /* Give it real history: enabled, run, then disabled again. */
    CHECK(jw_svc_supervisor_enable(sup, "org.umrk.test.spent",
                                   reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_disable(sup, "org.umrk.test.spent",
                                    reason, sizeof(reason)));
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.spent");
    CHECK(e && e->control_loaded);
    CHECK(jw_svc_supervisor_entry_is_listable(e));

    /* Uninstall the package. */
    char pak[PATH_MAX];
    jw__join(pak, sizeof(pak), f.apps_primary, "spent.pak");
    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", pak);
    CHECK(system(cmd) == 0);

    CHECK(jw_svc_supervisor_scan(sup) == 0);
    CHECK(jw_svc_supervisor_find(sup, "org.umrk.test.spent") == NULL);
    CHECK(jw_svc_supervisor_count(sup) == 0);

    /* A fresh supervisor over the same store must not resurrect the row. */
    jw_svc_supervisor_close(sup);
    sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 0);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

/* An entry whose id cannot survive a client's reverse-DNS validation must
 * never reach the wire: a strict CTL-1 client rejects the whole `list`
 * response on one bad row, so a single invalid manifest would blank the
 * entire Services screen. */
static void test_noncanonical_ids_stay_off_the_wire(void) {
    fixture f;
    fixture_setup(&f);
    /* Invalid manifest with no recoverable service.id: the entry falls back
     * to the pak directory name, which is not reverse-DNS. */
    fixture_write_pak(&f, "Weird_Name.pak", "NotReverseDNS", "exit 0",
                      ",\"restart\":\"no\"");
    fixture_write_pak(&f, "good.pak", "org.umrk.test.good", "exit 0",
                      ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    jw_svc_supervisor_scan(sup);

    int listable = 0;
    for (int i = 0; i < jw_svc_supervisor_count(sup); i++) {
        const jw_svc_supervised *e = jw_svc_supervisor_at(sup, i);
        if (jw_svc_supervisor_entry_is_listable(e)) {
            CHECK(jw_service_id_is_reverse_dns(e->service_id));
            listable++;
        }
    }
    CHECK(listable == 1);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_reservation_write_failure_reaps_child(void) {
    fixture f;
    fixture_setup(&f);
    const char *id = "org.umrk.test.reservationfail";
    fixture_write_pak(&f, "reservationfail.pak", id,
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);

    int lease = jw_svc_lease_acquire(f.runtime, id, reason, sizeof(reason));
    CHECK(lease >= 0);
    if (lease >= 0) close(lease);
    char service_dir[PATH_MAX];
    jw__join3(service_dir, sizeof(service_dir), f.runtime, "services", id);
    char reservation_path[PATH_MAX];
    jw__join(reservation_path, sizeof(reservation_path), service_dir,
             "reservation");
    jw__mkdir(reservation_path); /* rename(temp, reservation) must fail */

    CHECK(!jw_svc_supervisor_run(sup, id, reason, sizeof(reason)));
    CHECK(strcmp(reason, "reservation-failed") == 0);
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, id);
    CHECK(e != NULL && e->pgid <= 0);
    CHECK(e != NULL && e->lease_fd < 0);

    /* No child may remain waitable. The buggy cleanup proved absence but
     * forgot the final reap, so this call returned the leaked zombie pid. */
    errno = 0;
    CHECK(waitpid(-1, NULL, WNOHANG) == -1 && errno == ECHILD);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_hostile_id_tail_bound_and_status_shape(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "evil.pak", "../../escape", "exit 0", NULL);
    fixture_write_pak(&f, "status.pak", "org.umrk.test.status",
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 2);
    CHECK(jw_svc_supervisor_find(sup, "../../escape") == NULL);
    const jw_svc_supervised *evil = jw_svc_supervisor_find(sup, "evil");
    CHECK(evil != NULL && evil->state == JW_SVC_STATE_UNAVAILABLE);
    CHECK(!jw_svc_supervisor_service_id_is_safe("../../escape"));
    CHECK(jw_svc_supervisor_service_id_is_safe("org.umrk.test.status"));
    CHECK(jw_svc_supervisor_bound_log_tail(-1) == JW_SVC_LOG_TAIL_DEFAULT);
    CHECK(jw_svc_supervisor_bound_log_tail(1) == 1);
    CHECK(jw_svc_supervisor_bound_log_tail(INT_MAX) == JW_SVC_LOG_TAIL_MAX);
    char scan_root[32];
    CHECK(jw_svc_supervisor_join_scan_root(scan_root, sizeof(scan_root),
                                            "/Apps", "mac"));
    CHECK(strcmp(scan_root, "/Apps/mac") == 0);
    CHECK(!jw_svc_supervisor_join_scan_root(scan_root, sizeof(scan_root),
                                             NULL, "mac"));
    CHECK(!jw_svc_supervisor_join_scan_root(scan_root, 4, "/Apps", "shared"));
    CHECK(jw_svc_supervisor_ctl_op_requires_id("status"));
    CHECK(jw_svc_supervisor_ctl_op_requires_id("export-logs"));
    CHECK(!jw_svc_supervisor_ctl_op_requires_id("not-an-operation"));
    CHECK(!jw_svc_supervisor_ctl_op_requires_id(NULL));

    CHECK(jw_svc_supervisor_enable(sup, "org.umrk.test.status",
                                   reason, sizeof(reason)));
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.status");
    long long epoch_floor_us = ((long long)time(NULL) - 5LL) * 1000000LL;
    CHECK(e != NULL && e->control.last_transition_at_us >= epoch_floor_us);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.status",
                                reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_stop_all(sup) == 0);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_many_entries_do_not_use_uninitialized_runtime_fields(void) {
    fixture f;
    fixture_setup(&f);
    for (int i = 0; i < 24; i++) {
        char pak[32];
        char id[64];
        snprintf(pak, sizeof(pak), "many-%02d.pak", i);
        snprintf(id, sizeof(id), "org.umrk.test.many-%02d", i);
        fixture_write_pak(&f, pak, id, "exit 0", NULL);
    }
    int sentinel = open("/dev/null", O_RDONLY);
    CHECK(sentinel >= 0);
    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 24);
    CHECK(fcntl(sentinel, F_GETFD) >= 0);
    close(sentinel);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_retained_control_row_survives_missing_package(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "retained.pak", "org.umrk.test.retained",
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_enable(sup, "org.umrk.test.retained",
                                   reason, sizeof(reason)));
    jw_svc_supervisor_close(sup);

    char installed[PATH_MAX], removed[PATH_MAX];
    jw__join(installed, sizeof(installed), f.apps_primary, "retained.pak");
    jw__join(removed, sizeof(removed), f.root, "removed-retained.pak");
    CHECK(rename(installed, removed) == 0);

    sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    const jw_svc_supervised *e =
        jw_svc_supervisor_find(sup, "org.umrk.test.retained");
    CHECK(e != NULL);
    CHECK(e && !e->pak_present);
    CHECK(e && e->desired_enabled);
    CHECK(e && e->state == JW_SVC_STATE_UNAVAILABLE);
    CHECK(e && strcmp(e->reject_reason, "package-missing") == 0);
    CHECK(e && strcmp(e->installed_package_version, "1.2.3") == 0);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_missing_package_scan_does_not_create_lease_tree(void) {
    fixture f;
    fixture_setup(&f);
    const char *id = "org.umrk.test.retained-only";
    char reason[JW_SVC_REASON_BUF];
    char db_path[PATH_MAX];
    jw__join(db_path, sizeof(db_path), f.state, "services-control.db");

    jw_svc_control_store *store = NULL;
    CHECK(jw_svc_control_store_open(db_path, &store, reason, sizeof(reason)));
    jw_svc_control_state control = {0};
    control.start_with_leaf = true;
    snprintf(control.installed_package_id,
             sizeof(control.installed_package_id), "%s", "retained.pak");
    snprintf(control.installed_package_version,
             sizeof(control.installed_package_version), "%s", "1.2.3");
    CHECK(store != NULL &&
          jw_svc_control_store_put(store, id, &control,
                                   reason, sizeof(reason)));
    jw_svc_control_store_close(store);

    char services_dir[PATH_MAX];
    jw__join(services_dir, sizeof(services_dir), f.runtime, "services");
    struct stat st;
    CHECK(lstat(services_dir, &st) != 0 && errno == ENOENT);

    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, id);
    CHECK(e != NULL && e->state == JW_SVC_STATE_UNAVAILABLE);
    CHECK(lstat(services_dir, &st) != 0 && errno == ENOENT);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_locked_runtime_lease_survives_without_pak_or_control_row(void) {
    fixture f;
    fixture_setup(&f);
    const char *id = "org.umrk.test.runtime-only";
    char reason[JW_SVC_REASON_BUF];
    int old_lease = jw_svc_lease_acquire(f.runtime, id, reason,
                                         sizeof(reason));
    CHECK(old_lease >= 0);
    jw_svc_reservation reservation = {
        .pgid = getpid(),
        .launch_instant_us = jw_svc_reservation_now_us(),
        .game_policy = JW_SVC_RESERVATION_GAME_STOP,
        .stop_on_storage_change = true,
        .stop_on_suspend = true,
    };
    CHECK(jw_svc_reservation_write(f.runtime, id, &reservation,
                                   reason, sizeof(reason)));

    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, id);
    CHECK(e != NULL);
    CHECK(e && !e->control_loaded);
    CHECK(e && !e->pak_present);
    CHECK(e && e->state == JW_SVC_STATE_STALE_GENERATION);
    CHECK(e && strcmp(e->reject_reason, "package-missing") == 0);
    CHECK(e && e->active_stop_on_storage_change);
    CHECK(e && e->active_stop_on_suspend);

    jw_svc_supervisor_close(sup);
    if (old_lease >= 0) close(old_lease);
    fixture_teardown(&f);
}

static void test_stale_missing_policy_fails_safe_for_all_stop_triggers(void) {
    fixture f;
    fixture_setup(&f);
    const char *id = "org.umrk.test.stalefailsafe";
    fixture_write_pak(&f, "stalefailsafe.pak", id,
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");
    char reason[JW_SVC_REASON_BUF];
    int old_lease = jw_svc_lease_acquire(f.runtime, id, reason,
                                         sizeof(reason));
    CHECK(old_lease >= 0);
    /* Deliberately no reservation file: a locked old generation with a
     * missing policy snapshot must enable every stop trigger fail-safe. */
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, id);
    CHECK(e && e->state == JW_SVC_STATE_STALE_GENERATION);
    CHECK(e && e->active_lifecycle_game == JW_SVC_LIFECYCLE_GAME_STOP);
    CHECK(e && e->active_stop_on_storage_change);
    CHECK(e && e->active_stop_on_suspend);
    char stuck[JW_SVC_SUPERVISOR_ID_BUF];
    CHECK(jw_svc_supervisor_suspend_begin(sup, stuck, sizeof(stuck)) == 0);
    CHECK(strcmp(stuck, id) == 0);
    CHECK(jw_svc_supervisor_storage_change_begin(sup, stuck,
                                                  sizeof(stuck)) == 0);
    CHECK(strcmp(stuck, id) == 0);

    jw_svc_supervisor_close(sup);
    if (old_lease >= 0) close(old_lease);
    fixture_teardown(&f);
}

static void test_package_quiesce_restores_only_persistent_intent(void) {
    fixture f;
    fixture_setup(&f);
    const char *enabled_id = "org.umrk.test.packageenabled";
    const char *session_id = "org.umrk.test.packagesession";
    fixture_write_pak(&f, "package-enabled.pak", enabled_id,
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");
    fixture_write_pak(&f, "package-session.pak", session_id,
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 2);
    CHECK(jw_svc_supervisor_enable(sup, enabled_id, reason,
                                   sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, enabled_id, reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, session_id, reason, sizeof(reason)));
    CHECK(wait_for_state(sup, enabled_id, JW_SVC_STATE_RUNNING, 3000));
    CHECK(wait_for_state(sup, session_id, JW_SVC_STATE_RUNNING, 3000));

    char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
    CHECK(jw_svc_supervisor_package_begin(
        sup, "stage-app-17", stuck, sizeof(stuck), reason, sizeof(reason)));
    CHECK(stuck[0] == '\0');
    CHECK(jw_svc_supervisor_package_active(sup));
    const jw_svc_supervised *enabled =
        jw_svc_supervisor_find(sup, enabled_id);
    const jw_svc_supervised *session =
        jw_svc_supervisor_find(sup, session_id);
    CHECK(enabled && enabled->pgid <= 0 && enabled->package_blocked);
    CHECK(enabled && enabled->package_snapshot_valid &&
          enabled->package_restore_desired && enabled->package_was_effective);
    CHECK(enabled && enabled->pending_stop_reason ==
                         JW_SVC_STOP_LIFECYCLE_PACKAGE);
    CHECK(enabled && strcmp(jw_svc_stop_reason_lifecycle_slug(
                                enabled->pending_stop_reason),
                            "package") == 0);
    CHECK(session && session->pgid <= 0 && session->package_blocked);
    CHECK(session && session->package_snapshot_valid &&
          !session->package_restore_desired && session->package_was_effective);

    /* Persistent/session controls and service starts are frozen until the
     * matching package operation performs its final scan. */
    CHECK(!jw_svc_supervisor_run(sup, session_id, reason, sizeof(reason)));
    CHECK(strcmp(reason, "package-in-progress") == 0);
    CHECK(!jw_svc_supervisor_disable(sup, enabled_id, reason,
                                     sizeof(reason)));
    CHECK(strcmp(reason, "package-in-progress") == 0);
    CHECK(!jw_svc_supervisor_package_end(sup, "wrong-operation", reason,
                                         sizeof(reason)));
    CHECK(strcmp(reason, "operation-mismatch") == 0);
    CHECK(jw_svc_supervisor_package_active(sup));

    /* Stand in for replacement bytes: the final scan must consume this new
     * manifest before releasing and starting the desired service. */
    fixture_write_pak(&f, "package-enabled.pak", enabled_id,
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\","
                      "\"lifecycle\":{\"stop_on_suspend\":true}");
    CHECK(jw_svc_supervisor_package_end(sup, "stage-app-17", reason,
                                       sizeof(reason)));
    CHECK(!jw_svc_supervisor_package_active(sup));
    CHECK(wait_for_state(sup, enabled_id, JW_SVC_STATE_RUNNING, 3000));
    enabled = jw_svc_supervisor_find(sup, enabled_id);
    session = jw_svc_supervisor_find(sup, session_id);
    CHECK(enabled && enabled->desired_enabled && enabled->pgid > 0);
    CHECK(enabled && enabled->manifest.stop_on_suspend);
    CHECK(enabled && !enabled->package_blocked &&
          !enabled->package_snapshot_valid);
    CHECK(session && !session->desired_enabled && !session->session_run &&
          session->pgid <= 0);
    CHECK(session && session->state == JW_SVC_STATE_DISABLED);

    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_package_quiesce_stale_preflight_changes_nothing(void) {
    fixture f;
    fixture_setup(&f);
    const char *id = "org.umrk.test.packagestale";
    fixture_write_pak(&f, "package-stale.pak", id,
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");
    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *owner = fixture_open(&f, reason);
    CHECK(owner != NULL);
    CHECK(jw_svc_supervisor_scan(owner) == 1);
    CHECK(jw_svc_supervisor_run(owner, id, reason, sizeof(reason)));
    CHECK(wait_for_state(owner, id, JW_SVC_STATE_RUNNING, 3000));
    const jw_svc_supervised *owned = jw_svc_supervisor_find(owner, id);
    pid_t owner_pgid = owned ? owned->pgid : -1;
    CHECK(owner_pgid > 0);

    jw_svc_supervisor *observer = fixture_open(&f, reason);
    CHECK(observer != NULL);
    CHECK(jw_svc_supervisor_scan(observer) == 1);
    const jw_svc_supervised *stale =
        jw_svc_supervisor_find(observer, id);
    CHECK(stale && stale->state == JW_SVC_STATE_STALE_GENERATION);
    char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
    CHECK(!jw_svc_supervisor_package_begin(
        observer, "leaf-update", stuck, sizeof(stuck), reason,
        sizeof(reason)));
    CHECK(strcmp(reason, "stale-generation") == 0);
    CHECK(strcmp(stuck, id) == 0);
    CHECK(!jw_svc_supervisor_package_active(observer));
    CHECK(kill(owner_pgid, 0) == 0);

    jw_svc_supervisor_close(observer);
    CHECK(jw_svc_supervisor_stop_all(owner) == 0);
    jw_svc_supervisor_close(owner);
    fixture_teardown(&f);
}

static void test_target_mutation_quiesces_only_matching_service(void) {
    fixture f;
    fixture_setup(&f);
    const char *target_id = "org.umrk.test.mutationtarget";
    const char *other_id = "org.umrk.test.mutationother";
    fixture_write_pak(&f, "target.pak", target_id,
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");
    fixture_write_pak(&f, "other.pak", other_id,
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");
    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 2);
    CHECK(jw_svc_supervisor_run(sup, target_id, reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, other_id, reason, sizeof(reason)));
    CHECK(wait_for_state(sup, target_id, JW_SVC_STATE_RUNNING, 3000));
    CHECK(wait_for_state(sup, other_id, JW_SVC_STATE_RUNNING, 3000));
    const jw_svc_supervised *other = jw_svc_supervisor_find(sup, other_id);
    pid_t other_pgid = other ? other->pgid : -1;

    char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
    CHECK(jw_svc_supervisor_mutation_begin(
        sup, "pakrat-test-1", "apps/target.pak", target_id,
        stuck, sizeof(stuck), reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_mutation_active(sup));
    CHECK(jw_svc_supervisor_mutation_target_blocked(
        sup, "Apps/apps/target.pak"));
    CHECK(!jw_svc_supervisor_mutation_target_blocked(
        sup, "apps/other.pak"));
    const jw_svc_supervised *target =
        jw_svc_supervisor_find(sup, target_id);
    other = jw_svc_supervisor_find(sup, other_id);
    CHECK(target && target->pgid <= 0 && target->package_blocked);
    CHECK(other && other->pgid == other_pgid && other->pgid > 0);
    CHECK(!jw_svc_supervisor_run(sup, target_id, reason, sizeof(reason)));
    CHECK(strcmp(reason, "package-in-progress") == 0);
    CHECK(!jw_svc_supervisor_package_begin(
        sup, "leaf-update", stuck, sizeof(stuck), reason, sizeof(reason)));
    CHECK(strcmp(reason, "package-in-progress") == 0);

    CHECK(jw_svc_supervisor_mutation_end(
        sup, "pakrat-test-1", true, reason, sizeof(reason)));
    CHECK(!jw_svc_supervisor_mutation_active(sup));
    CHECK(wait_for_state(sup, target_id, JW_SVC_STATE_RUNNING, 3000));
    target = jw_svc_supervisor_find(sup, target_id);
    other = jw_svc_supervisor_find(sup, other_id);
    CHECK(target && target->pgid > 0 && !target->desired_enabled &&
          target->session_run && !target->package_blocked);
    CHECK(other && other->pgid == other_pgid && other->pgid > 0);

    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_target_mutation_floor_transitions_start_disabled(void) {
    fixture f;
    fixture_setup(&f);
    const char *id = "org.umrk.test.mutationfloor";
    fixture_write_floor(&f, "floor.pak", id);
    char reason[JW_SVC_REASON_BUF];
    char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 0);

    CHECK(jw_svc_supervisor_mutation_begin(
        sup, "pakrat-floor-real", "apps/floor.pak", id,
        stuck, sizeof(stuck), reason, sizeof(reason)));
    fixture_write_pak(&f, "floor.pak", id,
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");
    CHECK(jw_svc_supervisor_mutation_end(
        sup, "pakrat-floor-real", true, reason, sizeof(reason)));
    const jw_svc_supervised *entry = jw_svc_supervisor_find(sup, id);
    CHECK(entry && entry->pak_present && entry->manifest_valid);
    CHECK(entry && !entry->desired_enabled && !entry->session_run &&
          entry->pgid <= 0 && entry->state == JW_SVC_STATE_DISABLED);

    CHECK(jw_svc_supervisor_enable(sup, id, reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, id, reason, sizeof(reason)));
    CHECK(wait_for_state(sup, id, JW_SVC_STATE_RUNNING, 3000));
    CHECK(jw_svc_supervisor_mutation_begin(
        sup, "pakrat-real-floor", "apps/floor.pak", id,
        stuck, sizeof(stuck), reason, sizeof(reason)));
    fixture_write_floor(&f, "floor.pak", id);
    CHECK(jw_svc_supervisor_mutation_end(
        sup, "pakrat-real-floor", false, reason, sizeof(reason)));
    CHECK(!jw_svc_supervisor_mutation_active(sup));

    /* Reinstalling the real pak after a committed removal/rollback must not
       resurrect either persistent enablement or the old session run bit. */
    fixture_write_pak(&f, "floor.pak", id,
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    entry = jw_svc_supervisor_find(sup, id);
    CHECK(entry && !entry->desired_enabled && !entry->session_run &&
          entry->pgid <= 0 && entry->state == JW_SVC_STATE_DISABLED);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_target_mutation_stale_preflight_changes_nothing(void) {
    fixture f;
    fixture_setup(&f);
    const char *id = "org.umrk.test.mutationstale";
    fixture_write_pak(&f, "stale.pak", id,
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");
    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *owner = fixture_open(&f, reason);
    CHECK(owner != NULL);
    CHECK(jw_svc_supervisor_scan(owner) == 1);
    CHECK(jw_svc_supervisor_run(owner, id, reason, sizeof(reason)));
    CHECK(wait_for_state(owner, id, JW_SVC_STATE_RUNNING, 3000));
    const jw_svc_supervised *owned = jw_svc_supervisor_find(owner, id);
    pid_t owner_pgid = owned ? owned->pgid : -1;

    jw_svc_supervisor *observer = fixture_open(&f, reason);
    CHECK(observer != NULL);
    CHECK(jw_svc_supervisor_scan(observer) == 1);
    char stuck[JW_SVC_SUPERVISOR_ID_BUF] = {0};
    CHECK(!jw_svc_supervisor_mutation_begin(
        observer, "pakrat-stale", "apps/stale.pak", id,
        stuck, sizeof(stuck), reason, sizeof(reason)));
    CHECK(strcmp(reason, "stale-generation") == 0);
    CHECK(strcmp(stuck, id) == 0);
    CHECK(!jw_svc_supervisor_mutation_active(observer));
    CHECK(kill(owner_pgid, 0) == 0);

    jw_svc_supervisor_close(observer);
    CHECK(jw_svc_supervisor_stop_all(owner) == 0);
    jw_svc_supervisor_close(owner);
    fixture_teardown(&f);
}

static void test_circuit_breaker_opens(void) {
    fixture f;
    fixture_setup(&f);
    fixture_write_pak(&f, "crasher.pak", "org.umrk.test.crasher", "exit 1", ",\"restart\":\"on-failure\"");

    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 1);
    CHECK(jw_svc_supervisor_run(sup, "org.umrk.test.crasher", reason, sizeof(reason)));

    /* Tick until the breaker opens. Each crash is a failure; collapsing the
     * backoff delay each tick keeps the test fast while still exercising the
     * real 5-failures-in-5-minutes breaker. */
    bool failed = false;
    for (int i = 0; i < 40000 && !failed; i++) {
        jw_svc_supervisor_tick(sup);
        const jw_svc_supervised *ro = jw_svc_supervisor_find(sup, "org.umrk.test.crasher");
        if (ro && ro->state == JW_SVC_STATE_FAILED) {
            failed = true;
            break;
        }
        if (ro && ro->state == JW_SVC_STATE_BACKOFF) {
            jw_svc_supervised *m = (jw_svc_supervised *)ro;
            m->backoff_retry_at_ms = 0;
        }
        usleep(1000);
    }
    CHECK(failed);
    const jw_svc_supervised *e = jw_svc_supervisor_find(sup, "org.umrk.test.crasher");
    CHECK(e->state == JW_SVC_STATE_FAILED);
    CHECK(e->backoff.breaker_open);
    CHECK(e->pgid <= 0);

    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_life1_subscriber_authentication(void) {
    fixture f;
    fixture_setup(&f);
    const char *one = "org.umrk.test.subscriber";
    const char *two = "org.umrk.test.other";
    fixture_write_pak(&f, "subscriber.pak", one,
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");
    fixture_write_pak(&f, "other.pak", two,
                      "while :; do sleep 1; done", ",\"restart\":\"no\"");
    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 2);
    CHECK(jw_svc_supervisor_run(sup, one, reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, two, reason, sizeof(reason)));
    CHECK(wait_for_state(sup, one, JW_SVC_STATE_RUNNING, 3000));
    CHECK(wait_for_state(sup, two, JW_SVC_STATE_RUNNING, 3000));
    const jw_svc_supervised *entry_one = jw_svc_supervisor_find(sup, one);
    const jw_svc_supervised *entry_two = jw_svc_supervisor_find(sup, two);
    CHECK(entry_one && entry_one->pgid > 0);
    CHECK(entry_two && entry_two->pgid > 0);

    jw_svc_subscriber_binding binding;
    CHECK(jw_svc_supervisor_authenticate_subscriber(
              sup, one, entry_one->pgid, &binding) ==
          JW_SVC_SUBSCRIBER_ACCEPTED);
    CHECK(binding.peer_pid == entry_one->pgid);
    CHECK(binding.pgid == entry_one->pgid);
    CHECK(binding.launch_instant_us == entry_one->launch_instant_us);
    CHECK(jw_svc_supervisor_revalidate_subscriber(sup, one, &binding));
    CHECK(jw_svc_supervisor_authenticate_subscriber(
              sup, one, entry_two->pgid, NULL) ==
          JW_SVC_SUBSCRIBER_WRONG_GROUP);
    CHECK(jw_svc_supervisor_authenticate_subscriber(
              sup, one, getpid(), NULL) == JW_SVC_SUBSCRIBER_FOREGROUND);
    CHECK(jw_svc_supervisor_authenticate_subscriber(
              sup, "org.umrk.test.missing", getpid(), NULL) ==
          JW_SVC_SUBSCRIBER_UNKNOWN_SERVICE);
    CHECK(jw_svc_supervisor_authenticate_subscriber(
              sup, one, -1, NULL) ==
          JW_SVC_SUBSCRIBER_MISSING_CREDENTIAL);

    /* A second daemon sees the still-held lease as a stale generation and
     * cannot authenticate the old pid as a current subscriber. */
    jw_svc_supervisor *observer = fixture_open(&f, reason);
    CHECK(observer != NULL);
    CHECK(jw_svc_supervisor_scan(observer) == 2);
    CHECK(jw_svc_supervisor_authenticate_subscriber(
              observer, one, entry_one->pgid, NULL) ==
          JW_SVC_SUBSCRIBER_STALE_GENERATION);
    jw_svc_supervisor_close(observer);

    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    CHECK(!jw_svc_supervisor_revalidate_subscriber(sup, one, &binding));
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_game_stop_uses_validated_coordinator_first(void) {
    fixture f;
    fixture_setup(&f);
    const char *clean_id = "org.umrk.test.coordinated";
    const char *stuck_id = "org.umrk.test.coordinatorstuck";
    const char *ordinary_id = "org.umrk.test.coordinatorinvalid";
    fixture_write_pak(&f, "coordinated.pak", clean_id,
                      "trap 'kill \"$child\"; wait \"$child\"; exit 0' TERM; "
                      "sleep 30 & child=$!; wait \"$child\"",
                      ",\"restart\":\"no\"");
    fixture_write_pak(&f, "coordinatorstuck.pak", stuck_id,
                      "trap '' TERM; while :; do sleep 1; done",
                      ",\"restart\":\"no\"");
    fixture_write_pak(&f, "coordinatorinvalid.pak", ordinary_id,
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\"");
    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 3);

    CHECK(jw_svc_supervisor_run(sup, clean_id, reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, stuck_id, reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, ordinary_id, reason, sizeof(reason)));
    CHECK(wait_for_state(sup, clean_id, JW_SVC_STATE_RUNNING, 3000));
    CHECK(wait_for_state(sup, stuck_id, JW_SVC_STATE_RUNNING, 3000));
    CHECK(wait_for_state(sup, ordinary_id, JW_SVC_STATE_RUNNING, 3000));
    const jw_svc_supervised *entry =
        jw_svc_supervisor_find(sup, clean_id);
    jw_svc_subscriber_binding binding;
    CHECK(entry && jw_svc_supervisor_authenticate_subscriber(
        sup, clean_id, entry->pgid, &binding) ==
        JW_SVC_SUBSCRIBER_ACCEPTED);
    jw_svc_stop_result result;
    CHECK(jw_svc_supervisor_game_stop_service(
        sup, clean_id, &binding, &result, reason, sizeof(reason)));
    CHECK(result.coordinator_first);
    CHECK(!result.group_term_sent);
    CHECK(result.verified_absent);

    entry = jw_svc_supervisor_find(sup, stuck_id);
    CHECK(entry && jw_svc_supervisor_authenticate_subscriber(
        sup, stuck_id, entry->pgid, &binding) ==
        JW_SVC_SUBSCRIBER_ACCEPTED);
    CHECK(jw_svc_supervisor_game_stop_service(
        sup, stuck_id, &binding, &result, reason, sizeof(reason)));
    CHECK(result.coordinator_first);
    CHECK(result.group_term_sent);
    CHECK(result.escalated_to_kill);
    CHECK(result.verified_absent);

    entry = jw_svc_supervisor_find(sup, ordinary_id);
    CHECK(entry != NULL);
    binding = (jw_svc_subscriber_binding){
        .peer_pid = entry->pgid + 1,
        .pgid = entry->pgid,
        .launch_instant_us = entry->launch_instant_us,
    };
    CHECK(jw_svc_supervisor_game_stop_service(
        sup, ordinary_id, &binding, &result, reason, sizeof(reason)));
    CHECK(!result.coordinator_first);
    CHECK(result.group_term_sent);
    CHECK(result.verified_absent);

    jw_svc_supervisor_game_finish(sup);
    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_game_gate_stop_and_resume(void) {
    fixture f;
    fixture_setup(&f);
    const char *stop_id = "org.umrk.test.gamestop";
    const char *notify_id = "org.umrk.test.gamenotify";
    const char *ignore_id = "org.umrk.test.gameignore";
    fixture_write_pak(&f, "gamestop.pak", stop_id,
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\",\"lifecycle\":{\"game\":\"stop\"}");
    fixture_write_pak(&f, "gamenotify.pak", notify_id,
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\",\"lifecycle\":{\"game\":\"notify\"}");
    fixture_write_pak(&f, "gameignore.pak", ignore_id,
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\",\"lifecycle\":{\"game\":\"ignore\"}");
    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *sup = fixture_open(&f, reason);
    CHECK(sup != NULL);
    CHECK(jw_svc_supervisor_scan(sup) == 3);
    CHECK(!jw_svc_supervisor_game_has_participant(sup));
    CHECK(jw_svc_supervisor_run(sup, ignore_id, reason, sizeof(reason)));
    CHECK(wait_for_state(sup, ignore_id, JW_SVC_STATE_RUNNING, 3000));
    CHECK(!jw_svc_supervisor_game_has_participant(sup));
    CHECK(jw_svc_supervisor_run(sup, stop_id, reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_run(sup, notify_id, reason, sizeof(reason)));
    CHECK(wait_for_state(sup, stop_id, JW_SVC_STATE_RUNNING, 3000));
    CHECK(wait_for_state(sup, notify_id, JW_SVC_STATE_RUNNING, 3000));
    CHECK(jw_svc_supervisor_game_has_participant(sup));

    CHECK(jw_svc_supervisor_game_stop_service(
        sup, stop_id, NULL, NULL, reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_game_active(sup));
    const jw_svc_supervised *stop = jw_svc_supervisor_find(sup, stop_id);
    CHECK(stop && stop->pgid <= 0 && stop->game_restart_pending);
    CHECK(jw_svc_supervisor_game_has_participant(sup));
    CHECK(!jw_svc_supervisor_run(sup, stop_id, reason, sizeof(reason)));
    CHECK(strcmp(reason, "lifecycle-in-progress") == 0);
    CHECK(!jw_svc_supervisor_restart(sup, notify_id,
                                     reason, sizeof(reason)));
    CHECK(strcmp(reason, "lifecycle-in-progress") == 0);

    /* Runtime mode notify fallback selects the otherwise-notify service for
     * a verified stop. Ignore remains live and startable outside the gate. */
    CHECK(jw_svc_supervisor_game_stop_service(
        sup, notify_id, NULL, NULL, reason, sizeof(reason)));
    const jw_svc_supervised *notify =
        jw_svc_supervisor_find(sup, notify_id);
    const jw_svc_supervised *ignore =
        jw_svc_supervisor_find(sup, ignore_id);
    CHECK(notify && notify->pgid <= 0 && notify->game_restart_pending);
    CHECK(ignore && ignore->pgid > 0);
    CHECK(jw_svc_supervisor_game_has_participant(sup));
    for (int i = 0; i < 20; i++) {
        jw_svc_supervisor_tick(sup);
        usleep(20000);
    }
    stop = jw_svc_supervisor_find(sup, stop_id);
    notify = jw_svc_supervisor_find(sup, notify_id);
    CHECK(stop && stop->pgid <= 0 && stop->game_restart_pending);
    CHECK(notify && notify->pgid <= 0 && notify->game_restart_pending);

    jw_svc_supervisor_game_finish(sup);
    CHECK(!jw_svc_supervisor_game_active(sup));
    CHECK(wait_for_state(sup, stop_id, JW_SVC_STATE_RUNNING, 3000));
    CHECK(wait_for_state(sup, notify_id, JW_SVC_STATE_RUNNING, 3000));
    stop = jw_svc_supervisor_find(sup, stop_id);
    notify = jw_svc_supervisor_find(sup, notify_id);
    CHECK(stop && !stop->game_restart_pending);
    CHECK(notify && !notify->game_restart_pending);

    CHECK(jw_svc_supervisor_stop_all(sup) == 0);
    CHECK(!jw_svc_supervisor_game_has_participant(sup));
    jw_svc_supervisor_close(sup);
    fixture_teardown(&f);
}

static void test_recovered_game_gate_suppresses_autostart(void) {
    fixture f;
    fixture_setup(&f);
    const char *notify_id = "org.umrk.test.recoverednotify";
    const char *ignore_id = "org.umrk.test.recoveredignore";
    fixture_write_pak(&f, "recoverednotify.pak", notify_id,
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\",\"lifecycle\":{\"game\":\"notify\"}");
    fixture_write_pak(&f, "recoveredignore.pak", ignore_id,
                      "while :; do sleep 1; done",
                      ",\"restart\":\"no\",\"lifecycle\":{\"game\":\"ignore\"}");
    char reason[JW_SVC_REASON_BUF];
    jw_svc_supervisor *first = fixture_open(&f, reason);
    CHECK(first != NULL);
    CHECK(jw_svc_supervisor_scan(first) == 2);
    CHECK(jw_svc_supervisor_enable(first, notify_id, reason, sizeof(reason)));
    CHECK(jw_svc_supervisor_enable(first, ignore_id, reason, sizeof(reason)));
    jw_svc_supervisor_close(first);

    jw_svc_supervisor *recovered = fixture_open(&f, reason);
    CHECK(recovered != NULL);
    jw_svc_supervisor_game_set_active(recovered, true);
    CHECK(jw_svc_supervisor_scan(recovered) == 2);
    for (int i = 0; i < 100; i++) {
        jw_svc_supervisor_tick(recovered);
        usleep(20000);
    }
    const jw_svc_supervised *notify =
        jw_svc_supervisor_find(recovered, notify_id);
    const jw_svc_supervised *ignore =
        jw_svc_supervisor_find(recovered, ignore_id);
    CHECK(notify && notify->pgid <= 0 && notify->autostart_pending);
    CHECK(ignore && ignore->pgid > 0);

    jw_svc_supervisor_game_finish(recovered);
    CHECK(wait_for_state(recovered, notify_id, JW_SVC_STATE_RUNNING, 3000));
    CHECK(jw_svc_supervisor_stop_all(recovered) == 0);
    jw_svc_supervisor_close(recovered);
    fixture_teardown(&f);
}

int main(int argc, char **argv) {
    g_program_path = argv[0];
    if (argc == 3 && strcmp(argv[1], "--fd-zero-worker") == 0) {
        return run_descriptor_zero_worker(argv[2]);
    }
    test_clean_exit();
    test_stop_sequence();
    test_ignore_term_escalates();
    test_orphan_descendant();
    test_persistent_vs_session_across_restart();
    test_invalid_manifest_isolated();
    test_duplicate_ids_both_unavailable();
    test_stale_generation_blocks_second_daemon();
    test_descriptor_zero_lease_is_released();
    test_enabled_stop_is_not_immediately_undone();
    test_running_policy_survives_rescan();
    test_suspend_and_storage_lifecycle_stops();
    test_async_control_stop_blocks_lifecycle();
    test_safe_unmount_followup_latch_matrix();
    test_unverified_latch_resets_for_replacement();
    test_disable_preserves_stale_generation();
    test_store_failure_precedes_stop_and_restart();
    test_reservation_write_failure_reaps_child();
    test_shared_root_is_primary_secondary_root_is_not();
    test_lifecycle_stop_reasons_are_distinct();
    test_restart_is_async_and_starts_one_generation();
    test_shutdown_discards_pending_restart();
    test_spent_record_is_shed();
    test_noncanonical_ids_stay_off_the_wire();
    test_hostile_id_tail_bound_and_status_shape();
    test_many_entries_do_not_use_uninitialized_runtime_fields();
    test_retained_control_row_survives_missing_package();
    test_missing_package_scan_does_not_create_lease_tree();
    test_locked_runtime_lease_survives_without_pak_or_control_row();
    test_stale_missing_policy_fails_safe_for_all_stop_triggers();
    test_package_quiesce_restores_only_persistent_intent();
    test_package_quiesce_stale_preflight_changes_nothing();
    test_target_mutation_quiesces_only_matching_service();
    test_target_mutation_floor_transitions_start_disabled();
    test_target_mutation_stale_preflight_changes_nothing();
    test_life1_subscriber_authentication();
    test_game_stop_uses_validated_coordinator_first();
    test_game_gate_stop_and_resume();
    test_recovered_game_gate_suppresses_autostart();
    test_circuit_breaker_opens();

    if (g_failures == 0) {
        printf("PASS supervisor-test\n");
        return 0;
    }
    fprintf(stderr, "FAIL supervisor-test (%d failures)\n", g_failures);
    return 1;
}
