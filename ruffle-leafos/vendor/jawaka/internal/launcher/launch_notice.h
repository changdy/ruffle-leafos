#ifndef JW_LAUNCH_NOTICE_H
#define JW_LAUNCH_NOTICE_H

#include "internal/launcher/system_activity.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* A launch the user asked for did not happen. Unlike system-activity feedback,
   navigation never clears the notice: only a new launch attempt does, and the
   lifetime expires on its own after JW_SYSTEM_NOTICE_MS. Pure so it can be
   tested natively; the launcher owns the rendering and the frame requests. */

static inline void jw_launch_notice_clear(jw_system_notice *notice) {
    notice->text[0] = '\0';
    notice->started_ms = 0;
}

/* Start of a launch attempt: a retry replaces the previous refusal instead of
   stacking on it or inheriting its remaining lifetime. */
static inline void jw_launch_notice_begin(jw_system_notice *notice) {
    jw_launch_notice_clear(notice);
}

static inline void jw_launch_notice_show(jw_system_notice *notice,
                                         const char *text, uint32_t now) {
    jw_system_notice_set(notice, text, now);
}

static inline uint32_t jw_launch_notice_remaining(const jw_system_notice *notice,
                                                  uint32_t now) {
    return jw_system_notice_remaining(notice, now);
}

static inline const char *jw_launch_notice_text(const jw_system_notice *notice,
                                                uint32_t now) {
    return jw_launch_notice_remaining(notice, now) ? notice->text : "";
}

/* Expiry tick. Returns true on the one tick that removed an expired notice, so
   the caller can request the frame that erases the pill. Reads never clear, so
   a view that draws late cannot shorten the lifetime. */
static inline bool jw_launch_notice_expire(jw_system_notice *notice, uint32_t now) {
    if (jw_system_notice_remaining(notice, now)) return false;
    if (!notice->text[0]) return false;
    jw_launch_notice_clear(notice);
    return true;
}

/* ─── Line split ─────────────────────────────────────────────────────────────
   The pill shows at most two lines. Translations run 30-50% longer than
   English, and an ellipsis would hide why the launch failed. Pure over a width
   callback, so the breaking rules are tested without a font. */

typedef int (*jw_launch_notice_measure_fn)(void *context, const char *text);

typedef struct {
    int  count;   /* 0 for an empty notice, else 1 or 2 */
    char line[2][sizeof(((jw_system_notice *)0)->text)];
} jw_launch_notice_lines;

/* Code point at s and its UTF-8 length; a malformed byte stands for itself. */
static inline uint32_t jw__notice_cp(const char *s, int *len) {
    const unsigned char *u = (const unsigned char *)s;
    int n = u[0] >= 0xF0 ? 4 : u[0] >= 0xE0 ? 3 : u[0] >= 0xC0 ? 2 : 1;
    uint32_t cp = n == 1 ? u[0] : u[0] & (0x3Fu >> (n - 1));
    for (int i = 1; i < n; ++i) {
        if ((u[i] & 0xC0) != 0x80) {
            *len = 1;
            return u[0];
        }
        cp = (cp << 6) | (u[i] & 0x3Fu);
    }
    *len = n;
    return cp;
}

/* CJK, kana, Hangul and fullwidth forms are written without spaces, so a line
   may break between any two of them. */
static inline bool jw__notice_wide(uint32_t cp) {
    return cp >= 0x2E80;
}

/* The basic CJK line-breaking rule: closing punctuation never starts a line
   and opening punctuation never ends one. */
static inline bool jw__notice_closing(uint32_t cp) {
    switch (cp) {
    case '.': case ',': case ':': case ';': case '!': case '?': case ')': case ']':
    case 0x2026: case 0x3001: case 0x3002: case 0x3009: case 0x300B: case 0x300D:
    case 0x300F: case 0x3011: case 0x30FC: case 0xFF01: case 0xFF09: case 0xFF0C:
    case 0xFF0E: case 0xFF1A: case 0xFF1B: case 0xFF1F:
        return true;
    default:
        return false;
    }
}

