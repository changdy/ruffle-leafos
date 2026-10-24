/* pipe2, for an atomically close-on-exec pipe. Plain pipe() plus fcntl would
   leave a window in which a fork on the daemon's main thread inherits the fds,
   which is the race this is closing. Same pattern as legacy_migration.c. */
#define _GNU_SOURCE

#include "internal/platform/input_proxy.h"
#include "internal/platform/calibration.h"
#include "internal/core/log.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define JW_MLP1_INPUT_NAME "Loong Gamepad"
#define JW_MLP1_PWRKEY_NAME "rk805 pwrkey"   /* physical power button (KEY_POWER) */
#define JW_MLP1_BRIGHTNESS_REPEAT_MS 120u
#define JW_MLP1_MENU_TAP_MS 80u
#define JW_MLP1_POWER_EDGE_MAX 8   /* pending press/release edges (4 full taps) */
/* The forwarding thread re-checks an overdue Menu deadline at this cadence, the
   daemon loop's old one, while the deadline waits on state rather than time. */
#define JW_MLP1_IO_RECHECK_MS 50
/* A jump this large in BOOTTIME - MONOTONIC means the device slept. Same
   threshold as jawakad's own resume check. */
#define JW_MLP1_RESUME_GAP_MS 2000LL
/* Force-feedback slots the virtual pad offers. SDL only ever holds one rumble
   effect per joystick, but a client may re-upload before erasing the old one, so
   leave headroom rather than making a re-upload fail. */
#define JW_MLP1_FF_EFFECTS_MAX 8

/* Older kernel uapi headers predate the y2038 input_event_sec accessors. */
#ifndef input_event_sec
#define input_event_sec  time.tv_sec
#define input_event_usec time.tv_usec
#endif

/* A daemon callback the forwarding thread hands to the thread that runs
   jawakad's main loop (see jw__proxy_call_run). */
typedef enum {
    JW__PROXY_CALL_MENU_TAP,
    JW__PROXY_CALL_SHORTCUT,
    JW__PROXY_CALL_MENU_ESCAPE,
    JW__PROXY_CALL_VOLUME,
    JW__PROXY_CALL_BRIGHTNESS,
} jw__proxy_call_kind;

typedef struct {
    jw__proxy_call_kind kind;
    int      value;       /* shortcut button, or percent delta */
    uint64_t hold_id;     /* menu escape */
    bool     threshold;   /* menu escape */
} jw__proxy_call;

typedef struct {
    int input_fd;
    int uinput_fd;
    int power_fd;     /* physical power key, watched read-only for auto-sleep wake (-1 = none) */
    char physical_path[JW_INPUT_PROXY_MAX_PATH];
    bool menu_held;
    bool menu_forwarded;
    bool chord_active;
    /* Physical state is independent of forwarded/consumed state. */
    unsigned char physical_keys[(KEY_MAX + 8) / 8];
    int abs_value[ABS_CNT];
    int abs_deadzone[ABS_CNT];
    bool physical_state_valid;
    bool input_evdev_clock;
    bool input_dropped;
    bool power_held;
    uint64_t menu_hold_id;
    uint64_t menu_down_ms;
    bool menu_escape_reported;
    /* Codes a claimed Menu chord ate the press of. Every later event for a
       marked code is swallowed until its release, which clears the bit.
       A bitset rather than one remembered code: several chord buttons can be
       held at once, and a per-button boolean would have to be added for every
       button the user can now bind. */
    unsigned char chord_consumed_keys[(KEY_MAX + 8) / 8];
    bool deferred_menu_release;
    uint64_t deferred_menu_release_at_ms;
    /* Action buttons forwarded as part of the current Menu chord, and the
       virtual Menu-up they are holding back. RetroArch stops masking libretro
       input the frame its hotkey modifier goes up, so an action still held at
       that moment turns into a live press inside the running game. */
    unsigned char chord_forwarded_keys[(KEY_MAX + 8) / 8];
    bool menu_up_pending;
    uint64_t last_brightness_ms;
    uint64_t last_activity_ms;     /* monotonic ms of the last EV_KEY (auto-sleep) */
    bool swallow;                  /* screen-off stage: wake on input but don't forward it */
    bool power_grabbed;            /* we hold the power key exclusively (jawakad owns sleep) */
    bool power_evdev_clock;        /* EVIOCSCLOCKID(CLOCK_MONOTONIC) took on power_fd, so
                                      event timestamps are usable as edge times */
    jw_power_edge power_edges[JW_MLP1_POWER_EDGE_MAX];  /* ring of unconsumed edges */
    int power_edge_head;
    int power_edge_count;
    /* Resting value per ABS axis, so a release can neutralize the stick and the
       D-pad hat, not just the buttons. Hats rest at 0; sticks at their midpoint. */
    bool abs_present[ABS_CNT];
    int  abs_neutral[ABS_CNT];
    jw_stick_calibration cal;      /* analog-stick normalization: profile or default */
    unsigned char held_keys[(KEY_MAX + 8) / 8];  /* buttons currently forwarded-down */
    /* Force-feedback effects uploaded by whoever holds the virtual pad. The
       kernel hands us the effect on upload and only the id on playback, so the
       magnitude has to be remembered here to be there when the play arrives. */
    struct {
        bool     used;
        uint16_t magnitude;   /* strong and weak collapsed to one 0..0xFFFF */
        uint32_t length_ms;   /* replay length; 0 = until stopped */
    } ff_effects[JW_MLP1_FF_EFFECTS_MAX];
    /* Which effect is driving the motor, or -1. An id rather than a bool: a
       client may hold several effects at once (RetroArch's udev joypad driver
       uses one per rumble channel), and with only a flag a stop for effect B
       would silence effect A and nothing would ever restart it. */
    int ff_playing_id;
    pthread_t ff_thread;
    bool      ff_thread_running;
    int       ff_quit_pipe[2];
    /* Forwarding thread (jw_input_proxy_start). While it runs it alone reads
       the pad and the power key, and `lock` guards everything above except the
       force-feedback fields, which stay the FF thread's. Recursive so a daemon
       callback run inline under it (menu_escape, from a flush) may call back
       in; the forwarding thread itself only ever holds it once, which its
       pthread_cond_wait on call_cond relies on. */
    pthread_t       io_thread;
    bool            io_thread_running;   /* written by the daemon thread only */
    bool            io_quit;
    pthread_mutex_t lock;
    int             io_kick_pipe[2];     /* daemon -> io thread: re-check, or quit */
    int             main_wake_pipe[2];   /* io thread -> daemon: call pending, power edge */
    long long       io_sleep_ref_ms;     /* BOOTTIME - MONOTONIC at the last check */
    bool            input_fd_failed;
    bool            power_fd_failed;
    uint64_t        generation;          /* tells a replaced proxy from this one */
    uint64_t        input_generation;    /* flush/screen-off invalidates in-flight input */
    /* The one callback the forwarding thread is blocked on, served by
       jw_input_proxy_tick() on the daemon thread. */
    pthread_cond_t  call_cond;
    jw__proxy_call  call;
    bool            call_pending;
    bool            call_taken;
    bool            call_result;
} jw_mlp1_input_proxy_data;

/* Stamped on each proxy at init; see jw_input_proxy_tick(). */
static uint64_t s_proxy_generation;

static bool jw__bit_is_set(const unsigned char *bits, int bit) {
    return (bits[bit / 8] & (1u << (bit % 8))) != 0;
}

static void jw__bit_set(unsigned char *bits, int bit) {
    bits[bit / 8] |= (unsigned char)(1u << (bit % 8));
}

static void jw__bit_clear(unsigned char *bits, int bit) {
    bits[bit / 8] &= (unsigned char)~(1u << (bit % 8));
}

static bool jw__bits_any(const unsigned char *bits, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (bits[i]) {
            return true;
        }
    }
    return false;
}

static uint64_t jw__monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* Time spent asleep so far: BOOTTIME keeps counting through suspend,
   MONOTONIC does not. */
static long long jw__slept_ms(void) {
    struct timespec mono, boot;
    if (clock_gettime(CLOCK_MONOTONIC, &mono) != 0 ||
        clock_gettime(CLOCK_BOOTTIME, &boot) != 0) {
        return 0;
    }
    return ((long long)boot.tv_sec - (long long)mono.tv_sec) * 1000LL +
           ((long long)boot.tv_nsec - (long long)mono.tv_nsec) / 1000000LL;
}

/* Both pipes are nonblocking: a kick into a full pipe is already pending. */
static void jw__pipe_kick(int fd) {
    if (fd >= 0) {
        ssize_t ignored = write(fd, "k", 1);
        (void)ignored;
    }
}

static void jw__pipe_drain(int fd) {
    char buf[64];
    while (fd >= 0 && read(fd, buf, sizeof(buf)) > 0) {
    }
}

/* Called under the proxy lock. An already-dispatched callback must finish
   before its waiter resumes; otherwise its completion could answer a newer
   call. Both cases invalidate the input that was waiting for the answer. */
static void jw__invalidate_input(jw_mlp1_input_proxy_data *data) {
    data->input_generation++;
    if (data->io_thread_running && data->call_pending && !data->call_taken) {
        data->call_result = false;
        data->call_pending = false;
        pthread_cond_signal(&data->call_cond);
    }
}

static bool jw__proxy_call_dispatch(jw_input_proxy *proxy, const jw__proxy_call *call) {
    switch (call->kind) {
    case JW__PROXY_CALL_MENU_TAP:
        return proxy->menu_tap && proxy->menu_tap(proxy->userdata);
    case JW__PROXY_CALL_SHORTCUT:
        return proxy->shortcut &&
               proxy->shortcut(proxy->userdata, (jw_input_shortcut_button)call->value);
    case JW__PROXY_CALL_MENU_ESCAPE:
        if (proxy->menu_escape)
            proxy->menu_escape(proxy->userdata, call->hold_id, call->threshold);
        return false;
    case JW__PROXY_CALL_VOLUME:
        if (proxy->volume_delta) proxy->volume_delta(proxy->userdata, call->value);
        return false;
    case JW__PROXY_CALL_BRIGHTNESS:
        if (proxy->brightness_delta) proxy->brightness_delta(proxy->userdata, call->value);
        return false;
    }
    return false;
}

