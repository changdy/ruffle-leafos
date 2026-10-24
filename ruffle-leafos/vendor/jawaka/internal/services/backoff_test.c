#include "internal/services/backoff.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

static const long long jw__expected_delays_ms[JW_SVC_BACKOFF_TRACKED] = {
    1000, 2000, 4000, 8000, 16000,
};

static void jw__test_escalating_backoff_then_breaker(void) {
    jw_svc_backoff_state state;
    jw_svc_backoff_init(&state);

    long long now_ms = 0;

    for (int i = 0; i < JW_SVC_BACKOFF_TRACKED - 1; i++) {
        jw_svc_backoff_decision d = jw_svc_backoff_record_failure(&state, now_ms);
        assert(!d.breaker_open);
        assert(d.delay_ms == jw__expected_delays_ms[i]);
        now_ms += 1000; /* well within the 5-minute window */
    }

    /* The 5th failure, still within five minutes of the 1st, opens the
     * breaker instead of returning a 5th delay. */
    jw_svc_backoff_decision d = jw_svc_backoff_record_failure(&state, now_ms);
    assert(d.breaker_open);
    assert(state.breaker_open);

    /* Calling again after the breaker is open just repeats the verdict,
     * without moving anything. */
    jw_svc_backoff_decision d2 = jw_svc_backoff_record_failure(&state, now_ms + 999999);
    assert(d2.breaker_open);

    puts("PASS backoff-test escalating delays then breaker on the 5th clustered failure");
}

static void jw__test_widely_spaced_failures_never_trip_breaker(void) {
    jw_svc_backoff_state state;
    jw_svc_backoff_init(&state);

    /* Ten failures, each ten minutes apart: every one is a "5th most
     * recent" failure eventually, but the oldest-to-newest span of any
     * window of 5 is always well over five minutes, so the breaker must
     * never open. Each delay saturates at the 16s ceiling once 5 or more
     * have accumulated. */
    long long now_ms = 0;
    for (int i = 0; i < 10; i++) {
        jw_svc_backoff_decision d = jw_svc_backoff_record_failure(&state, now_ms);
        assert(!d.breaker_open);
        if (i < JW_SVC_BACKOFF_TRACKED) {
            assert(d.delay_ms == jw__expected_delays_ms[i]);
        } else {
            assert(d.delay_ms == jw__expected_delays_ms[JW_SVC_BACKOFF_TRACKED - 1]);
        }
        now_ms += 10LL * 60LL * 1000LL;
    }
    assert(!state.breaker_open);

    puts("PASS backoff-test widely spaced failures never open the breaker");
}

static void jw__test_breaker_boundary_is_inclusive(void) {
    /* Exactly five minutes apart between the 1st and 5th of a cluster
     * still counts as "within five minutes" (inclusive boundary). */
    jw_svc_backoff_state state;
    jw_svc_backoff_init(&state);

    long long now_ms = 0;
    for (int i = 0; i < JW_SVC_BACKOFF_TRACKED - 1; i++) {
        jw_svc_backoff_decision d = jw_svc_backoff_record_failure(&state, now_ms);
        assert(!d.breaker_open);
        now_ms += JW_SVC_BACKOFF_WINDOW_MS / (JW_SVC_BACKOFF_TRACKED - 1);
    }
    assert(now_ms == JW_SVC_BACKOFF_WINDOW_MS);

    jw_svc_backoff_decision d = jw_svc_backoff_record_failure(&state, now_ms);
    assert(d.breaker_open);

    puts("PASS backoff-test exactly-five-minute span is inclusive of the breaker window");
}

static void jw__test_just_over_the_window_does_not_trip(void) {
    jw_svc_backoff_state state;
    jw_svc_backoff_init(&state);

    long long now_ms = 0;
    for (int i = 0; i < JW_SVC_BACKOFF_TRACKED - 1; i++) {
        jw_svc_backoff_decision d = jw_svc_backoff_record_failure(&state, now_ms);
        assert(!d.breaker_open);
        now_ms += JW_SVC_BACKOFF_WINDOW_MS / (JW_SVC_BACKOFF_TRACKED - 1);
    }
    now_ms += 1; /* one millisecond past the exact boundary */

    jw_svc_backoff_decision d = jw_svc_backoff_record_failure(&state, now_ms);
    assert(!d.breaker_open);
    assert(d.delay_ms == 16000);
    assert(!state.breaker_open);

    puts("PASS backoff-test one millisecond past the window does not trip the breaker");
}

