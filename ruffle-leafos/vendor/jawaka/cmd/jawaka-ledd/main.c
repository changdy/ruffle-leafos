/* jawaka-ledd — Miniloong Pocket 1 LED effects engine.
 *
 * The stock loong_light daemon owns the AW20036 ring and re-asserts its config
 * on a refresh loop, so to run custom per-LED animations we FREEZE it
 * (SIGSTOP), drive /sys/.../mmrgball directly at a per-effect frame rate, and THAW
 * it (SIGCONT) on exit. On SIGCONT the daemon resumes and re-applies its cfg,
 * so the ring returns to the cooperative (Path A) state automatically.
 *
 * Crash-safety: the freeze is a hostage situation — if this process dies while
 * loong_light is stopped, the ring would be stuck. So we thaw from atexit() AND
 * from handlers for every fatal/term signal. Killing this process is therefore
 * the clean "stop the effect" operation: death itself thaws the daemon.
 * jawakad arms SIGTERM as this process's parent-death signal, so a daemon that
 * dies without stopping the effect stops it all the same.
 *
 * Usage: jawaka-ledd <effect> <r> <g> <b> <brightness 0-10> <speed 0-10>
 *   effects: off static breath rainbow comet sweep fountain hiccup battery
 *   (battery ignores the color and speed; see battery.h)
 *   off, static, and battery write only when the frame changes (steady.h).
 *
 * Standalone + MLP1-specific (like device_mlp1.c): pure libc + sysfs, no deps.
 */
#include <dirent.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "battery.h"
#include "steady.h"

/* CLOCK_BOOTTIME counts time spent suspended; Battery Level sleeps on it so a
   resume ends the sleep at once. The host build (ledd-spawn-test, macOS
   included) never suspends, so CLOCK_MONOTONIC stands in there, as in
   jawakad. On Linux a missing CLOCK_BOOTTIME is a build error, not a silent
   fallback. */
#if !defined(__linux__) && !defined(CLOCK_BOOTTIME)
#define CLOCK_BOOTTIME CLOCK_MONOTONIC
#endif

#define JW_LED_COUNT      8
/* Overridable so a host run can point the helper at a fixture. */
#ifndef JW_LED_MMRGBALL
#define JW_LED_MMRGBALL   "/sys/class/leds/aw20036_led/mmrgball"
#endif
#define JW_LED_DAEMON     "loong_light"

/* Sub-LED resolution: the moving dot's position is tracked in 1/256 of an LED
   so it can sit *between* physical LEDs, splitting its glow across the two it
   straddles. That sub-pixel positioning at a high frame rate is what makes the
   motion glide instead of hopping LED-to-LED. */
#define JW_LED_SUB   256
#define JW_LED_RING  (JW_LED_COUNT * JW_LED_SUB)
/* The effects are written in ticks of this length. An effect with a slower
   frame advances several ticks per frame, so it runs at the same speed. */
#define JW_LED_TICK_MS 25   /* ~40fps for smooth motion */

/* Physical ring geometry (verified): index 0 ≈ 4 o'clock, winding CCW ~45°.
 * For the Fountain effect we fill from the bottom up each side to the top.
 * Bottom sits between LED7 (~5:30) and LED6 (~7:00). */
static const int kRightChain[4] = { 7, 0, 1, 2 };  /* bottom → top, right side */
static const int kLeftChain[4]  = { 6, 5, 4, 3 };  /* bottom → top, left side  */

static volatile sig_atomic_t g_running = 1;
static pid_t g_daemon_pid = -1;