/* Run a daemon callback. jawakad's callbacks read and change state only its
 * main loop may touch, so from the forwarding thread the call is handed to
 * jw_input_proxy_tick() and waited for, with `lock` released for the wait:
 * that is what lets the callback call back into the proxy, as it could when
 * everything ran on one thread. Everywhere else it runs inline. A call still
 * waiting when the proxy shuts down answers false, i.e. not handled.
 *
 * Input that follows waits too, so a Menu gesture or hotkey still costs a trip
 * through the daemon loop. Plain buttons and the D-pad never make one. */
static bool jw__proxy_call_run(jw_input_proxy *proxy, jw__proxy_call call) {
    jw_mlp1_input_proxy_data *data = proxy->backend_data;
    if (!data->io_thread_running || !pthread_equal(pthread_self(), data->io_thread)) {
        return jw__proxy_call_dispatch(proxy, &call);
    }
    if (data->io_quit) {
        return false;
    }
    data->call = call;
    data->call_pending = true;
    data->call_taken = false;
    data->call_result = false;
    jw__pipe_kick(data->main_wake_pipe[1]);
    while (data->call_pending && !data->io_quit) {
        pthread_cond_wait(&data->call_cond, &data->lock);
    }
    bool result = !data->call_pending && data->call_result;
    data->call_pending = false;
    return result;
}

/* Public entry points run on the daemon thread and take `lock` while the
   forwarding thread runs; the proxy's own code calls the unlocked jw__ forms
   below them, since it already holds it. */
static jw_mlp1_input_proxy_data *jw__api_lock(const jw_input_proxy *proxy) {
    jw_mlp1_input_proxy_data *data = proxy ? proxy->backend_data : NULL;
    if (data && data->io_thread_running) {
        pthread_mutex_lock(&data->lock);
    }
    return data;
}

/* Kicks the forwarding thread as well: the call may have armed a timer (a
   deferred Menu release) that its current poll timeout knows nothing about. */
static void jw__api_unlock(jw_mlp1_input_proxy_data *data) {
    if (data && data->io_thread_running) {
        pthread_mutex_unlock(&data->lock);
        jw__pipe_kick(data->io_kick_pipe[1]);
    }
}

/* For calls that only read state or pop the power-edge queue, which arm no
   timer. jawakad makes those on every loop pass, and a kick each time woke
   the forwarding thread for nothing. */
static void jw__api_unlock_quiet(jw_mlp1_input_proxy_data *data) {
    if (data && data->io_thread_running) {
        pthread_mutex_unlock(&data->lock);
    }
}

static bool jw__event_name_matches(int fd, const char *expected) {
    if (!expected || !expected[0]) {
        return false;
    }

    char name[128];
    memset(name, 0, sizeof(name));
    return ioctl(fd, EVIOCGNAME(sizeof(name)), name) >= 0 &&
           strcmp(name, expected) == 0;
}

static int jw__open_loong_gamepad(char *out_path, size_t out_size) {
    for (int i = 0; i < 32; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        if (jw__event_name_matches(fd, JW_MLP1_INPUT_NAME)) {
            snprintf(out_path, out_size, "%s", path);
            return fd;
        }
        close(fd);
    }
    return -1;
}

/* Open the physical power key read-only (no grab — stock loong_power still owns
   it). We only watch it so a power press counts as activity and wakes the
   auto-sleep screen-off stage. Returns fd or -1 if not present. */
static int jw__open_power_key(void) {
    for (int i = 0; i < 32; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }
        if (jw__event_name_matches(fd, JW_MLP1_PWRKEY_NAME)) {
            return fd;
        }
        close(fd);
    }
    return -1;
}

static int jw__uinput_copy_capabilities(int input_fd, int uinput_fd,
                                        const jw_stick_calibration *cal) {
    unsigned char ev_bits[(EV_MAX + 8) / 8];
    unsigned char key_bits[(KEY_MAX + 8) / 8];
    unsigned char abs_bits[(ABS_MAX + 8) / 8];
    memset(ev_bits, 0, sizeof(ev_bits));
    memset(key_bits, 0, sizeof(key_bits));
    memset(abs_bits, 0, sizeof(abs_bits));

    if (ioctl(input_fd, EVIOCGBIT(0, sizeof(ev_bits)), ev_bits) < 0) {
        return -1;
    }

    if (jw__bit_is_set(ev_bits, EV_KEY)) {
        if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
            ioctl(input_fd, EVIOCGBIT(EV_KEY, sizeof(key_bits)), key_bits) < 0) {
            return -1;
        }
        for (int code = 0; code <= KEY_MAX; code++) {
            if (jw__bit_is_set(key_bits, code)) {
                if (ioctl(uinput_fd, UI_SET_KEYBIT, code) < 0) {
                    return -1;
                }
            }
        }
    }

    if (jw__bit_is_set(ev_bits, EV_ABS)) {
        if (ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS) < 0 ||
            ioctl(input_fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
            return -1;
        }
        for (int code = 0; code <= ABS_MAX; code++) {
            if (!jw__bit_is_set(abs_bits, code)) {
                continue;
            }
            if (ioctl(uinput_fd, UI_SET_ABSBIT, code) < 0) {
                return -1;
            }
            struct input_absinfo absinfo;
            memset(&absinfo, 0, sizeof(absinfo));
            if (ioctl(input_fd, EVIOCGABS(code), &absinfo) < 0) {
                return -1;
            }
            /* The stick's current value seeds the virtual axis, and anything
               that opens the pad reads it before the first event. Seed it
               normalized like every forwarded value, or a resting stick's
               latched leftover shows up as a deflection until it next moves. */
            if (code == ABS_X || code == ABS_Y) {
                absinfo.value = jw_calibration_axis(cal, code == ABS_X, absinfo.value);
            }
            {
                struct uinput_abs_setup setup;
                memset(&setup, 0, sizeof(setup));
                setup.code = (uint16_t)code;
                setup.absinfo = absinfo;
                if (ioctl(uinput_fd, UI_ABS_SETUP, &setup) < 0) {
                    return -1;
                }
            }
        }
    }

    return 0;
}

/* Record where each ABS axis rests, for jw_input_proxy_release_buttons. A hat
   rests at 0; a stick rests at the midpoint of its range. */
static bool jw__capture_abs_neutrals(jw_mlp1_input_proxy_data *data) {
    unsigned char abs_bits[(ABS_MAX + 8) / 8];
    memset(abs_bits, 0, sizeof(abs_bits));
    if (ioctl(data->input_fd, EVIOCGBIT(EV_ABS, sizeof(abs_bits)), abs_bits) < 0) {
        return false;
    }
    for (int code = 0; code <= ABS_MAX; code++) {
        if (!jw__bit_is_set(abs_bits, code)) {
            continue;
        }
        data->abs_present[code] = true;
        struct input_absinfo absinfo;
        memset(&absinfo, 0, sizeof(absinfo));
        if (ioctl(data->input_fd, EVIOCGABS(code), &absinfo) == 0) {
            data->abs_neutral[code] = code >= ABS_HAT0X && code <= ABS_HAT3Y
                ? 0 : (absinfo.minimum + absinfo.maximum) / 2;
            data->abs_deadzone[code] = absinfo.flat;
            data->abs_value[code] = absinfo.value;
        } else {
            return false;
        }
    }
    return true;
}

static int jw__create_virtual_gamepad(int input_fd, const jw_stick_calibration *cal) {
    /* O_RDWR, not O_WRONLY: force-feedback upload requests and playback commands
       come back to us *through* this fd, so the write-only handle the pad used
       before FF existed can no longer serve the device. */
    int ufd = open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (ufd < 0) {
        return -1;
    }

    if (jw__uinput_copy_capabilities(input_fd, ufd, cal) != 0) {
        close(ufd);
        return -1;
    }

    /* FF_RUMBLE is *added*, not copied: the physical Loong Gamepad has no force
       feedback at all -- the motor hangs off a PWM channel jawakad owns. Putting
       it on the virtual pad is what lets an emulator rumble through the ordinary
       SDL/evdev path and reach that channel, with no per-emulator sysfs sink. */
    bool have_ff = ioctl(ufd, UI_SET_EVBIT, EV_FF) >= 0 &&
                   ioctl(ufd, UI_SET_FFBIT, FF_RUMBLE) >= 0;
    if (!have_ff) {
        jw_log_warn("input proxy: force feedback unavailable on the virtual pad: %s",
                    strerror(errno));
    }

    struct input_id id;
    memset(&id, 0, sizeof(id));
    if (ioctl(input_fd, EVIOCGID, &id) != 0) {
        close(ufd);
        return -1;
    }

    struct uinput_setup setup;
    memset(&setup, 0, sizeof(setup));
    if (ioctl(input_fd, EVIOCGNAME(sizeof(setup.name)), setup.name) < 0 ||
        !setup.name[0]) {
        close(ufd);
        return -1;
    }
    setup.id = id;
    setup.ff_effects_max = have_ff ? JW_MLP1_FF_EFFECTS_MAX : 0;
    if (ioctl(ufd, UI_DEV_SETUP, &setup) < 0 ||
        ioctl(ufd, UI_DEV_CREATE) < 0) {
        close(ufd);
        return -1;
    }

    return ufd;
}

static bool jw__same_rdev(const char *a, const char *b) {
    struct stat sa;
    struct stat sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_rdev == sb.st_rdev;
}

static bool jw__same_event_path(const char *a, const char *b) {
    if (!a || !b || !a[0] || !b[0]) {
        return false;
    }
    return strcmp(a, b) == 0 || jw__same_rdev(a, b);
}

static int jw__find_virtual_event(const char *physical_path, char *out, size_t out_size) {
    for (int attempt = 0; attempt < 50; attempt++) {
        for (int i = 0; i < 64; i++) {
            char path[64];
            snprintf(path, sizeof(path), "/dev/input/event%d", i);
            if (physical_path && jw__same_rdev(path, physical_path)) {
                continue;
            }

            int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
            if (fd < 0) {
                continue;
            }

            bool match = jw__event_name_matches(fd, JW_MLP1_INPUT_NAME);
            close(fd);
            if (match) {
                snprintf(out, out_size, "%s", path);
                return 0;
            }
        }
        usleep(20000);
    }
    return -1;
}

