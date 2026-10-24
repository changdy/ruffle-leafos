#ifndef JW_PLATFORM_BLUETOOTH_H
#define JW_PLATFORM_BLUETOOTH_H

#include <stdbool.h>
#include <stddef.h>

#define JW_BT_MAC_LEN      18
#define JW_BT_NAME_LEN     96
#define JW_BT_ICON_LEN     48
#define JW_BT_MAX_DEVICES  32

typedef enum {
    JW_BT_DEVICE_UNKNOWN = 0,
    JW_BT_DEVICE_HEADSET,
    JW_BT_DEVICE_JOYPAD,
    JW_BT_DEVICE_KEYBOARD,
    JW_BT_DEVICE_MOUSE,
    JW_BT_DEVICE_OTHER,
} jw_bt_device_kind;

typedef enum {
    JW_BT_OP_NONE = 0,
    JW_BT_OP_SCAN,
    JW_BT_OP_PAIR_CONNECT,
    JW_BT_OP_CONNECT,
} jw_bt_operation_kind;

typedef enum {
    JW_BT_OP_IDLE = 0,
    JW_BT_OP_RUNNING,
    JW_BT_OP_OK,
    JW_BT_OP_FAILED,
    JW_BT_OP_TIMEOUT,
} jw_bt_operation_status;

typedef struct {
    bool available;
    bool powered;
    bool pairable;
    bool discovering;
    bool any_connected;
    char adapter_mac[JW_BT_MAC_LEN];
    char local_name[JW_BT_NAME_LEN];
} jw_bt_status_t;

typedef struct {
    char mac[JW_BT_MAC_LEN];
    char name[JW_BT_NAME_LEN];
    char alias[JW_BT_NAME_LEN];
    char icon[JW_BT_ICON_LEN];
    jw_bt_device_kind kind;
    bool paired;
    bool bonded;
    bool trusted;
    bool connected;
    bool blocked;
    int rssi;
    int battery_percent;
    unsigned class_hex;
    unsigned appearance_hex;
    bool has_audio_sink;
    bool has_a2dp;
    bool has_avrcp;
    bool has_hid;
} jw_bt_device_t;

/* Platform capability guard. When false, other jw_bt_* calls are safe but
 * report unavailable/no-op behavior. */
bool jw_bt_available(void);

bool jw_bt_mac_valid(const char *mac);
void jw_bt_mac_canonical(const char *mac, char out[JW_BT_MAC_LEN]);

int  jw_bt_status(jw_bt_status_t *out);
bool jw_bt_radio_is_on(void);
/* Adapter power and "anything connected" straight from the kernel, for status
 * icons polled every few seconds. Returns 0 on success, -1 when the kernel
 * cannot say (no adapter, no HCI socket); callers then fall back to
 * jw_bt_radio_is_on() and jw_bt_any_connected(). */
int  jw_bt_kernel_state(bool *powered, bool *connected);
int  jw_bt_set_radio(bool on);
int  jw_bt_any_connected(void);
int  jw_bt_audio_connected(void);

int jw_bt_list_paired(jw_bt_device_t *out, int max);
int jw_bt_list_nearby(jw_bt_device_t *out, int max);
int jw_bt_list_paired_summary(jw_bt_device_t *out, int max);
int jw_bt_list_nearby_summary(jw_bt_device_t *out, int max);
int jw_bt_list_summaries(jw_bt_device_t *paired, int paired_max, int *paired_count,
                         jw_bt_device_t *nearby, int nearby_max, int *nearby_count);
int jw_bt_refresh_device(const char *mac, jw_bt_device_t *out);

int jw_bt_scan_start(void);
jw_bt_operation_status jw_bt_scan_poll(char *message, size_t message_len);
int jw_bt_connect_start(const char *mac, bool pair_if_needed);
jw_bt_operation_status jw_bt_connect_poll(char *message, size_t message_len);
void jw_bt_cancel_operation(void);

int jw_bt_disconnect(const char *mac);
int jw_bt_forget(const char *mac);
int jw_bt_sync_stock_saved_list(void);

const char *jw_bt_device_kind_name(jw_bt_device_kind kind);
const char *jw_bt_device_kind_stock_name(jw_bt_device_kind kind);

#endif /* JW_PLATFORM_BLUETOOTH_H */
