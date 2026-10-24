#include "internal/launcher/focus_screen.h"

#include "catastrophe.h"

#include <SDL2/SDL.h>
#include <string.h>

/* Battery percent at/below which the corner glyph blinks (not charging). */
#define JW_FOCUS_BATT_CRITICAL 10

/* ── style palette ──────────────────────────────────────────────────────── */

typedef struct {
    SDL_Color bg;        /* screen + tile interior */
    SDL_Color tile_bg;   /* tile interior (slightly off the screen bg) */
    SDL_Color border;    /* unselected tile border */
    SDL_Color sel;       /* selected tile border + title accents */
    SDL_Color text;      /* title-fallback text + battery glyph */
} jw__focus_palette;

static jw__focus_palette jw__focus_resolve_palette(bool bw) {
    jw__focus_palette p;
    if (bw) {
        p.bg      = (SDL_Color){ 0, 0, 0, 255 };
        p.tile_bg = (SDL_Color){ 22, 22, 22, 255 };
        p.border  = (SDL_Color){ 90, 90, 90, 255 };
        p.sel     = (SDL_Color){ 255, 255, 255, 255 };
        p.text    = (SDL_Color){ 235, 235, 235, 255 };
        return p;
    }
    const cat_theme *t = cat_get_theme();
    p.bg      = t->background;
    p.border  = t->hint;        /* dim, recedes */
    p.sel     = t->emphasis;    /* the highlight hue, luminance-safe on bg */
    p.text    = t->text;
    /* Tile interior: a subtle lift off the background so empty tiles read as
       framed. Nudge each channel toward mid-gray by a small amount. */
    SDL_Color b = t->background;
    int dir = (b.r + b.g + b.b) / 3 < 128 ? +1 : -1;
    p.tile_bg = (SDL_Color){
        (Uint8)(b.r + dir * 18 < 0 ? 0 : (b.r + dir * 18 > 255 ? 255 : b.r + dir * 18)),
        (Uint8)(b.g + dir * 18 < 0 ? 0 : (b.g + dir * 18 > 255 ? 255 : b.g + dir * 18)),
        (Uint8)(b.b + dir * 18 < 0 ? 0 : (b.b + dir * 18 > 255 ? 255 : b.b + dir * 18)),
        255
    };
    return p;
}

/* ── layout ─────────────────────────────────────────────────────────────── */

/* Split `count` (1..5) into top-row and bottom-row counts: bottom fills up to 3,
   the top row takes the remainder (0..2). idx 0..top-1 = top row (reading order),
   top..count-1 = bottom row. */
static void jw__focus_rows(int count, int *top, int *bottom) {
    int b = count < 3 ? count : 3;
    *bottom = b;
    *top = count - b;   /* 0, 1, or 2 */
}

bool jw_focus_screen_tile_rect(int idx, int count, SDL_Rect *out) {
    if (!out || count <= 0 || count > JW_FOCUS_SCREEN_MAX_TILES ||
        idx < 0 || idx >= count) {
        return false;
    }
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();

    int gap = (int)(sw * 0.03f);   /* gap between tiles */
    if (gap < 10) gap = 10;

    int top = 0, bottom = 0;
    jw__focus_rows(count, &top, &bottom);
    int rows = top > 0 ? 2 : 1;
    int cols = bottom;             /* the widest (bottom) row sets the box width */

    /* Square tile that fills the width (`gap` side margins), then the whole grid
       is centered vertically so the top and bottom margins are equal. */
    int fit_w = (sw - (cols + 1) * gap) / cols;
    int fit_h = (sh - (rows + 1) * gap) / rows;
    int t = fit_w < fit_h ? fit_w : fit_h;
    if (t < 1) t = 1;

    /* Center the whole box; each row is centered within it. */
    int grid_w = cols * t + (cols - 1) * gap;
    int grid_h = rows * t + (rows - 1) * gap;
    int ox = (sw - grid_w) / 2;
    int oy = (sh - grid_h) / 2;

    int row, col, row_count;
    if (top > 0 && idx < top) {
        row = 0; col = idx; row_count = top;
    } else {
        row = (top > 0) ? 1 : 0;
        col = (top > 0) ? idx - top : idx;
        row_count = bottom;
    }

    int row_w = row_count * t + (row_count - 1) * gap;
    int row_x = ox + (grid_w - row_w) / 2;

    out->x = row_x + col * (t + gap);
    out->y = oy + row * (t + gap);
    out->w = t;
    out->h = t;
    return true;
}