static void jw__write_event(jw_mlp1_input_proxy_data *data,
                            uint16_t type, uint16_t code, int32_t value) {
    if (data->uinput_fd < 0) {
        return;   /* watch-only mode: observe hotkeys, forward nothing */
    }
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    gettimeofday(&ev.time, NULL);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(data->uinput_fd, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) {
        jw_log_warn("input proxy: uinput write failed: %s", strerror(errno));
    }
}

static void jw__forward_event(jw_mlp1_input_proxy_data *data,
                              const struct input_event *ev) {
    if (data->uinput_fd < 0) {
        return;   /* watch-only mode: observe hotkeys, forward nothing */
    }
    /* Track which buttons are held-down on the virtual pad so the in-game menu
       can neutralize them before unpausing (see jw_input_proxy_release_buttons). */
    if (ev->type == EV_KEY && ev->code <= KEY_MAX) {
        if (ev->value > 0) {
            data->held_keys[ev->code / 8] |= (unsigned char)(1u << (ev->code % 8));
        } else {
            data->held_keys[ev->code / 8] &= (unsigned char)~(1u << (ev->code % 8));
        }
    }
    if (write(data->uinput_fd, ev, sizeof(*ev)) != (ssize_t)sizeof(*ev)) {
        jw_log_warn("input proxy: uinput forward failed: %s", strerror(errno));
    }
}

static void jw__emit_syn(jw_mlp1_input_proxy_data *data) {
    jw__write_event(data, EV_SYN, SYN_REPORT, 0);
}

/* Bring the virtual stick back in line with the physical one after a stretch of
   dropped events (screen-off, sleep). Otherwise the virtual pad keeps the last
   value it forwarded, often one from mid-motion, and reports that deflection
   until the stick next moves. */
static void jw__sync_stick(jw_mlp1_input_proxy_data *data) {
    if (data->uinput_fd < 0) {
        return;
    }
    bool wrote = false;
    for (int code = ABS_X; code <= ABS_Y; code++) {
        struct input_absinfo info;
        if (!data->abs_present[code] ||
            ioctl(data->input_fd, EVIOCGABS(code), &info) < 0) {
            continue;
        }
        data->abs_value[code] = info.value;
        jw__write_event(data, EV_ABS, (uint16_t)code,
                        jw_calibration_axis(&data->cal, code == ABS_X, info.value));
        wrote = true;
    }
    if (wrote) {
        jw__emit_syn(data);
    }
}

/* Forward an analog-stick axis event, remapped to the normalized output range
   (profile or built-in default). Non-stick ABS events never reach here. */
static void jw__forward_stick_abs(jw_mlp1_input_proxy_data *data,
                                  const struct input_event *ev) {
    if (data->uinput_fd < 0) {
        return;   /* watch-only mode: there is nothing to forward */
    }
    struct input_event out = *ev;
    out.value = jw_calibration_axis(&data->cal, ev->code == ABS_X, ev->value);
    jw__forward_event(data, &out);
}

/* The one place symbolic shortcut buttons meet Linux codes. Settings and the
   database deal only in the symbols; this table is why neither has to include
   <linux/input.h>, and why an evdev code renumbering could not silently change
   which action a stored binding means.

   Note BTN_TL2/BTN_TR2: this pad reports its triggers as ordinary keys, even
   though RetroArch's autoconfig maps them to SDL axes 4 and 5. The chord layer
   sees the key, so L2 and R2 are bindable here regardless. */
static jw_input_shortcut_button jw__shortcut_button_for_code(uint16_t code) {
    switch (code) {
        case BTN_EAST:   return JW_INPUT_SHORTCUT_BUTTON_A;
        case BTN_SOUTH:  return JW_INPUT_SHORTCUT_BUTTON_B;
        case BTN_NORTH:  return JW_INPUT_SHORTCUT_BUTTON_X;
        case BTN_WEST:   return JW_INPUT_SHORTCUT_BUTTON_Y;
        case BTN_TL:     return JW_INPUT_SHORTCUT_BUTTON_L1;
        case BTN_TR:     return JW_INPUT_SHORTCUT_BUTTON_R1;
        case BTN_TL2:    return JW_INPUT_SHORTCUT_BUTTON_L2;
        case BTN_TR2:    return JW_INPUT_SHORTCUT_BUTTON_R2;
        case BTN_SELECT: return JW_INPUT_SHORTCUT_BUTTON_SELECT;
        case BTN_START:  return JW_INPUT_SHORTCUT_BUTTON_START;
        case BTN_THUMBL: return JW_INPUT_SHORTCUT_BUTTON_L3;
        default:         return JW_INPUT_SHORTCUT_BUTTON_NONE;
    }
}

static void jw__flush_menu_press(jw_mlp1_input_proxy_data *data) {
    if (!data->menu_held || data->menu_forwarded || data->chord_active) {
        return;
    }
    jw__write_event(data, EV_KEY, BTN_MODE, 1);
    jw__emit_syn(data);
    data->menu_forwarded = true;
}

/* Emit the virtual Menu-up a forwarded chord was holding back, once the last
   action button it forwarded is up. Forced from the reset paths, where the
   releases being waited on are never going to arrive. */
static void jw__reset_chord_state(jw_input_proxy *proxy);

static void jw__release_pending_menu_up(jw_mlp1_input_proxy_data *data,
                                        bool force) {
    if (!data->menu_up_pending) {
        return;
    }
    if (!force &&
        jw__bits_any(data->chord_forwarded_keys,
                     sizeof(data->chord_forwarded_keys))) {
        return;
    }
    jw__write_event(data, EV_KEY, BTN_MODE, 0);
    jw__emit_syn(data);
    data->menu_up_pending = false;
    memset(data->chord_forwarded_keys, 0, sizeof(data->chord_forwarded_keys));
}

static void jw__emit_deferred_menu_tap(jw_mlp1_input_proxy_data *data) {
    jw__write_event(data, EV_KEY, BTN_MODE, 1);
    jw__emit_syn(data);
    data->deferred_menu_release = true;
    data->deferred_menu_release_at_ms = jw__monotonic_ms() + JW_MLP1_MENU_TAP_MS;
}

static void jw__release_deferred_menu_tap(jw_mlp1_input_proxy_data *data, bool force) {
    if (!data->deferred_menu_release) {
        return;
    }
    if (!force && jw__monotonic_ms() < data->deferred_menu_release_at_ms) {
        return;
    }

    jw__write_event(data, EV_KEY, BTN_MODE, 0);
    jw__emit_syn(data);
    data->deferred_menu_release = false;
    data->deferred_menu_release_at_ms = 0;
}

static bool jw__volume_key(uint16_t code) {
    return code == KEY_VOLUMEUP || code == KEY_VOLUMEDOWN;
}

static bool jw__axis_active(const jw_mlp1_input_proxy_data *data, int code) {
    int64_t center = data->abs_neutral[code];
    int64_t deadzone = data->abs_deadzone[code];
    if (code >= ABS_HAT0X && code <= ABS_HAT3Y) {
        center = 0;
        deadzone = 0;
    } else if (data->cal.loaded && (code == ABS_X || code == ABS_Y)) {
        center = code == ABS_X ? data->cal.x_zero : data->cal.y_zero;
        /* Menu gestures must tolerate the device's resting stick noise even
         * when gameplay calibration uses a smaller deadzone. */
        if (data->cal.deadzone > deadzone) deadzone = data->cal.deadzone;
    }
    int64_t delta = (int64_t)data->abs_value[code] - center;
    return delta > deadzone || delta < -deadzone;
}

static bool jw__menu_alone(const jw_mlp1_input_proxy_data *data) {
    if (!data->physical_state_valid || data->power_held) return false;
    for (int code = 0; code <= KEY_MAX; code++) {
        if (code != BTN_MODE && jw__bit_is_set(data->physical_keys, code))
            return false;
    }
    for (int code = 0; code < ABS_CNT; code++) {
        if (data->abs_present[code] && jw__axis_active(data, code)) return false;
    }
    return true;
}

static void jw__read_physical_state(jw_mlp1_input_proxy_data *data) {
    data->physical_state_valid =
        ioctl(data->input_fd, EVIOCGKEY(sizeof(data->physical_keys)),
              data->physical_keys) >= 0;
    for (int code = 0; code < ABS_CNT; code++) {
        if (!data->abs_present[code]) continue;
        struct input_absinfo info;
        if (ioctl(data->input_fd, EVIOCGABS(code), &info) < 0) {
            data->physical_state_valid = false;
        } else {
            data->abs_value[code] = info.value;
        }
    }
    if (data->power_fd >= 0) {
        unsigned char keys[(KEY_MAX + 8) / 8] = {0};
        if (ioctl(data->power_fd, EVIOCGKEY(sizeof(keys)), keys) < 0)
            data->physical_state_valid = false;
        else
            data->power_held = jw__bit_is_set(keys, KEY_POWER);
    }
}

static void jw__menu_end(jw_input_proxy *proxy) {
    jw_mlp1_input_proxy_data *data = proxy->backend_data;
    if (data->menu_held && proxy->menu_config.escape_enabled && proxy->menu_escape)
        jw__proxy_call_run(proxy, (jw__proxy_call){
            .kind = JW__PROXY_CALL_MENU_ESCAPE, .hold_id = data->menu_hold_id,
            .threshold = false });
}

static void jw__cancel_menu(jw_input_proxy *proxy) {
    if (!proxy || !proxy->backend_data || !proxy->menu_config.tap_only) return;
    jw_mlp1_input_proxy_data *data = proxy->backend_data;
    uint64_t generation = data->input_generation;
    if (data->menu_held && !data->chord_active) {
        jw__menu_end(proxy);
        if (generation != data->input_generation) return;
        data->chord_active = true;
    }
}

void jw_input_proxy_cancel_menu(jw_input_proxy *proxy) {
    jw_mlp1_input_proxy_data *data = jw__api_lock(proxy);
    jw__cancel_menu(proxy);
    jw__api_unlock(data);
}

