#include "internal/platform/wifi.h"

#include <string.h>

bool jw_wifi_available(void) {
    return false;
}

int jw_wifi_status(jw_wifi_status_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return -1;
}

int jw_wifi_strength_now(void) {
    return 0;
}

int jw_wifi_scan_start(void) {
    return -1;
}

int jw_wifi_scan_results(const char *current_ssid, jw_wifi_network_t *out, int max) {
    (void)current_ssid;
    if (out && max > 0) {
        memset(out, 0, sizeof(*out) * (size_t)max);
    }
    return 0;
}

jw_wifi_connect_result jw_wifi_connect(const char *ssid, bool secured) {
    (void)ssid;
    (void)secured;
    return JW_WIFI_CONNECT_FAILED;
}

jw_wifi_connect_result jw_wifi_connect_psk(const char *ssid, const char *psk) {
    (void)ssid;
    (void)psk;
    return JW_WIFI_CONNECT_FAILED;
}

int jw_wifi_monitor_open(void) {
    return -1;
}

jw_wifi_evt jw_wifi_monitor_poll(int fd) {
    (void)fd;
    return JW_WIFI_EVT_NONE;
}

void jw_wifi_monitor_close(int fd) {
    (void)fd;
}

void jw_wifi_recover(void) {
}

bool jw_wifi_radio_is_on(void) {
    return false;
}

int jw_wifi_set_radio(bool on) {
    (void)on;
    return -1;
}

int jw_wifi_forget(const char *ssid) {
    (void)ssid;
    return -1;
}

int jw_wifi_disconnect(void) {
    return -1;
}

int jw_wifi_restore(void) {
    return 0;
}

int jw_wifi_harden(void) {
    return 0;
}
