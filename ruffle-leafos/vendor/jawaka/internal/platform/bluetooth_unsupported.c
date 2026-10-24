#include "internal/platform/bluetooth.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

bool jw_bt_available(void) {
    return false;
}

bool jw_bt_mac_valid(const char *mac) {
    if (!mac) {
        return false;
    }
    for (int i = 0; i < JW_BT_MAC_LEN - 1; i++) {
        if ((i + 1) % 3 == 0) {
            if (mac[i] != ':') {
                return false;
            }
            continue;
        }
        if (!isxdigit((unsigned char)mac[i])) {
            return false;
        }
    }
    return mac[JW_BT_MAC_LEN - 1] == '\0';
}

void jw_bt_mac_canonical(const char *mac, char out[JW_BT_MAC_LEN]) {
    if (!out) {
        return;
    }
    if (!jw_bt_mac_valid(mac)) {
        out[0] = '\0';
        return;
    }
    for (int i = 0; i < JW_BT_MAC_LEN; i++) {
        out[i] = (char)toupper((unsigned char)mac[i]);
    }
}

int jw_bt_status(jw_bt_status_t *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return -1;
}

bool jw_bt_radio_is_on(void) {
    return false;
}

int jw_bt_kernel_state(bool *powered, bool *connected) {
    (void)powered;
    (void)connected;
    return -1;
}

int jw_bt_set_radio(bool on) {
    (void)on;
    return -1;
}

int jw_bt_any_connected(void) {
    return 0;
}

int jw_bt_audio_connected(void) {
    return 0;
}

int jw_bt_list_paired(jw_bt_device_t *out, int max) {
    if (out && max > 0) {
        memset(out, 0, sizeof(*out) * (size_t)max);
    }
    return 0;
}

int jw_bt_list_nearby(jw_bt_device_t *out, int max) {
    if (out && max > 0) {
        memset(out, 0, sizeof(*out) * (size_t)max);
    }
    return 0;
}

int jw_bt_list_paired_summary(jw_bt_device_t *out, int max) {
    return jw_bt_list_paired(out, max);
}

int jw_bt_list_nearby_summary(jw_bt_device_t *out, int max) {
    return jw_bt_list_nearby(out, max);
}

int jw_bt_list_summaries(jw_bt_device_t *paired, int paired_max, int *paired_count,
                         jw_bt_device_t *nearby, int nearby_max, int *nearby_count) {
    if (paired_count) {
        *paired_count = jw_bt_list_paired(paired, paired_max);
    }
    if (nearby_count) {
        *nearby_count = jw_bt_list_nearby(nearby, nearby_max);
    }
    return 0;
}

int jw_bt_refresh_device(const char *mac, jw_bt_device_t *out) {
    (void)mac;
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    return -1;
}

int jw_bt_scan_start(void) {
    return -1;
}

jw_bt_operation_status jw_bt_scan_poll(char *message, size_t message_len) {
    if (message && message_len > 0) {
        snprintf(message, message_len, "Bluetooth unavailable");
    }
    return JW_BT_OP_FAILED;
}

int jw_bt_connect_start(const char *mac, bool pair_if_needed) {
    (void)mac;
    (void)pair_if_needed;
    return -1;
}

jw_bt_operation_status jw_bt_connect_poll(char *message, size_t message_len) {
    if (message && message_len > 0) {
        snprintf(message, message_len, "Bluetooth unavailable");
    }
    return JW_BT_OP_FAILED;
}

void jw_bt_cancel_operation(void) {
}

int jw_bt_disconnect(const char *mac) {
    (void)mac;
    return -1;
}

int jw_bt_forget(const char *mac) {
    (void)mac;
    return -1;
}

int jw_bt_sync_stock_saved_list(void) {
    return 0;
}

const char *jw_bt_device_kind_name(jw_bt_device_kind kind) {
    switch (kind) {
    case JW_BT_DEVICE_HEADSET: return "Headset";
    case JW_BT_DEVICE_JOYPAD: return "Controller";
    case JW_BT_DEVICE_KEYBOARD: return "Keyboard";
    case JW_BT_DEVICE_MOUSE: return "Mouse";
    case JW_BT_DEVICE_OTHER: return "Device";
    case JW_BT_DEVICE_UNKNOWN:
    default: return "Unknown";
    }
}

const char *jw_bt_device_kind_stock_name(jw_bt_device_kind kind) {
    switch (kind) {
    case JW_BT_DEVICE_HEADSET: return "speaker";
    case JW_BT_DEVICE_JOYPAD: return "joypad";
    case JW_BT_DEVICE_KEYBOARD: return "keyboard";
    case JW_BT_DEVICE_MOUSE: return "mouse";
    case JW_BT_DEVICE_OTHER: return "other";
    case JW_BT_DEVICE_UNKNOWN:
    default: return "unknown";
    }
}
