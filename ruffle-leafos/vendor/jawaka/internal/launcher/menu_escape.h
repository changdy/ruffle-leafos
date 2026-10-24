#ifndef JW_MENU_ESCAPE_H
#define JW_MENU_ESCAPE_H

#include <stdbool.h>
#include <stdint.h>

/* Daemon-owned escalation. Session generation, rather than a PID alone,
   prevents a deadline surviving child replacement or PID reuse. */
typedef struct {
    uint64_t session;
    uint64_t hold;
    uint64_t term_ms;
    bool pending;
} jw_menu_escape;

static inline void jw_menu_escape_cancel(jw_menu_escape *escape) {
    *escape = (jw_menu_escape){0};
}

/* Called only after SIGTERM was actually sent, using the send time. */
static inline void jw_menu_escape_sent(jw_menu_escape *escape, uint64_t session,
                                       uint64_t hold, uint64_t now_ms) {
    *escape = (jw_menu_escape){session, hold, now_ms, true};
}

static inline void jw_menu_escape_end(jw_menu_escape *escape, uint64_t hold) {
    if (escape->hold == hold) jw_menu_escape_cancel(escape);
}

/* The caller drains queued input before this check. Release/cancel callbacks
   clear pending even when they arrived while the daemon was busy. */
static inline bool jw_menu_escape_kill(jw_menu_escape *escape, uint64_t session,
                                       bool running, uint64_t now_ms) {
    if (!running || escape->session != session) {
        jw_menu_escape_cancel(escape);
        return false;
    }
    if (!escape->pending || now_ms < escape->term_ms ||
        now_ms - escape->term_ms < 2000) return false;
    jw_menu_escape_cancel(escape);
    return true;
}

#endif