/* ── tile + battery drawing ─────────────────────────────────────────────── */

/* CONTAIN-fit a (aw x ah) source inside `box`, centered, returning the dst. */
static SDL_Rect jw__focus_contain(SDL_Rect box, int aw, int ah) {
    if (aw <= 0 || ah <= 0) return box;
    float sx = (float)box.w / (float)aw;
    float sy = (float)box.h / (float)ah;
    float s = sx < sy ? sx : sy;
    int w = (int)(aw * s);
    int h = (int)(ah * s);
    SDL_Rect dst = { box.x + (box.w - w) / 2, box.y + (box.h - h) / 2, w, h };
    return dst;
}

static void jw__focus_draw_tile(const jw_focus_tile *tile, SDL_Rect r,
                                bool selected, const jw__focus_palette *pal) {
    int radius = (int)(r.w * 0.12f);
    if (radius < 6) radius = 6;

    int bw = (int)(r.w * 0.02f);
    if (bw < 2) bw = 2;
    if (selected) {
        bw = (int)(r.w * 0.045f);
        if (bw < 4) bw = 4;
    }
    SDL_Color border = selected ? pal->sel : pal->border;

    /* Border frame = outer rounded rect in the border color, inner rounded rect
       in the tile interior color inset by the border width. */
    cat_draw_rounded_rect(r.x, r.y, r.w, r.h, radius, border);
    SDL_Rect inner = { r.x + bw, r.y + bw, r.w - 2 * bw, r.h - 2 * bw };
    int inner_radius = radius - bw;
    if (inner_radius < 3) inner_radius = 3;
    cat_draw_rounded_rect(inner.x, inner.y, inner.w, inner.h, inner_radius,
                          pal->tile_bg);

    /* Contents: box art CONTAIN-fit inside the inner area (small pad), else the
       title centered. */
    int pad = (int)(inner.w * 0.06f);
    SDL_Rect content = { inner.x + pad, inner.y + pad,
                         inner.w - 2 * pad, inner.h - 2 * pad };
    if (content.w < 1) content.w = 1;
    if (content.h < 1) content.h = 1;

    if (tile->art) {
        SDL_Rect dst = jw__focus_contain(content, tile->art_w, tile->art_h);
        SDL_RenderCopy(cat_get_renderer(), tile->art, NULL, &dst);
    } else {
        const char *title = tile->title ? tile->title : "";
        TTF_Font *font = cat_get_font(CAT_FONT_SMALL);
        int th = cat_measure_wrapped_text_height(font, title, content.w);
        int ty = content.y + (content.h - th) / 2;
        if (ty < content.y) ty = content.y;
        cat_draw_text_wrapped(font, title, content.x, ty, content.w,
                              pal->text, CAT_ALIGN_CENTER);
    }
}

/* Monochrome battery glyph with proportional fill + charging bolt, drawn with
   its right edge at (right_x, top_y). No color-coding (bars only). When charging,
   a lightning bolt is overlaid on the body in the `bg` color so it reads as a
   cutout against the fill. */