/* Never infer a live hold from an event's read time. A lost input clock or
   state disables escape until the device can be read reliably again. */
static void jw__menu_deadline(jw_input_proxy *proxy, uint64_t now) {
    jw_mlp1_input_proxy_data *data = proxy->backend_data;
    if (proxy->menu_config.escape_enabled && data->input_evdev_clock &&
        !data->swallow && data->menu_held && !data->chord_active &&
        !data->menu_escape_reported && jw__menu_alone(data) &&
        now >= data->menu_down_ms &&
        now - data->menu_down_ms >= proxy->menu_config.escape_ms) {
        data->menu_escape_reported = true;
        if (proxy->menu_escape)
            jw__proxy_call_run(proxy, (jw__proxy_call){
                .kind = JW__PROXY_CALL_MENU_ESCAPE, .hold_id = data->menu_hold_id,
                .threshold = true });
    }
}

static void jw__handle_volume_key(jw_input_proxy *proxy, uint16_t code, int32_t value) {
    if (value <= 0 || !proxy->volume_delta) {
        return;
    }

    int delta = (code == KEY_VOLUMEUP) ? 5 : -5;
    jw__proxy_call_run(proxy, (jw__proxy_call){ .kind = JW__PROXY_CALL_VOLUME,
                                                .value = delta });
}

static void jw__handle_brightness_key(jw_input_proxy *proxy, uint16_t code, int32_t value) {
    if (value <= 0 || !proxy->brightness_delta) {
        return;
    }

    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    uint64_t now = jw__monotonic_ms();
    if (value == 2 && data->last_brightness_ms != 0 &&
        now - data->last_brightness_ms < JW_MLP1_BRIGHTNESS_REPEAT_MS) {
        return;
    }

    data->last_brightness_ms = now;
    int delta = (code == KEY_VOLUMEUP) ? 5 : -5;
    jw__proxy_call_run(proxy, (jw__proxy_call){ .kind = JW__PROXY_CALL_BRIGHTNESS,
                                                .value = delta });
}

static void jw__handle_key(jw_input_proxy *proxy, const struct input_event *ev) {
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    uint64_t generation = data->input_generation;

    if (ev->code == BTN_MODE) {
        if (proxy->menu_config.tap_only && ev->value == 2) return;
        if (ev->value > 0 && !data->menu_held) {
            data->menu_held = true;
            /* The virtual modifier can still be down from a chord whose
               release was deferred. Adopt it instead of deferring a second
               press: the re-flush would be a duplicate the input core drops,
               and the old chord's last action release would then emit a
               Menu-up underneath this new hold. */
            data->menu_forwarded = data->menu_up_pending;
            data->menu_up_pending = false;
            data->chord_active = false;
            if (proxy->menu_config.tap_only) {
                data->menu_hold_id++;
                data->menu_down_ms = (uint64_t)ev->input_event_sec * 1000u +
                                     (uint64_t)ev->input_event_usec / 1000u;
                data->menu_escape_reported = false;
                jw__release_deferred_menu_tap(data, true);
                if (!jw__menu_alone(data))
                    jw__cancel_menu(proxy);
            }
            return;
        }
        if (ev->value == 0 && data->menu_held) {
            uint64_t released_ms = (uint64_t)ev->input_event_sec * 1000u +
                                   (uint64_t)ev->input_event_usec / 1000u;
            if (proxy->menu_config.escape_enabled && data->input_evdev_clock &&
                released_ms >= data->menu_down_ms &&
                released_ms - data->menu_down_ms >= proxy->menu_config.escape_ms)
                data->menu_escape_reported = true; /* queued completed hold: no tap */
            if (data->menu_forwarded) {
                /* Physical Menu up while an action it forwarded is still down.
                   Sending the modifier release now would unmask libretro input
                   with that action held, and the core would read it as a fresh
                   press. Hold the virtual Menu down; the action's own release
                   below lets it go, so RetroArch always sees action-up before
                   modifier-up. */
                if (jw__bits_any(data->chord_forwarded_keys,
                                 sizeof(data->chord_forwarded_keys))) {
                    data->menu_up_pending = true;
                } else {
                    jw__forward_event(data, ev);
                }
            } else if (!data->chord_active && !data->menu_escape_reported) {
                bool handled = proxy->menu_tap &&
                               jw__proxy_call_run(proxy, (jw__proxy_call){
                                   .kind = JW__PROXY_CALL_MENU_TAP });
                if (generation != data->input_generation) return;
                if (!handled) {
                    jw__emit_deferred_menu_tap(data);
                }
            }
            jw__menu_end(proxy);
            if (generation != data->input_generation) return;
            data->menu_held = false;
            data->menu_forwarded = false;
            data->chord_active = false;
            return;
        }
        if (proxy->menu_config.tap_only) return; /* includes orphan releases */
    }

    if (ev->value > 0) {
        jw__cancel_menu(proxy);
        if (generation != data->input_generation) return;
    }

    /* Menu + a bindable button: the user's configured Leaf action.
       jawakad resolves the symbol against its cached snapshot and returns
       whether it claimed the chord; this layer only recognizes and consumes.
       Nothing here knows which action a button means. */
    jw_input_shortcut_button chord_button = jw__shortcut_button_for_code(ev->code);
    if (chord_button != JW_INPUT_SHORTCUT_BUTTON_NONE) {
        /* Once a chord has eaten the press, eat EVERYTHING for that code until
           the release. Swallowing only value==0 was not enough: a held button
           emits value==2 repeats, which matched neither guard, fell through to
           the forward at the bottom -- and jw__forward_event sets held_keys for
           any value > 0. The real release was then swallowed here, so the game
           saw the button go down and never come up. This device does not appear
           to emit repeats at all, but the guard costs nothing and an external
           pad may; the proxy test drives synthetic repeats through this path. */
        if (jw__bit_is_set(data->chord_consumed_keys, ev->code)) {
            if (ev->value == 0) {
                jw__bit_clear(data->chord_consumed_keys, ev->code);
            }
            return;
        }
        /* value==1 only. Claiming a repeat would leave the earlier real press
           forwarded-down and then swallow its release: the same stuck key by
           the other route. The pre-refactor Select block dispatched on
           value > 0 and could re-open the switcher on a repeat; unifying on
           the L1/R1 rule fixes that latent difference. */
        /* uinput_fd < 0 is watch-only: nothing is grabbed, so a "consumed"
           chord would reach the emulator anyway. Enforced here rather than
           left to the caller not to assign the hook. */
        if (ev->value == 1 && data->menu_held && !data->menu_forwarded &&
            data->uinput_fd >= 0) {
            bool handled = proxy->shortcut &&
                           jw__proxy_call_run(proxy, (jw__proxy_call){
                               .kind = JW__PROXY_CALL_SHORTCUT,
                               .value = (int)chord_button });
            if (generation != data->input_generation) return;
            if (handled) {
                data->chord_active = true;   /* suppress the Menu tap */
                jw__bit_set(data->chord_consumed_keys, ev->code);
                return; /* keep the deferred Menu unflushed; drop the press */
            }
            /* Declined: fall through so the deferred Menu flushes and the
               button forwards as an ordinary Menu chord for RetroArch. */
        }
    }

    if (jw__volume_key(ev->code)) {
        if (data->menu_held) {
            data->chord_active = true;
            jw__handle_brightness_key(proxy, ev->code, ev->value);
        } else {
            jw__handle_volume_key(proxy, ev->code, ev->value);
        }
        return;
    }

    if (!proxy->menu_config.tap_only &&
        data->menu_held && !data->menu_forwarded && ev->value > 0) {
        jw__flush_menu_press(data);
    }
    jw__forward_event(data, ev);

    /* Bookkeeping for the ordering guard above. Only real presses under an
       already-flushed Menu join the chord: a repeat would re-add a code the
       release is about to clear, and a button held from before Menu went down
       was never masked in the first place. BTN_MODE reaches here only as its
       own autorepeat, and must never wait on itself. */
    if (ev->code != BTN_MODE) {
        if (ev->value == 1 && data->menu_held && data->menu_forwarded) {
            jw__bit_set(data->chord_forwarded_keys, ev->code);
        } else if (ev->value == 0) {
            jw__bit_clear(data->chord_forwarded_keys, ev->code);
            jw__release_pending_menu_up(data, false);
        }
    }
}

/* ---- Force feedback -----------------------------------------------------
 *
 * uinput turns evdev force feedback inside out. A client that uploads an effect
 * with EVIOCSFF, or plays one by writing EV_FF, is not talking to the kernel --
 * the kernel relays the request back to whoever created the device, here. So the
 * pad's own creator has to answer them.
 *
 * That makes latency the whole design. EVIOCSFF *blocks the calling emulator*
 * until we answer it, and SDL re-uploads on every magnitude change, so serving
 * this from the daemon's 50ms housekeeping loop would stall the emulation thread
 * for up to three frames every time rumble starts or stops. It gets its own
 * thread blocked in poll() instead, which answers in well under a millisecond.
 */
static void jw__ff_handle_upload(jw_mlp1_input_proxy_data *data, int32_t request_id) {
    struct uinput_ff_upload upload;
    memset(&upload, 0, sizeof(upload));
    upload.request_id = (uint32_t)request_id;
    if (ioctl(data->uinput_fd, UI_BEGIN_FF_UPLOAD, &upload) < 0) {
        jw_log_warn("input proxy: FF upload begin failed: %s", strerror(errno));
        return;
    }

    int id = upload.effect.id;
    if (id < 0 || id >= JW_MLP1_FF_EFFECTS_MAX || upload.effect.type != FF_RUMBLE) {
        /* FF_RUMBLE is all the pad claims, so anything else is a client bug --
           and an out-of-range id would be a kernel one. Refuse rather than
           silently accepting an effect we would never play. */
        upload.retval = -EINVAL;
    } else {
        /* One motor, two magnitudes. Take the louder channel rather than
           averaging: a mix would dilute a strong-only effect and could leave a
           weak-only one below the motor's stiction floor, so the two most
           common single-channel cases both come out wrong. */
        uint16_t strong = upload.effect.u.rumble.strong_magnitude;
        uint16_t weak   = upload.effect.u.rumble.weak_magnitude;
        data->ff_effects[id].used      = true;
        data->ff_effects[id].magnitude = strong > weak ? strong : weak;
        data->ff_effects[id].length_ms = upload.effect.replay.length;
        upload.retval = 0;
    }

    if (ioctl(data->uinput_fd, UI_END_FF_UPLOAD, &upload) < 0) {
        jw_log_warn("input proxy: FF upload end failed: %s", strerror(errno));
    }
}

