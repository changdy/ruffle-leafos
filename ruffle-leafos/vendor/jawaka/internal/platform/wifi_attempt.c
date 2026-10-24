#include "internal/platform/wifi.h"

/* One auth failure is not a verdict. A network can span several access points
 * (an iPhone hotspot has shown up as two BSSIDs), and when one of them rejects
 * the association wpa_supplicant moves on to the next by itself: on MLP1 an
 * ASSOC-REJECT from one iPhone BSSID was followed 1.2 s later by a completed
 * connection through the other. Failing on the first one abandoned that join,
 * and the recovery that follows re-enabled every saved network while none was
 * current, so wpa_supplicant went straight back to the previous network. A
 * second failure means its own retry failed as well; otherwise the timeout
 * decides. */
jw_wifi_attempt_result jw_wifi_attempt_resolve(bool connected, jw_wifi_evt evt,
                                               int *auth_fails, unsigned elapsed_ms) {
    if (connected) {
        return JW_WIFI_ATTEMPT_CONNECTED;
    }
    if (evt == JW_WIFI_EVT_WRONG_KEY) {
        return JW_WIFI_ATTEMPT_WRONG_KEY;
    }
    if (evt == JW_WIFI_EVT_AUTH_FAIL && ++*auth_fails >= JW_WIFI_ATTEMPT_AUTH_FAILS) {
        return JW_WIFI_ATTEMPT_FAILED;
    }
    return elapsed_ms > JW_WIFI_ATTEMPT_TIMEOUT_MS ? JW_WIFI_ATTEMPT_FAILED
                                                  : JW_WIFI_ATTEMPT_PENDING;
}
