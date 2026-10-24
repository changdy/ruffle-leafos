#ifndef JW_OSD_LAYOUT_H
#define JW_OSD_LAYOUT_H

#include <stdbool.h>
#include <stddef.h>

/* Banner geometry shared by both OSD backends, derived from the launcher's
   launch-notice pill. Everything is measured against the full output, never a
   host window, so the SDL preview and the Wayland overlay agree. Pure, so the
   numbers are tested natively. */

/* Fixed dark palette: the banner sits over any game, not a launcher theme. */
#define JW_OSD_BANNER_BACKGROUND 0x0F160Eu
#define JW_OSD_BANNER_TEXT       0xE8F1E3u
#define JW_OSD_BANNER_MUTED      0x7E9579u   /* border and secondary line */

#define JW_OSD_FONT_BUMP_DEFAULT 2
#define JW_OSD_FONT_BUMP_MAX     5

typedef struct {
    int x, y, w, h;
} jw_osd_rect;

typedef struct {
    int max_w;          /* floor(W * 0.9) */
    int inset;          /* screen inset and horizontal padding: floor(12 * s) */
    int pad_y;          /* vertical padding and row gap: floor(4 * s) */
    int radius;         /* floor(8 * s) */
    int inner_radius;   /* floor(7 * s) */
    int border;         /* one physical pixel */
    int font_px;        /* MLP1 device scale 2: 2 * (12 + bump), bold */
} jw_osd_banner_metrics;

/* CAT_FONT_BUMP, validated to 0..5; anything else is the default. */
int jw_osd_font_bump(const char *value);
void jw_osd_banner_metrics_for(int output_w, int font_bump, jw_osd_banner_metrics *out);
int jw_osd_banner_text_max_w(const jw_osd_banner_metrics *metrics);

typedef int (*jw_osd_measure_fn)(void *ctx, const char *text);

/* Copies `text` into `out`, shortened at a character boundary with "..." until
   it measures within `max_w`. Returns the measured width, or 0 with an empty
   `out` when not even the ellipsis fits. */
int jw_osd_fit_text(const char *text, int max_w, jw_osd_measure_fn measure,
                    void *ctx, char *out, size_t out_size);

typedef struct {
    jw_osd_rect box;
    int title_x, title_y;
    int action_x, action_y;   /* meaningful only with an action row */
} jw_osd_banner_layout;

/* Bottom-centred box around one or two measured rows. */
void jw_osd_banner_layout_for(const jw_osd_banner_metrics *metrics,
                              int output_w, int output_h,
                              int title_w, int action_w, bool has_action,
                              int row_h, jw_osd_banner_layout *out);

bool jw_osd_rect_equal(jw_osd_rect a, jw_osd_rect b);

/* The region to report when `next` replaces `prev` on a w x h surface. Any
   change of bounds, a shrink included, reports the whole surface so no old
   pixels stay behind; unchanged bounds report just the box. */
jw_osd_rect jw_osd_damage_rect(bool prev_valid, jw_osd_rect prev, jw_osd_rect next,
                               int surface_w, int surface_h);

#endif /* JW_OSD_LAYOUT_H */