static void jw__ff_handle_erase(jw_input_proxy *proxy,
                                jw_mlp1_input_proxy_data *data,
                                int32_t request_id) {
    struct uinput_ff_erase erase;
    memset(&erase, 0, sizeof(erase));
    erase.request_id = (uint32_t)request_id;
    if (ioctl(data->uinput_fd, UI_BEGIN_FF_ERASE, &erase) < 0) {
        jw_log_warn("input proxy: FF erase begin failed: %s", strerror(errno));
        return;
    }

    if (erase.effect_id < JW_MLP1_FF_EFFECTS_MAX) {
        /* Erasing the effect that is driving the motor has to stop it. The
           client is entitled to erase without stopping first, and after this
           the slot is unused -- so any stop it does send afterwards would be
           dropped, and nothing else would turn the motor off until the hold
           timed out or the session ended. */
        if (data->ff_playing_id == (int)erase.effect_id) {
            data->ff_playing_id = -1;
            if (proxy->rumble) {
                proxy->rumble(proxy->userdata, 0, 0);
            }
        }
        data->ff_effects[erase.effect_id].used = false;
        erase.retval = 0;
    } else {
        erase.retval = -EINVAL;
    }

    if (ioctl(data->uinput_fd, UI_END_FF_ERASE, &erase) < 0) {
        jw_log_warn("input proxy: FF erase end failed: %s", strerror(errno));
    }
}

/* EV_FF: code is the effect id, value the repeat count (0 = stop). Note that
   SDL never sends a stop -- it stops by re-uploading the effect at magnitude 0
   and playing that -- so a zero magnitude has to mean stop as surely as a zero
   value does. */
static void jw__ff_handle_play(jw_input_proxy *proxy,
                               jw_mlp1_input_proxy_data *data,
                               uint16_t effect_id, int32_t value) {
    /* EV_FF also carries FF_GAIN and FF_AUTOCENTER, whose codes sit well above
       any effect id. The bound check is what keeps them out. */
    if (effect_id >= JW_MLP1_FF_EFFECTS_MAX) {
        return;
    }

    /* A stop is honored even for a slot we no longer have, and that ordering
       matters: refusing unknown effects first would swallow the stop for an
       effect erased while it was still playing, and leave the motor running. */
    bool known = data->ff_effects[effect_id].used;
    uint16_t magnitude = known ? data->ff_effects[effect_id].magnitude : 0u;
    if (value == 0 || magnitude == 0) {
        /* Only the effect actually holding the motor may stop it. Otherwise a
           client winding down one of several channels silences the others. */
        if (data->ff_playing_id != (int)effect_id) {
            return;
        }
        data->ff_playing_id = -1;
        if (proxy->rumble) {
            proxy->rumble(proxy->userdata, 0, 0);
        }
        return;
    }

    if (!known) {
        return;   /* play of an effect we were never given */
    }

    data->ff_playing_id = (int)effect_id;
    if (proxy->rumble) {
        proxy->rumble(proxy->userdata, magnitude,
                      data->ff_effects[effect_id].length_ms);
    }
}

static void jw__ff_drain(jw_input_proxy *proxy, jw_mlp1_input_proxy_data *data) {
    struct input_event ev;
    while (read(data->uinput_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_UINPUT) {
            if (ev.code == UI_FF_UPLOAD) {
                jw__ff_handle_upload(data, ev.value);
            } else if (ev.code == UI_FF_ERASE) {
                jw__ff_handle_erase(proxy, data, ev.value);
            }
        } else if (ev.type == EV_FF) {
            jw__ff_handle_play(proxy, data, ev.code, ev.value);
        }
    }
}

static void *jw__ff_thread_main(void *arg) {
    jw_input_proxy *proxy = (jw_input_proxy *)arg;
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;

    for (;;) {
        struct pollfd fds[2];
        fds[0].fd = data->uinput_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = data->ff_quit_pipe[0];
        fds[1].events = POLLIN;
        fds[1].revents = 0;

        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) {
                continue;
            }
            jw_log_warn("input proxy: FF poll failed: %s", strerror(errno));
            break;
        }
        if (fds[1].revents) {
            break;              /* shutdown asked us to stop */
        }
        /* Leave on any error condition. Without this an fd reporting POLLERR but
           not POLLIN would neither drain nor break, and poll() would return
           immediately every time round: a silent 100% CPU spin inside the
           daemon, which is a far worse failure than losing rumble. */
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            jw_log_warn("input proxy: FF fd error (revents=0x%x); stopping",
                        (unsigned)fds[0].revents);
            break;
        }
        if (fds[0].revents & POLLIN) {
            jw__ff_drain(proxy, data);
        }
    }

    /* Never hand the motor back still running: if the emulator is killed
       mid-rumble there is no one left to send the stop. */
    if (data->ff_playing_id >= 0) {
        data->ff_playing_id = -1;
        if (proxy->rumble) {
            proxy->rumble(proxy->userdata, 0, 0);
        }
    }
    return NULL;
}

static void jw__ff_thread_start(jw_input_proxy *proxy,
                                jw_mlp1_input_proxy_data *data) {
    if (data->uinput_fd < 0) {
        return;
    }
    /* O_CLOEXEC, like every other fd here: the daemon forks and execs launchers,
       emulators and third-party pak code constantly, and a plain pipe() would
       hand all of them both ends of the daemon's own shutdown channel. */
    if (pipe2(data->ff_quit_pipe, O_CLOEXEC) != 0) {
        jw_log_warn("input proxy: FF quit pipe failed: %s", strerror(errno));
        data->ff_quit_pipe[0] = data->ff_quit_pipe[1] = -1;
        return;
    }
    if (pthread_create(&data->ff_thread, NULL, jw__ff_thread_main, proxy) != 0) {
        jw_log_warn("input proxy: FF thread failed to start; rumble unavailable");
        close(data->ff_quit_pipe[0]);
        close(data->ff_quit_pipe[1]);
        data->ff_quit_pipe[0] = data->ff_quit_pipe[1] = -1;
        return;
    }
    data->ff_thread_running = true;
}

/* Stop the thread before anything it touches goes away. It must be joined, not
   just signalled: it owns the uinput fd for reads and the rumble callback, and
   both are torn down the instant this returns. */
static void jw__ff_thread_stop(jw_mlp1_input_proxy_data *data) {
    if (!data->ff_thread_running) {
        return;
    }
    if (data->ff_quit_pipe[1] >= 0) {
        ssize_t ignored = write(data->ff_quit_pipe[1], "q", 1);
        (void)ignored;
    }
    pthread_join(data->ff_thread, NULL);
    data->ff_thread_running = false;
    if (data->ff_quit_pipe[0] >= 0) close(data->ff_quit_pipe[0]);
    if (data->ff_quit_pipe[1] >= 0) close(data->ff_quit_pipe[1]);
    data->ff_quit_pipe[0] = data->ff_quit_pipe[1] = -1;
}

