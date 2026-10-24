#include "cmd/jawaka-osd/osd_layout.h"
#include "cmd/jawaka-osd/osd_utf8.h"

#include <stdlib.h>
#include <string.h>

int jw_osd_font_bump(const char *value) {
    if (!value || !value[0]) return JW_OSD_FONT_BUMP_DEFAULT;
    char *end = NULL;
    long bump = strtol(value, &end, 10);
    if (!end || *end != '\0' || bump < 0 || bump > JW_OSD_FONT_BUMP_MAX) {
        return JW_OSD_FONT_BUMP_DEFAULT;
    }
    return (int)bump;
}

/* Catastrophe's scale: linear below its 1024 px reference, damped above. */
static int jw__scaled(int output_w, int base) {
    double raw = (double)output_w / 1024.0;
    double scale = raw <= 1.0 ? raw : 1.0 + (raw - 1.0) * 0.75;
    int value = (int)(base * scale);
    return value < 1 ? 1 : value;
}

void jw_osd_banner_metrics_for(int output_w, int font_bump, jw_osd_banner_metrics *out) {
    if (!out) return;
    if (output_w < 1) output_w = 1;
    if (font_bump < 0 || font_bump > JW_OSD_FONT_BUMP_MAX) font_bump = JW_OSD_FONT_BUMP_DEFAULT;
    out->max_w = output_w * 9 / 10;
    out->inset = jw__scaled(output_w, 12);
    out->pad_y = jw__scaled(output_w, 4);
    out->radius = jw__scaled(output_w, 8);
    out->inner_radius = jw__scaled(output_w, 7);
    out->border = 1;
    out->font_px = 2 * (12 + font_bump);
}

int jw_osd_banner_text_max_w(const jw_osd_banner_metrics *metrics) {
    if (!metrics) return 1;
    int w = metrics->max_w - 2 * metrics->inset;
    return w < 1 ? 1 : w;
}

int jw_osd_fit_text(const char *text, int max_w, jw_osd_measure_fn measure,
                    void *ctx, char *out, size_t out_size) {
    if (!out || out_size == 0) return 0;
    out[0] = '\0';
    if (!text || !measure) return 0;

    size_t len = jw_osd_utf8_boundary(text, strlen(text));
    size_t n = len < out_size ? len : jw_osd_utf8_boundary(text, out_size - 1);
    memcpy(out, text, n);
    out[n] = '\0';
    int w = measure(ctx, out);
    if (n == len && w <= max_w) return w;

    if (out_size < 4) {
        out[0] = '\0';
        return 0;
    }
    size_t cut = n <= out_size - 4 ? n : jw_osd_utf8_boundary(text, out_size - 4);
    for (;;) {
        while (cut > 0 && text[cut - 1] == ' ') cut--;
        memcpy(out + cut, "...", 4);
        w = measure(ctx, out);
        if (w <= max_w || cut == 0) break;
        cut = jw_osd_utf8_prev(text, cut);
    }
    if (w > max_w) {
        out[0] = '\0';
        return 0;
    }
    return w;
}

void jw_osd_banner_layout_for(const jw_osd_banner_metrics *metrics,
                              int output_w, int output_h,
                              int title_w, int action_w, bool has_action,
                              int row_h, jw_osd_banner_layout *out) {
    if (!metrics || !out) return;
    memset(out, 0, sizeof(*out));
    if (title_w < 0) title_w = 0;
    if (action_w < 0 || !has_action) action_w = 0;
    if (row_h < 1) row_h = 1;

    int content_w = title_w > action_w ? title_w : action_w;
    int w = content_w + 2 * metrics->inset;
    if (w > metrics->max_w) w = metrics->max_w;
    int h = 2 * metrics->pad_y + row_h + (has_action ? metrics->pad_y + row_h : 0);

    out->box.w = w;
    out->box.h = h;
    out->box.x = (output_w - w) / 2;
    out->box.y = output_h - metrics->inset - h;
    if (out->box.x < 0) out->box.x = 0;
    if (out->box.y < 0) out->box.y = 0;

    out->title_x = out->box.x + (w - title_w) / 2;
    out->title_y = out->box.y + metrics->pad_y;
    if (has_action) {
        out->action_x = out->box.x + (w - action_w) / 2;
        out->action_y = out->title_y + row_h + metrics->pad_y;
    }
}

bool jw_osd_rect_equal(jw_osd_rect a, jw_osd_rect b) {
    return a.x == b.x && a.y == b.y && a.w == b.w && a.h == b.h;
}

jw_osd_rect jw_osd_damage_rect(bool prev_valid, jw_osd_rect prev, jw_osd_rect next,
                               int surface_w, int surface_h) {
    if (prev_valid && jw_osd_rect_equal(prev, next)) return next;
    return (jw_osd_rect){ 0, 0, surface_w, surface_h };
}