static void jw__focus_draw_battery(int right_x, int top_y, int h,
                                   jw_focus_battery b, SDL_Color c, SDL_Color bg) {
    int body_w = (int)(h * 1.9f);
    int nub_w  = (int)(h * 0.14f);
    if (nub_w < 2) nub_w = 2;
    int nub_h  = (int)(h * 0.45f);

    int body_x = right_x - nub_w - body_w;
    int body_y = top_y;

    /* Outline (draw filled outer, then punch the interior back to transparent by
       redrawing the screen bg is unreliable; instead draw a thin frame via four
       bars). */
    int fw = (int)(h * 0.08f);
    if (fw < 2) fw = 2;
    cat_draw_rect(body_x, body_y, body_w, fw, c);                        /* top */
    cat_draw_rect(body_x, body_y + h - fw, body_w, fw, c);              /* bottom */
    cat_draw_rect(body_x, body_y, fw, h, c);                            /* left */
    cat_draw_rect(body_x + body_w - fw, body_y, fw, h, c);              /* right */
    cat_draw_rect(right_x - nub_w, body_y + (h - nub_h) / 2, nub_w, nub_h, c); /* nub */

    if (b.percent < 0) {
        /* Unknown reading (e.g. a transient IPC miss at spawn): a centered "?" so
           it reads as "unknown", not an empty cell that looks like a dead battery.
           No fill, and the caller suppresses the critical blink for percent < 0. */
        TTF_Font *qf = cat_get_font(CAT_FONT_SMALL);
        int qw = 0, qh = 0;
        TTF_SizeUTF8(qf, "?", &qw, &qh);
        cat_draw_text(qf, "?", body_x + (body_w - qw) / 2, body_y + (h - qh) / 2, c);
    } else {
        /* Fill proportional to percent, inset from the frame. */
        int pct = b.percent > 100 ? 100 : b.percent;
        int inset = fw + (int)(h * 0.10f);
        int fill_max_w = body_w - 2 * inset;
        int fill_w = fill_max_w * pct / 100;
        if (fill_w > 0)
            cat_draw_rect(body_x + inset, body_y + inset, fill_w, h - 2 * inset, c);
    }

    if (b.charging) {
        /* Lightning bolt overlaid on the body, in the background color, so it
           cuts out of the fill and reads clearly. */
        float blh = (float)h * 0.86f;
        float blw = blh * 0.60f;
        float blx = (float)body_x + ((float)body_w - blw) / 2.0f;
        float bly = (float)body_y + ((float)h - blh) / 2.0f;
        static const float pts[6][2] = {
            { 0.62f, 0.00f }, { 0.10f, 0.56f }, { 0.50f, 0.52f },
            { 0.50f, 0.48f }, { 0.90f, 0.44f }, { 0.38f, 1.00f },
        };
        SDL_Vertex v[6];
        for (int i = 0; i < 6; i++) {
            v[i].position.x = blx + pts[i][0] * blw;
            v[i].position.y = bly + pts[i][1] * blh;
            v[i].color = bg;
            v[i].tex_coord.x = 0.0f;
            v[i].tex_coord.y = 0.0f;
        }
        SDL_RenderGeometry(cat_get_renderer(), NULL, v, 6, NULL, 0);
    }
}

void jw_focus_screen_render(const jw_focus_tile *tiles, int count, int cursor,
                            bool bw, jw_focus_battery battery, bool bt_pip) {
    jw__focus_palette pal = jw__focus_resolve_palette(bw);

    SDL_SetRenderDrawColor(cat_get_renderer(), pal.bg.r, pal.bg.g, pal.bg.b, 255);
    SDL_RenderClear(cat_get_renderer());

    if (count < 0) count = 0;
    if (count > JW_FOCUS_SCREEN_MAX_TILES) count = JW_FOCUS_SCREEN_MAX_TILES;

    for (int i = 0; i < count; i++) {
        SDL_Rect r;
        if (!jw_focus_screen_tile_rect(i, count, &r)) continue;
        jw__focus_draw_tile(&tiles[i], r, i == cursor, &pal);
    }

    /* Corner battery: top-right, inside the reserved strip. */
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    int bh = (int)(sh * 0.032f);
    if (bh < 14) bh = 14;
    int margin = (int)(sw * 0.03f);
    /* Critical-battery blink: below the threshold and not charging, blink the
       glyph so "plug in now" still shouts without reintroducing color. The
       caller re-renders on a throttle to animate it. */
    bool critical = !battery.charging && battery.percent >= 0 &&
                    battery.percent <= JW_FOCUS_BATT_CRITICAL;
    if (!(critical && (SDL_GetTicks() / 450u) % 2u == 1u))
        jw__focus_draw_battery(sw - margin, margin, bh, battery, pal.text, pal.bg);

    /* BT pip: a headset is paired but disconnected — the Bluetooth glyph sits just
       left of the battery (the bolt is now overlaid on the body, not beside it). */
    if (bt_pip) {
        int icon = (int)(bh * 1.2f);
        int batt_span = (int)(bh * 1.9f) + (int)(bh * 0.14f);   /* body + nub */
        int x = sw - margin - batt_span - (int)(bh * 0.5f) - icon;
        int y = margin + (bh - icon) / 2;
        cat_draw_bluetooth_icon(x, y, icon, icon, pal.text);
    }
}

