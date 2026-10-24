#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/stop.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

/* This file's absence checks are deliberately NOT jw_svc_group_absent()
 * (internal/services/ownership.c) -- that function lives on a separate,
 * independently-reviewed branch/PR and is meant to be wired in by
 * whatever assembles the real supervisor. stop.c's own contract is that
 * it works with ANY absence_check a caller supplies; this test suite's
 * job is to verify the SEQUENCE (signal, wait, escalate, wait, report),
 * not to re-validate a general-purpose absence primitive that has its
 * own test suite already. */

/* Real (not fake) absence signal for a single test-controlled child with
 * no descendants: WNOHANG-waits it. Returns true exactly once the child
 * has exited (reaping it in the process) or was already reaped. This is
 * a correct absence signal for this narrow scenario -- it does not
 * generalize to orphaned descendants or multiple members, which is
 * exactly the harder problem jw_svc_group_absent() exists to solve. */
static bool jw__test_real_absent(pid_t pgid) {
    int status = 0;
    pid_t r = waitpid(pgid, &status, WNOHANG);
    if (r == pgid) {
        return true; /* just exited; reaped now */
    }
    if (r < 0 && errno == ECHILD) {
        return true; /* no such child: already reaped */
    }
    return false;
}

/* Fault injection for the "stop cannot be verified" path -- see
 * phase-a2-supervisor.md: "the deterministic contract test is fault
 * injection at the absence-verification point... do not spend a blocker
 * trying to make FUSE emulate a kernel hang." This always lies that the
 * group is still present, regardless of what actually happened to the
 * real child stop.c's own kill() calls act on. */
static bool jw__test_always_present(pid_t pgid) {
    (void)pgid;
    return false;
}

/* child_body receives an fd to signal readiness on: it must write exactly
 * one byte and close the fd only once EVERY piece of its own setup is
 * complete (installing a signal disposition, etc.), then block. */
typedef void (*jw__test_child_body_fn)(int ready_fd);

static pid_t jw__test_fork_group_leader(jw__test_child_body_fn child_body) {
    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    pid_t pid = fork();
    assert(pid >= 0);
    if (pid == 0) {
        close(ready_pipe[0]);
        /* setpgid() must happen before child_body()'s own setup: this
         * parent's kill(-pid, ...) calls target group `pid`, so nothing
         * in the child may be considered "ready" until it has actually
         * joined that group. */
        assert(setpgid(0, 0) == 0);
        child_body(ready_pipe[1]);
        _exit(1); /* child_body() must not return */
    }

    close(ready_pipe[1]);
    /* Block until child_body() itself reports readiness, rather than
     * polling getpgid() -- that would only prove the group was joined,
     * not that a per-body setup step (e.g. installing SIG_IGN) has also
     * finished, which is exactly the race that made the ignore-TERM
     * fixture flaky: the parent could send SIGTERM after group-join but
     * before SIG_IGN was installed, so the child died on the default
     * disposition instead of ever needing escalation. */
    char ready = 0;
    ssize_t n = read(ready_pipe[0], &ready, 1);
    close(ready_pipe[0]);
    assert(n == 1);
    return pid;
}

static void jw__test_signal_ready(int ready_fd) {
    char byte = 'R';
    assert(write(ready_fd, &byte, 1) == 1);
    close(ready_fd);
}

static void jw__test_child_default_term(int ready_fd) {
    jw__test_signal_ready(ready_fd);
    pause(); /* default SIGTERM disposition terminates us immediately */
}

static void jw__test_child_ignores_term(int ready_fd) {
    assert(signal(SIGTERM, SIG_IGN) != SIG_ERR);
    jw__test_signal_ready(ready_fd); /* only after SIG_IGN is installed */
    for (;;) {
        pause(); /* SIGTERM is ignored; only SIGKILL can end this */
    }
}

static void jw__test_rejects_invalid_args(void) {
    jw_svc_stop_result r;

    r = jw_svc_stop_group(0, 100, jw__test_real_absent);
    assert(!r.verified_absent && !r.escalated_to_kill);

    r = jw_svc_stop_group(-1, 100, jw__test_real_absent);
    assert(!r.verified_absent && !r.escalated_to_kill);

    /* pgid == 1 must be refused, not treated as an ordinary group: POSIX
     * defines kill(-1, sig) as "every process the caller may signal", not
     * "process group 1" -- this function must never send that. */
    r = jw_svc_stop_group(1, 100, jw__test_always_present);
    assert(!r.verified_absent && !r.escalated_to_kill);

    r = jw_svc_stop_group(12345, 100, NULL);
    assert(!r.verified_absent && !r.escalated_to_kill);

    puts("PASS stop-test rejects invalid pgid/absence_check");
}

