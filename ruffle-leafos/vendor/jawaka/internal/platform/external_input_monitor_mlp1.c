#include "internal/platform/external_input_monitor.h"

#include "internal/core/log.h"
#include "internal/platform/input_roster.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>
#include <linux/input.h>

#define JW__EXT_SCAN_MAX JW_EXT_INPUT_SCAN_NODES
#define JW__EXT_RESCAN_MS 2000
/* Past this, a stick deflection counts as user activity; below it, wireless
   axis noise is ignored so it cannot hold the device awake. */
#define JW__EXT_AXIS_DEADZONE 8000

static bool jw__ext_same_rdev(const char *a, const char *b) {
    struct stat sa;
    struct stat sb;
    return a && b && a[0] && b[0] && stat(a, &sa) == 0 && stat(b, &sb) == 0 &&
           sa.st_rdev == sb.st_rdev;
}

static bool jw__ext_is_loong_path(const jw_input_proxy *proxy,
                                  const char *path) {
    return proxy &&
           (jw__ext_same_rdev(path, proxy->physical_event_path) ||
            jw__ext_same_rdev(path, proxy->virtual_event_path));
}

int jw_external_input_monitor_init(jw_external_input_monitor *monitor,
                                   jw_input_menu_tap_cb menu_tap,
                                   void *userdata) {
    if (!monitor) {
        return -1;
    }
    memset(monitor, 0, sizeof(*monitor));
    for (int i = 0; i < JW_EXT_INPUT_MAX_PADS; i++) {
        monitor->fds[i] = -1;
    }
    monitor->menu_tap = menu_tap;
    monitor->menu_userdata = userdata;
    return 0;
}

static void jw__ext_publish_wake_events(jw_external_input_monitor *monitor,
                                        const jw_input_proxy *proxy) {
    const char *virtual_path =
        proxy && proxy->enabled && proxy->virtual_event_path[0]
            ? proxy->virtual_event_path
            : "";

    char events[sizeof(monitor->wake_events)];
    size_t off = 0;
    events[0] = '\0';
    if (virtual_path[0]) {
        off += snprintf(events + off, sizeof(events) - off, "%s", virtual_path);
    }
    for (int i = 0; i < JW_EXT_INPUT_MAX_PADS; i++) {
        if (monitor->fds[i] < 0) {
            continue;
        }
        if (off + strlen(monitor->paths[i]) + 2 >= sizeof(events)) {
            break; /* bounded; extra pads are monitored but not wake env */
        }
        off += snprintf(events + off, sizeof(events) - off, "%s%s",
                        off > 0 ? ":" : "", monitor->paths[i]);
    }

    if (strcmp(events, monitor->wake_events) == 0) {
        return;
    }
    snprintf(monitor->wake_events, sizeof(monitor->wake_events), "%s", events);
    if (events[0]) {
        setenv("CAT_INPUT_WAKE_EVENTS", events, 1);
    } else {
        unsetenv("CAT_INPUT_WAKE_EVENTS");
    }
}

static void jw__ext_drop(jw_external_input_monitor *monitor, int slot,
                         const char *reason) {
    if (monitor->fds[slot] >= 0) {
        close(monitor->fds[slot]);
        jw_log_info("external input: closed %s (%s)", monitor->paths[slot],
                    reason);
    }
    monitor->fds[slot] = -1;
    monitor->paths[slot][0] = '\0';
}

static bool jw__ext_seen_matches(const jw_ext_input_seen *seen,
                                 const struct stat *st) {
    return seen->valid && seen->rdev == (unsigned long long)st->st_rdev &&
           seen->ino == (unsigned long long)st->st_ino &&
           seen->ctime_ns == (long long)st->st_ctim.tv_sec * 1000000000LL +
                                 st->st_ctim.tv_nsec;
}

static void jw__ext_seen_set(jw_ext_input_seen *seen, const struct stat *st) {
    seen->rdev = (unsigned long long)st->st_rdev;
    seen->ino = (unsigned long long)st->st_ino;
    seen->ctime_ns = (long long)st->st_ctim.tv_sec * 1000000000LL +
                     st->st_ctim.tv_nsec;
    seen->valid = true;
}