/* Centered text helper (measures then draws). Returns the baseline advance. */
static void jw__focus_text_centered(TTF_Font *font, const char *text, int cx,
                                    int y, SDL_Color c) {
    if (!text || !text[0]) return;
    int w = cat_measure_text(font, text);
    cat_draw_text(font, text, cx - w / 2, y, c);
}

static int jw__focus_fh(TTF_Font *f) {
    int h = 0;
    TTF_SizeUTF8(f, "Ay", NULL, &h);
    return h;
}

/* One legend cell "key label", key colored, left-aligned at (x,y). */
static void jw__focus_hint_cell(int x, int y, const char *key, const char *label,
                                SDL_Color kc, SDL_Color lc) {
    if (!key || !key[0]) return;
    TTF_Font *f = cat_get_font(CAT_FONT_SMALL);
    char keybuf[24];
    snprintf(keybuf, sizeof(keybuf), "%s:", key);
    int kw = cat_draw_text(f, keybuf, x, y, kc);
    if (label && label[0])
        cat_draw_text(f, label, x + kw + cat_measure_text(f, " "), y, lc);
}

/* Draw the substring s[a..b) in color c at (x,y); return its advance width. */
static int jw__hint_span(TTF_Font *f, const char *s, int a, int b,
                         int x, int y, SDL_Color c) {
    if (b <= a) return 0;
    char buf[128];
    int n = b - a;
    if (n > (int)sizeof(buf) - 1) n = (int)sizeof(buf) - 1;
    memcpy(buf, s + a, (size_t)n);
    buf[n] = '\0';
    return cat_draw_text(f, buf, x, y, c);
}

/* Render a "KEY: Value   KEY: Value" hint line centered at cx, coloring the
   "KEY:" tokens in key_color and the values in val_color. Segments are split on
   runs of 3+ spaces, so a value may safely contain up to a double space
   ("Shut Down", "1  2"). This is the one two-tone hint style shared by the
   wizard footers, the confirm popup, and (via jw__focus_hint_cell) the unlock
   legend, so key/value colors match everywhere. */
void jw_focus_draw_hint_kv(TTF_Font *f, const char *s, int cx, int y,
                           SDL_Color key_color, SDL_Color val_color) {
    if (!s || !s[0]) return;
    int len = (int)strlen(s);
    int x = cx - cat_measure_text(f, s) / 2;
    int tok = 0, i = 0, in_value = 0;
    while (i < len) {
        if (!in_value) {
            if (s[i] == ':') {
                x += jw__hint_span(f, s, tok, i + 1, x, y, key_color);
                tok = i + 1; in_value = 1;
            }
            i++;
        } else if (s[i] == ' ' && i + 2 < len &&
                   s[i + 1] == ' ' && s[i + 2] == ' ') {
            int j = i;
            while (j < len && s[j] == ' ') j++;
            x += jw__hint_span(f, s, tok, j, x, y, val_color);
            tok = j; in_value = 0; i = j;
        } else {
            i++;
        }
    }
    jw__hint_span(f, s, tok, len, x, y, in_value ? val_color : key_color);
}