static int jw__input_proxy_init_impl(jw_input_proxy *proxy,
                                     jw_input_brightness_delta_cb brightness_delta,
                                     jw_input_volume_delta_cb volume_delta,
                                     jw_input_menu_tap_cb menu_tap,
                                     void *userdata,
                                     bool watch_only) {
    if (!proxy) {
        return -1;
    }
    memset(proxy, 0, sizeof(*proxy));
    proxy->brightness_delta = brightness_delta;
    proxy->volume_delta = volume_delta;
    proxy->menu_tap = menu_tap;
    proxy->userdata = userdata;

    const char *enabled = getenv("JAWAKA_INPUT_PROXY");
    if (enabled && strcmp(enabled, "0") == 0) {
        jw_log_info("input proxy: disabled by JAWAKA_INPUT_PROXY=0");
        return 0;
    }

    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)calloc(1, sizeof(*data));
    if (!data) {
        return -1;
    }
    data->input_fd = -1;
    data->uinput_fd = -1;
    data->power_fd = -1;
    data->ff_quit_pipe[0] = data->ff_quit_pipe[1] = -1;
    data->io_kick_pipe[0] = data->io_kick_pipe[1] = -1;
    data->main_wake_pipe[0] = data->main_wake_pipe[1] = -1;
    data->generation = ++s_proxy_generation;
    /* Explicit: calloc leaves this 0, which would read as "effect 0 is playing"
       and let a stray stop for effect 0 through before anything was ever sent. */
    data->ff_playing_id = -1;
    data->last_activity_ms = jw__monotonic_ms();   /* don't count boot as idle */
    if (!jw_calibration_load(&data->cal)) {
        jw_calibration_default(&data->cal);
        jw_log_info("calibration: no profile; default X[%d..%d] Y[%d..%d] dz=%d",
                    data->cal.x_min, data->cal.x_max, data->cal.y_min,
                    data->cal.y_max, data->cal.deadzone);
    }

    data->input_fd = jw__open_loong_gamepad(data->physical_path, sizeof(data->physical_path));
    if (data->input_fd < 0) {
        jw_log_warn("input proxy: Loong Gamepad not found");
        free(data);
        return 0;
    }
    snprintf(proxy->physical_event_path, sizeof(proxy->physical_event_path),
             "%s", data->physical_path);
    snprintf(proxy->device_name, sizeof(proxy->device_name), "%s", JW_MLP1_INPUT_NAME);

    int input_clock = CLOCK_MONOTONIC;
    data->input_evdev_clock =
        ioctl(data->input_fd, EVIOCSCLOCKID, &input_clock) == 0;
    if (!watch_only && !data->input_evdev_clock) {
        jw_log_warn("input proxy: monotonic gamepad clock unavailable: %s", strerror(errno));
        close(data->input_fd);
        free(data);
        return -1;
    }

    if (!watch_only) {
        if (!jw__capture_abs_neutrals(data)) {
            jw_log_warn("input proxy: could not read axis state");
            close(data->input_fd);
            free(data);
            return -1;
        }
        data->uinput_fd = jw__create_virtual_gamepad(data->input_fd, &data->cal);
        if (data->uinput_fd < 0) {
            jw_log_warn("input proxy: could not create virtual gamepad: %s", strerror(errno));
            close(data->input_fd);
            free(data);
            return 0;
        }

        if (jw__find_virtual_event(data->physical_path,
                                   proxy->virtual_event_path,
                                   sizeof(proxy->virtual_event_path)) != 0) {
            jw_log_warn("input proxy: virtual event path not found");
        }

        if (ioctl(data->input_fd, EVIOCGRAB, 1) < 0) {
            jw_log_warn("input proxy: EVIOCGRAB failed: %s", strerror(errno));
            ioctl(data->uinput_fd, UI_DEV_DESTROY);
            close(data->uinput_fd);
            close(data->input_fd);
            free(data);
            return 0;
        }
    }

    /* Take over the power key: EVIOCGRAB it so stock loong_power never sees a press
       and jawakad owns the whole sleep/wake story (power = sleep when the screen is
       on, wake when it's off — all through jawakad's own real-suspend path). The
       PMIC still hard-powers-off on a long hold regardless of this grab. */
    data->power_fd = jw__open_power_key();
    if (data->power_fd >= 0 && ioctl(data->power_fd, EVIOCGRAB, 1) == 0) {
        data->power_grabbed = true;
    }
    if (data->power_fd >= 0) {
        /* Stamp edges with kernel event time in the clock jawakad measures hold
           durations in, so a press/release that queues during a stalled daemon
           tick still reports its true duration (long-press vs tap). */
        int clk = CLOCK_MONOTONIC;
        data->power_evdev_clock = ioctl(data->power_fd, EVIOCSCLOCKID, &clk) == 0;
        if (!data->power_evdev_clock) {
            jw_log_warn("input proxy: EVIOCSCLOCKID failed on power key; "
                        "edge times fall back to read time");
        }
    }
    jw_log_info("input proxy: power key %s",
                data->power_grabbed ? "grabbed (jawakad owns it)"
                                    : (data->power_fd >= 0 ? "open (grab failed)"
                                                           : "not found"));

    proxy->backend_data = data;
    proxy->enabled = true;
    if (!watch_only && !data->power_evdev_clock) {
        jw_log_warn("input proxy: monotonic power input unavailable");
        jw_input_proxy_shutdown(proxy);
        return -1;
    }
    jw__read_physical_state(data);
    /* Safe to start before the caller assigns proxy->rumble: nothing has opened
       the pad yet, so no effect can arrive until well after that write. */
    jw__ff_thread_start(proxy, data);
    if (watch_only) {
        jw_log_info("input proxy: watching %s (no grab; hotkeys only)",
                    data->physical_path);
    } else {
        jw_log_info("input proxy: grabbed %s, virtual=%s",
                    data->physical_path,
                    proxy->virtual_event_path[0] ? proxy->virtual_event_path : "(unknown)");
    }
    return 0;
}

int jw_input_proxy_init(jw_input_proxy *proxy,
                        jw_input_brightness_delta_cb brightness_delta,
                        jw_input_volume_delta_cb volume_delta,
                        jw_input_menu_tap_cb menu_tap,
                        void *userdata) {
    return jw__input_proxy_init_impl(proxy, brightness_delta, volume_delta,
                                     menu_tap, userdata, false);
}

/* Watch-only variant for standalone emulator sessions: the emulator reads the
   physical gamepad directly (no grab, no virtual device), while jawakad still
   observes the same device for volume/brightness and Menu hotkeys. The emulator
   also receives those hotkey presses, but PPSSPP's guide button mapping is
   patched out so Jawaka can open its pause menu via SIGUSR2 instead. */
int jw_input_proxy_init_watch(jw_input_proxy *proxy,
                              jw_input_brightness_delta_cb brightness_delta,
                              jw_input_volume_delta_cb volume_delta,
                              jw_input_menu_tap_cb menu_tap,
                              void *userdata) {
    return jw__input_proxy_init_impl(proxy, brightness_delta, volume_delta,
                                     menu_tap, userdata, true);
}

int jw_input_proxy_retroarch_joypad_index(const jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->virtual_event_path[0]) {
        return -1;
    }

    const char *device_name = proxy->device_name[0] ? proxy->device_name : JW_MLP1_INPUT_NAME;
    int joypad_index = 0;
    for (int i = 0; i < 64; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);

        int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            continue;
        }

        bool match = jw__event_name_matches(fd, device_name);
        close(fd);
        if (!match) {
            continue;
        }

        if (jw__same_event_path(path, proxy->virtual_event_path)) {
            return joypad_index;
        }
        joypad_index++;
    }

    return -1;
}

bool jw_input_proxy_needs_tick_cadence(const jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->backend_data) {
        return false;
    }
    const jw_mlp1_input_proxy_data *data =
        (const jw_mlp1_input_proxy_data *)proxy->backend_data;
    return !data->io_thread_running;
}

int jw_input_proxy_poll_fd(const jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->backend_data) {
        return -1;
    }
    const jw_mlp1_input_proxy_data *data =
        (const jw_mlp1_input_proxy_data *)proxy->backend_data;
    /* Threaded, the pad is the forwarding thread's to read; the daemon only
       needs waking for a callback or a power edge. */
    return data->io_thread_running ? data->main_wake_pipe[0] : data->input_fd;
}

static void jw__handle_input(jw_input_proxy *proxy, const struct input_event *ev) {
    jw_mlp1_input_proxy_data *data = proxy->backend_data;
    uint64_t generation = data->input_generation;
    if (ev->type == EV_SYN && ev->code == SYN_DROPPED) {
        data->physical_state_valid = false;
        data->input_dropped = true;
        jw__reset_chord_state(proxy);
        return;
    }
    if (data->input_dropped) {
        if (ev->type == EV_SYN && ev->code == SYN_REPORT) {
            jw__read_physical_state(data);
            data->input_dropped = false;
        }
        return;
    }
    if (ev->type == EV_KEY && ev->code <= KEY_MAX) {
        if (ev->value > 0) jw__bit_set(data->physical_keys, ev->code);
        else jw__bit_clear(data->physical_keys, ev->code);
    } else if (ev->type == EV_ABS && ev->code < ABS_CNT) {
        data->abs_value[ev->code] = ev->value;
        if (jw__axis_active(data, ev->code)) jw__cancel_menu(proxy);
    }
    if (generation != data->input_generation) return;

    bool activity = ev->type == EV_KEY ||
        (ev->type == EV_ABS &&
         (ev->code == ABS_HAT0X || ev->code == ABS_HAT0Y) && ev->value != 0);
    if (activity)
        data->last_activity_ms = jw__monotonic_ms();
    if (data->swallow) {
        /* Swallowed input is how a screen-off standby wakes. jawakad sleeps
           up to a second between passes when idle, so tell it now rather than
           leave the press for its next pass to notice. */
        if (activity && data->io_thread_running)
            jw__pipe_kick(data->main_wake_pipe[1]);
        return;
    }
    if (ev->type == EV_KEY) jw__handle_key(proxy, ev);
    else if (ev->type == EV_ABS && (ev->code == ABS_X || ev->code == ABS_Y))
        jw__forward_stick_abs(data, ev);
    else jw__forward_event(data, ev);
}

static void jw__handle_power(jw_input_proxy *proxy, const struct input_event *ev) {
    jw_mlp1_input_proxy_data *data = proxy->backend_data;
    uint64_t generation = data->input_generation;
    if (ev->type == EV_SYN && ev->code == SYN_DROPPED) {
        jw__cancel_menu(proxy);
        if (generation != data->input_generation) return;
        jw__read_physical_state(data);
    }
    if (ev->type != EV_KEY || ev->code != KEY_POWER ||
        (ev->value != 0 && ev->value != 1)) return;
    data->power_held = ev->value == 1;
    jw__cancel_menu(proxy);
    if (generation != data->input_generation) return;
    if (data->power_edge_count == JW_MLP1_POWER_EDGE_MAX) {
        data->power_edge_head = (data->power_edge_head + 1) % JW_MLP1_POWER_EDGE_MAX;
        data->power_edge_count--;
    }
    int tail = (data->power_edge_head + data->power_edge_count) % JW_MLP1_POWER_EDGE_MAX;
    data->power_edges[tail].down = data->power_held;
    data->power_edges[tail].ms = data->power_evdev_clock
        ? (uint64_t)ev->input_event_sec * 1000u + (uint64_t)ev->input_event_usec / 1000u
        : jw__monotonic_ms();
    data->power_edge_count++;
    if (data->io_thread_running) {
        jw__pipe_kick(data->main_wake_pipe[1]);   /* jawakad routes the edge */
    }
}

static bool jw__read_input_edge(jw_input_proxy *proxy, int fd, struct input_event *ev) {
    if (fd < 0) return false;
    ssize_t got;
    do { got = read(fd, ev, sizeof(*ev)); } while (got < 0 && errno == EINTR);
    if (got == (ssize_t)sizeof(*ev)) return true;
    if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;
    jw_mlp1_input_proxy_data *data = proxy->backend_data;
    data->physical_state_valid = false;
    jw__cancel_menu(proxy);
    return false;
}

/* Read and route everything queued on the pad and the power key, then run the
   timers. On the forwarding thread when there is one, else from
   jw_input_proxy_tick(). */
