/* jawaka-ledd must not outlive the daemon that started it. A daemon killed on
   the MLP1 left its ledd reparented to init, holding the session log on the
   card, and the next shutdown could not remount the card read-only. Nor may
   jawakad restart a ledd that dies at once in a loop.

   Usage: ledd-spawn-test <host-built jawaka-ledd> */
#include "internal/platform/ledd_spawn.h"

#include <stdio.h>

static int g_failures;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            g_failures++;                                             \
        }                                                             \
    } while (0)

/* Pure logic, so it runs on every host. */
static void test_restart_decision(void) {
    jw_ledd_restarts r;
    jw_ledd_restarts_reset(&r);

    CHECK(!jw_ledd_should_restart(&r, 1, 50000, 100000),
          "a ledd that rejected its arguments is not restarted");

    /* A long-running ledd always comes back, and does not count. */
    for (int i = 0; i < 5; i++) {
        CHECK(jw_ledd_should_restart(&r, 0, JW_LEDD_QUICK_EXIT_MS, 100000 + i),
              "crash after a long run %d is restarted", i);
    }
    CHECK(r.quick_exits == 0, "long runs are not quick exits");

    /* Quick exits: the third inside a minute stops the restarts. */
    CHECK(jw_ledd_should_restart(&r, 0, 10, 200000), "first quick exit is restarted");
    CHECK(jw_ledd_should_restart(&r, 0, 10, 200100), "second quick exit is restarted");
    CHECK(!jw_ledd_should_restart(&r, 0, 10, 200200), "third quick exit gives up");

    /* Quick exits spread wider than the window start over. */
    jw_ledd_restarts_reset(&r);
    CHECK(jw_ledd_should_restart(&r, 0, 10, 300000), "quick exit 1");
    CHECK(jw_ledd_should_restart(&r, 0, 10, 330000), "quick exit 2");
    CHECK(jw_ledd_should_restart(&r, 0, 10, 300000 + JW_LEDD_QUICK_WINDOW_MS + 1),
          "a quick exit after the window starts a new count");
    CHECK(jw_ledd_should_restart(&r, 0, 10, 300000 + JW_LEDD_QUICK_WINDOW_MS + 2),
          "second exit of the new count");

    /* A new setting clears the count. */
    jw_ledd_restarts_reset(&r);
    CHECK(jw_ledd_should_restart(&r, 0, 10, 500000), "count cleared by a reset");
}

