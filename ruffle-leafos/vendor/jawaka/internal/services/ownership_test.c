#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/ownership.h"

/* The MLP1 release profile defines NDEBUG, but these fixtures intentionally
 * use assert() for checked setup and cleanup as well as expectations. Keep
 * assertions active so a release-flag ownership-test remains a real test and
 * cannot silently omit fork/pipe/setpgid/kill/wait operations. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <pthread.h>
#include <sys/prctl.h>
#endif

/* Real fork()/setpgid() fixtures, not mocks -- the whole point of this
 * module is to be trustworthy against the actual kernel's process table,
 * on whichever platform the test runs (see ownership.c: Linux/proc(5) and
 * macOS/libproc both get a real implementation, not a stub, precisely so
 * this test exercises the real thing on a dev machine too). */

static bool jw__wait_until(bool (*pred)(pid_t), pid_t arg, int timeout_ms) {
    int elapsed = 0;
    for (;;) {
        if (pred(arg)) {
            return true;
        }
        if (elapsed >= timeout_ms) {
            return false;
        }
        usleep(5000);
        elapsed += 5;
    }
}

static bool jw__is_absent(pid_t pgid) {
    return jw_svc_group_absent(pgid);
}

#if defined(__linux__)
static void jw__test_zero_pgid_kernel_thread_stat(void) {
    const char stat[] =
        "2 (kthreadd) S 0 0 0 0 -1 4194560 0 0 0 0 0 0 0 0 20 0 1 0";
    pid_t pgid = -1;
    bool zombie = true;
    int threads = -1;
    assert(jw_svc_proc_stat_group_state_for_test(
        stat, &pgid, &zombie, &threads));
    assert(pgid == 0);
    assert(!zombie);
    assert(threads == 1);
    puts("PASS ownership-test accepts unrelated kernel-thread pgid 0");
}

static bool jw__linux_stat_state_threads(pid_t pid, char *out_state,
                                         int *out_threads) {
    char stat_path[64];
    int n = snprintf(stat_path, sizeof(stat_path), "/proc/%ld/stat", (long)pid);
    assert(n > 0 && (size_t)n < sizeof(stat_path));

    FILE *fp = fopen(stat_path, "r");
    if (!fp) {
        return false;
    }
    char stat_text[4096];
    size_t used = fread(stat_text, 1, sizeof(stat_text) - 1, fp);
    bool failed = ferror(fp);
    fclose(fp);
    if (failed || used == 0) {
        return false;
    }
    stat_text[used] = '\0';
    const char *close_paren = strrchr(stat_text, ')');
    if (!close_paren || close_paren[1] != ' ' || close_paren[3] != ' ') {
        return false;
    }
    *out_state = close_paren[2];

    const char *cursor = close_paren + 4; /* field 4: ppid */
    for (int field = 4; field <= 20; field++) {
        char *end = NULL;
        long long value = strtoll(cursor, &end, 10);
        if (end == cursor || *end != ' ') {
            return false;
        }
        if (field == 20) {
            if (value < 0 || value > INT_MAX) {
                return false;
            }
            *out_threads = (int)value;
            return true;
        }
        while (*end == ' ') {
            end++;
        }
        cursor = end;
    }
    return false;
}

static bool jw__kernel_reports_zombie(pid_t pid) {
    char state = '\0';
    int threads = 0;
    return jw__linux_stat_state_threads(pid, &state, &threads) && state == 'Z';
}
#endif

static void jw__wait_for_unreaped_zombie(pid_t pid) {
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    assert(waitid(P_PID, (id_t)pid, &info, WEXITED | WNOWAIT) == 0);
    assert(info.si_pid == pid);
    /* Linux also exposes the unreaped task as /proc state Z, so cross-check
     * the actual parser input there. On macOS, waitid(WNOWAIT) is the kernel
     * API that proves exit while preserving the child's waitable state. */
#if defined(__linux__)
    assert(jw__kernel_reports_zombie(pid));
#endif
}

static void jw__test_rejects_nonpositive_pgid(void) {
    assert(jw_svc_group_absent(0) == false);
    assert(jw_svc_group_absent(-1) == false);
    puts("PASS ownership-test rejects non-positive pgid");
}