static void jw__test_reset_clears_breaker_and_history(void) {
    jw_svc_backoff_state state;
    jw_svc_backoff_init(&state);

    long long now_ms = 0;
    for (int i = 0; i < JW_SVC_BACKOFF_TRACKED; i++) {
        jw_svc_backoff_record_failure(&state, now_ms);
        now_ms += 1000;
    }
    assert(state.breaker_open);

    jw_svc_backoff_reset(&state);
    assert(!state.breaker_open);
    assert(state.count == 0);

    /* After reset, the very next failure is treated as a fresh 1st
     * failure, not a continuation of the tripped streak. */
    jw_svc_backoff_decision d = jw_svc_backoff_record_failure(&state, now_ms);
    assert(!d.breaker_open);
    assert(d.delay_ms == 1000);

    puts("PASS backoff-test reset clears the breaker and the failure history");
}

static void jw__test_init_is_a_clean_slate(void) {
    jw_svc_backoff_state state;
    memset(&state, 0xa5, sizeof(state));
    jw_svc_backoff_init(&state);
    assert(!state.breaker_open);
    assert(state.count == 0);

    puts("PASS backoff-test init produces a clean slate regardless of prior contents");
}

static void jw__test_null_state_is_handled(void) {
    jw_svc_backoff_init(NULL);
    jw_svc_backoff_reset(NULL);

    jw_svc_backoff_decision d = jw_svc_backoff_record_failure(NULL, 12345);
    assert(d.breaker_open);

    puts("PASS backoff-test NULL state is handled without crashing");
}

static void jw__test_out_of_order_timestamp_is_tolerated(void) {
    /* Regressing timestamps are each clamped to the prior recorded
     * instant. The first five effective timestamps below are therefore
     * 0, 300001, 300001, 300001, 300001 (WINDOW_MS+1, repeated): not a
     * breaker window. */
    jw_svc_backoff_state state;
    jw_svc_backoff_init(&state);

    jw_svc_backoff_record_failure(&state, 0);
    jw_svc_backoff_record_failure(&state, JW_SVC_BACKOFF_WINDOW_MS + 1);
    jw_svc_backoff_record_failure(&state, LLONG_MIN);
    jw_svc_backoff_record_failure(&state, -1);
    jw_svc_backoff_decision d = jw_svc_backoff_record_failure(&state, 0);
    assert(!d.breaker_open);
    assert(d.delay_ms == 16000);

    /* Shifting out the initial zero leaves five coincident effective
     * timestamps, so the next regressing input opens the breaker. */
    d = jw_svc_backoff_record_failure(&state, LLONG_MIN);
    assert(d.breaker_open);

    puts("PASS backoff-test non-increasing timestamps are tolerated, not rejected");
}

static void jw__test_extreme_timestamp_range_does_not_overflow(void) {
    jw_svc_backoff_state state;
    jw_svc_backoff_init(&state);

    jw_svc_backoff_record_failure(&state, LLONG_MIN);
    jw_svc_backoff_record_failure(&state, LLONG_MAX);
    jw_svc_backoff_record_failure(&state, LLONG_MAX);
    jw_svc_backoff_record_failure(&state, LLONG_MAX);
    jw_svc_backoff_decision d = jw_svc_backoff_record_failure(&state, LLONG_MAX);
    assert(!d.breaker_open);
    assert(d.delay_ms == 16000);

    /* Once LLONG_MIN is evicted, the five retained LLONG_MAX failures
     * are coincident and must open the breaker. */
    d = jw_svc_backoff_record_failure(&state, LLONG_MAX);
    assert(d.breaker_open);

    puts("PASS backoff-test extreme timestamp range is handled without overflow");
}

static void jw__test_ring_shift_finds_new_cluster(void) {
    jw_svc_backoff_state state;
    jw_svc_backoff_init(&state);

    jw_svc_backoff_record_failure(&state, 0);
    for (int i = 0; i < JW_SVC_BACKOFF_TRACKED - 1; i++) {
        jw_svc_backoff_decision d =
            jw_svc_backoff_record_failure(&state, 10LL * 60LL * 1000LL + i);
        assert(!d.breaker_open);
    }

    /* The distant first timestamp is now evicted. The five newest
     * failures form a cluster, so this 6th failure opens the breaker. */
    jw_svc_backoff_decision d =
        jw_svc_backoff_record_failure(&state, 10LL * 60LL * 1000LL + 4);
    assert(d.breaker_open);

    puts("PASS backoff-test ring shift detects a cluster after evicting the oldest failure");
}

int main(void) {
    jw__test_escalating_backoff_then_breaker();
    jw__test_widely_spaced_failures_never_trip_breaker();
    jw__test_breaker_boundary_is_inclusive();
    jw__test_just_over_the_window_does_not_trip();
    jw__test_reset_clears_breaker_and_history();
    jw__test_init_is_a_clean_slate();
    jw__test_null_state_is_handled();
    jw__test_out_of_order_timestamp_is_tolerated();
    jw__test_extreme_timestamp_range_does_not_overflow();
    jw__test_ring_shift_finds_new_cluster();
    puts("PASS backoff-test");
    return 0;
}
