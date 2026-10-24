/* Chord state-machine test for the MLP1 input proxy.
 *
 * Runs on the device (see `make mlp1-adb-chord-test`), because the proxy needs
 * <linux/input.h> and uinput and cannot be built on the Mac host.
 *
 * It includes the implementation directly to reach the static state machine.
 * The alternative -- widening input_proxy.h until the internals are public --
 * would make the API worse to make it testable, and the chord logic is exactly
 * the part that has to be right.
 *
 * No real device is touched: the physical input is a nonblocking pipe and the
 * "virtual pad" is a temp file, so every emitted event is readable back and asserted. That is
 * what lets this drive cases the hardware cannot produce -- notably autorepeat.
 * This pad emits no repeats (measured 2026-08-30: no EV_REP, and zero value==2
 * over six buttons held to 8s), but a repeat must not strand a button if a
 * future unit or an external pad sends one, and Jawaka@0b0edd6 reports exactly
 * that failure from a device that apparently did. */

#define _GNU_SOURCE

#include <stdarg.h>
#include <sys/ioctl.h>
#include <time.h>
static int test_ioctl(int fd, unsigned long request, ...);
static int test_clock_gettime(clockid_t clock, struct timespec *ts);
#define ioctl test_ioctl
#define clock_gettime test_clock_gettime
#include "internal/platform/input_proxy_mlp1.c"
#undef clock_gettime
#undef ioctl

#include <stdio.h>

/* Deliberately no <assert.h>. The device build is a release profile with
   -DNDEBUG, so assert() compiles to nothing and an assert-based test would
   pass without executing a single check. Everything here reports through
   expect() instead, and no call with a side effect is ever wrapped. */

/* ── harness ─────────────────────────────────────────────────────────── */

static jw_input_proxy g_proxy;
static jw_mlp1_input_proxy_data g_data;
static char g_sink_path[64];

static uint64_t g_now;
static int g_source[2] = {-1, -1};
static bool g_claim_tap;
static int g_taps, g_brightness, g_thresholds, g_ends;
static uint64_t g_last_hold;

static int test_clock_gettime(clockid_t clock, struct timespec *ts) {
    (void)clock;
    ts->tv_sec = g_now / 1000;
    ts->tv_nsec = (g_now % 1000) * 1000000;
    return 0;
}

/* Kernel-state reads on our fake physical pad. Writes still go to the real
   temp-file sink. No device is opened or grabbed by this harness. */
static int test_ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == EVIOCGKEY(sizeof(g_data.physical_keys))) {
        memcpy(arg, g_data.physical_keys, sizeof(g_data.physical_keys));
        return 0;
    }
    for (int code = 0; code < ABS_CNT; code++) {
        if (request == (unsigned long)EVIOCGABS(code)) {
            struct input_absinfo *info = arg;
            memset(info, 0, sizeof(*info));
            info->value = g_data.abs_value[code];
            return 0;
        }
    }
    errno = EINVAL;
    return -1;
}

static pthread_t g_main_thread;
static bool g_tap_on_main;
static bool g_flush_in_callback;

static bool test_tap(void *unused) {
    (void)unused;
    g_taps++;
    g_tap_on_main = pthread_equal(pthread_self(), g_main_thread);
    if (g_flush_in_callback) jw_input_proxy_flush(&g_proxy);
    return g_claim_tap;
}
static void test_brightness(void *unused, int delta) {
    (void)unused; (void)delta; g_brightness++;
}
static void test_escape(void *unused, uint64_t hold, bool threshold) {
    (void)unused;
    if (threshold) g_thresholds++; else g_ends++;
    g_last_hold = hold;
}

/* Which buttons the dispatcher claims, and what it was asked about. */
static bool g_claim[JW_INPUT_SHORTCUT_BUTTON_COUNT];
static jw_input_shortcut_button g_last_button;
static int g_dispatch_calls;

static bool test_dispatch(void *userdata, jw_input_shortcut_button button) {
    (void)userdata;
    g_last_button = button;
    g_dispatch_calls++;
    if (g_flush_in_callback) jw_input_proxy_flush(&g_proxy);
    return button < JW_INPUT_SHORTCUT_BUTTON_COUNT && g_claim[button];
}

static bool g_have_sink;

