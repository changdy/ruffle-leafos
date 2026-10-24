#ifndef JW_SYSTEM_ACTIVITY_H
#define JW_SYSTEM_ACTIVITY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#define JW_SYSTEM_NOTICE_MS 6000u

typedef struct {
    char text[256];
    uint32_t started_ms;
} jw_system_notice;

typedef struct {
    jw_system_notice feedback;
    jw_system_notice completion;
    char scrape[256];
    char storage[256];   /* persistent while a card is read-only */
    bool scan_running;
} jw_system_activity;

/* Call for every emitted message, even when its text matches the previous one. */
static inline void jw_system_notice_set(jw_system_notice *notice,
                                        const char *text, uint32_t now) {
    snprintf(notice->text, sizeof(notice->text), "%s", text);
    notice->started_ms = now;
}

static inline uint32_t jw_system_notice_remaining(const jw_system_notice *notice,
                                                 uint32_t now) {
    uint32_t elapsed = now - notice->started_ms;
    return notice->text[0] && elapsed < JW_SYSTEM_NOTICE_MS
        ? JW_SYSTEM_NOTICE_MS - elapsed : 0;
}

static inline const char *jw_system_activity_text(const jw_system_activity *activity,
                                                  uint32_t now, const char *scan) {
    if (jw_system_notice_remaining(&activity->feedback, now)) return activity->feedback.text;
    if (activity->scrape[0]) return activity->scrape;
    if (activity->storage[0]) return activity->storage;
    if (jw_system_notice_remaining(&activity->completion, now)) return activity->completion.text;
    return activity->scan_running ? scan : "";
}

#endif