static void jw__process(jw_input_proxy *proxy) {
    jw_mlp1_input_proxy_data *data = proxy->backend_data;
    jw__release_deferred_menu_tap(data, false);

    /* Merge the two monotonic streams before routing taps or testing deadlines.
       Draining Power first would suppress a completed Menu tap followed by a
       separate Power press; draining it last could leak a queued Menu+Power tap. */
    struct input_event input, power;
    bool have_input = false, have_power = false;
    uint64_t generation = data->input_generation;
    for (;;) {
        if (data->io_quit) break;   /* shutting down: leave the rest unread */
        if (generation != data->input_generation) {
            /* Either stream may already have a prefetched edge whose release
               the daemon flushed while a callback was waiting. */
            have_input = have_power = false;
            generation = data->input_generation;
        }
        if (!have_input && !data->input_fd_failed)
            have_input = jw__read_input_edge(proxy, data->input_fd, &input);
        if (!have_power && !data->power_fd_failed)
            have_power = jw__read_input_edge(proxy, data->power_fd, &power);
        if (generation != data->input_generation) continue;
        if (!have_input && !have_power) break;
        bool power_first = !have_input || (have_power &&
            (power.input_event_sec < input.input_event_sec ||
             (power.input_event_sec == input.input_event_sec &&
              power.input_event_usec <= input.input_event_usec)));
        if (power_first) {
            jw__handle_power(proxy, &power);
            have_power = false;
        } else {
            jw__handle_input(proxy, &input);
            have_input = false;
        }
    }
    jw__release_deferred_menu_tap(data, false);
    jw__menu_deadline(proxy, jw__monotonic_ms());
}

void jw_input_proxy_tick(jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->backend_data) return;
    jw_mlp1_input_proxy_data *data = proxy->backend_data;
    if (!data->io_thread_running) {
        jw__process(proxy);
        return;
    }

    /* Threaded: the forwarding thread reads and forwards on its own; this side
       serves the callback it is waiting on, if any. */
    jw__pipe_drain(data->main_wake_pipe[0]);
    pthread_mutex_lock(&data->lock);
    if (!data->call_pending || data->call_taken) {
        pthread_mutex_unlock(&data->lock);
        return;
    }
    jw__proxy_call call = data->call;
    data->call_taken = true;
    uint64_t generation = data->generation;
    pthread_mutex_unlock(&data->lock);

    bool result = jw__proxy_call_dispatch(proxy, &call);

    /* The callback may have shut this proxy down, or replaced it; then there
       is no one left waiting on the answer. */
    if (proxy->backend_data != data || data->generation != generation) return;
    pthread_mutex_lock(&data->lock);
    data->call_result = result;
    data->call_pending = false;
    data->call_taken = false;
    pthread_cond_signal(&data->call_cond);
    pthread_mutex_unlock(&data->lock);
}

uint64_t jw_input_proxy_idle_ms(const jw_input_proxy *proxy) {
    if (!proxy || !proxy->backend_data) {
        return 0;
    }
    jw_mlp1_input_proxy_data *data = jw__api_lock(proxy);
    uint64_t now = jw__monotonic_ms();
    uint64_t idle = (now > data->last_activity_ms) ? (now - data->last_activity_ms) : 0;
    jw__api_unlock_quiet(data);
    return idle;
}

void jw_input_proxy_mark_activity(jw_input_proxy *proxy) {
    if (!proxy || !proxy->backend_data) {
        return;
    }
    jw_mlp1_input_proxy_data *data = jw__api_lock(proxy);
    data->last_activity_ms = jw__monotonic_ms();
    jw__api_unlock(data);
}

void jw_input_proxy_set_swallow(jw_input_proxy *proxy, bool swallow) {
    if (!proxy || !proxy->backend_data) {
        return;
    }
    jw_mlp1_input_proxy_data *data = jw__api_lock(proxy);
    /* Entering the screen-off stage drops every event on the floor, including the
       releases the chord state machine is waiting on. Left set, a *_chord_consumed
       flag outlives the press it belongs to and eats the first real press of that
       button after the screen comes back -- R1 is run or aim in most cores, so
       that one is felt. Clear the in-flight chord state instead: with the screen
       off nothing is mid-gesture, and everything here is "waiting for a release
       that is no longer coming". */
    if (swallow && !data->swallow) {
        /* Entering screen-off drops every event on the floor, including the
           releases the chord machine is waiting on. Nothing is mid-gesture
           with the screen off, so put the machine back to rest rather than
           leaving it waiting for releases that are no longer coming. */
        jw__invalidate_input(data);
        jw__reset_chord_state(proxy);
    } else if (!swallow && data->swallow) {
        jw__sync_stick(data);   /* stick events were dropped with the screen off */
    }
    data->swallow = swallow;
    jw__api_unlock(data);
}

void jw_input_proxy_emit_menu_tap(jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->backend_data) {
        return;
    }
    jw_mlp1_input_proxy_data *data = jw__api_lock(proxy);
    /* Watch-only has no virtual pad to emit onto, and a tap already in flight
       must not stack a second press, which would double-toggle. */
    if (data->uinput_fd >= 0 && !data->deferred_menu_release) {
        jw__emit_deferred_menu_tap(data);
    }
    jw__api_unlock(data);
}

static void jw__release_buttons(jw_input_proxy *proxy) {
    if (!proxy || !proxy->backend_data) {
        return;
    }
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    if (data->uinput_fd < 0) {
        return;   /* watch-only mode: nothing forwarded to release */
    }
    bool released_any = false;
    for (int code = 0; code <= KEY_MAX; code++) {
        if (jw__bit_is_set(data->held_keys, code)) {
            jw__write_event(data, EV_KEY, (uint16_t)code, 0);
            data->held_keys[code / 8] &= (unsigned char)~(1u << (code % 8));
            released_any = true;
        }
    }
    /* Buttons alone are not enough: on this pad the D-pad is an ABS hat, so a
       key-only release leaves the hat pinned and the consumer keeps auto-
       repeating a direction that is physically already up. Send every axis back
       to rest as well. */
    for (int code = 0; code <= ABS_MAX; code++) {
        if (data->abs_present[code]) {
            jw__write_event(data, EV_ABS, (uint16_t)code, data->abs_neutral[code]);
            released_any = true;
        }
    }
    if (released_any) {
        jw__emit_syn(data);
    }
    /* Every forwarded action just went up, so nothing is left for a deferred
       Menu-up to wait on. Callers use this to flush input across a transition;
       leaving the virtual modifier latched would strand it for the next app. */
    memset(data->chord_forwarded_keys, 0, sizeof(data->chord_forwarded_keys));
    jw__release_pending_menu_up(data, true);
}

void jw_input_proxy_release_buttons(jw_input_proxy *proxy) {
    jw_mlp1_input_proxy_data *data = jw__api_lock(proxy);
    jw__release_buttons(proxy);
    jw__api_unlock(data);
}

bool jw_input_proxy_take_held_button(jw_input_proxy *proxy,
                                     jw_input_shortcut_button button) {
    if (!proxy || !proxy->enabled || !proxy->backend_data ||
        button == JW_INPUT_SHORTCUT_BUTTON_NONE) {
        return false;
    }
    jw_mlp1_input_proxy_data *data = jw__api_lock(proxy);
    bool held = false;
    if (data->physical_state_valid) {
        for (int code = 0; code <= KEY_MAX; code++) {
            if (jw__shortcut_button_for_code((uint16_t)code) != button ||
                !jw__bit_is_set(data->physical_keys, code)) {
                continue;
            }
            held = true;
            if (data->uinput_fd < 0) {
                continue;   /* watch-only: nothing reaches anyone through us */
            }
            if (jw__bit_is_set(data->held_keys, code)) {
                jw__write_event(data, EV_KEY, (uint16_t)code, 0);
                jw__emit_syn(data);
                jw__bit_clear(data->held_keys, code);
            }
            /* The same swallow a claimed Menu chord uses: every event for the
               code until its release, which clears the bit. */
            jw__bit_set(data->chord_consumed_keys, code);
        }
    }
    jw__api_unlock(data);
    return held;
}

bool jw_input_proxy_take_power_edge(jw_input_proxy *proxy, jw_power_edge *edge) {
    if (!proxy || !proxy->backend_data) {
        return false;
    }
    jw_mlp1_input_proxy_data *data = jw__api_lock(proxy);
    bool took = data->power_edge_count > 0;
    if (took) {
        if (edge) *edge = data->power_edges[data->power_edge_head];
        data->power_edge_head = (data->power_edge_head + 1) % JW_MLP1_POWER_EDGE_MAX;
        data->power_edge_count--;
    }
    jw__api_unlock_quiet(data);
    return took;
}

/* Put the chord state machine back to rest and let go of anything the virtual
 * pad is still holding.
 *
 * flush, screen-off and shutdown all discard events the machine is waiting on
 * -- specifically the releases that clear a consumed chord and free a deferred
 * Menu-up. Left set, a consumed bit outlives the press it belongs to and eats
 * that button's next real press; a flushed-but-unreleased Menu strands the
 * virtual modifier down with nothing left that knows.
 *
 * Idempotent. Every release is guarded by the state that records it, so a
 * caller that already called jw_input_proxy_release_buttons() does not emit a
 * second set of releases. */
static void jw__reset_chord_state(jw_input_proxy *proxy) {
    if (!proxy || !proxy->backend_data) {
        return;
    }
    jw_mlp1_input_proxy_data *data =
        (jw_mlp1_input_proxy_data *)proxy->backend_data;

    uint64_t generation = data->input_generation;
    jw__menu_end(proxy);
    if (generation != data->input_generation) return;
    data->menu_escape_reported = false;

    /* Forwarded buttons and axes, plus any deferred Menu-up they were holding
       back. No-op in watch-only mode, where nothing was forwarded. */
    jw__release_buttons(proxy);

    /* Forced: the un-forced form waits out a hold timer driven by a loop that
       may not run again. */
    jw__release_deferred_menu_tap(data, true);

    /* The remaining way BTN_MODE can be down: flushed by a chord that fell
       through to an ordinary press. That goes out through jw__write_event, so
       held_keys carries no bit for it and release_buttons cannot free it. */
    if (data->menu_forwarded) {
        jw__write_event(data, EV_KEY, BTN_MODE, 0);
        jw__emit_syn(data);
        data->menu_up_pending = false;
    }
    jw__release_pending_menu_up(data, true);

    memset(data->chord_consumed_keys, 0, sizeof(data->chord_consumed_keys));
    memset(data->chord_forwarded_keys, 0, sizeof(data->chord_forwarded_keys));
    data->menu_held      = false;
    data->menu_forwarded = false;
    data->chord_active   = false;
}