static void jw__ext_rescan(jw_external_input_monitor *monitor,
                           jw_input_proxy *proxy, uint64_t now_ms) {
    monitor->next_rescan_ms = now_ms + JW__EXT_RESCAN_MS;

    /* Drop descriptors whose nodes disappeared (disconnect). */
    for (int i = 0; i < JW_EXT_INPUT_MAX_PADS; i++) {
        if (monitor->fds[i] >= 0 && access(monitor->paths[i], F_OK) != 0) {
            jw__ext_drop(monitor, i, "disconnected");
        }
    }

    int pad_count = 0;
    for (int i = 0; i < JW_EXT_INPUT_MAX_PADS; i++) {
        if (monitor->fds[i] >= 0) {
            pad_count++;
        }
    }

    for (int index = 0; index < JW__EXT_SCAN_MAX &&
                       pad_count < JW_EXT_INPUT_MAX_PADS; index++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", index);
        struct stat st;
        if (stat(path, &st) != 0) {
            monitor->not_gamepad[index].valid = false;
            continue;
        }
        /* Probing a node means opening and closing it, and closing an evdev
           fd waits for an RCU grace period: 10-160 ms of the daemon loop in
           D state on this board, for every node, every rescan. A node already
           found not to be a gamepad is skipped until it changes. */
        if (jw__ext_seen_matches(&monitor->not_gamepad[index], &st) ||
            jw__ext_is_loong_path(proxy, path)) {
            continue;
        }
        bool already = false;
        for (int i = 0; i < JW_EXT_INPUT_MAX_PADS; i++) {
            if (monitor->fds[i] >= 0 &&
                jw__ext_same_rdev(monitor->paths[i], path)) {
                already = true;
                break;
            }
        }
        if (already) {
            continue;
        }
        if (!jw_input_device_is_gamepad(path)) {
            jw__ext_seen_set(&monitor->not_gamepad[index], &st);
            continue;
        }
        monitor->not_gamepad[index].valid = false;

        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        /* Never grab: Jawaka only observes. */

        for (int i = 0; i < JW_EXT_INPUT_MAX_PADS; i++) {
            if (monitor->fds[i] < 0) {
                monitor->fds[i] = fd;
                snprintf(monitor->paths[i], sizeof(monitor->paths[i]), "%s",
                         path);
                pad_count++;
                jw_log_info("external input: watching %s", path);
                break;
            }
        }
    }
}

static bool jw__ext_axis_code_is_stick(uint16_t code) {
    return code == ABS_X || code == ABS_Y || code == ABS_Z || code == ABS_RZ ||
           code == ABS_RX || code == ABS_RY;
}

static bool jw__ext_axis_code_is_hat(uint16_t code) {
    return code >= ABS_HAT0X && code <= ABS_HAT3Y;
}

static void jw__ext_drain_events(jw_external_input_monitor *monitor,
                                 jw_input_proxy *proxy) {
    for (int i = 0; i < JW_EXT_INPUT_MAX_PADS; i++) {
        if (monitor->fds[i] < 0) {
            continue;
        }
        struct input_event ev;
        for (;;) {
            ssize_t got = read(monitor->fds[i], &ev, sizeof(ev));
            if (got < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
                    break;
                }
                jw__ext_drop(monitor, i, strerror(errno));
                break;
            }
            if (got != (ssize_t)sizeof(ev)) {
                break;
            }
            if (ev.type == EV_KEY && ev.value == 1) {
                jw_input_proxy_mark_activity(proxy);
                if (ev.code == BTN_MODE && monitor->menu_tap) {
                    /* Same Menu action as the handheld Menu button, including
                       what happens when nothing in the daemon claims it. The
                       built-in pad is grabbed, so an unclaimed press is
                       forwarded to the virtual pad and the launcher UI acts on
                       it. An external pad is only watched, never grabbed, so
                       there is nothing to forward -- synthesise the same tap on
                       the virtual pad or Guide does nothing outside a game. */
                    if (!monitor->menu_tap(monitor->menu_userdata)) {
                        jw_input_proxy_emit_menu_tap(proxy);
                    }
                }
            } else if (ev.type == EV_ABS) {
                if (jw__ext_axis_code_is_hat(ev.code) && ev.value != 0) {
                    jw_input_proxy_mark_activity(proxy);
                } else if (jw__ext_axis_code_is_stick(ev.code) &&
                           (ev.value > JW__EXT_AXIS_DEADZONE ||
                            ev.value < -JW__EXT_AXIS_DEADZONE)) {
                    jw_input_proxy_mark_activity(proxy);
                }
            }
        }
    }
}

void jw_external_input_monitor_tick(jw_external_input_monitor *monitor,
                                    jw_input_proxy *proxy, uint64_t now_ms) {
    if (!monitor) {
        return;
    }
    if (now_ms >= monitor->next_rescan_ms) {
        jw__ext_rescan(monitor, proxy, now_ms);
    }
    jw__ext_drain_events(monitor, proxy);
    jw__ext_publish_wake_events(monitor, proxy);
}

void jw_external_input_monitor_shutdown(jw_external_input_monitor *monitor) {
    if (!monitor) {
        return;
    }
    for (int i = 0; i < JW_EXT_INPUT_MAX_PADS; i++) {
        jw__ext_drop(monitor, i, "shutdown");
    }
    monitor->wake_events[0] = '\0';
    unsetenv("CAT_INPUT_WAKE_EVENTS");
}