void jw_focus_screen_render_unlock(bool bw, const jw_focus_unlock_view *v) {
    if (!v) return;
    jw__focus_palette pal = jw__focus_resolve_palette(bw);
    int sw = cat_get_screen_width();
    int sh = cat_get_screen_height();
    SDL_Renderer *rend = cat_get_renderer();

    /* Dim the grid behind the panel. */
    SDL_BlendMode prev;
    SDL_GetRenderDrawBlendMode(rend, &prev);
    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(rend, 0, 0, 0, 170);
    SDL_Rect full = { 0, 0, sw, sh };
    SDL_RenderFillRect(rend, &full);
    SDL_SetRenderDrawBlendMode(rend, prev);

    TTF_Font *title_font = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *hint_font = cat_get_font(CAT_FONT_SMALL);
    int th = jw__focus_fh(title_font);
    int hh = jw__focus_fh(hint_font);
    int rh = hh + (int)(sh * 0.016f);          /* legend row height */
    int pad = (int)(sh * 0.045f);
    int slot = (int)(sh * 0.10f);
    int slot_block = v->pin_mode ? slot + (int)(sh * 0.03f) : 0;
    int err_block = (v->error && v->pin_mode) ? hh + (int)(sh * 0.014f) : 0;
    int legend_h = v->row_count * rh;

    /* Panel sized to its content. */
    int pw = (int)(sw * 0.62f);
    int gap_ty = (int)(sh * 0.03f);
    int ph = v->confirm
        ? pad + th + (int)(sh * 0.07f) + hh + pad
        : pad + th + gap_ty + slot_block + err_block + gap_ty + legend_h + pad;
    int px = (sw - pw) / 2;
    int py = (sh - ph) / 2;
    int radius = (int)(sh * 0.03f);
    int border = (int)(sh * 0.006f);
    if (border < 3) border = 3;
    cat_draw_rounded_rect(px, py, pw, ph, radius, pal.sel);
    cat_draw_rounded_rect(px + border, py + border, pw - 2 * border,
                          ph - 2 * border, radius - border, pal.tile_bg);

    int cx = sw / 2;
    int y = py + pad;

    /* Title. */
    jw__focus_text_centered(title_font, v->confirm ? v->confirm
                                        : (v->title ? v->title : ""), cx, y, pal.text);
    y += th + gap_ty;

    /* Confirm prompt (Reboot? / Shut Down?) takes over the panel. */
    if (v->confirm) {
        jw_focus_draw_hint_kv(hint_font,
                              v->confirm_hint ? v->confirm_hint
                                              : "B: Back      A: Confirm",
                              cx, py + ph - pad - hh, pal.sel, pal.text);
        return;
    }

    /* PIN slots. */
    if (v->pin_mode && v->pin) {
        int gap = (int)(slot * 0.35f);
        int total = JW_FOCUS_PIN_LEN * slot + (JW_FOCUS_PIN_LEN - 1) * gap;
        int x0 = cx - total / 2;
        TTF_Font *digit_font = cat_get_font(CAT_FONT_LARGE);
        for (int i = 0; i < JW_FOCUS_PIN_LEN; i++) {
            int x = x0 + i * (slot + gap);
            bool active = (i == v->pin_slot);
            SDL_Color bcol = active ? pal.sel : pal.border;
            int bw2 = active ? border + 1 : border;
            cat_draw_rounded_rect(x, y, slot, slot, radius / 2, bcol);
            cat_draw_rounded_rect(x + bw2, y + bw2, slot - 2 * bw2,
                                  slot - 2 * bw2, radius / 2 - bw2, pal.tile_bg);
            char d[2] = { (char)('0' + (v->pin[i] % 10)), '\0' };
            int dw = cat_measure_text(digit_font, d);
            int dh = jw__focus_fh(digit_font);
            cat_draw_text(digit_font, d, x + (slot - dw) / 2,
                          y + (slot - dh) / 2, pal.text);
        }
        y += slot_block;
    }

    /* Error note. */
    if (err_block) {
        SDL_Color err = bw ? (SDL_Color){ 255, 255, 255, 255 }
                           : (SDL_Color){ 0xFF, 0x3B, 0x30, 0xFF };
        jw__focus_text_centered(hint_font,
                                v->error_text ? v->error_text
                                              : "Wrong PIN, try again",
                                cx, y, err);
    }

    /* Two-column button legend, bottom-anchored. */
    int legend_y = py + ph - pad - legend_h;
    int left_x = px + (int)(pw * 0.12f);
    int right_x = px + (int)(pw * 0.54f);
    for (int r = 0; r < v->row_count; r++) {
        int ry = legend_y + r * rh;
        if (v->rows[r].center) {
            const char *k = v->rows[r].lkey ? v->rows[r].lkey : "";
            const char *l = v->rows[r].llabel ? v->rows[r].llabel : "";
            char kbuf[24];
            snprintf(kbuf, sizeof(kbuf), "%s:", k);
            int total = cat_measure_text(hint_font, kbuf);
            if (l[0]) total += cat_measure_text(hint_font, " ") +
                               cat_measure_text(hint_font, l);
            jw__focus_hint_cell(cx - total / 2, ry, v->rows[r].lkey,
                                v->rows[r].llabel, pal.sel, pal.text);
        } else {
            jw__focus_hint_cell(left_x, ry, v->rows[r].lkey, v->rows[r].llabel,
                                pal.sel, pal.text);
            jw__focus_hint_cell(right_x, ry, v->rows[r].rkey, v->rows[r].rlabel,
                                pal.sel, pal.text);
        }
    }
}