#if !defined(__linux__)
int main(void) {
    test_restart_decision();
    if (g_failures) {
        fprintf(stderr, "ledd-spawn-test: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("ledd-spawn-test: restart decision ok; parent-death checks skipped, they are Linux-only");
    return 0;
}
#else

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>

static const char *g_ledd;

static void sleep_ms(long ms) {
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/* The state letter from /proc/<pid>/stat, or 0 once the process is gone. */
static char proc_state(pid_t pid) {
    char path[64];
    char buf[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    char *end = strrchr(buf, ')');
    return end && end[1] == ' ' ? end[2] : 0;
}

static bool wait_for_state(pid_t pid, char want, long timeout_ms) {
    for (long waited = 0; waited <= timeout_ms; waited += 10) {
        if (proc_state(pid) == want) return true;
        sleep_ms(10);
    }
    return false;
}

/* An orphan is reparented to this process (a subreaper), so it can be reaped
   here once its own parent is gone. */
static bool wait_for_exit(pid_t pid, long timeout_ms, int *status) {
    for (long waited = 0; waited <= timeout_ms; waited += 10) {
        pid_t got = waitpid(pid, status, WNOHANG);
        if (got == pid) return true;
        if (got < 0 && errno != ECHILD) return false;
        sleep_ms(10);
    }
    return false;
}

static void stop(pid_t pid) {
    if (pid <= 0) return;
    kill(pid, SIGKILL);
    kill(pid, SIGCONT);
    waitpid(pid, NULL, 0);
}

/* Stands in for the stock LED daemon that ledd freezes while it draws. */
static pid_t start_fake_loong_light(void) {
    pid_t pid = fork();
    if (pid == 0) {
        prctl(PR_SET_NAME, (unsigned long)"loong_light", 0L, 0L, 0L);
        for (;;) pause();
    }
    char path[64];
    char comm[32] = "";
    snprintf(path, sizeof(path), "/proc/%d/comm", (int)pid);
    for (int i = 0; i < 200 && strcmp(comm, "loong_light\n") != 0; i++) {
        FILE *f = fopen(path, "r");
        if (f) {
            if (!fgets(comm, sizeof(comm), f)) comm[0] = '\0';
            fclose(f);
        }
        sleep_ms(10);
    }
    return pid;
}

/* Stands in for jawakad: starts ledd through the real spawn path, reports its
   pid, then waits to be killed. */
static pid_t start_daemon(bool hostile_sigterm, pid_t *ledd) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        if (hostile_sigterm) {
            /* Ignored and blocked both survive fork and exec. */
            sigset_t term;
            signal(SIGTERM, SIG_IGN);
            sigemptyset(&term);
            sigaddset(&term, SIGTERM);
            sigprocmask(SIG_BLOCK, &term, NULL);
        }
        char *const argv[] = { (char *)g_ledd, "static", "255", "0", "0", "5", "5", NULL };
        pid_t child = jw_ledd_spawn(g_ledd, argv);
        if (write(fds[1], &child, sizeof(child)) != (ssize_t)sizeof(child)) _exit(1);
        for (;;) pause();
    }
    close(fds[1]);
    *ledd = -1;
    if (pid > 0 && read(fds[0], ledd, sizeof(*ledd)) != (ssize_t)sizeof(*ledd)) {
        *ledd = -1;
    }
    close(fds[0]);
    return pid;
}

static void test_killed_daemon_takes_ledd_with_it(bool hostile_sigterm) {
    const char *label = hostile_sigterm ? "daemon ignoring and blocking SIGTERM" : "daemon";
    pid_t light = start_fake_loong_light();
    pid_t ledd = -1;
    pid_t daemon = start_daemon(hostile_sigterm, &ledd);
    CHECK(daemon > 0 && ledd > 0, "%s: could not start ledd", label);
    if (daemon <= 0 || ledd <= 0) {
        stop(daemon);
        stop(light);
        return;
    }

    /* A frozen stand-in means ledd is past its handlers and drawing. */
    CHECK(wait_for_state(light, 'T', 3000), "%s: ledd never froze loong_light", label);

    kill(daemon, SIGKILL);
    waitpid(daemon, NULL, 0);

    int status = 0;
    bool exited = wait_for_exit(ledd, 3000, &status);
    CHECK(exited, "%s: ledd pid %d outlived its killed parent", label, (int)ledd);
    if (exited) {
        /* Exit 0 is ledd's own SIGTERM path, the one that thaws the daemon. */
        CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "%s: ledd did not stop cleanly (status 0x%x)", label, status);
        CHECK(proc_state(light) != 'T', "%s: loong_light left frozen", label);
    } else {
        stop(ledd);
    }
    stop(light);
}

static void test_parent_already_gone_is_refused(void) {
    pid_t pid = fork();
    if (pid == 0) {
        /* The process's own pid stands for "not the parent any more". */
        _exit(jw_ledd_arm_parent_death(getpid()) == -1 ? 0 : 1);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "arming against a parent that has changed must fail");

    pid = fork();
    if (pid == 0) {
        int sig = 0;
        if (jw_ledd_arm_parent_death(getppid()) != 0) _exit(1);
        if (prctl(PR_GET_PDEATHSIG, (unsigned long)&sig, 0L, 0L, 0L) != 0) _exit(2);
        _exit(sig == SIGTERM ? 0 : 3);
    }
    waitpid(pid, &status, 0);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the death signal is SIGTERM (status 0x%x)", status);
}

int main(int argc, char **argv) {
    if (argc != 2 || access(argv[1], X_OK) != 0) {
        fprintf(stderr, "usage: %s <jawaka-ledd>\n", argv[0]);
        return 2;
    }
    g_ledd = argv[1];
    if (prctl(PR_SET_CHILD_SUBREAPER, 1L, 0L, 0L, 0L) != 0) {
        perror("PR_SET_CHILD_SUBREAPER");
        return 2;
    }

    test_restart_decision();
    test_killed_daemon_takes_ledd_with_it(false);
    test_killed_daemon_takes_ledd_with_it(true);
    test_parent_already_gone_is_refused();

    if (g_failures) {
        fprintf(stderr, "ledd-spawn-test: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("ledd-spawn-test: ok");
    return 0;
}
#endif