static void reset_proxy(bool watch_only) {
    /* Guarded by a flag, not by uinput_fd >= 0: on the first call g_data is
       still zeroed, and fd 0 would close stdin. */
    if (g_have_sink && g_data.uinput_fd >= 0) close(g_data.uinput_fd);
    g_have_sink = false;
    if (g_source[0] >= 0) close(g_source[0]);
    if (g_source[1] >= 0) close(g_source[1]);
    if (pipe2(g_source, O_NONBLOCK | O_CLOEXEC) != 0) exit(2);
    g_now = 10000;
    g_claim_tap = false;
    g_flush_in_callback = false;
    g_taps = g_brightness = g_thresholds = g_ends = 0;
    g_last_hold = 0;
    memset(&g_proxy, 0, sizeof(g_proxy));
    memset(&g_data, 0, sizeof(g_data));
    memset(g_claim, 0, sizeof(g_claim));
    g_dispatch_calls = 0;
    g_last_button = JW_INPUT_SHORTCUT_BUTTON_NONE;

    snprintf(g_sink_path, sizeof(g_sink_path), "/tmp/jw-chord-XXXXXX");
    int fd = mkstemp(g_sink_path);
    if (fd < 0) {
        fprintf(stderr, "input-proxy-chord-test: mkstemp failed\n");
        exit(2);
    }
    unlink(g_sink_path);   /* the fd is all we need */
    /* Watch-only is "no virtual pad", which is exactly uinput_fd < 0. */
    g_data.uinput_fd = watch_only ? -1 : fd;
    if (watch_only) close(fd); else g_have_sink = true;
    g_data.input_fd = g_source[0];
    g_data.input_evdev_clock = true;
    g_data.physical_state_valid = true;
    g_data.power_fd = -1;
    g_proxy.backend_data = &g_data;
    g_proxy.enabled = true;
    g_proxy.shortcut = test_dispatch;
    g_proxy.menu_tap = test_tap;
    g_proxy.brightness_delta = test_brightness;
    g_proxy.menu_escape = test_escape;
}

/* Feed one evdev event through the state machine. */
static void feed(uint16_t code, int32_t value) {
    struct input_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    ev.input_event_sec = g_now / 1000;
    ev.input_event_usec = (g_now % 1000) * 1000;
    jw__handle_input(&g_proxy, &ev);
}

static void feed_abs(uint16_t code, int32_t value) {
    struct input_event ev = {0};
    ev.type = EV_ABS; ev.code = code; ev.value = value;
    g_data.abs_present[code] = true;
    jw__handle_input(&g_proxy, &ev);
}

static void tap_only(bool supports_menu) {
    reset_proxy(false);
    jw_input_proxy_configure_menu(&g_proxy, (jw_input_menu_config){true, true, 3000});
    g_claim_tap = !supports_menu;
}

static void advance(uint64_t ms) {
    g_now += ms;
    jw_input_proxy_tick(&g_proxy);
}

static void queue_key(uint16_t code, int value, uint64_t ms) {
    struct input_event ev = {0};
    ev.type = EV_KEY; ev.code = code; ev.value = value;
    ev.input_event_sec = ms / 1000;
    ev.input_event_usec = (ms % 1000) * 1000;
    if (write(g_source[1], &ev, sizeof(ev)) != sizeof(ev)) exit(2);
}

/* Everything written to the virtual pad since the last drain, EV_KEY only.
   Returns the count; SYN reports are skipped as framing. */
typedef struct { uint16_t code; int32_t value; } emitted;

static int drain(emitted *out, int max) {
    if (g_data.uinput_fd < 0) return 0;
    int fd = g_data.uinput_fd;
    if (lseek(fd, 0, SEEK_SET) != 0) {
        fprintf(stderr, "input-proxy-chord-test: rewind failed\n");
        exit(2);
    }
    int n = 0;
    struct input_event ev;
    while (n < max && read(fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
        if (ev.type != EV_KEY) continue;   /* SYN is framing */
        out[n].code = ev.code;
        out[n].value = ev.value;
        n++;
    }
    /* Start the next window clean. */
    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) != 0) {
        fprintf(stderr, "input-proxy-chord-test: reset failed\n");
        exit(2);
    }
    return n;
}

/* Forwarding-thread helpers. The thread writes the sink on its own schedule,
   so read it only under the proxy lock, and give it real time to get there. */
static int sink_keys_locked(void) {
    struct stat st;
    if (fstat(g_data.uinput_fd, &st) != 0) return 0;
    int keys = 0;
    struct input_event ev;
    for (off_t off = 0; off + (off_t)sizeof(ev) <= st.st_size; off += (off_t)sizeof(ev)) {
        if (pread(g_data.uinput_fd, &ev, sizeof(ev), off) == (ssize_t)sizeof(ev) &&
            ev.type == EV_KEY) keys++;
    }
    return keys;
}

static int drain_when(emitted *out, int max, int want) {
    for (int waited = 0; waited < 2000; waited += 5) {
        jw_mlp1_input_proxy_data *data = jw__api_lock(&g_proxy);
        int keys = sink_keys_locked();
        int n = keys >= want ? drain(out, max) : -1;
        jw__api_unlock(data);
        if (n >= 0) return n;
        usleep(5000);
    }
    jw_mlp1_input_proxy_data *data = jw__api_lock(&g_proxy);
    int n = drain(out, max);
    jw__api_unlock(data);
    return n;
}

