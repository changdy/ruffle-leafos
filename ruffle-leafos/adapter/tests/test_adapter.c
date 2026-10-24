/* Link the actual preload implementation to a deterministic fake SDL2/libc.
 * These tests verify interception and passthrough, not the ARM driver itself. */
#include "fake_platform.h"
#include "frame_metrics.h"
#include <stdint.h>
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
typedef union { uint32_t type; unsigned char bytes[56]; uint64_t align; } Event;
int SDL_PollEvent(Event *);
void SDL_GL_SwapWindow(void *);
void *SDL_GL_CreateContext(void *);
void probe_test_init(void);
static int mode, pending, poll_calls, swap_calls, context_calls, exit_calls, timing_logs;
static Event source;
static uint64_t clock_us = 100000;
void *memset(void *p, int c, size_t n) { unsigned char *b = p; while (n--) *b++ = (unsigned char)c; return p; }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) { ++a; ++b; } return (unsigned char)*a - (unsigned char)*b; }
char *strstr(const char *text, const char *needle) {
    for (; *text; ++text) { const char *a = text, *b = needle; while (*b && *a == *b) { ++a; ++b; } if (!*b) return (char *)text; }
    return !*needle ? (char *)text : 0;
}
int clock_gettime(int clock, struct timespec *t) {
    (void)clock; t->tv_sec = (long)(clock_us / 1000000); t->tv_nsec = (long)(clock_us % 1000000) * 1000; return 0;
}
char *getenv(const char *name) {
    if (strcmp(name, "RUFFLE_PROBE_ANY_KEY_EXIT") == 0) return mode ? "1" : "0";
    return "1";
}
int dprintf(int fd, const char *format, ...) {
    (void)fd;
    if (strstr(format, "present_samples=")) ++timing_logs;
    return 0;
}
void _exit(int code) { (void)code; ++exit_calls; }
static int mock_poll(Event *e) {
    ++poll_calls;
    if (!pending) return 0;
    if (e) { *e = source; pending = 0; }
    return 1;
}
static void mock_swap(void *w) { (void)w; ++swap_calls; clock_us += 1000; }
static void *mock_context(void *w) { ++context_calls; return w; }
static const unsigned char *mock_string(unsigned int key) { (void)key; return (const unsigned char *)"llvmpipe test renderer"; }
static void *mock_proc(const char *name) { return strcmp(name, "glGetString") == 0 ? (void *)mock_string : 0; }
void *dlsym(void *library, const char *name) {
    (void)library;
    if (!strcmp(name, "SDL_PollEvent")) return (void *)mock_poll;
    if (!strcmp(name, "SDL_GL_SwapWindow")) return (void *)mock_swap;
    if (!strcmp(name, "SDL_GL_CreateContext")) return (void *)mock_context;
    if (!strcmp(name, "SDL_GL_GetProcAddress")) return (void *)mock_proc;
    return 0;
}
static void event(uint32_t type) { memset(&source, 0, sizeof(source)); source.type = type; source.bytes[12] = 1; pending = 1; }
int adapter_tests(int probe) {
    mode = probe;
    probe_test_init();
    CHECK(SDL_GL_CreateContext((void *)123) == (void *)123);
    CHECK(context_calls == 1);
    SDL_GL_SwapWindow(0);
    CHECK(swap_calls == 1);
    clock_us += 600000;
    Event out;
    if (!probe) {
        const uint32_t types[] = {0x300, 0x301, 0x603, 0x604, 0x600, 0x602, 0x651, 0x652, 0x400, 0x401, 0x402, 0x403, 0x200, 0x100};
        for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
            event(types[i]);
            for (unsigned j = 16; j < 56; ++j) source.bytes[j] = (unsigned char)(j * 7);
            CHECK(SDL_PollEvent(0) == 1 && pending == 1);
            CHECK(SDL_PollEvent(&out) == 1 && pending == 0);
            for (unsigned j = 0; j < 56; ++j) CHECK(out.bytes[j] == source.bytes[j]);
            CHECK(SDL_PollEvent(&out) == 0);
        }
        CHECK(poll_calls == 42);
    } else {
        event(0x603);
        CHECK(SDL_PollEvent(&out) == 1);
        CHECK(out.type == 0x100); /* Probe converts a fresh button into quit. */
    }
    for (unsigned i = 0; i < 299; ++i) { clock_us += 16000; SDL_GL_SwapWindow(0); }
    CHECK(swap_calls == 300);
    CHECK(timing_logs == 1);
    CHECK(exit_calls == 0); /* Native mode never runs the probe's _exit path. */
    return 0;
}
int metric_tests(void) {
    struct frame_metric m; metric_reset(&m);
    CHECK(metric_percentile_bound(&m, 95) == 0);
    for (unsigned i = 0; i < 95; ++i) metric_add(&m, 16000);
    for (unsigned i = 0; i < 4; ++i) metric_add(&m, 33000);
    metric_add(&m, 120000);
    CHECK(m.count == 100 && m.min == 16000 && m.max == 120000);
    CHECK(metric_percentile_bound(&m, 50) == 16384);
    CHECK(metric_percentile_bound(&m, 95) == 16384);
    CHECK(metric_percentile_bound(&m, 99) == 65536);
    CHECK(metric_percentile_bound(&m, 100) == 131072);
    metric_reset(&m); metric_add(&m, 0); metric_add(&m, 1ULL << 40);
    CHECK(metric_percentile_bound(&m, 100) == (1ULL << 40));
    CHECK(m.min == 0);
    metric_reset(&m); CHECK(m.count == 0 && m.sum == 0);
    return 0;
}
