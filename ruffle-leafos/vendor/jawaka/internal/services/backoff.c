#include "internal/services/backoff.h"

#include <limits.h>
#include <string.h>

static const long long jw__backoff_delays_ms[JW_SVC_BACKOFF_TRACKED] = {
    1000, 2000, 4000, 8000, 16000,
};

static bool jw__timestamps_within_window(long long oldest_ms, long long newest_ms) {
    if (newest_ms <= oldest_ms) {
        return true;
    }

    /* Comparing against oldest + window avoids overflowing the much
     * wider possible subtraction newest - oldest. If the addition itself
     * would overflow, every representable newer timestamp is necessarily
     * within the window. */
    if (oldest_ms > LLONG_MAX - JW_SVC_BACKOFF_WINDOW_MS) {
        return true;
    }
    return newest_ms <= oldest_ms + JW_SVC_BACKOFF_WINDOW_MS;
}

void jw_svc_backoff_init(jw_svc_backoff_state *state) {
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

void jw_svc_backoff_reset(jw_svc_backoff_state *state) {
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
}

jw_svc_backoff_decision jw_svc_backoff_record_failure(jw_svc_backoff_state *state, long long now_ms) {
    jw_svc_backoff_decision decision;
    memset(&decision, 0, sizeof(decision));

    if (!state) {
        decision.breaker_open = true;
        return decision;
    }

    if (state->breaker_open) {
        decision.breaker_open = true;
        return decision;
    }

    if (state->count > 0) {
        long long previous_ms = state->failure_times_ms[state->count - 1];
        if (now_ms < previous_ms) {
            now_ms = previous_ms;
        }
    }

    if (state->count < JW_SVC_BACKOFF_TRACKED) {
        state->failure_times_ms[state->count] = now_ms;
        state->count++;
    } else {
        for (int i = 1; i < JW_SVC_BACKOFF_TRACKED; i++) {
            state->failure_times_ms[i - 1] = state->failure_times_ms[i];
        }
        state->failure_times_ms[JW_SVC_BACKOFF_TRACKED - 1] = now_ms;
    }

    if (state->count == JW_SVC_BACKOFF_TRACKED) {
        long long oldest_ms = state->failure_times_ms[0];
        long long newest_ms = state->failure_times_ms[JW_SVC_BACKOFF_TRACKED - 1];
        if (jw__timestamps_within_window(oldest_ms, newest_ms)) {
            state->breaker_open = true;
            decision.breaker_open = true;
            return decision;
        }
    }

    decision.breaker_open = false;
    decision.delay_ms = jw__backoff_delays_ms[state->count - 1];
    return decision;
}
