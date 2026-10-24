#define _GNU_SOURCE 1
#define _POSIX_C_SOURCE 200809L
#include "probe_input.h"
#include "frame_metrics.h"
#ifdef PROBE_WASM_TEST
#include "fake_platform.h"
#else
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#endif

typedef union { uint32_t type; unsigned char bytes[56]; uint64_t alignment; } Event;
_Static_assert(sizeof(Event) == 56, "SDL2 event ABI mismatch");
typedef int (*PollFn)(Event *);
typedef void (*SwapFn)(void *);
typedef void *(*ContextFn)(void *);
typedef void *(*ProcFn)(const char *);
typedef const unsigned char *(*StringFn)(unsigned int);
static PollFn original_poll;
static SwapFn original_swap;
static ContextFn original_context;
static struct probe_input input;
static int enabled, diagnostics, timing, quit_requested, ready_logged, gl_logged;
static uint64_t quit_at;
static uint64_t last_swap_us;
static struct frame_metric intervals, swaps;

static uint64_t now_us(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000000 + (uint64_t)t.tv_nsec / 1000;
}

static uint64_t now_ms(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return (uint64_t)t.tv_sec * 1000 + (uint64_t)t.tv_nsec / 1000000;
}

static void *required_symbol(const char *name) {
    void *symbol = dlsym(RTLD_NEXT, name);
    if (!symbol) {
        dprintf(2, "ruffle-probe: missing SDL2 symbol: %s\n", name);
        _exit(125);
    }
    return symbol;
}

__attribute__((constructor)) static void initialize(void) {
    const char *mode = getenv("RUFFLE_PROBE_ANY_KEY_EXIT");
    enabled = mode && strcmp(mode, "1") == 0;
    mode = getenv("RUFFLE_PROBE_DIAGNOSTICS");
    diagnostics = enabled || (mode && strcmp(mode, "1") == 0);
    mode = getenv("RUFFLE_PROBE_TIMING");
    timing = diagnostics && mode && strcmp(mode, "1") == 0;
    probe_input_init(&input, 500);
    if (enabled) dprintf(2, "ruffle-probe: exit layer loaded; waiting for first GL swap\n");
    else if (diagnostics) dprintf(2, "ruffle-probe: native input passthrough; any-key exit disabled; Menu routing follows the pak contract\n");
    if (timing) dprintf(2, "ruffle-probe: timing enabled; interval/swap wall time in us, histogram quantile upper bounds; not GPU execution time\n");
}

#ifdef PROBE_WASM_TEST
void probe_test_init(void) { initialize(); }
#endif

static void mark_ready(uint64_t now) {
    if (!ready_logged && probe_input_ready(&input, now)) {
        ready_logged = 1;
        dprintf(2, "ruffle-probe: ready; release launch key, then press any game button to exit\n");
    }
}

static int send_quit(Event *event, uint64_t now) {
    if (!quit_requested) {
        quit_requested = 1; quit_at = now;
        dprintf(2, "ruffle-probe: fresh button press; requesting SDL_QUIT\n");
    } else if (now >= quit_at && now - quit_at > 2000) {
        /* Smoke build has no progress-saving contract. Do not hang on a quit loop. */
        dprintf(2, "ruffle-probe: quit was not handled within 2s; ending probe\n");
        _exit(0);
    }
    if (event) {
        memset(event, 0, sizeof(*event));
        event->type = 0x100; /* SDL_QUIT */
    }
    return 1;
}

int SDL_PollEvent(Event *event) {
    if (!original_poll) original_poll = (PollFn)required_symbol("SDL_PollEvent");
    if (!enabled) return original_poll(event);
    uint64_t now = now_ms();
    mark_ready(now);
    if (quit_requested) return send_quit(event, now);
    if (!event) return original_poll(event);
    for (unsigned consumed = 0; consumed < 4096 && original_poll(event); ++consumed) {
        now = now_ms();
        enum probe_action action = probe_input_event(&input, event->bytes, now);
        if (action == PROBE_QUIT) return send_quit(event, now);
        if (action == PROBE_PASS) return 1;
    }
    return 0;
}

void *SDL_GL_CreateContext(void *window) {
    if (!original_context)
        original_context = (ContextFn)required_symbol("SDL_GL_CreateContext");
    void *context = original_context(window);
    if (diagnostics) dprintf(2, "ruffle-probe: GL context %s\n", context ? "created" : "FAILED");
    return context;
}

void SDL_GL_SwapWindow(void *window) {
    if (!original_swap) original_swap = (SwapFn)required_symbol("SDL_GL_SwapWindow");
    uint64_t before = timing ? now_us() : 0;
    original_swap(window);
    uint64_t after = timing ? now_us() : 0;
    if (!diagnostics) return;
    if (!gl_logged) {
        ProcFn proc = (ProcFn)required_symbol("SDL_GL_GetProcAddress");
        StringFn gl_string = (StringFn)proc("glGetString");
        const unsigned int keys[] = {0x1F00, 0x1F01, 0x1F02, 0x8B8C};
        const char *names[] = {"GL_VENDOR", "GL_RENDERER", "GL_VERSION", "GLSL_VERSION"};
        if (gl_string) for (unsigned i = 0; i < 4; ++i) {
            const unsigned char *value = gl_string(keys[i]);
            dprintf(2, "ruffle-probe: %s=%s\n", names[i], value ? (const char *)value : "unavailable");
            if (i == 1 && value) {
                const char *renderer = (const char *)value;
                int software = strstr(renderer, "llvmpipe") || strstr(renderer, "softpipe") ||
                    strstr(renderer, "lavapipe") || strstr(renderer, "SwiftShader") ||
                    strstr(renderer, "Software Rasterizer") || strstr(renderer, "software rasterizer");
                dprintf(2, "ruffle-probe: software_renderer_hint=%s (classification hint, not hardware verification)\n",
                        software ? "yes" : "no-known-software-name");
            }
        }
        gl_logged = 1;
        if (enabled) {
            probe_input_presented(&input, now_ms());
            dprintf(2, "ruffle-probe: first GL swap returned; key exit arms after 500ms\n");
        } else dprintf(2, "ruffle-probe: first GL swap returned; native controls active\n");
    }
    if (enabled) mark_ready(now_ms());
    if (timing && before && after >= before) {
        metric_add(&swaps, after - before);
        if (last_swap_us && after >= last_swap_us) metric_add(&intervals, after - last_swap_us);
        last_swap_us = after;
        if (swaps.count >= 300) {
            dprintf(2, "ruffle-probe: present_samples=%u interval_mean_us=%llu interval_min_us=%llu interval_max_us=%llu p50_upper_us=%llu p95_upper_us=%llu p99_upper_us=%llu swap_mean_us=%llu swap_max_us=%llu\n",
                intervals.count,
                (unsigned long long)(intervals.count ? intervals.sum / intervals.count : 0),
                (unsigned long long)intervals.min, (unsigned long long)intervals.max,
                (unsigned long long)metric_percentile_bound(&intervals, 50),
                (unsigned long long)metric_percentile_bound(&intervals, 95),
                (unsigned long long)metric_percentile_bound(&intervals, 99),
                (unsigned long long)(swaps.sum / swaps.count), (unsigned long long)swaps.max);
            metric_reset(&intervals); metric_reset(&swaps);
        }
    }
}