static inline bool jw__notice_opening(uint32_t cp) {
    switch (cp) {
    case '(': case '[': case 0x3008: case 0x300A: case 0x300C: case 0x300E:
    case 0x3010: case 0xFF08:
        return true;
    default:
        return false;
    }
}

/* Whether a line may break at byte b, between code points prev and next: at a
   run of spaces, which neither line keeps, or beside a CJK character. Sets
   where line one ends and line two starts. */
static inline bool jw__notice_break_at(const char *s, int b, uint32_t prev,
                                       uint32_t next, int *end, int *start) {
    if (next == ' ') {
        if (prev == ' ') return false;   /* offered at the run's first space */
        int i = b;
        while (s[i] == ' ') ++i;
        if (!s[i]) return false;
        *end = b;
        *start = i;
        return true;
    }
    if (prev == ' ') return false;
    if (!jw__notice_wide(prev) && !jw__notice_wide(next)) return false;
    if (jw__notice_closing(next) || jw__notice_opening(prev)) return false;
    *end = *start = b;
    return true;
}

/* Fit a notice into at most two lines of max_w. Text that fits stays on one
   line. Otherwise it breaks where the wider line is narrowest, so the pill
   stays compact. Text too long for two lines fills line one and leaves the
   rest on line two for the caller to ellipsize; a first word wider than max_w
   is cut at the last character that fits. Returns the line count. */
static inline int jw_launch_notice_split(const char *text, int max_w,
                                         jw_launch_notice_measure_fn measure,
                                         void *context, jw_launch_notice_lines *out) {
    char src[sizeof(out->line[0])];
    snprintf(src, sizeof(src), "%s", text ? text : "");
    out->count = 0;
    out->line[0][0] = out->line[1][0] = '\0';
    if (!src[0]) return 0;
    out->count = 1;
    memcpy(out->line[0], src, sizeof(src));
    if (measure(context, src) <= max_w) return 1;

    char *head = out->line[0];   /* candidate line one, rebuilt per break */
    int best_end = -1, best_start = 0, best_w = 0;   /* narrowest wider line */
    int fill_end = -1, fill_start = 0;               /* longest line one that fits */
    int len;
    uint32_t prev = jw__notice_cp(src, &len);
    for (int b = len; src[b]; b += len) {
        uint32_t next = jw__notice_cp(src + b, &len);
        int end, start;
        if (jw__notice_break_at(src, b, prev, next, &end, &start)) {
            memcpy(head, src, (size_t)end);
            head[end] = '\0';
            int w1 = measure(context, head);
            if (w1 > max_w) break;   /* later breaks only lengthen line one */
            fill_end = end;
            fill_start = start;
            int w2 = measure(context, src + start);
            if (w2 <= max_w) {
                int wider = w1 > w2 ? w1 : w2;
                if (best_end < 0 || wider < best_w) {
                    best_end = end;
                    best_start = start;
                    best_w = wider;
                }
                if (w1 >= w2) break;   /* line one, now the wider, only grows */
            }
        }
        prev = next;
    }

    int end = best_end >= 0 ? best_end : fill_end;
    int start = best_end >= 0 ? best_start : fill_start;
    if (end < 0) {
        end = 0;
        for (int b = 0; src[b]; b += len) {
            jw__notice_cp(src + b, &len);
            memcpy(head, src, (size_t)(b + len));
            head[b + len] = '\0';
            if (end > 0 && measure(context, head) > max_w) break;
            end = b + len;
        }
        start = end;
        while (src[start] == ' ') ++start;
    }
    memcpy(out->line[0], src, (size_t)end);
    out->line[0][end] = '\0';
    if (!src[start]) return 1;
    snprintf(out->line[1], sizeof(out->line[1]), "%s", src + start);
    out->count = 2;
    return 2;
}

#endif /* JW_LAUNCH_NOTICE_H */