static void jw__flush(jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->backend_data) {
        return;
    }
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    jw__invalidate_input(data);
    /* Drain the physical gamepad without forwarding — drops presses that queued
       while suspended so they don't replay into the launcher on wake. */
    struct input_event ev;
    while (read(data->input_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        /* discard */
    }
    if (data->power_fd >= 0) {
        struct input_event pev;
        while (read(data->power_fd, &pev, sizeof(pev)) == (ssize_t)sizeof(pev)) {
            /* discard */
        }
    }
    data->power_edge_count = 0;   /* drop already-queued edges too */

    /* The drain above just discarded whatever releases were queued, which
       includes the ones that would clear a consumed chord and free a deferred
       Menu-up. Reset for the same reason screen-off does. */
    jw__reset_chord_state(proxy);
    jw__read_physical_state(data);
    jw__sync_stick(data);   /* the drain dropped stick events too */
    /* Whatever the sleep left queued is gone now, so the forwarding thread's
       resume check must not fire on the next, real press. */
    data->io_sleep_ref_ms = jw__slept_ms();
}

void jw_input_proxy_flush(jw_input_proxy *proxy) {
    jw_mlp1_input_proxy_data *data = jw__api_lock(proxy);
    jw__flush(proxy);
    jw__api_unlock(data);
}

void jw_input_proxy_configure_menu(jw_input_proxy *proxy,
                                  jw_input_menu_config config) {
    if (!proxy) return;
    jw_mlp1_input_proxy_data *data = jw__api_lock(proxy);
    jw__flush(proxy);
    proxy->menu_config = config;
    jw__api_unlock(data);
}

/* ---- Forwarding thread ---------------------------------------------------
 *
 * jawakad's main loop answers IPC, supervises children and spawns platform
 * helpers, and some of that blocks for hundreds of milliseconds. When that
 * same loop also forwarded the pad, a D-pad release made during one of those
 * stalls reached the launcher late, the launcher saw a long hold, and it
 * repeated. This thread reads and forwards on its own, so a busy daemon can no
 * longer stretch a tap into a hold. Daemon callbacks still run on the daemon
 * thread (jw__proxy_call_run).
 */

/* The poll timeout that wakes the thread for its next timer: the release of a
   deferred Menu tap, or the Menu-escape threshold. -1 when neither is armed. */
static int jw__io_timeout_ms(jw_input_proxy *proxy) {
    jw_mlp1_input_proxy_data *data = proxy->backend_data;
    uint64_t due = UINT64_MAX;
    if (data->deferred_menu_release) {
        due = data->deferred_menu_release_at_ms;
    }
    if (proxy->menu_config.escape_enabled && data->menu_held &&
        !data->chord_active && !data->menu_escape_reported) {
        uint64_t escape_at = data->menu_down_ms + proxy->menu_config.escape_ms;
        if (escape_at < due) due = escape_at;
    }
    if (due == UINT64_MAX) {
        return -1;
    }
    uint64_t now = jw__monotonic_ms();
    /* Overdue: the deadline is waiting on state (Menu not alone yet) rather
       than on time. Recheck at the old loop cadence instead of spinning. */
    if (due <= now) {
        return JW_MLP1_IO_RECHECK_MS;
    }
    uint64_t wait = due - now;
    return wait > (uint64_t)INT32_MAX ? INT32_MAX : (int)wait;
}

/* jawakad drops input that queued while the device slept, and it could do that
   only because it read the pad itself, after checking. Reading here, this
   thread would forward those presses before jawakad noticed the resume, so it
   checks first too. The daemon's own flush on the same resume moves the
   reference (jw__flush), so the two never both fire. */
static void jw__io_check_resume(jw_input_proxy *proxy) {
    jw_mlp1_input_proxy_data *data = proxy->backend_data;
    long long slept = jw__slept_ms();
    long long gap = slept - data->io_sleep_ref_ms;
    data->io_sleep_ref_ms = slept;
    if (gap > JW_MLP1_RESUME_GAP_MS) {
        jw_log_info("input proxy: ~%lldms asleep, dropping queued input", gap);
        jw__flush(proxy);
        data->last_activity_ms = jw__monotonic_ms();
    }
}

static void *jw__io_thread_main(void *arg) {
    jw_input_proxy *proxy = (jw_input_proxy *)arg;
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;

    /* Taken before anything else: jw_input_proxy_start() holds it across
       pthread_create, so data->io_thread is set by the time this runs. */
    pthread_mutex_lock(&data->lock);
    while (!data->io_quit) {
        struct pollfd fds[3] = {
            { data->input_fd_failed ? -1 : data->input_fd, POLLIN, 0 },
            { data->power_fd_failed ? -1 : data->power_fd, POLLIN, 0 },
            { data->io_kick_pipe[0], POLLIN, 0 },
        };
        int timeout = jw__io_timeout_ms(proxy);
        pthread_mutex_unlock(&data->lock);
        int rc = poll(fds, 3, timeout);
        int poll_errno = errno;
        pthread_mutex_lock(&data->lock);
        if (data->io_quit) {
            break;
        }
        if (rc < 0 && poll_errno != EINTR) {
            /* Transient (ENOMEM); losing the pad would be far worse than a
               short pause, so back off and carry on. */
            jw_log_warn("input proxy: poll failed: %s", strerror(poll_errno));
            pthread_mutex_unlock(&data->lock);
            usleep(JW_MLP1_IO_RECHECK_MS * 1000);
            pthread_mutex_lock(&data->lock);
            continue;
        }
        if (fds[2].revents) {
            jw__pipe_drain(data->io_kick_pipe[0]);
        }
        /* A device that reports an error never becomes readable again; polling
           it would return at once, every time, and spin. */
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            jw_log_warn("input proxy: gamepad fd error (revents=0x%x); no longer reading it",
                        (unsigned)fds[0].revents);
            data->input_fd_failed = true;
            data->physical_state_valid = false;
            jw__cancel_menu(proxy);
        }
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            jw_log_warn("input proxy: power key fd error (revents=0x%x); no longer reading it",
                        (unsigned)fds[1].revents);
            data->power_fd_failed = true;
        }
        jw__io_check_resume(proxy);
        jw__process(proxy);
    }
    pthread_mutex_unlock(&data->lock);
    return NULL;
}

int jw_input_proxy_start(jw_input_proxy *proxy) {
    if (!proxy || !proxy->enabled || !proxy->backend_data) {
        return -1;
    }
    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    if (data->io_thread_running) {
        return 0;
    }

    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) {
        return -1;
    }
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int rc = pthread_mutex_init(&data->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rc != 0) {
        return -1;
    }
    if (pthread_cond_init(&data->call_cond, NULL) != 0) {
        pthread_mutex_destroy(&data->lock);
        return -1;
    }
    /* O_CLOEXEC for the same reason as the FF quit pipe: jawakad forks and
       execs constantly. */
    if (pipe2(data->io_kick_pipe, O_CLOEXEC | O_NONBLOCK) != 0 ||
        pipe2(data->main_wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0) {
        jw_log_warn("input proxy: forwarding pipes failed: %s", strerror(errno));
        for (int i = 0; i < 2; i++) {
            if (data->io_kick_pipe[i] >= 0) close(data->io_kick_pipe[i]);
            if (data->main_wake_pipe[i] >= 0) close(data->main_wake_pipe[i]);
            data->io_kick_pipe[i] = data->main_wake_pipe[i] = -1;
        }
        pthread_cond_destroy(&data->call_cond);
        pthread_mutex_destroy(&data->lock);
        return -1;
    }
    data->io_sleep_ref_ms = jw__slept_ms();
    data->io_quit = false;

    pthread_mutex_lock(&data->lock);
    data->io_thread_running = true;
    rc = pthread_create(&data->io_thread, NULL, jw__io_thread_main, proxy);
    if (rc != 0) {
        data->io_thread_running = false;
    }
    pthread_mutex_unlock(&data->lock);
    if (rc != 0) {
        jw_log_warn("input proxy: forwarding thread failed to start; "
                    "forwarding from the daemon loop");
        for (int i = 0; i < 2; i++) {
            close(data->io_kick_pipe[i]);
            close(data->main_wake_pipe[i]);
            data->io_kick_pipe[i] = data->main_wake_pipe[i] = -1;
        }
        pthread_cond_destroy(&data->call_cond);
        pthread_mutex_destroy(&data->lock);
        return -1;
    }
    jw_log_info("input proxy: forwarding on its own thread");
    return 0;
}

/* Joined, not just signalled: the thread reads the fds and writes the virtual
   pad that shutdown is about to close. A callback it is waiting on answers
   false, including one this very shutdown is running inside of. */
static void jw__io_thread_stop(jw_mlp1_input_proxy_data *data) {
    if (!data->io_thread_running) {
        return;
    }
    pthread_mutex_lock(&data->lock);
    data->io_quit = true;
    jw__invalidate_input(data);
    pthread_cond_broadcast(&data->call_cond);
    pthread_mutex_unlock(&data->lock);
    jw__pipe_kick(data->io_kick_pipe[1]);
    pthread_join(data->io_thread, NULL);
    data->io_thread_running = false;
    for (int i = 0; i < 2; i++) {
        close(data->io_kick_pipe[i]);
        close(data->main_wake_pipe[i]);
        data->io_kick_pipe[i] = data->main_wake_pipe[i] = -1;
    }
    pthread_cond_destroy(&data->call_cond);
    pthread_mutex_destroy(&data->lock);
}

void jw_input_proxy_shutdown(jw_input_proxy *proxy) {
    if (!proxy) return;
    if (!proxy->backend_data) {
        memset(proxy, 0, sizeof(*proxy));
        return;
    }

    jw_mlp1_input_proxy_data *data = (jw_mlp1_input_proxy_data *)proxy->backend_data;
    jw__io_thread_stop(data);
    jw__ff_thread_stop(data);
    jw__reset_chord_state(proxy);

    if (data->input_fd >= 0) {
        ioctl(data->input_fd, EVIOCGRAB, 0);
        close(data->input_fd);
    }
    if (data->uinput_fd >= 0) {
        ioctl(data->uinput_fd, UI_DEV_DESTROY);
        close(data->uinput_fd);
    }
    if (data->power_fd >= 0) {
        close(data->power_fd);
    }
    free(data);
    memset(proxy, 0, sizeof(*proxy));
}