static pid_t jw__find_daemon(const char *name) {
    DIR *proc = opendir("/proc");
    if (!proc) return -1;
    struct dirent *entry;
    char path[300];
    char comm[128];
    pid_t found = -1;
    while ((entry = readdir(proc)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        if (fgets(comm, sizeof(comm), fp)) {
            comm[strcspn(comm, "\n")] = '\0';
            if (strcmp(comm, name) == 0) { found = (pid_t)atoi(entry->d_name); }
        }
        fclose(fp);
        if (found > 0) break;
    }
    closedir(proc);
    return found;
}

static void jw__thaw(void) {
    if (g_daemon_pid > 0) kill(g_daemon_pid, SIGCONT);
}

static void jw__on_term(int sig) {
    (void)sig;
    g_running = 0;   /* loop exits, atexit thaws */
}

static void jw__on_fatal(int sig) {
    jw__thaw();
    signal(sig, SIG_DFL);
    raise(sig);      /* re-raise so the real fault is still reported */
}

static void jw__write_frame(const uint32_t argb[JW_LED_COUNT]) {
    FILE *fp = fopen(JW_LED_MMRGBALL, "w");
    if (!fp) return;
    for (int i = 0; i < JW_LED_COUNT; i++)
        fprintf(fp, "%s0x%08x", i ? " " : "", argb[i]);
    fclose(fp);
}

static int jw__clamp8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static uint32_t jw__argb(int a, int r, int g, int b) {
    return ((uint32_t)jw__clamp8(a) << 24) | ((uint32_t)jw__clamp8(r) << 16) |
           ((uint32_t)jw__clamp8(g) << 8) | (uint32_t)jw__clamp8(b);
}

/* ─── Effects: fill argb[] for frame counter `t`. ──────────────────────── */

typedef struct {
    int r, g, b;        /* chosen color */
    int alpha_max;      /* 0..255 from brightness */
    int tail_sub;       /* comet/sweep tail length, in sub-LED units (0 = bare dot) */
} jw_effect_params;

static void jw__fx_fill(uint32_t out[JW_LED_COUNT], int a, int r, int g, int b) {
    uint32_t c = jw__argb(a, r, g, b);
    for (int i = 0; i < JW_LED_COUNT; i++) out[i] = c;
}

static uint32_t jw__wheel(int pos, int alpha) {
    pos &= 255;
    if (pos < 85) {
        return jw__argb(alpha, 255 - pos * 3, pos * 3, 0);
    }
    if (pos < 170) {
        pos -= 85;
        return jw__argb(alpha, 0, 255 - pos * 3, pos * 3);
    }
    pos -= 170;
    return jw__argb(alpha, pos * 3, 0, 255 - pos * 3);
}

static void jw__fx_breath(uint32_t out[JW_LED_COUNT], int t,
                          const jw_effect_params *p, int speed) {
    int period = 96 - speed * 7;
    if (period < 24) period = 24;
    int phase = t % period;
    int half = period / 2;
    int pulse;
    if (phase < half) {
        pulse = half > 0 ? phase * 255 / half : 255;
    } else {
        int down = period - phase;
        int span = period - half;
        pulse = span > 0 ? down * 255 / span : 0;
    }
    jw__fx_fill(out, p->alpha_max * pulse / 255, p->r, p->g, p->b);
}

static void jw__fx_rainbow(uint32_t out[JW_LED_COUNT], int t,
                           const jw_effect_params *p, int speed) {
    int advance = 1 + speed * 2;
    int offset = (t * advance) & 255;
    for (int i = 0; i < JW_LED_COUNT; i++) {
        out[i] = jw__wheel(offset + i * 256 / JW_LED_COUNT, p->alpha_max);
    }
}

/* Comet (bg white) / Sweep (bg off): a smoothly gliding color dot with an
   optional fading tail. `head` is a sub-LED position so the dot's glow splits
   across the two LEDs it sits between — that interpolation is the smoothness.
   Per LED we compute a 0..256 weight, then render it differently per background:
     - white bg (comet): blend color OVER white at FULL alpha, so edges are
       pale-color (still bright) instead of dim — no dark notches stepping along.
     - dark bg (sweep): fade the color's alpha, off where weight is 0. */
static void jw__fx_comet(uint32_t out[JW_LED_COUNT], long head,
                         const jw_effect_params *p, int bg_white) {
    const long head_spread = JW_LED_SUB;   /* symmetric glow radius: ±1 LED */
    for (int i = 0; i < JW_LED_COUNT; i++) {
        long center = (long)i * JW_LED_SUB;
        /* Symmetric (anti-aliased) glow at the head — glides between LEDs. */
        long dsym = (center - head + JW_LED_RING) % JW_LED_RING;
        if (dsym > JW_LED_RING - dsym) dsym = JW_LED_RING - dsym;
        long head_w = (dsym < head_spread) ? 256 * (head_spread - dsym) / head_spread : 0;
        /* One-sided fading tail behind the head (in travel direction). */
        long lag = (head - center + JW_LED_RING) % JW_LED_RING;
        long tail_w = (p->tail_sub > 0 && lag < p->tail_sub)
                    ? 256 * (p->tail_sub - lag) / p->tail_sub : 0;
        long w = head_w > tail_w ? head_w : tail_w;   /* 0..256 */

        if (bg_white) {
            int r = 255 + (int)((p->r - 255) * w / 256);
            int g = 255 + (int)((p->g - 255) * w / 256);
            int b = 255 + (int)((p->b - 255) * w / 256);
            out[i] = jw__argb(p->alpha_max, r, g, b);
        } else {
            out[i] = (w > 0) ? jw__argb((int)(p->alpha_max * w / 256), p->r, p->g, p->b)
                             : jw__argb(0, 0, 0, 0);
        }
    }
}

static int jw__fountain_hold(int speed) {
    int hold = 3 + (10 - speed) * 4;   /* ticks held per fill level */
    return hold < 1 ? 1 : hold;
}

/* Fountain: fill from the bottom up both sides to the top, then drain back. */
static void jw__fx_fountain(uint32_t out[JW_LED_COUNT], int t,
                            const jw_effect_params *p, int speed) {
    for (int i = 0; i < JW_LED_COUNT; i++) out[i] = jw__argb(0, 0, 0, 0);
    int hold = jw__fountain_hold(speed);
    static const int kLevels[8] = { 0, 1, 2, 3, 4, 3, 2, 1 };
    int level = kLevels[(t / hold) % 8];
    uint32_t lit = jw__argb(p->alpha_max, p->r, p->g, p->b);
    for (int n = 0; n < level; n++) {
        out[kRightChain[n]] = lit;
        out[kLeftChain[n]]  = lit;
    }
}

/* Hiccup: a sharp flash to full, then a smooth dim to off, brief pause, repeat.
   `step` is ticks per frame. Each cycle restarts on a frame boundary so it
   always opens on the flash; with several ticks per frame a plain t % cycle
   lands on it only every few cycles and the rhythm stutters. */
static void jw__fx_hiccup(uint32_t out[JW_LED_COUNT], int t, int step,
                          const jw_effect_params *p, int speed) {
    int decay = 6 + (10 - speed);      /* ticks to fade from full to off */
    int pause = 4 + (10 - speed) / 2;  /* dark ticks before the next flash */
    int cycle_frames = (decay + pause + step - 1) / step;
    int phase = ((t / step) % cycle_frames) * step;
    int a = (phase < decay) ? p->alpha_max * (decay - phase) / decay : 0;
    uint32_t c = jw__argb(a, p->r, p->g, p->b);
    for (int i = 0; i < JW_LED_COUNT; i++) out[i] = c;
}

/* Frame interval per effect. Every frame is a sysfs write that the aw20036
   driver turns into ~28 i2c transactions on a bus it shares with the codec and
   the backlight, so only the effects that need it get the fast rate.
     - comet, sweep, rainbow: one tick; the sub-LED glide and the hue roll
       are what the frame rate buys.
     - breath, hiccup, fountain: 100 ms; a uniform pulse or a fill level
       reads the same at 10 fps. Fountain drops to its hold time at top speed
       so no fill level is skipped.
   off, static, and battery do not use it: their frame changes rarely or
   never, and jw__run_steady writes it only when it does. */
static long jw__effect_interval_ms(const char *effect, int speed) {
    if (strcmp(effect, "breath") == 0 || strcmp(effect, "hiccup") == 0) {
        return 100;
    }
    if (strcmp(effect, "fountain") == 0) {
        long hold_ms = (long)jw__fountain_hold(speed) * JW_LED_TICK_MS;
        return hold_ms < 100 ? hold_ms : 100;
    }
    return JW_LED_TICK_MS;
}

static long long jw__clock_ms(clockid_t clock) {
    struct timespec ts;
    clock_gettime(clock, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Relative sleep measured on CLOCK_BOOTTIME: after a suspend longer than the
   time left it returns as the device resumes. It is not an alarm clock, so it
   never wakes the device. A signal ends it early (EINTR) so SIGTERM still
   stops the helper at once. */
static void jw__sleep_boottime_ms(long long ms) {
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
#if defined(__linux__)
    clock_nanosleep(CLOCK_BOOTTIME, 0, &ts, NULL);
#else
    nanosleep(&ts, NULL);
#endif
}

/* off, static, and Battery Level: frames that change rarely or never. Each
   pass (5 s, or 1 s while the low-battery flash runs) works out the frame and
   writes it only when steady.h says so. Battery read errors go dark rather
   than ending the helper: jawakad would only start it again. */
static void jw__run_steady(const char *effect, const jw_effect_params *p) {
    bool battery = strcmp(effect, "battery") == 0;
    bool off = strcmp(effect, "off") == 0;
    uint32_t frame[JW_LED_COUNT];
    uint32_t last = 0;
    bool have_written = false;
    bool lit = true;
    long long last_write_ms = 0;
    long long prev_gap_ms = jw__clock_ms(CLOCK_BOOTTIME) - jw__clock_ms(CLOCK_MONOTONIC);
    long long sampled_ms = jw__clock_ms(CLOCK_BOOTTIME);
    int percent = battery ? jw_ledd_battery_read(JW_LEDD_CAPACITY_PATH) : -1;

    while (g_running) {
        long long now_ms = jw__clock_ms(CLOCK_BOOTTIME);
        /* BOOTTIME pulls ahead of MONOTONIC only while suspended. */
        long long gap_ms = now_ms - jw__clock_ms(CLOCK_MONOTONIC);
        bool resumed = gap_ms - prev_gap_ms > 1000;
        prev_gap_ms = gap_ms;
        if (battery && (resumed || now_ms - sampled_ms >= JW_LEDD_STEADY_PASS_MS)) {
            percent = jw_ledd_battery_read(JW_LEDD_CAPACITY_PATH);
            sampled_ms = now_ms;
        }

        bool flashing = battery && jw_ledd_battery_flashing(percent, p->alpha_max);
        if (!flashing) lit = true;   /* the low band always opens lit */
        uint32_t color = battery ? jw_ledd_battery_color(percent, p->alpha_max, lit)
                       : off     ? jw__argb(0, 0, 0, 0)
                                 : jw__argb(p->alpha_max, p->r, p->g, p->b);
        if (jw_ledd_should_write(have_written, last, color, resumed,
                                 now_ms - last_write_ms)) {
            for (int i = 0; i < JW_LED_COUNT; i++) frame[i] = color;
            jw__write_frame(frame);
            last = color;
            have_written = true;
            last_write_ms = now_ms;
        }
        if (flashing) lit = !lit;
        jw__sleep_boottime_ms(flashing ? JW_LEDD_BATTERY_FLASH_MS
                                       : JW_LEDD_STEADY_PASS_MS);
    }
}

int main(int argc, char **argv) {
    if (argc < 7) {
        fprintf(stderr, "usage: %s <effect> <r> <g> <b> <brightness 0-10> <speed 0-10>\n", argv[0]);
        return 2;
    }
    const char *effect = argv[1];
    jw_effect_params p;
    p.r = atoi(argv[2]);
    p.g = atoi(argv[3]);
    p.b = atoi(argv[4]);
    int brightness = atoi(argv[5]);
    int speed = atoi(argv[6]);
    if (brightness < 0) brightness = 0;
    if (brightness > 10) brightness = 10;
    if (speed < 0) speed = 0;
    if (speed > 10) speed = 10;
    p.alpha_max = brightness * 255 / 10;
    /* Sweep is a bare gliding dot (color-on-dark, darkest); comet keeps a
       fading tail on its white background. */
    p.tail_sub = (strcmp(effect, "sweep") == 0) ? 0 : 3 * JW_LED_SUB;

    /* Speed drives how far each effect moves per tick (sub-LED units for the
       dot), not the frame interval. */
    long interval_ms = jw__effect_interval_ms(effect, speed);
    int step = (int)(interval_ms / JW_LED_TICK_MS);   /* ticks per frame */
    if (step < 1) step = 1;
    long advance = 6 + (long)speed * 6;   /* sub-LED units per tick */

    /* Handlers before the freeze: jawakad sends SIGTERM as soon as it stops
       an effect or dies, and a default-action SIGTERM after the SIGSTOP would
       leave the daemon frozen. */
    atexit(jw__thaw);
    signal(SIGTERM, jw__on_term);
    signal(SIGINT,  jw__on_term);
    signal(SIGHUP,  jw__on_term);
    signal(SIGSEGV, jw__on_fatal);
    signal(SIGABRT, jw__on_fatal);
    signal(SIGFPE,  jw__on_fatal);
    signal(SIGBUS,  jw__on_fatal);

    g_daemon_pid = jw__find_daemon(JW_LED_DAEMON);
    if (g_daemon_pid > 0) kill(g_daemon_pid, SIGSTOP);

    if (strcmp(effect, "off") == 0 || strcmp(effect, "static") == 0 ||
        strcmp(effect, "battery") == 0) {
        jw__run_steady(effect, &p);
        jw__thaw();
        return 0;
    }

    struct timespec sleep_for = {
        .tv_sec  = interval_ms / 1000,
        .tv_nsec = (interval_ms % 1000) * 1000000L,
    };

    uint32_t frame[JW_LED_COUNT];
    int t = 0;
    long head = 0;
    while (g_running) {
        if (strcmp(effect, "breath") == 0)        jw__fx_breath(frame, t, &p, speed);
        else if (strcmp(effect, "rainbow") == 0)  jw__fx_rainbow(frame, t, &p, speed);
        else if (strcmp(effect, "comet") == 0)    jw__fx_comet(frame, head, &p, 1);
        else if (strcmp(effect, "sweep") == 0)    jw__fx_comet(frame, head, &p, 0);
        else if (strcmp(effect, "fountain") == 0) jw__fx_fountain(frame, t, &p, speed);
        else if (strcmp(effect, "hiccup") == 0)   jw__fx_hiccup(frame, t, step, &p, speed);
        else { fprintf(stderr, "unknown effect: %s\n", effect); return 2; }

        jw__write_frame(frame);
        nanosleep(&sleep_for, NULL);
        head = (head + advance * step) % JW_LED_RING;
        t += step;
    }
    jw__thaw();
    return 0;
}