static void jw__test_group_tracking_release_policy(void) {
    assert(!jw_svc_group_tracking_may_end(false, false));
    assert(jw_svc_group_tracking_may_end(true, false));
    assert(jw_svc_group_tracking_may_end(false, true));
    puts("PASS ownership-test holds an unverified group until shutdown");
}

static void jw__test_simple_child(void) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        assert(setpgid(0, 0) == 0); /* group leader: pgid == its own pid */
        pause();
        _exit(0);
    }

    /* setpgid() in the child is a race against this parent reading its
     * pgid via getpgid(); poll briefly rather than assume it has already
     * taken effect. */
    pid_t child_pgid = 0;
    for (int i = 0; i < 200; i++) {
        child_pgid = getpgid(child);
        if (child_pgid == child) {
            break;
        }
        usleep(1000);
    }
    assert(child_pgid == child);

    assert(jw_svc_group_absent(child) == false);
    assert(jw_svc_process_is_live_group_member(child, child));
    assert(!jw_svc_process_is_live_group_member(child, getpgrp()));
    assert(!jw_svc_process_is_live_group_member(getpid(), child));

    assert(kill(child, SIGKILL) == 0);
    int status = 0;
    assert(waitpid(child, &status, 0) == child);

    assert(jw__wait_until(jw__is_absent, child, 2000));
    assert(!jw_svc_process_is_live_group_member(child, child));
    puts("PASS ownership-test simple child: alive=not-absent, reaped=absent");
}

/* Isolates the SZOMB/'Z' handling itself: a leader with NO live descendant
 * at all must be reported absent as soon as it becomes a zombie, without
 * requiring the test process to reap it first. The other two scenarios in
 * this file only check absence AFTER an explicit waitpid() has fully
 * reaped every process involved, which would pass even if zombie status
 * were mishandled (a fully reaped process is gone from the table either
 * way) -- this one specifically would fail if a live zombie entry were
 * miscounted as a writer. */
static void jw__test_zombie_alone_is_absent(void) {
    pid_t leader = fork();
    assert(leader >= 0);
    if (leader == 0) {
        assert(setpgid(0, 0) == 0);
        _exit(0);
    }

    pid_t pgid = leader;
    /* waitid(WNOWAIT) proves the leader has exited while deliberately
     * leaving it unreaped; Linux additionally verifies /proc state Z. */
    jw__wait_for_unreaped_zombie(leader);
    assert(jw_svc_group_absent(pgid));

    int status = 0;
    pid_t reaped = waitpid(leader, &status, 0); /* cleanup */
    assert(reaped == leader);
    puts("PASS ownership-test zombie alone (no descendant): absent without being reaped first");
}

/* The scenario contracts.md's "why the pgid is a stable reference" section
 * exists for: a zombie LEADER with a live DESCENDANT is not absent. Only
 * once every member -- leader included -- is gone or zombie is the group
 * actually free. */
static void jw__test_zombie_leader_live_descendant(void) {
    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    pid_t leader = fork();
    assert(leader >= 0);
    if (leader == 0) {
        close(ready_pipe[0]);
        assert(setpgid(0, 0) == 0); /* leader's pgid becomes its own pid */

        pid_t grandchild = fork();
        assert(grandchild >= 0);
        if (grandchild == 0) {
            /* Inherits the leader's pgid automatically; no setpgid needed
             * here. Report our own pid so the test process can kill us by
             * pid later (the leader forked us, not the test process, so
             * the test process never otherwise learns this pid). */
            char buf[32];
            int n = snprintf(buf, sizeof(buf), "%d\n", (int)getpid());
            assert(n > 0 && (size_t)n < sizeof(buf));
            assert(write(ready_pipe[1], buf, (size_t)n) == n);
            close(ready_pipe[1]);
            pause();
            _exit(0);
        }
        close(ready_pipe[1]);
        /* Exit immediately WITHOUT the test process having reaped us yet:
         * we become a zombie the moment the test process notices (via the
         * pipe closing) that we're gone, while our descendant is still
         * live and still a member of this same pgid. */
        _exit(0);
    }

    close(ready_pipe[1]);
    char buf[32] = {0};
    ssize_t n = read(ready_pipe[0], buf, sizeof(buf) - 1);
    close(ready_pipe[0]);
    assert(n > 0);
    pid_t grandchild_pid = (pid_t)atoi(buf);
    assert(grandchild_pid > 0);

    /* The leader's pgid equals its own pid (it made itself a group leader
     * before forking); the test process's parent-of-leader relationship
     * lets it identify that pgid without racing on getpgid() the way the
     * simple-child case above does, since the leader already reported
     * readiness (via the pipe write happening strictly after setpgid) by
     * the time we get here. */
    pid_t pgid = leader;

    /* The pipe proves the descendant is live. Independently wait until the
     * leader is an unreaped zombie so this cannot accidentally pass while
     * the leader itself is still a live group member. */
    jw__wait_for_unreaped_zombie(leader);
    assert(jw_svc_group_absent(pgid) == false);

    assert(kill(grandchild_pid, SIGKILL) == 0);

    /* Reap the already-proven-zombie leader. The orphaned descendant is
     * reaped by the platform's init process after SIGKILL. */
    int status = 0;
    assert(waitpid(leader, &status, 0) == leader);

    assert(jw__wait_until(jw__is_absent, pgid, 2000));
    puts("PASS ownership-test zombie leader + live descendant: "
         "not absent until BOTH are gone, reaping the leader last");
}

