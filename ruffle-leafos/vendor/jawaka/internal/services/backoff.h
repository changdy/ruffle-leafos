#ifndef JW_SERVICES_BACKOFF_H
#define JW_SERVICES_BACKOFF_H

#include <stdbool.h>

/* SVC-1's restart policy (contracts.md#svc-1--service-manifest, "Restart
 * policy"): "`on-failure` uses bounded backoff of 1, 2, 4, 8, 16 seconds.
 * Five failures within five minutes opens a circuit breaker and leaves
 * the service stopped with an actionable reason. A user-initiated Run or
 * Restart clears the breaker. `on-failure` never applies after an
 * intentional stop, a lifecycle-policy stop, a package operation, or
 * shutdown."
 *
 * This module is pure decision logic: it holds no clock, spawns nothing,
 * and touches no filesystem. The caller is responsible for actually
 * restarting the service after the returned delay, for arranging its own
 * timer/callback to do so, and for never calling
 * jw_svc_backoff_record_failure() at all for the excluded stop reasons
 * above -- this module cannot distinguish "the service crashed" from
 * "the caller stopped it on purpose" and does not try to.
 *
 * "Five failures within five minutes" is interpreted as a sliding window
 * over the CURRENT run of up to 5 most recent consecutive failures: the
 * breaker opens exactly when a 5th recorded failure exists and the
 * oldest of the 5 most recent failures is no more than five minutes
 * before the newest. This means widely-spaced failures (each more than
 * five minutes apart) never trip the breaker on count alone -- they are
 * evidence the service does eventually run, not that it is stuck in a
 * crash loop -- but every individual failure still gets the escalating
 * backoff delay, capped at the sequence's last entry (16s) once 5 or
 * more consecutive failures have been recorded.
 *
 * This state is not thread-safe. A caller must serialize all init,
 * record, and reset operations on each service's state (or provide its
 * own locking around them).
 */

#define JW_SVC_BACKOFF_TRACKED 5
#define JW_SVC_BACKOFF_WINDOW_MS (5LL * 60LL * 1000LL)

typedef struct {
    /* Timestamps (caller's clock, e.g. CLOCK_MONOTONIC milliseconds; any
     * monotonically nondecreasing unit works as long as it is consistent
     * across calls) of the most recent up to JW_SVC_BACKOFF_TRACKED
     * failures, oldest first. Only the first `count` entries are valid. */
    long long failure_times_ms[JW_SVC_BACKOFF_TRACKED];
    int count;
    bool breaker_open;
} jw_svc_backoff_state;

typedef struct {
    /* True once the circuit breaker has opened: the caller must leave
     * the service stopped with an actionable reason and must not
     * schedule another restart attempt until a user-initiated Run or
     * Restart calls jw_svc_backoff_reset(). delay_ms is meaningless when
     * this is true. */
    bool breaker_open;
    /* Milliseconds to wait before the next restart attempt. Only
     * meaningful when breaker_open is false. */
    long long delay_ms;
} jw_svc_backoff_decision;

/* Zeroes `state`: no recorded failures, breaker closed. Equivalent to a
 * zero-initialized jw_svc_backoff_state, provided for callers who prefer
 * not to rely on that. A NULL state is ignored. */
void jw_svc_backoff_init(jw_svc_backoff_state *state);

/* Records a failure occurring at `now_ms` and returns what the caller
 * should do next. `state` must previously have been initialized or
 * zero-initialized and must not be modified directly. `now_ms` should
 * normally be nondecreasing across calls on the same state (the caller's
 * own clock is assumed monotonic), but all long long values are valid:
 * passing a value before the most recently recorded failure is clamped
 * to that prior instant for windowing purposes, not rejected.
 *
 * Calling this again after the breaker has already opened (without an
 * intervening jw_svc_backoff_reset()) simply returns {breaker_open:
 * true} again -- it does not re-arm or extend anything.
 *
 * A NULL state defensively returns {breaker_open: true, delay_ms: 0}. */
jw_svc_backoff_decision jw_svc_backoff_record_failure(jw_svc_backoff_state *state, long long now_ms);

/* Clears all recorded failures and closes the breaker. Callers use this
 * on a user-initiated Run or Restart (SVC-1: "A user-initiated Run or
 * Restart clears the breaker"). This module does not decide when a
 * long-enough successful run should also clear the streak on its own --
 * that policy choice belongs to the caller, which is free to call this
 * after whatever grace period it chooses.
 *
 * A NULL state is ignored. */
void jw_svc_backoff_reset(jw_svc_backoff_state *state);

#endif
