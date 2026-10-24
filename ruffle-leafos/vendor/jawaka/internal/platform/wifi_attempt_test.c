#include "internal/platform/wifi.h"

#include <assert.h>
#include <stdio.h>

static jw_wifi_attempt_result step(bool connected, jw_wifi_evt evt, int *fails,
                                   unsigned elapsed_ms) {
    return jw_wifi_attempt_resolve(connected, evt, fails, elapsed_ms);
}

int main(void) {
    int fails = 0;

    /* iPhone hotspot on MLP1: one BSSID rejects, the other accepts 1.2 s later. */
    assert(step(false, JW_WIFI_EVT_AUTH_FAIL, &fails, 100) == JW_WIFI_ATTEMPT_PENDING);
    assert(step(false, JW_WIFI_EVT_NONE, &fails, 800) == JW_WIFI_ATTEMPT_PENDING);
    assert(step(true, JW_WIFI_EVT_NONE, &fails, 1300) == JW_WIFI_ATTEMPT_CONNECTED);

    /* wpa_supplicant's retry failing too ends the attempt before the timeout. */
    fails = 0;
    assert(step(false, JW_WIFI_EVT_AUTH_FAIL, &fails, 500) == JW_WIFI_ATTEMPT_PENDING);
    assert(step(false, JW_WIFI_EVT_AUTH_FAIL, &fails, 3000) == JW_WIFI_ATTEMPT_FAILED);

    /* A definitive wrong key ends it at once, even as the first event. */
    fails = 0;
    assert(step(false, JW_WIFI_EVT_WRONG_KEY, &fails, 200) == JW_WIFI_ATTEMPT_WRONG_KEY);

    /* Reaching the target wins over a failure drained in the same poll. */
    fails = 1;
    assert(step(true, JW_WIFI_EVT_AUTH_FAIL, &fails, 2000) == JW_WIFI_ATTEMPT_CONNECTED);

    /* With no verdict from events, the attempt runs until the timeout. */
    fails = 0;
    assert(step(false, JW_WIFI_EVT_NONE, &fails, JW_WIFI_ATTEMPT_TIMEOUT_MS) ==
           JW_WIFI_ATTEMPT_PENDING);
    assert(step(false, JW_WIFI_EVT_NONE, &fails, JW_WIFI_ATTEMPT_TIMEOUT_MS + 1) ==
           JW_WIFI_ATTEMPT_FAILED);
    fails = 0;
    assert(step(false, JW_WIFI_EVT_AUTH_FAIL, &fails, JW_WIFI_ATTEMPT_TIMEOUT_MS + 1) ==
           JW_WIFI_ATTEMPT_FAILED);

    puts("wifi attempt tests passed");
    return 0;
}