/* What jawakad's loop does: sleep on the proxy's fd, then tick. */
static bool serve_one_call(void) {
    struct pollfd pfd = { jw_input_proxy_poll_fd(&g_proxy), POLLIN, 0 };
    if (poll(&pfd, 1, 2000) <= 0) return false;
    jw_input_proxy_tick(&g_proxy);
    return true;
}

static int g_failures;

static void expect(const char *what, bool ok) {
    if (!ok) {
        fprintf(stderr, "input-proxy-chord-test: FAIL %s\n", what);
        g_failures++;
    }
}

static void expect_seq(const char *what, const emitted *got, int n,
                       const emitted *want, int want_n) {
    bool ok = (n == want_n);
    for (int i = 0; ok && i < n; i++) {
        ok = got[i].code == want[i].code && got[i].value == want[i].value;
    }
    if (!ok) {
        fprintf(stderr, "input-proxy-chord-test: FAIL %s\n  got: ", what);
        for (int i = 0; i < n; i++)
            fprintf(stderr, "%u=%d ", got[i].code, got[i].value);
        fprintf(stderr, "\n  want: ");
        for (int i = 0; i < want_n; i++)
            fprintf(stderr, "%u=%d ", want[i].code, want[i].value);
        fprintf(stderr, "\n");
        g_failures++;
    }
}

/* ── cases ───────────────────────────────────────────────────────────── */

