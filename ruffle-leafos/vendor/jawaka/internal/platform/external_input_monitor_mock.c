/* Desktop/mock backend: no external gamepads to monitor. */

#include "internal/platform/external_input_monitor.h"

int jw_external_input_monitor_init(jw_external_input_monitor *monitor,
                                   jw_input_menu_tap_cb menu_tap,
                                   void *userdata) {
    (void)menu_tap;
    (void)userdata;
    if (!monitor) {
        return -1;
    }
    for (int i = 0; i < JW_EXT_INPUT_MAX_PADS; i++) {
        monitor->fds[i] = -1;
        monitor->paths[i][0] = '\0';
    }
    monitor->wake_events[0] = '\0';
    monitor->next_rescan_ms = 0;
    return 0;
}

void jw_external_input_monitor_tick(jw_external_input_monitor *monitor,
                                    jw_input_proxy *proxy, uint64_t now_ms) {
    (void)monitor;
    (void)proxy;
    (void)now_ms;
}

void jw_external_input_monitor_shutdown(jw_external_input_monitor *monitor) {
    (void)monitor;
}
