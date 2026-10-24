#include "internal/platform/input_proxy.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Host smokes have no power key. JAWAKA_MOCK_POWER_EDGES names a file or FIFO
   the mock reads on every take: one edge per line, "down <stamp>" or
   "up <stamp>", where <stamp> is a CLOCK_MONOTONIC millisecond timestamp or
   "+N" / "-N" milliseconds relative to the moment the line is read. Edges keep
   file order and each is handed out no earlier than its timestamp, so a shell
   can script a hold ("down +0", then "up +2600" delivered 2.6 s later) or a
   press and release that both queued behind a stalled tick ("down -2600",
   "up -100", both already due). The production MLP1 proxy is untouched. */
#define JW_MOCK_POWER_EDGE_QUEUE 32
#define JW_MOCK_POWER_EDGES_ENV "JAWAKA_MOCK_POWER_EDGES"

/* JAWAKA_MOCK_HELD_BUTTONS stands in for the EVIOCGKEY sample the MLP1 proxy
   takes when it opens the pad: a comma-separated list of shortcut button
   names ("b", "start", ...) that read as held from start-up until something
   takes them. Read once at init. */
#define JW_MOCK_HELD_BUTTONS_ENV "JAWAKA_MOCK_HELD_BUTTONS"
static bool s_mock_held[JW_INPUT_SHORTCUT_BUTTON_COUNT];

static void jw__mock_held_load(void) {
    memset(s_mock_held, 0, sizeof(s_mock_held));
    const char *list = getenv(JW_MOCK_HELD_BUTTONS_ENV);
    if (!list || !list[0]) {
        return;
    }
    char copy[128];
    snprintf(copy, sizeof(copy), "%s", list);
    char *save = NULL;
    for (char *name = strtok_r(copy, ",", &save); name; name = strtok_r(NULL, ",", &save)) {
        jw_input_shortcut_button button = JW_INPUT_SHORTCUT_BUTTON_NONE;
        if (jw_input_shortcut_button_parse(name, &button) &&
            button != JW_INPUT_SHORTCUT_BUTTON_NONE) {
            s_mock_held[button] = true;
            fprintf(stderr, "input proxy (mock): %s held at start\n", name);
        } else {
            fprintf(stderr, "input proxy (mock): ignoring held button '%s'\n", name);
        }
    }
}

typedef struct {
    int fd;
    char partial[128];
    size_t partial_len;
    jw_power_edge queue[JW_MOCK_POWER_EDGE_QUEUE];
    int head;
    int count;
} jw_mock_power_feed;

static uint64_t jw__mock_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void jw__mock_feed_open(jw_input_proxy *proxy) {
    const char *path = getenv(JW_MOCK_POWER_EDGES_ENV);
    if (!path || !path[0]) {
        return;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "input proxy (mock): cannot open %s=%s: %s\n",
                JW_MOCK_POWER_EDGES_ENV, path, strerror(errno));
        return;
    }
    jw_mock_power_feed *feed = calloc(1, sizeof(*feed));
    if (!feed) {
        close(fd);
        return;
    }
    feed->fd = fd;
    proxy->backend_data = feed;
    fprintf(stderr, "input proxy (mock): power edges from %s\n", path);
}

static void jw__mock_feed_close(jw_input_proxy *proxy) {
    jw_mock_power_feed *feed = proxy ? proxy->backend_data : NULL;
    if (!feed) {
        return;
    }
    close(feed->fd);
    free(feed);
    proxy->backend_data = NULL;
}

static void jw__mock_feed_line(jw_mock_power_feed *feed, const char *line, uint64_t now) {
    char word[8];
    char stamp[32];
    if (sscanf(line, "%7s %31s", word, stamp) != 2) {
        if (line[0]) {
            fprintf(stderr, "input proxy (mock): ignoring power edge line '%s'\n", line);
        }
        return;
    }
    bool down;
    if (strcmp(word, "down") == 0) {
        down = true;
    } else if (strcmp(word, "up") == 0) {
        down = false;
    } else {
        fprintf(stderr, "input proxy (mock): ignoring power edge line '%s'\n", line);
        return;
    }
    char *end = NULL;
    long long value = strtoll(stamp, &end, 10);
    if (end == stamp || *end != '\0') {
        fprintf(stderr, "input proxy (mock): ignoring power edge line '%s'\n", line);
        return;
    }
    long long ms = (stamp[0] == '+' || stamp[0] == '-') ? (long long)now + value : value;
    if (ms < 0) {
        ms = 0;
    }
    if (feed->count >= JW_MOCK_POWER_EDGE_QUEUE) {
        fprintf(stderr, "input proxy (mock): power edge queue full; dropping '%s'\n", line);
        return;
    }
    int slot = (feed->head + feed->count) % JW_MOCK_POWER_EDGE_QUEUE;
    feed->queue[slot].down = down;
    feed->queue[slot].ms = (uint64_t)ms;
    feed->count++;
}