#if defined(__linux__)

static int jw__thread_ready_fd = -1;

static void *jw__live_worker(void *unused) {
    (void)unused;
    char ready = 'R';
    assert(write(jw__thread_ready_fd, &ready, 1) == 1);
    for (;;) {
        pause();
    }
    return NULL;
}

static bool jw__zombie_leader_has_live_thread(pid_t pid) {
    char state = '\0';
    int threads = 0;
    return jw__linux_stat_state_threads(pid, &state, &threads) &&
           state == 'Z' && threads > 1;
}

/* Linux /proc's top-level numeric directories identify thread groups, not
 * every task. pthread_exit() can leave the leader task in Z while a worker
 * thread remains live. Field 20 of /proc/PID/stat must keep that worker
 * visible even on kernels that hide /proc/PID/task after leader exit. */
static void jw__test_zombie_leader_live_thread(void) {
    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready_pipe[0]);
        assert(setpgid(0, 0) == 0);
        jw__thread_ready_fd = ready_pipe[1];
        pthread_t worker;
        assert(pthread_create(&worker, NULL, jw__live_worker, NULL) == 0);
        pthread_exit(NULL);
    }

    close(ready_pipe[1]);
    char ready = '\0';
    assert(read(ready_pipe[0], &ready, 1) == 1);
    close(ready_pipe[0]);
    assert(ready == 'R');
    assert(jw__wait_until(jw__zombie_leader_has_live_thread, child, 2000));
    assert(jw_svc_group_absent(child) == false);

    assert(kill(-child, SIGKILL) == 0);
    assert(waitpid(child, NULL, 0) == child);
    assert(jw__wait_until(jw__is_absent, child, 2000));
    puts("PASS ownership-test zombie leader + live thread: not absent");
}

/* A Linux task name may contain a newline. /proc/PID/stat still contains
 * the complete parenthesized comm field, so line-oriented fgets() parsing
 * can stop early and accidentally skip a live member. */
static void jw__test_newline_in_comm(void) {
    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) {
        close(ready_pipe[0]);
        assert(setpgid(0, 0) == 0);
        assert(prctl(PR_SET_NAME, "odd)\nname", 0, 0, 0) == 0);
        char ready = 'R';
        assert(write(ready_pipe[1], &ready, 1) == 1);
        close(ready_pipe[1]);
        pause();
        _exit(0);
    }

    close(ready_pipe[1]);
    char ready = '\0';
    assert(read(ready_pipe[0], &ready, 1) == 1);
    close(ready_pipe[0]);
    assert(ready == 'R');
    assert(jw_svc_group_absent(child) == false);

    assert(kill(child, SIGKILL) == 0);
    assert(waitpid(child, NULL, 0) == child);
    assert(jw__wait_until(jw__is_absent, child, 2000));
    puts("PASS ownership-test newline in comm: live group remains visible");
}

#endif

int main(void) {
    jw__test_rejects_nonpositive_pgid();
    jw__test_group_tracking_release_policy();
    jw__test_simple_child();
    jw__test_zombie_alone_is_absent();
    jw__test_zombie_leader_live_descendant();
#if defined(__linux__)
    jw__test_zero_pgid_kernel_thread_stat();
    jw__test_zombie_leader_live_thread();
    jw__test_newline_in_comm();
#endif
    puts("PASS ownership-test");
    return 0;
}
