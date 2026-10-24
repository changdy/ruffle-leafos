#ifndef JW_SERVICES_STOP_H
#define JW_SERVICES_STOP_H

#include <stdbool.h>
#include <sys/types.h>

/* SVC-1's stop sequence (contracts.md#svc-1--service-manifest, "Stop
 * sequence" table):
 *
 *   SIGTERM to the process group  -> stop_grace_ms (default 5000, ceiling
 *                                    15000, already resolved by the
 *                                    caller -- see internal/services/
 *                                    manifest.c's stop_grace_ms handling;
 *                                    this function does not re-clamp it)
 *   SIGKILL to the remaining group -> 2000 ms (JW_SVC_STOP_KILL_WAIT_MS,
 *                                    a fixed contract constant, not a
 *                                    parameter)
 *   Verify absence: no non-zombie member of the reserved pgid -- required
 *                                    before "stopped" is reported
 *
 * Worst case is stop_grace_ms + 2000 ms, matching the contract exactly.
 */
#define JW_SVC_STOP_KILL_WAIT_MS 2000
#define JW_SVC_COORDINATOR_STOP_LEAD_MS 1000

/* Injected rather than calling jw_svc_group_absent() (internal/services/
 * ownership.h) directly, so the "stop cannot be verified" path -- which
 * phase-a2-supervisor.md explicitly says to test by FAULT INJECTION at
 * the absence-verification point, not by trying to construct a real
 * kernel hang -- is deterministically testable. Every production caller
 * passes jw_svc_group_absent itself; only tests substitute a fake. */
typedef bool (*jw_svc_absence_check_fn)(pid_t pgid);

typedef struct {
    /* True iff, by the end of the sequence, absence_check reported no
     * non-zombie member of pgid remaining. This is the ONLY thing a
     * caller may treat as "stopped" -- see SVC-1's "When a stop cannot
     * be verified" table for what each caller (shutdown, safe-unmount,
     * game launch, package operations) must do when this is false. */
    bool verified_absent;
    /* True iff SIGTERM alone did not produce verified absence within
     * stop_grace_ms and SIGKILL was sent. A caller does not need this to
     * decide anything SVC-1 requires, but it is useful for logging why a
     * stop took the full worst-case bound. */
    bool escalated_to_kill;
    /* A validated service-group leader received SIGTERM first, allowing its
     * controller to stop descendants cleanly before the group fallback. */
    bool coordinator_first;
    bool group_term_sent;
    int coordinator_wait_ms;
    int group_wait_ms;
    int kill_wait_ms;
    int total_wait_ms;
} jw_svc_stop_result;

/* Runs the sequence above against process group `pgid`. `stop_grace_ms`
 * must already be the resolved value (absent-defaults-to-5000,
 * clamped-to-15000 per SVC-1); a negative value here is treated as 0
 * (skip straight to polling, i.e. an immediate escalation window) rather
 * than passed to a sleep call. `absence_check` must be non-NULL.
 *
 * pgid <= 0, pgid == 1, or a NULL absence_check returns {false, false}
 * immediately without sending any signal. pgid == 1 is refused
 * specifically because POSIX defines kill(-1, sig) as "every process the
 * caller is permitted to signal", not "process group 1" -- accepting it
 * here would turn a targeted stop into a system-wide signal broadcast.
 *
 * Sending SIGTERM/SIGKILL to a pgid that no longer exists (ESRCH) is not
 * a failure of this function: the subsequent absence poll simply
 * confirms the group is already gone.
 */
jw_svc_stop_result jw_svc_stop_group(pid_t pgid, int stop_grace_ms,
                                     jw_svc_absence_check_fn absence_check);

/* Same verified-absence sequence, but lets an authenticated group leader
 * coordinate a clean child shutdown for up to one second. The lead is part of
 * stop_grace_ms; it never extends the existing worst-case bound. Passing a
 * coordinator other than the group leader falls back to jw_svc_stop_group(). */
jw_svc_stop_result jw_svc_stop_group_coordinated(
    pid_t pgid, pid_t coordinator_pid, int stop_grace_ms,
    jw_svc_absence_check_fn absence_check);

#endif