/* Read whatever new complete lines the file holds. A regular file returns 0
   at its end and more after an append; a FIFO without a writer does the same. */
static void jw__mock_feed_refill(jw_mock_power_feed *feed) {
    for (;;) {
        char buf[256];
        ssize_t n = read(feed->fd, buf, sizeof(buf));
        if (n <= 0) {
            return;
        }
        uint64_t now = jw__mock_now_ms();
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                feed->partial[feed->partial_len] = '\0';
                jw__mock_feed_line(feed, feed->partial, now);
                feed->partial_len = 0;
            } else if (feed->partial_len + 1 < sizeof(feed->partial)) {
                feed->partial[feed->partial_len++] = buf[i];
            }
        }
    }
}

int jw_input_proxy_init(jw_input_proxy *proxy,
                        jw_input_brightness_delta_cb brightness_delta,
                        jw_input_volume_delta_cb volume_delta,
                        jw_input_menu_tap_cb menu_tap,
                        void *userdata) {
    if (!proxy) {
        return -1;
    }
    memset(proxy, 0, sizeof(*proxy));
    proxy->brightness_delta = brightness_delta;
    proxy->volume_delta = volume_delta;
    proxy->menu_tap = menu_tap;
    proxy->userdata = userdata;
    jw__mock_feed_open(proxy);
    jw__mock_held_load();
    return 0;
}

int jw_input_proxy_init_watch(jw_input_proxy *proxy,
                              jw_input_brightness_delta_cb brightness_delta,
                              jw_input_volume_delta_cb volume_delta,
                              jw_input_menu_tap_cb menu_tap,
                              void *userdata) {
    return jw_input_proxy_init(proxy, brightness_delta, volume_delta,
                               menu_tap, userdata);
}

int jw_input_proxy_retroarch_joypad_index(const jw_input_proxy *proxy) {
    (void)proxy;
    return -1;
}

int jw_input_proxy_start(jw_input_proxy *proxy) {
    (void)proxy;
    return -1;
}

int jw_input_proxy_poll_fd(const jw_input_proxy *proxy) {
    (void)proxy;
    return -1;
}

bool jw_input_proxy_needs_tick_cadence(const jw_input_proxy *proxy) {
    /* With an edge feed the daemon must keep ticking so a scripted edge is
       taken close to its timestamp; otherwise the mock never needs a pass. */
    return proxy && proxy->backend_data;
}

void jw_input_proxy_tick(jw_input_proxy *proxy) {
    (void)proxy;
}

void jw_input_proxy_configure_menu(jw_input_proxy *proxy,
                                  jw_input_menu_config config) {
    if (proxy) proxy->menu_config = config;
}

void jw_input_proxy_cancel_menu(jw_input_proxy *proxy) {
    (void)proxy;
}

uint64_t jw_input_proxy_idle_ms(const jw_input_proxy *proxy) {
    (void)proxy;
    return 0;   /* mock: never idle (auto-sleep is a no-op off-device) */
}

void jw_input_proxy_mark_activity(jw_input_proxy *proxy) {
    (void)proxy;
}

void jw_input_proxy_flush(jw_input_proxy *proxy) {
    (void)proxy;
}

void jw_input_proxy_set_swallow(jw_input_proxy *proxy, bool swallow) {
    (void)proxy;
    (void)swallow;
}

void jw_input_proxy_release_buttons(jw_input_proxy *proxy) {
    (void)proxy;   /* mock: no virtual pad to release; safe no-op */
}

void jw_input_proxy_emit_menu_tap(jw_input_proxy *proxy) {
    (void)proxy;   /* mock: no virtual pad to emit onto; safe no-op */
}

bool jw_input_proxy_take_held_button(jw_input_proxy *proxy,
                                     jw_input_shortcut_button button) {
    if (!proxy || button <= JW_INPUT_SHORTCUT_BUTTON_NONE ||
        button >= JW_INPUT_SHORTCUT_BUTTON_COUNT || !s_mock_held[button]) {
        return false;
    }
    /* Taken: like the real proxy dropping it until release, it is gone. */
    s_mock_held[button] = false;
    return true;
}

bool jw_input_proxy_take_power_edge(jw_input_proxy *proxy, jw_power_edge *edge) {
    jw_mock_power_feed *feed = proxy ? proxy->backend_data : NULL;
    if (!feed || !edge) {
        return false;   /* mock: no power key */
    }
    jw__mock_feed_refill(feed);
    if (feed->count == 0 || feed->queue[feed->head].ms > jw__mock_now_ms()) {
        return false;   /* nothing scripted, or the next edge is not due yet */
    }
    *edge = feed->queue[feed->head];
    feed->head = (feed->head + 1) % JW_MOCK_POWER_EDGE_QUEUE;
    feed->count--;
    return true;
}

void jw_input_proxy_shutdown(jw_input_proxy *proxy) {
    if (proxy) {
        jw__mock_feed_close(proxy);
        memset(proxy, 0, sizeof(*proxy));
    }
}
