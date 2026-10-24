#ifndef JW_PLATFORM_EXTERNAL_INPUT_H
#define JW_PLATFORM_EXTERNAL_INPUT_H

/* jawakad's external (paired wireless) controller monitor
   (plans/paired-wireless-controllers-mlp1.md, "Wake and hotkey monitoring").

   Opens non-grabbing, read-only descriptors on connected external gamepads and
   observes them alongside the calibrated virtual Loong stream:

   - Any button press or hat movement counts as user activity (auto-sleep idle
     reset goes through the input proxy so one place owns the idle clock).
   - Analog stick movement counts only after crossing a deadzone, so wireless
     stick noise cannot prevent sleep.
   - Guide/BTN_MODE invokes the same Menu action as the handheld Menu button.
   - Disconnects close the stale descriptor; a low-frequency rescan discovers
     new connections and reconnections without restarting the UI.
   - Republishes CAT_INPUT_WAKE_EVENTS (virtual Loong + externals, colon
     separated) so Catastrophe's wake path watches the same bounded set.

   The descriptors are never grabbed: Jawaka observes the kernel events while
   the UI or an emulator consumes the same device through its own fds. */

#include <stdbool.h>
#include <stdint.h>

#include "internal/platform/input_proxy.h"

#define JW_EXT_INPUT_MAX_PADS 3
#define JW_EXT_INPUT_SCAN_NODES 64   /* /dev/input/event0..63 */

/* A node the rescan already probed and found not to be a gamepad, by identity:
   the same device number, inode and ctime means the same device. */
typedef struct {
    unsigned long long rdev;
    unsigned long long ino;
    long long ctime_ns;
    bool valid;
} jw_ext_input_seen;

typedef struct {
    int fds[JW_EXT_INPUT_MAX_PADS]; /* -1 when free */
    char paths[JW_EXT_INPUT_MAX_PADS][JW_INPUT_PROXY_MAX_PATH];
    uint64_t next_rescan_ms;
    char wake_events[(JW_EXT_INPUT_MAX_PADS + 1) *
                     (JW_INPUT_PROXY_MAX_PATH + 1)];
    jw_input_menu_tap_cb menu_tap;
    void *menu_userdata;
    jw_ext_input_seen not_gamepad[JW_EXT_INPUT_SCAN_NODES];
} jw_external_input_monitor;

/* Initialize the monitor. menu_tap is invoked (without swallow semantics —
   jawakad does not own forwarding for external pads) when Guide/BTN_MODE is
   pressed. Returns 0; a non-zero failure just disables monitoring. */
int jw_external_input_monitor_init(jw_external_input_monitor *monitor,
                                   jw_input_menu_tap_cb menu_tap,
                                   void *userdata);

/* Drain events, close dead descriptors, and rescan for (re)connected pads at
   the internal cadence. proxy identifies the physical/virtual Loong paths so
   they are never opened here. Resets the proxy idle timer on real activity
   and republishes CAT_INPUT_WAKE_EVENTS when the device set changes. */
void jw_external_input_monitor_tick(jw_external_input_monitor *monitor,
                                    jw_input_proxy *proxy, uint64_t now_ms);

/* Close every descriptor and republish the wake env without externals. */
void jw_external_input_monitor_shutdown(jw_external_input_monitor *monitor);

#endif /* JW_PLATFORM_EXTERNAL_INPUT_H */
