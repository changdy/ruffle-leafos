/* kill() and usleep() need broader-than-bare-C11 visibility on glibc; see
 * the matching comment in internal/services/lease.c. Must precede every
 * #include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/stop.h"

#include <signal.h>
#include <time.h>
#include <unistd.h>

#define JW_SVC_STOP_POLL_INTERVAL_US 10000 /* 10ms: fine enough not to overshoot
                                             * a short test bound by much, coarse
                                             * enough not to spin needlessly. */

static long jw__stop_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000000L + ts.tv_nsec / 1000L;
}

/* Polls absence_check(pgid) until it reports true or timeout_ms elapses.
 * Always performs at least one check, even for timeout_ms <= 0 -- a zero
 * or negative budget still deserves one immediate look, matching "an
 * already-absent group costs nothing" rather than being skipped outright.
 *
 * The deadline is computed once from a monotonic clock and re-checked
 * against a fresh "now" each iteration, rather than accumulating the
 * requested sleep durations. absence_check() itself takes real time
 * (the production implementation scans /proc), and usleep() can oversleep;
 * accumulating only the requested duration would silently let the actual
 * wall-clock budget run over stop_grace_ms + JW_SVC_STOP_KILL_WAIT_MS,
 * which contracts.md states as a hard worst-case bound, not a suggestion. */
static bool jw__stop_poll_absent(pid_t pgid, int timeout_ms,
                                 jw_svc_absence_check_fn absence_check) {
    if (timeout_ms < 0) {
        timeout_ms = 0;
    }
    long deadline_us = jw__stop_now_us() + (long)timeout_ms * 1000L;
    for (;;) {
        if (absence_check(pgid)) {
            return true;
        }
        long now_us = jw__stop_now_us();
        if (now_us >= deadline_us) {
            return false;
        }
        long remaining_us = deadline_us - now_us;
        long sleep_us = remaining_us < JW_SVC_STOP_POLL_INTERVAL_US
                            ? remaining_us
                            : JW_SVC_STOP_POLL_INTERVAL_US;
        usleep((unsigned int)sleep_us);
    }
}

static int jw__stop_elapsed_ms(long started_us) {
    long elapsed_us = jw__stop_now_us() - started_us;
    return elapsed_us <= 0 ? 0 : (int)(elapsed_us / 1000L);
}

jw_svc_stop_result jw_svc_stop_group_coordinated(
    pid_t pgid, pid_t coordinator_pid, int stop_grace_ms,
    jw_svc_absence_check_fn absence_check) {
    jw_svc_stop_result result = {0};
    /* pgid == 1 is rejected alongside <= 0: POSIX defines kill(-1, sig) as
     * "every process the caller is permitted to signal", not "process
     * group 1". Treating 1 as an ordinary pgid would turn both signals
     * below into a system-wide broadcast instead of a targeted stop. */
    if (pgid <= 1 || !absence_check) {
        return result;
    }

    if (stop_grace_ms < 0) {
        stop_grace_ms = 0;
    }
    long started_us = jw__stop_now_us();
    int remaining_grace_ms = stop_grace_ms;

    if (coordinator_pid == pgid) {
        int lead_ms = remaining_grace_ms < JW_SVC_COORDINATOR_STOP_LEAD_MS
                          ? remaining_grace_ms
                          : JW_SVC_COORDINATOR_STOP_LEAD_MS;
        result.coordinator_first = true;
        kill(coordinator_pid, SIGTERM);
        long coordinator_started_us = jw__stop_now_us();
        if (jw__stop_poll_absent(pgid, lead_ms, absence_check)) {
            result.coordinator_wait_ms =
                jw__stop_elapsed_ms(coordinator_started_us);
            result.total_wait_ms = jw__stop_elapsed_ms(started_us);
            result.verified_absent = true;
            return result;
        }
        result.coordinator_wait_ms = jw__stop_elapsed_ms(coordinator_started_us);
        remaining_grace_ms -= lead_ms;
        if (remaining_grace_ms < 0) remaining_grace_ms = 0;
    }

    /* kill()'s return value is deliberately not checked: ESRCH (group
     * already gone) is not a failure here -- the poll below simply
     * confirms absence immediately. Any other errno still leaves
     * absence_check as the sole source of truth. */
    result.group_term_sent = true;
    kill(-pgid, SIGTERM);
    long group_started_us = jw__stop_now_us();
    if (jw__stop_poll_absent(pgid, remaining_grace_ms, absence_check)) {
        result.group_wait_ms = jw__stop_elapsed_ms(group_started_us);
        result.total_wait_ms = jw__stop_elapsed_ms(started_us);
        result.verified_absent = true;
        return result;
    }
    result.group_wait_ms = jw__stop_elapsed_ms(group_started_us);

    result.escalated_to_kill = true;
    kill(-pgid, SIGKILL);
    long kill_started_us = jw__stop_now_us();
    result.verified_absent =
        jw__stop_poll_absent(pgid, JW_SVC_STOP_KILL_WAIT_MS, absence_check);
    result.kill_wait_ms = jw__stop_elapsed_ms(kill_started_us);
    result.total_wait_ms = jw__stop_elapsed_ms(started_us);
    return result;
}

jw_svc_stop_result jw_svc_stop_group(pid_t pgid, int stop_grace_ms,
                                     jw_svc_absence_check_fn absence_check) {
    return jw_svc_stop_group_coordinated(
        pgid, 0, stop_grace_ms, absence_check);
}