static void jw__test_cooperative_exit_on_term(void) {
    pid_t child = jw__test_fork_group_leader(jw__test_child_default_term);

    jw_svc_stop_result r = jw_svc_stop_group(child, 3000, jw__test_real_absent);
    assert(r.verified_absent);
    assert(!r.escalated_to_kill); /* TERM alone must have sufficed */

    puts("PASS stop-test cooperative exit: SIGTERM alone verified absent, no escalation");
}

static void jw__test_coordinator_first_exit(void) {
    pid_t child = jw__test_fork_group_leader(jw__test_child_default_term);

    jw_svc_stop_result r = jw_svc_stop_group_coordinated(
        child, child, 3000, jw__test_real_absent);
    assert(r.verified_absent);
    assert(r.coordinator_first);
    assert(!r.group_term_sent);
    assert(!r.escalated_to_kill);
    assert(r.coordinator_wait_ms < JW_SVC_COORDINATOR_STOP_LEAD_MS);
    assert(r.group_wait_ms == 0);
    assert(r.kill_wait_ms == 0);
    assert(r.total_wait_ms < JW_SVC_COORDINATOR_STOP_LEAD_MS);

    puts("PASS stop-test coordinator-first exit: leader stopped before group fallback");
}

static void jw__test_coordinator_falls_back_to_group_kill(void) {
    pid_t child = jw__test_fork_group_leader(jw__test_child_ignores_term);

    jw_svc_stop_result r = jw_svc_stop_group_coordinated(
        child, child, 100, jw__test_real_absent);
    assert(r.verified_absent);
    assert(r.coordinator_first);
    assert(r.group_term_sent);
    assert(r.escalated_to_kill);
    assert(r.group_wait_ms <= 100);
    assert(r.kill_wait_ms < JW_SVC_STOP_KILL_WAIT_MS);
    assert(r.total_wait_ms < 100 + JW_SVC_STOP_KILL_WAIT_MS);

    puts("PASS stop-test coordinator fallback: unchanged group kill verifies absence");
}

static void jw__test_invalid_coordinator_uses_group_stop(void) {
    pid_t child = jw__test_fork_group_leader(jw__test_child_default_term);

    jw_svc_stop_result r = jw_svc_stop_group_coordinated(
        child, child + 1, 3000, jw__test_real_absent);
    assert(r.verified_absent);
    assert(!r.coordinator_first);
    assert(r.group_term_sent);
    assert(r.group_wait_ms < 3000);

    puts("PASS stop-test invalid coordinator: ordinary group stop retained");
}

static void jw__test_ignore_term_needs_kill(void) {
    pid_t child = jw__test_fork_group_leader(jw__test_child_ignores_term);

    /* Short grace window: the child truly ignores SIGTERM, so this must
     * escalate every time, not merely "might". */
    jw_svc_stop_result r = jw_svc_stop_group(child, 100, jw__test_real_absent);
    assert(r.escalated_to_kill);
    assert(r.verified_absent); /* SIGKILL cannot be ignored or blocked */

    puts("PASS stop-test ignore-TERM: escalates to SIGKILL, still verifies absent");
}

static void jw__test_unverifiable_stop_via_fault_injection(void) {
    pid_t child = jw__test_fork_group_leader(jw__test_child_default_term);

    /* The real child actually dies for real (stop.c's own kill() calls
     * are unmocked), but the injected absence_check lies that it never
     * went away -- this is what makes the "cannot be verified" path
     * deterministic to test without a genuine kernel hang. */
    jw_svc_stop_result r = jw_svc_stop_group(child, 50, jw__test_always_present);
    assert(r.escalated_to_kill);
    assert(!r.verified_absent);

    /* Clean up the real child ourselves: jw__test_always_present never
     * reaped it. */
    int status = 0;
    assert(waitpid(child, &status, 0) == child);

    puts("PASS stop-test fault injection: unverifiable stop reported honestly, "
         "never claimed absent");
}

int main(void) {
    jw__test_rejects_invalid_args();
    jw__test_cooperative_exit_on_term();
    jw__test_coordinator_first_exit();
    jw__test_coordinator_falls_back_to_group_kill();
    jw__test_invalid_coordinator_uses_group_stop();
    jw__test_ignore_term_needs_kill();
    jw__test_unverifiable_stop_via_fault_injection();
    puts("PASS stop-test");
    return 0;
}