int main(void) {
    emitted got[16];
    int n;
    g_main_thread = pthread_self();

    /* A claimed chord reaches the game as nothing at all. */
    reset_proxy(false);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_SELECT] = true;
    feed(BTN_MODE, 1);
    feed(BTN_SELECT, 1);
    feed(BTN_SELECT, 0);
    feed(BTN_MODE, 0);
    n = drain(got, 16);
    expect("claimed chord emits nothing", n == 0);
    expect("dispatch saw Select", g_last_button == JW_INPUT_SHORTCUT_BUTTON_SELECT);
    expect("dispatch called once", g_dispatch_calls == 1);

    /* Autorepeat inside a claimed chord. The hardware does not produce this;
       the code must survive it anyway. Every repeat is swallowed, the action
       fires once, and the release does not strand the button down. */
    reset_proxy(false);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_R1] = true;
    feed(BTN_MODE, 1);
    feed(BTN_TR, 1);
    feed(BTN_TR, 2);
    feed(BTN_TR, 2);
    feed(BTN_TR, 2);
    feed(BTN_TR, 0);
    feed(BTN_MODE, 0);
    n = drain(got, 16);
    expect("repeats in a claimed chord emit nothing", n == 0);
    expect("repeats do not re-dispatch", g_dispatch_calls == 1);
    expect("no key left held", !jw__bits_any(g_data.held_keys,
                                             sizeof(g_data.held_keys)));

    /* Same, with Select. The pre-refactor Select block dispatched on value>0
       and would have fired the switcher again on every repeat. */
    reset_proxy(false);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_SELECT] = true;
    feed(BTN_MODE, 1);
    feed(BTN_SELECT, 1);
    feed(BTN_SELECT, 2);
    feed(BTN_SELECT, 2);
    feed(BTN_SELECT, 0);
    feed(BTN_MODE, 0);
    n = drain(got, 16);
    expect("Select repeats emit nothing", n == 0);
    expect("Select dispatches exactly once", g_dispatch_calls == 1);

    /* A declined chord forwards Menu first, then the button: RetroArch needs
       its modifier down before the action to read it as a hotkey. */
    reset_proxy(false);
    feed(BTN_MODE, 1);
    feed(BTN_NORTH, 1);
    n = drain(got, 16);
    {
        const emitted want[] = {{BTN_MODE, 1}, {BTN_NORTH, 1}};
        expect_seq("declined chord forwards Menu then button", got, n, want, 2);
    }
    expect("dispatch was asked", g_dispatch_calls == 1);
    expect("dispatch saw X", g_last_button == JW_INPUT_SHORTCUT_BUTTON_X);

    /* ...and releasing Menu first holds the modifier up until the action is
       released, so the core never sees a live press after the modifier goes.
       This is the Slice A regression, re-asserted here now that the chord
       path has been rewritten around it. */
    feed(BTN_MODE, 0);
    n = drain(got, 16);
    expect("Menu-up deferred while action held", n == 0);
    feed(BTN_NORTH, 0);
    n = drain(got, 16);
    {
        const emitted want[] = {{BTN_NORTH, 0}, {BTN_MODE, 0}};
        expect_seq("action-up precedes deferred Menu-up", got, n, want, 2);
    }

    /* Ordinary release order needs no deferral. */
    reset_proxy(false);
    feed(BTN_MODE, 1);
    feed(BTN_NORTH, 1);
    (void)drain(got, 16);
    feed(BTN_NORTH, 0);
    feed(BTN_MODE, 0);
    n = drain(got, 16);
    {
        const emitted want[] = {{BTN_NORTH, 0}, {BTN_MODE, 0}};
        expect_seq("ordinary order forwards both releases", got, n, want, 2);
    }

    /* A repeat on a DECLINED chord must forward, not be swallowed: the game
       is holding that button and the proxy is only a wire here. */
    reset_proxy(false);
    feed(BTN_MODE, 1);
    feed(BTN_NORTH, 1);
    (void)drain(got, 16);
    feed(BTN_NORTH, 2);
    n = drain(got, 16);
    {
        const emitted want[] = {{BTN_NORTH, 2}};
        expect_seq("declined chord forwards repeats", got, n, want, 1);
    }
    /* And must not ask again. This is the only place the dispatch guard's
       value==1 is observable: once a chord is claimed the consumed bitset
       returns early, so a value>0 guard looks identical there. Re-asking
       mid-hold would let a handler that has since changed its mind consume a
       button the game is already holding down. */
    expect("declined chord does not re-dispatch on a repeat",
           g_dispatch_calls == 1);

    /* An unbindable button (d-pad arrives as EV_KEY on some pads) is never
       offered to the dispatcher, and forwards as an ordinary chord. */
    reset_proxy(false);
    feed(BTN_MODE, 1);
    feed(KEY_UP, 1);
    n = drain(got, 16);
    expect("unbindable code is not dispatched", g_dispatch_calls == 0);
    {
        const emitted want[] = {{BTN_MODE, 1}, {KEY_UP, 1}};
        expect_seq("unbindable code forwards as a chord", got, n, want, 2);
    }

    /* Two claimed buttons held at once: the bitset must track both, and
       neither release may leak. */
    reset_proxy(false);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_L1] = true;
    g_claim[JW_INPUT_SHORTCUT_BUTTON_R1] = true;
    feed(BTN_MODE, 1);
    feed(BTN_TL, 1);
    feed(BTN_TR, 1);
    feed(BTN_TL, 0);
    feed(BTN_TR, 0);
    feed(BTN_MODE, 0);
    n = drain(got, 16);
    expect("two claimed buttons emit nothing", n == 0);
    expect("both dispatched", g_dispatch_calls == 2);

    /* Screen-off must not strand a deferred Menu. */
    reset_proxy(false);
    feed(BTN_MODE, 1);
    feed(BTN_NORTH, 1);
    (void)drain(got, 16);
    feed(BTN_MODE, 0);          /* deferred: action still held */
    (void)drain(got, 16);
    jw_input_proxy_set_swallow(&g_proxy, true);
    n = drain(got, 16);
    {
        /* The reset frees the still-held action first, then the modifier --
           the same ordering the deferred Menu-up exists to guarantee. jawakad
           calls jw_input_proxy_release_buttons() just before this, so in the
           real caller the action is already up and only the modifier release
           is emitted; the reset being idempotent is what makes both correct. */
        const emitted want[] = {{BTN_NORTH, 0}, {BTN_MODE, 0}};
        expect_seq("screen-off releases the action then the pending Menu",
                   got, n, want, 2);
    }
    expect("no pending Menu after screen-off", !g_data.menu_up_pending);
    expect("no key held after screen-off",
           !jw__bits_any(g_data.held_keys, sizeof(g_data.held_keys)));

    /* A bare press with no Menu held is never a chord. */
    reset_proxy(false);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_SELECT] = true;
    feed(BTN_SELECT, 1);
    feed(BTN_SELECT, 0);
    n = drain(got, 16);
    expect("bare press is not dispatched", g_dispatch_calls == 0);
    {
        const emitted want[] = {{BTN_SELECT, 1}, {BTN_SELECT, 0}};
        expect_seq("bare press forwards unchanged", got, n, want, 2);
    }

    /* --- flush mid-chord -------------------------------------------------
       jw_input_proxy_flush() discards queued physical events, including the
       releases the chord machine is waiting on. If it left state behind, the
       consumed bit would outlive its press and eat that button's NEXT real
       press, and a flushed Menu would stay latched down. */

    /* Handled chord flushed before its physical release. */
    reset_proxy(false);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_SELECT] = true;
    feed(BTN_MODE, 1);
    feed(BTN_SELECT, 1);          /* claimed; consumed bit set, release pending */
    (void)drain(got, 16);
    jw_input_proxy_flush(&g_proxy);
    expect("flush clears the consumed bitset",
           !jw__bits_any(g_data.chord_consumed_keys,
                         sizeof(g_data.chord_consumed_keys)));
    expect("flush clears menu_held", !g_data.menu_held);
    expect("flush clears chord_active", !g_data.chord_active);
    (void)drain(got, 16);
    /* The next ordinary press must reach the game: not swallowed as though it
       were still the old chord's release. */
    g_claim[JW_INPUT_SHORTCUT_BUTTON_SELECT] = false;
    feed(BTN_SELECT, 1);
    feed(BTN_SELECT, 0);
    n = drain(got, 16);
    {
        const emitted want[] = {{BTN_SELECT, 1}, {BTN_SELECT, 0}};
        expect_seq("press after flush is forwarded, not swallowed", got, n, want, 2);
    }

    /* Forwarded chord flushed while Menu and the action are both held. The
       virtual modifier must not be left down with no release coming. */
    reset_proxy(false);
    feed(BTN_MODE, 1);
    feed(BTN_NORTH, 1);           /* declined: Menu-down + action-down forwarded */
    (void)drain(got, 16);
    jw_input_proxy_flush(&g_proxy);
    n = drain(got, 16);
    {
        /* release_buttons frees the forwarded action, then the modifier. */
        const emitted want[] = {{BTN_NORTH, 0}, {BTN_MODE, 0}};
        expect_seq("flush releases the action then the modifier", got, n, want, 2);
    }
    expect("flush clears menu_forwarded", !g_data.menu_forwarded);
    expect("flush leaves no pending Menu-up", !g_data.menu_up_pending);
    expect("flush clears the forwarded bitset",
           !jw__bits_any(g_data.chord_forwarded_keys,
                         sizeof(g_data.chord_forwarded_keys)));
    expect("flush leaves no key held", !jw__bits_any(g_data.held_keys,
                                                     sizeof(g_data.held_keys)));

    /* Flushing at rest emits nothing: the reset is idempotent, so a caller
       that already released buttons does not get a second set. */
    reset_proxy(false);
    jw_input_proxy_flush(&g_proxy);
    n = drain(got, 16);
    expect("flush at rest emits nothing", n == 0);

    /* Watch-only never dispatches: nothing is grabbed, so a claimed chord
       would reach the emulator anyway. */
    reset_proxy(true);
    g_claim[JW_INPUT_SHORTCUT_BUTTON_SELECT] = true;
    feed(BTN_MODE, 1);
    feed(BTN_SELECT, 1);
    feed(BTN_SELECT, 0);
    feed(BTN_MODE, 0);
    expect("watch-only does not dispatch", g_dispatch_calls == 0);

    for (int supports = 0; supports < 2; supports++) {
        tap_only(supports);
        feed(BTN_MODE, 1);
        feed(BTN_MODE, 2);
        expect("tap withheld through physical hold", drain(got, 16) == 0);
        advance(150);
        feed(BTN_MODE, 0);
        expect("one solitary action", g_taps == 1);
        advance(80);
        n = drain(got, 16);
        const emitted tap[] = {{BTN_MODE, 1}, {BTN_MODE, 0}};
        expect_seq("logical tap only when supported", got, n, tap, supports ? 2 : 0);
        expect("release reported before threshold", g_ends == 1 && g_thresholds == 0);

        tap_only(supports);
        feed(BTN_MODE, 1); feed(BTN_NORTH, 1); feed(BTN_MODE, 0); feed(BTN_NORTH, 0);
        const emitted x[] = {{BTN_NORTH, 1}, {BTN_NORTH, 0}};
        n = drain(got, 16);
        expect_seq("declined chord forwards X only", got, n, x, 2);
        expect("declined chord never taps", g_taps == 0);

        tap_only(supports);
        feed(BTN_MODE, 1); advance(900); feed(KEY_VOLUMEUP, 1);
        advance(6000); feed(KEY_VOLUMEUP, 0); feed(BTN_MODE, 0);
        expect("brightness without guide or escape", drain(got, 16) == 0 &&
               g_brightness == 1 && g_taps == 0 && g_thresholds == 0);

        tap_only(supports);
        g_claim[JW_INPUT_SHORTCUT_BUTTON_SELECT] = true;
        feed(BTN_MODE, 1); feed(BTN_SELECT, 1); feed(BTN_SELECT, 2);
        feed(BTN_SELECT, 0); feed(BTN_MODE, 0);
        expect("claimed shortcut unchanged", drain(got, 16) == 0 &&
               g_dispatch_calls == 1 && g_taps == 0);

        tap_only(supports);
        feed(BTN_MODE, 1); advance(2999);
        expect("not before threshold", g_thresholds == 0);
        advance(1); advance(2500);
        expect("one threshold per hold", g_thresholds == 1 && g_last_hold != 0);
        feed(BTN_MODE, 0); advance(100);
        expect("escape release never taps", g_ends == 1 && g_taps == 0 && drain(got, 16) == 0);
    }

    /* Physical controls already held, or first moved during Menu, disqualify
       the whole press. Returning to neutral must never re-arm it. */
    for (int phase = 0; phase < 3; phase++) {
        for (int control = 0; control < 4; control++) {
            tap_only(true);
            g_data.cal = (jw_stick_calibration){.loaded=true, .x_zero=100, .y_zero=0,
                .x_min=-1000, .x_max=1100, .y_min=-1000, .y_max=1000,
                .deadzone=10, .out_min=-32768, .out_max=32767};
            g_data.abs_value[ABS_X] = 100;
            if (phase > 0) feed(BTN_MODE, 1);
            if (phase == 2) advance(3000);
            if (control == 0) feed(BTN_EAST, 1);
            if (control == 1) feed_abs(ABS_HAT0X, 1);
            if (control == 2) feed_abs(ABS_X, 111);
            if (control == 3) feed(KEY_VOLUMEDOWN, 1);
            if (phase == 0) feed(BTN_MODE, 1);
            if (control == 0) feed(BTN_EAST, 0);
            if (control == 1) feed_abs(ABS_HAT0X, 0);
            if (control == 2) feed_abs(ABS_X, 100);
            if (control == 3) feed(KEY_VOLUMEDOWN, 0);
            advance(6000); feed(BTN_MODE, 0);
            expect("other control cancels escape and tap",
                   g_thresholds == (phase == 2 ? 1 : 0) && g_taps == 0);
            n = drain(got, 16);
            for (int i = 0; i < n; i++) expect("cancelled gesture has no guide", got[i].code != BTN_MODE);
        }
    }
    tap_only(true);
    g_data.cal.loaded = true; g_data.cal.x_zero = 100; g_data.cal.deadzone = 10;
    g_data.cal.x_min = -1000; g_data.cal.x_max = 1100;
    g_data.cal.out_min = -32768; g_data.cal.out_max = 32767;
    feed_abs(ABS_X, 109); feed(BTN_MODE, 1); feed_abs(ABS_X, 90); advance(3000);
    expect("calibrated center noise permits hold", g_thresholds == 1);
    feed_abs(ABS_X, 111);
    expect("movement after threshold ends hold", g_ends == 1);
    feed(BTN_MODE, 0);
    expect("movement after threshold suppresses tap", g_taps == 0);

    /* MLP1 at rest: calibrated deadzone 600, kernel flat 1152, Y=-1008.
       Menu must tolerate the device's neutral range even when calibration
       uses a smaller deadzone for gameplay. Exercise noise before/during
       Menu and deliberate movement just outside either larger threshold. */
    for (int axis = ABS_X; axis <= ABS_Y; axis++) {
        for (int calibration_zone = 600; calibration_zone <= 1600; calibration_zone += 1000) {
            for (int sign = -1; sign <= 1; sign += 2) {
                tap_only(true);
                g_data.cal = (jw_stick_calibration){.loaded=true,
                    .x_zero=100, .y_zero=-100, .deadzone=calibration_zone,
                    .x_min=-27000, .x_max=28000, .y_min=-25000, .y_max=26000,
                    .out_min=-32768, .out_max=32767};
                int center = axis == ABS_X ? 100 : -100;
                int neutral = calibration_zone > 1152 ? calibration_zone : 1152;
                g_data.abs_deadzone[axis] = 1152;
                feed_abs(axis, center + sign * 1008);
                feed(BTN_MODE, 1);
                feed_abs(axis, center - sign * neutral);
                advance(150); feed(BTN_MODE, 0); advance(80);
                const emitted tap[] = {{BTN_MODE, 1}, {BTN_MODE, 0}};
                n = drain(got, 16);
                expect_seq("resting stick noise still forwards Menu", got, n, tap, 2);

                feed(BTN_MODE, 1);
                feed_abs(axis, center + sign * (neutral + 1));
                feed_abs(axis, center);
                advance(3000); feed(BTN_MODE, 0); advance(80);
                expect("real movement cancels tap and escape after returning to center",
                       g_taps == 1 && g_thresholds == 0 && drain(got, 16) == 0);
            }
        }
    }

    for (int after_term = 0; after_term < 2; after_term++) {
        for (int reset = 0; reset < 3; reset++) {
            tap_only(true); feed(BTN_MODE, 1);
            if (after_term) advance(3000);
            if (reset == 0) jw_input_proxy_flush(&g_proxy);
            if (reset == 1) jw_input_proxy_set_swallow(&g_proxy, true);
            if (reset == 2) jw_input_proxy_cancel_menu(&g_proxy); /* Power */
            expect("reset ends hold", g_ends == 1);
            feed(BTN_MODE, 0); jw_input_proxy_set_swallow(&g_proxy, false);
            expect("reset retains session policy", g_proxy.menu_config.tap_only &&
                   g_proxy.menu_config.escape_enabled && g_proxy.menu_config.escape_ms == 3000);
            feed(BTN_MODE, 1); feed(BTN_NORTH, 1); feed(BTN_NORTH, 0); feed(BTN_MODE, 0);
            n = drain(got, 16);
            const emitted x[] = {{BTN_NORTH, 1}, {BTN_NORTH, 0}};
            expect_seq("chord after reset still hides Menu", got, n, x, 2);
            feed(BTN_MODE, 1); advance(10); feed(BTN_MODE, 0); advance(80);
            const emitted tap[] = {{BTN_MODE, 1}, {BTN_MODE, 0}};
            n = drain(got, 16);
            expect_seq("tap after reset still works", got, n, tap, 2);
        }
    }

    tap_only(true);
    queue_key(BTN_MODE, 1, 1000); queue_key(BTN_MODE, 0, 4500);
    jw_input_proxy_tick(&g_proxy);
    expect("queued completed hold never signals or taps", g_thresholds == 0 && g_taps == 0);
    tap_only(true); feed(BTN_MODE, 1); advance(3000);
    queue_key(BTN_MODE, 0, g_now + 100);
    advance(2500);
    expect("queued release ends old hold before daemon deadline", g_ends == 1 && g_taps == 0);
    feed(BTN_MODE, 1); advance(2999);
    expect("fresh hold has a fresh threshold", g_thresholds == 1);
    advance(1); expect("fresh hold reaches threshold", g_thresholds == 2);

    tap_only(true);
    g_data.power_grabbed = true; g_data.power_evdev_clock = true;
    int power_pipe[2];
    if (pipe2(power_pipe, O_NONBLOCK | O_CLOEXEC) != 0) exit(2);
    g_data.power_fd = power_pipe[0];
    struct input_event pev = {0};
    pev.type = EV_KEY; pev.code = KEY_POWER; pev.value = 1;
    pev.input_event_sec = 2;
    if (write(power_pipe[1], &pev, sizeof(pev)) != sizeof(pev)) exit(2);
    pev.value = 0; pev.input_event_sec = 3;
    if (write(power_pipe[1], &pev, sizeof(pev)) != sizeof(pev)) exit(2);
    queue_key(BTN_MODE, 1, 1000); queue_key(BTN_MODE, 0, 3500);
    jw_input_proxy_tick(&g_proxy);
    expect("queued Power chord cannot become a Menu tap", g_thresholds == 0 && g_taps == 0);
    /* A later, separate Power press must not retroactively consume a tap. */
    queue_key(BTN_MODE, 1, 4000); queue_key(BTN_MODE, 0, 4100);
    pev.value = 1; pev.input_event_sec = 5;
    if (write(power_pipe[1], &pev, sizeof(pev)) != sizeof(pev)) exit(2);
    pev.value = 0; pev.input_event_sec = 6;
    if (write(power_pipe[1], &pev, sizeof(pev)) != sizeof(pev)) exit(2);
    jw_input_proxy_tick(&g_proxy); advance(80);
    expect("separate queued Power does not consume earlier tap", g_taps == 1);
    n = drain(got, 16);
    const emitted before_power[] = {{BTN_MODE, 1}, {BTN_MODE, 0}};
    expect_seq("tap before separate Power reaches child", got, n, before_power, 2);
    close(power_pipe[0]); close(power_pipe[1]); g_data.power_fd = -1;

    tap_only(true); feed(BTN_MODE, 1); advance(3000);
    struct input_event lost = {.type = EV_SYN, .code = SYN_DROPPED};
    jw__handle_input(&g_proxy, &lost);
    expect("lost events end pending escape", g_ends == 1 && !g_data.menu_held);
    lost.code = SYN_REPORT; jw__handle_input(&g_proxy, &lost);
    feed(BTN_MODE, 0); advance(6000);
    expect("lost gesture cannot tap or re-arm", g_taps == 0 && g_thresholds == 1);
    expect("lost events retain session configuration", g_proxy.menu_config.tap_only);

    /* ── forwarding thread ─────────────────────────────────────────────── */

    /* Plain input goes out with the daemon thread doing nothing at all, which
       is the point of the thread: nothing below calls tick for it. */
    reset_proxy(false);
    expect("forwarding thread starts", jw_input_proxy_start(&g_proxy) == 0);
    expect("daemon polls the wake pipe once threaded",
           jw_input_proxy_poll_fd(&g_proxy) == g_data.main_wake_pipe[0]);
    queue_key(BTN_SOUTH, 1, g_now); queue_key(BTN_SOUTH, 0, g_now);
    n = drain_when(got, 16, 2);
    const emitted plain[] = {{BTN_SOUTH, 1}, {BTN_SOUTH, 0}};
    expect_seq("thread forwards without the daemon ticking", got, n, plain, 2);

    /* A Menu tap's callback runs on the daemon thread, from tick, and the
       thread waits for the answer before emitting the tap it was refused. */
    g_tap_on_main = false;
    queue_key(BTN_MODE, 1, g_now); queue_key(BTN_MODE, 0, g_now);
    expect("thread wakes the daemon for the callback", serve_one_call());
    expect("menu tap asked once", g_taps == 1);
    expect("menu tap ran on the daemon thread", g_tap_on_main);
    n = drain_when(got, 16, 1);
    const emitted tap_down[] = {{BTN_MODE, 1}};
    expect_seq("declined tap reaches the pad", got, n, tap_down, 1);
    {
        /* Past the 80 ms hold. Under the lock, which also kicks the thread. */
        jw_mlp1_input_proxy_data *data = jw__api_lock(&g_proxy);
        g_now += 100;
        jw__api_unlock(data);
    }
    n = drain_when(got, 16, 1);
    const emitted tap_up[] = {{BTN_MODE, 0}};
    expect_seq("thread releases the tap on its own timer", got, n, tap_up, 1);

    /* Public calls from the daemon thread still reach the pad. */
    queue_key(BTN_EAST, 1, g_now);
    n = drain_when(got, 16, 1);
    jw_input_proxy_release_buttons(&g_proxy);
    n = drain_when(got, 16, 1);
    expect("release_buttons lets go of a held button from the daemon thread",
           n >= 1 && got[0].code == BTN_EAST && got[0].value == 0);
    jw__io_thread_stop(&g_data);
    expect("thread stops", !g_data.io_thread_running);

    /* Stopping while the thread waits on a callback answers it instead of
       hanging: shutdown can run inside that very callback. */
    reset_proxy(false);
    jw_input_proxy_start(&g_proxy);
    queue_key(BTN_MODE, 1, g_now); queue_key(BTN_MODE, 0, g_now);
    {
        struct pollfd pfd = { jw_input_proxy_poll_fd(&g_proxy), POLLIN, 0 };
        expect("thread asks for the callback", poll(&pfd, 1, 2000) == 1);
    }
    jw__io_thread_stop(&g_data);
    expect("stop answers a waiting callback", g_taps == 0 && !g_data.io_thread_running);

    /* Reset while a tap or shortcut is waiting for the daemon. A reset must
       cancel the callback AND its input continuation: returning "declined"
       alone would forward a press after its release was discarded. Also
       exercise a reset inside the callback, after tick has already taken it. */
    for (int reset = 0; reset < 4; reset++) {
        for (int shortcut = 0; shortcut < 2; shortcut++) {
            reset_proxy(false);
            int pending_power[2];
            if (pipe2(pending_power, O_NONBLOCK | O_CLOEXEC) != 0) exit(2);
            g_data.power_fd = pending_power[0];
            g_data.power_evdev_clock = true;
            /* Prefetched by process() but ordered after the Menu gesture.
               Flushing the fd alone cannot discard this local copy. */
            struct input_event power = {.type = EV_KEY, .code = KEY_POWER, .value = 1};
            power.input_event_sec = (g_now + 1000) / 1000;
            if (write(pending_power[1], &power, sizeof(power)) != sizeof(power)) exit(2);
            queue_key(BTN_MODE, 1, g_now);
            if (shortcut) {
                queue_key(BTN_SOUTH, 1, g_now);
                queue_key(BTN_SOUTH, 0, g_now);
            }
            queue_key(BTN_MODE, 0, g_now);
            expect("reset test starts thread", jw_input_proxy_start(&g_proxy) == 0);
            struct pollfd pfd = { jw_input_proxy_poll_fd(&g_proxy), POLLIN, 0 };
            expect("reset test reaches callback wait", poll(&pfd, 1, 2000) == 1);
            if (reset == 0) jw_input_proxy_flush(&g_proxy);
            if (reset == 1) jw_input_proxy_configure_menu(&g_proxy,
                (jw_input_menu_config){.tap_only = true});
            if (reset == 2) jw_input_proxy_set_swallow(&g_proxy, true);
            if (reset == 3) g_flush_in_callback = true;
            jw_input_proxy_tick(&g_proxy);

            /* A fresh marker proves the worker resumed beyond the cancelled
               event. While swallowed, observe physical state under the lock
               instead of waiting for output that must never be emitted. */
            queue_key(BTN_EAST, 1, g_now);
            bool reached = false;
            for (int waited = 0; waited < 2000 && !reached; waited += 5) {
                jw_mlp1_input_proxy_data *data = jw__api_lock(&g_proxy);
                reached = jw__bit_is_set(data->physical_keys, BTN_EAST);
                jw__api_unlock(data);
                if (!reached) usleep(5000);
            }
            expect("reset releases the callback waiter", reached);
            jw__io_thread_stop(&g_data);
            expect("reset cancels unstarted callbacks",
                   g_taps + g_dispatch_calls == (reset == 3 ? 1 : 0));
            expect("reset drops prefetched power input", g_data.power_edge_count == 0);
            n = drain(got, 16);
            const emitted marker[] = {{BTN_EAST, 1}};
            expect_seq("reset cannot replay a tap or leave a button held", got, n,
                       marker, reset == 2 ? 0 : 1);
            close(pending_power[0]); close(pending_power[1]);
            g_data.power_fd = -1;
        }
    }

    /* Teardown clears configuration even if initialization failed without a backend. */
    jw_input_proxy empty = {.menu_config = {.tap_only = true, .escape_enabled = true}};
    jw_input_proxy_shutdown(&empty);
    expect("teardown clears session configuration", !empty.menu_config.tap_only &&
           !empty.menu_config.escape_enabled);

    if (g_failures) {
        fprintf(stderr, "input-proxy-chord-test: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("input proxy chord tests passed");
    return 0;
}
