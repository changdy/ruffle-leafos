#include "internal/launcher/grid_games.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Panel geometry, all in logical units so it scales with the panel. */
#define GG_MARGIN     16
#define GG_GAP        20
#define GG_TOPBAR     40
#define GG_ROW_PAD    10
#define GG_CHIP_H     32
#define GG_CHIP_GAP    8
#define GG_SYN_MAX_LINES 24
#define GG_SYN_SCROLL_MS 900   /* per line */
#define GG_SYN_HOLD_MS  2200   /* pause at each end */
#define GG_MARQUEE_HOLD_MS   1400  /* pause at each end of a too-wide chip */
#define GG_MARQUEE_MS_PER_PX   18  /* creep speed, ~55px a second */
#define GG_WORDMARK_H 96   /* tall enough that a typical wordmark spans the column */
#define GG_ART_PCT    50   /* cover owns the top half of the right column */

static int gg_list_w(void) {
    /* Just under half: the cover needs the wider side because the screen is
       landscape and most covers are not. */
    return (cat_get_screen_width() * 47) / 100;
}

/* The list is the small font: a game list is long and its names are long, so
   rows that fit beat rows that shout. */
static TTF_Font *gg_row_font(void) { return cat_get_font(CAT_FONT_SMALL); }

static int gg_row_h(void) {
    TTF_Font *f = gg_row_font();
    return TTF_FontHeight(f) + cat_scale(GG_ROW_PAD);
}


/* Text on a translucent panel is derived, never authored: a theme cannot end up
   with white on white by accident. Same rule as the status polarity over a
   wallpaper. */
static cat_draw_color gg_ink(cat_draw_color under) {
    unsigned luma = (under.r * 299u + under.g * 587u + under.b * 114u) / 1000u;
    /* A sheer panel shows the wallpaper through it, so bias toward the ink that
       survives either way rather than trusting the panel colour alone. */
    if (under.a < 110) return (cat_draw_color){ 0xF2, 0xF5, 0xEF, 0xFF };
    /* A soft neutral charcoal rather than near-black: on a sheer white panel over
       a wallpaper, true black reads harsh and fights the artwork. */
    return luma > 140 ? (cat_draw_color){ 0x41, 0x47, 0x4A, 0xFF }
                      : (cat_draw_color){ 0xEE, 0xF1, 0xEC, 0xFF };
}

/* SDL2's renderer has no blur, so the soft edge is four rounded rects at
   falling alpha. They are drawn as a ring around the panel, never under it: a
   translucent panel over black reads grey no matter how opaque it is. */
static void gg_panel(int x, int y, int w, int h, int r,
                     const jw_grid_games_style *st) {
    if (w <= 0 || h <= 0) return;
    if (st->shadow > 0) {
        for (int i = 3; i >= 1; i -= 2) {
            int g = cat_scale(3) * i;
            int a = (st->shadow * (4 - i)) / 14;
            if (a <= 0) continue;
            cat_draw_rounded_rect(x - g, y - g + cat_scale(3), w + g * 2, h + g * 2,
                                  r + g, (cat_draw_color){ 0, 0, 0, (Uint8)a });
        }
    }
    cat_draw_rounded_rect(x, y, w, h, r, st->underlay);
}

/* Bounded copy. snprintf("%s") from a wider buffer trips gcc's truncation
   analysis even when the result always fits, so copy by length instead. */
static void gg_copy(char *dst, size_t n, const char *src) {
    if (!dst || n == 0) return;
    size_t len = src ? strlen(src) : 0;
    if (len > n - 1) len = n - 1;
    if (len) memcpy(dst, src, len);
    dst[len] = '\0';
}


/* Fit `src` to `maxw`, appending an ellipsis, and cache the result: the caller
   redraws every frame while a cover decodes, and re-fitting a long name means a
   binary search of harfbuzz shaping passes each time. Recomputed only when the
   text, width or font actually change. */
typedef struct {
    char      src[256];
    char      out[256];
    int       w, maxw;
    TTF_Font *font;
} gg_fit_slot;

static const char *gg_fit(gg_fit_slot *slot, TTF_Font *f, const char *src,
                          int maxw, int *out_w) {
    if (!src) src = "";
    if (slot->font == f && slot->maxw == maxw &&
        strncmp(slot->src, src, sizeof(slot->src) - 1) == 0) {
        if (out_w) *out_w = slot->w;
        return slot->out;
    }
    gg_copy(slot->src, sizeof(slot->src), src);
    slot->font = f;
    slot->maxw = maxw;
    gg_copy(slot->out, sizeof(slot->out), src);
    if (cat_measure_text(f, slot->out) > maxw) {
        size_t n = strlen(slot->out);
        while (n > 0) {
            n--;
            while (n > 0 && ((unsigned char)slot->out[n] & 0xC0) == 0x80) n--;
            if (n + 4 > sizeof(slot->out)) continue;
            slot->out[n] = '.'; slot->out[n+1] = '.'; slot->out[n+2] = '.';
            slot->out[n+3] = '\0';
            if (cat_measure_text(f, slot->out) <= maxw) break;
            slot->out[n] = '\0';
        }
    }
    slot->w = cat_measure_text(f, slot->out);
    if (out_w) *out_w = slot->w;
    return slot->out;
}

/* Step past one UTF-8 character. Continuation bytes are skipped one at a time
   rather than trusting the lead byte's length, so a truncated sequence at the
   end of the string can never step over its terminator. */
static const char *gg_utf8_next(const char *s) {
    if (!*s) return s;
    s++;
    while ((*s & 0xC0) == 0x80) s++;
    return s;
}

/* Greedy wrap: break at spaces, and inside a run only when the run is itself
   wider than the panel (a space-free script, or one very long token), always
   on a character boundary.
   Cost is what matters here, because this runs on every cursor move. Shaping
   a string costs in proportion to its length, so measuring the growing line
   once per word shapes each character many times over. Instead each word is
   measured once on its own and the line's width is the running sum. Only a
   word that the sum says will not fit gets the real line measured, and that
   measurement replaces the sum so rounding cannot drift. Each finished line
   is measured once as a whole and gives back its last pieces if kerning or
   font fallback made it wider than the sum said. No line can overflow, and a
   synopsis costs one short measurement per word plus about two per line. */
typedef struct {
    const char *at;             /* where the piece starts in the source text */
    size_t      cl;             /* line length before the piece was added */
} gg_piece;

static int gg_wrap(TTF_Font *f, const char *text, int maxw,
                   char lines[][256], int max_lines) {
    if (!text || !text[0] || max_lines <= 0) return 0;

    enum { GG_LINE = 256 };
    char cur[GG_LINE];
    gg_piece piece[GG_LINE];
    size_t cl = 0;
    int cw = 0, pieces = 0, n = 0;
    /* A lone space measures oddly (no ink, so no extent); take its advance
       from between two glyphs instead. */
    int xx_w = cat_measure_text(f, "xx");
    int space_w = cat_measure_text(f, "x x") - xx_w;
    /* What measuring in pieces over-counts per piece: the extent a glyph
       reaches past its advance, counted once per piece instead of once per
       line. Taking it off keeps the sum close enough that the real line is
       rarely measured before it is actually full. */
    int piece_bias = 2 * cat_measure_text(f, "x") - xx_w;
    if (piece_bias < 0) piece_bias = 0;
    const char *p = text;
    cur[0] = '\0';

    while (n < max_lines && (*p || cl > 0)) {
        if (!*p) goto commit;
        if (*p == ' ' && cl == 0) { p++; continue; }   /* a line never opens on a space */

        {
            const char *w = p;
            while (*w && *w != ' ') w = gg_utf8_next(w);
            size_t wlen = (size_t)(w - p);
            const char *run_end = w;

            if (cl + wlen < GG_LINE) {
                memcpy(cur + cl, p, wlen);
                cur[cl + wlen] = '\0';
                int est = cw + cat_measure_text(f, cur + cl) - (cl > 0 ? piece_bias : 0);
                if (est > maxw && cl > 0) est = cat_measure_text(f, cur);
                if (est <= maxw) {
                    piece[pieces].at = p; piece[pieces].cl = cl; pieces++;
                    cl += wlen; cw = est;
                    p = w;
                    goto spaces;
                }
                cur[cl] = '\0';
            }
            if (cl > 0) goto commit;                    /* try it on a fresh line */

            /* A run wider than the panel: walk its characters. The first one on
               a line is always taken, so even a panel narrower than a glyph
               makes progress. */
            while (p < run_end) {
                const char *nx = gg_utf8_next(p);
                size_t clen = (size_t)(nx - p);
                if (cl + clen >= GG_LINE) {
                    if (cl == 0) p = nx;                /* malformed; drop it */
                    break;
                }
                memcpy(cur + cl, p, clen);
                cur[cl + clen] = '\0';
                int est = cw + cat_measure_text(f, cur + cl) - (cl > 0 ? piece_bias : 0);
                if (est > maxw && cl > 0) est = cat_measure_text(f, cur);
                if (est > maxw && cl > 0) {
                    cur[cl] = '\0';
                    break;
                }
                piece[pieces].at = p; piece[pieces].cl = cl; pieces++;
                cl += clen; cw = est;
                p = nx;
            }
            if (p < run_end) goto commit;               /* the rest goes on the next line */
        }

    spaces:
        while (*p == ' ') {                             /* spaces may hang past the edge */
            if (cl + 1 < GG_LINE) { cur[cl++] = ' '; cw += space_w; }
            p++;
        }
        cur[cl] = '\0';
        continue;

    commit:
        while (cl > 0 && cur[cl - 1] == ' ') cur[--cl] = '\0';
        while (pieces > 1 && cat_measure_text(f, cur) > maxw) {
            pieces--;                                   /* hand the last piece back */
            p = piece[pieces].at;
            cl = piece[pieces].cl;
            cur[cl] = '\0';
            while (cl > 0 && cur[cl - 1] == ' ') cur[--cl] = '\0';
        }
        if (cl > 0) gg_copy(lines[n++], GG_LINE, cur);
        cl = 0; cw = 0; pieces = 0;
        cur[0] = '\0';
    }
    return n;
}


/* Rows go through Catastrophe's layered list pane, the same widget the tabbed
   browser uses: the highlight is its own layer that eases between rows while the
   row content stays put, and the pane owns scrolling and the scrollbar. Drawing
   the rows by hand meant no sliding focus and a list that felt slower than every
   other view in Leaf. */
typedef struct {
    jw_grid_games_name_fn name_fn;
    void          *ctx;
    TTF_Font      *font;
    cat_draw_color ink, sel_ink, highlight;
    int            pad_x;
} gg_list_ctx;

static void gg_focus_draw(int x, int y, int w, int h, void *user) {
    gg_list_ctx *c = (gg_list_ctx *)user;
    int pill_h = TTF_FontHeight(c->font) + cat_scale(8);
    cat_draw_pill(x, y + (h - pill_h) / 2, w, pill_h, c->highlight);
}

static void gg_item_draw(int idx, int x, int y, int w, int h, float focus, void *user) {
    gg_list_ctx *c = (gg_list_ctx *)user;
    static gg_fit_slot row_fit[32];
    const char *label = gg_fit(&row_fit[idx % 32], c->font,
                               c->name_fn ? c->name_fn(c->ctx, idx) : "",
                               w - c->pad_x * 2, NULL);
    /* Colour follows the moving highlight rather than snapping at either end. */
    cat_draw_color col = cat_draw_color_lerp(c->ink, c->sel_ink, focus);
    cat_draw_text(c->font, label, x + c->pad_x,
                  y + (h - TTF_FontHeight(c->font)) / 2, col);
}

void jw_grid_games_draw(const cat_list_state *ls, int count,
                        jw_grid_games_name_fn name_fn,
                        jw_grid_games_art_fn art_fn, void *ctx,
                        const jw_grid_games_meta *meta, int meta_count,
                        const char *synopsis,
                        SDL_Texture *wordmark, int wm_w, int wm_h, bool wm_color,
                        const char *system_name, int top_bar,
                        const jw_grid_games_style *st) {
    if (!ls || !st) return;

    TTF_Font *body  = cat_get_font(CAT_FONT_MEDIUM);
    TTF_Font *rowf  = gg_row_font();
    /* The info section runs a tier below the rest of the view: seven labelled
       facts and a synopsis in one column is dense, and at the list's size they
       crowd each other and truncate. */
    /* Info values carry no label: a year, a rating, a player count and a genre
       each read from their own shape, so the words naming them were spending
       half of every box to say what the value already says. Small type suits
       them -- they are reference, not the thing being read. */
    TTF_Font *small = cat_get_font(CAT_FONT_TINY);
    if (!body || !small) return;

    const int M   = cat_scale(GG_MARGIN);
    const int GAP = cat_scale(GG_GAP);
    const int top = top_bar > 0 ? top_bar : cat_scale(GG_TOPBAR);
    const int sw  = cat_get_screen_width();
    const int sh  = cat_get_screen_height();
    const int r   = st->radius;

    /* A theme may state its text colour; otherwise it is derived from the
       underlay so a theme cannot end up with white on white. */
    cat_draw_color ink = st->ink.a ? st->ink : gg_ink(st->underlay);

    /* ── left: the list ─────────────────────────────────────────────────── */
    int lx = M, ly = top, lw = gg_list_w(), lh = sh - top - M;
    gg_panel(lx, ly, lw, lh, r, st);

    int row_h = gg_row_h();
    int rows  = lh / row_h;
    if (rows < 1) rows = 1;
    ((cat_list_state *)ls)->visible_rows = rows;

    gg_list_ctx lctx = {
        .name_fn   = name_fn,
        .ctx       = ctx,
        .font      = rowf,
        .ink       = ink,
        .sel_ink   = st->highlight_text,
        .highlight = st->highlight,
        .pad_x     = cat_scale(20),
    };
    cat_draw_list_pane_layered(lx + cat_scale(8), ly + cat_scale(6),
                               lw - cat_scale(16), lh - cat_scale(12),
                               count, ls, row_h,
                               gg_focus_draw, gg_item_draw, &lctx);

    /* ── right: cover, then whatever facts we have ──────────────────────── */
    int rx = lx + lw + GAP;
    int rw = sw - rx - M;

    /* The right column is three fixed zones, top to bottom: the cover in the
       top half, the game's info under it, then the wordmark. Fixed on purpose --
       sizing the cover from what is left over made it swell whenever a game had
       no metadata, so the same cover changed size depending on whether it had
       been played. */
    const int col_h  = sh - top - M;
    const int wm_slot = cat_scale(GG_WORDMARK_H);
    const int art_h   = col_h * GG_ART_PCT / 100;
    const int info_y  = top + art_h + GAP;
    const int info_h  = sh - M - wm_slot - info_y;

    /* Chips flow across three columns and a chip may claim two of them, so the
       row count comes from the flow rather than from the count. */
    int chip_rows = 0;
    {
        int used = 0;
        for (int i = 0; i < meta_count && i < JW_GRID_GAMES_MAX_META; ++i) {
            int span = meta[i].span < 1 ? 1 : (meta[i].span > 3 ? 3 : meta[i].span);
            if (used == 0 || used + span > 3) { chip_rows++; used = 0; }
            used += span;
        }
    }
    int chip_h    = chip_rows > 0
                  ? chip_rows * cat_scale(GG_CHIP_H) + (chip_rows - 1) * cat_scale(GG_CHIP_GAP)
                  : 0;
    if (chip_h > info_h) chip_h = info_h;

    /* The synopsis takes whatever the chips leave, and wraps to fit it. */
    int syn_avail = info_h - chip_h - (chip_h ? cat_scale(GG_CHIP_GAP) : 0);
    /* The synopsis is the one long-form block here, so it takes the smallest
       tier and the tightest leading: it is read as a paragraph, not scanned. */
    TTF_Font *syn_f = cat_get_font(CAT_FONT_MICRO);
    int syn_line  = TTF_FontHeight(syn_f) + cat_scale(1);
    int syn_vis   = syn_avail > cat_scale(16) ? (syn_avail - cat_scale(16)) / syn_line : 0;
    if (syn_vis > GG_SYN_MAX_LINES) syn_vis = GG_SYN_MAX_LINES;
    /* Wrap the whole blurb, not just the part that fits: the extra lines are
       what the panel scrolls through. */
    static struct {
        char      text[1400];
        int       maxw;
        TTF_Font *font;
        char      lines[GG_SYN_MAX_LINES][256];
        int       n;
    } syn_cache;
    int syn_wrap_w = rw - cat_scale(26);
    if (synopsis && syn_vis > 0) {
        if (syn_cache.font != syn_f || syn_cache.maxw != syn_wrap_w ||
            strcmp(syn_cache.text, synopsis) != 0) {
            gg_copy(syn_cache.text, sizeof(syn_cache.text), synopsis);
            syn_cache.font = syn_f;
            syn_cache.maxw = syn_wrap_w;
            syn_cache.n = gg_wrap(syn_f, synopsis, syn_wrap_w,
                                  syn_cache.lines, GG_SYN_MAX_LINES);
        }
    } else {
        syn_cache.text[0] = '\0';
        syn_cache.font = NULL;
        syn_cache.n = 0;
    }
    char (*syn_lines)[256] = syn_cache.lines;
    int syn_n = syn_cache.n;
    int syn_shown = syn_n < syn_vis ? syn_n : syn_vis;
    int syn_h = syn_shown ? cat_scale(12) + syn_shown * syn_line + cat_scale(6) : 0;

    /* The cover draws pure: no plate, no shadow, no rounding of its own. A
       portrait cover in a landscape slot simply sits narrower, which is the
       nature of the shape rather than something to crop away. */
    int aw = 0, ah = 0;
    SDL_Texture *art = art_fn ? art_fn(ctx, ls->cursor, &aw, &ah) : NULL;
    if (art && aw > 0 && ah > 0) {
        float s = (float)rw / (float)aw;
        float sy = (float)art_h / (float)ah;
        if (sy < s) s = sy;
        int dw = (int)(aw * s), dh = (int)(ah * s);
        SDL_Rect dst = { rx + (rw - dw) / 2, top + (art_h - dh) / 2, dw, dh };
        SDL_RenderCopy(cat_get_renderer(), art, NULL, &dst);
    }

    int y = info_y;
    int cw = (rw - cat_scale(GG_CHIP_GAP) * 2) / 3;
    int used = 0, crow = -1;
    for (int i = 0; i < meta_count && i < JW_GRID_GAMES_MAX_META; ++i) {
        int span = meta[i].span < 1 ? 1 : (meta[i].span > 3 ? 3 : meta[i].span);
        if (crow < 0 || used + span > 3) { crow++; used = 0; }
        int cx  = rx + used * (cw + cat_scale(GG_CHIP_GAP));
        int cyw = span * cw + (span - 1) * cat_scale(GG_CHIP_GAP);
        int cy  = y + crow * (cat_scale(GG_CHIP_H) + cat_scale(GG_CHIP_GAP));
        int chh = cat_scale(GG_CHIP_H);
        used += span;
        gg_panel(cx, cy, cyw, chh, cat_scale(10), st);
        if (meta[i].stars > 0) {
            /* Five stars, filled to the nearest half. Drawn rather than set in
               type: the user can change the font family, and a star glyph is not
               guaranteed to survive that. */
            int r    = chh / 4;
            int step = r * 5 / 2;
            int sx   = cx + (cyw - (step * 4 + r * 2)) / 2 + r;
            int sy   = cy + chh / 2;
            cat_draw_color dim = ink;
            dim.a = 70;
            for (int k = 0; k < 5; k++) {
                int filled = meta[i].stars - k * 2;   /* 2 = full, 1 = half */
                cat_draw_star(sx, sy, r, filled >= 2 ? ink : dim);
                if (filled == 1) {
                    /* Half: redraw the left side over the dim star. */
                    SDL_Renderer *rr = cat_get_renderer();
                    SDL_Rect clip_prev, half = { sx - r, sy - r, r, r * 2 };
                    SDL_RenderGetClipRect(rr, &clip_prev);
                    SDL_RenderSetClipRect(rr, &half);
                    cat_draw_star(sx, sy, r, ink);
                    if (clip_prev.w == 0 && clip_prev.h == 0) SDL_RenderSetClipRect(rr, NULL);
                    else                                      SDL_RenderSetClipRect(rr, &clip_prev);
                }
                sx += step;
            }
        } else {
            int pad   = cat_scale(10);
            int inner = cyw - pad * 2;
            int tw = 0, th = 0;
            TTF_SizeUTF8(small, meta[i].value, &tw, &th);
            int ty = cy + (chh - TTF_FontHeight(small)) / 2;

            if (tw <= inner) {
                cat_draw_text(small, meta[i].value, cx + (cyw - tw) / 2, ty, ink);
            } else {
                /* Wider than its chip: creep it through rather than cut it. A
                   pak's author is often longer than a year or a playtime, and
                   half a name tells you less than the whole one. Held at each
                   end so both are readable. State is per chip slot and resets
                   whenever the text changes, which is on every selection. */
                static char     mq_text[JW_GRID_GAMES_MAX_META][128];
                static uint32_t mq_start[JW_GRID_GAMES_MAX_META];
                int slot = i % JW_GRID_GAMES_MAX_META;
                uint32_t now = SDL_GetTicks();
                if (strcmp(mq_text[slot], meta[i].value) != 0) {
                    gg_copy(mq_text[slot], sizeof(mq_text[slot]), meta[i].value);
                    mq_start[slot] = now;
                }

                float travel = (float)(tw - inner);
                float creep  = travel * (float)GG_MARQUEE_MS_PER_PX;
                float total  = GG_MARQUEE_HOLD_MS * 2.0f + creep;
                float t      = fmodf((float)(now - mq_start[slot]), total);
                int   off;
                if (t < GG_MARQUEE_HOLD_MS)             off = 0;
                else if (t < GG_MARQUEE_HOLD_MS + creep)
                    off = (int)((t - GG_MARQUEE_HOLD_MS) / (float)GG_MARQUEE_MS_PER_PX);
                else                                    off = (int)travel;

                SDL_Renderer *rr = cat_get_renderer();
                SDL_Rect prev, clip = { cx + pad, cy, inner, chh };
                SDL_RenderGetClipRect(rr, &prev);
                SDL_RenderSetClipRect(rr, &clip);
                cat_draw_text(small, meta[i].value, cx + pad - off, ty, ink);
                if (prev.w == 0 && prev.h == 0) SDL_RenderSetClipRect(rr, NULL);
                else                            SDL_RenderSetClipRect(rr, &prev);
                cat_request_frame();
            }
        }
    }
    y += chip_h + (chip_h ? cat_scale(GG_CHIP_GAP) : 0);

    if (syn_shown) {
        gg_panel(rx, y, rw, syn_h, cat_scale(10), st);

        /* Longer than the panel: creep through it, pausing at each end so the
           first and last lines are actually readable. State is function-static
           because only one synopsis is on screen, and it resets whenever the
           text changes. */
        float first_line = 0.0f;
        if (syn_n > syn_shown) {
            static char     last_text[128];
            static uint32_t started_ms;
            uint32_t now = SDL_GetTicks();
            char key[128];
            gg_copy(key, sizeof(key), syn_lines[0]);
            if (strcmp(key, last_text) != 0) {
                gg_copy(last_text, sizeof(last_text), key);
                started_ms = now;
            }
            int   over    = syn_n - syn_shown;
            float travel  = (float)over * (float)GG_SYN_SCROLL_MS;
            float total   = GG_SYN_HOLD_MS * 2.0f + travel;
            float t       = fmodf((float)(now - started_ms), total);
            if (t < GG_SYN_HOLD_MS)                 first_line = 0.0f;
            else if (t < GG_SYN_HOLD_MS + travel)   first_line = (t - GG_SYN_HOLD_MS) / (float)GG_SYN_SCROLL_MS;
            else                                    first_line = (float)over;
            cat_request_frame();               /* keep it moving */
        }

        /* Clipped so a part-scrolled line is cut rather than spilling past the
           panel. No render target changes inside, so the rect is safe here. */
        SDL_Renderer *r = cat_get_renderer();
        SDL_Rect prev_clip;
        SDL_RenderGetClipRect(r, &prev_clip);
        SDL_Rect clip = { rx, y + cat_scale(6), rw, syn_h - cat_scale(10) };
        SDL_RenderSetClipRect(r, &clip);

        int base = y + cat_scale(10) - (int)(first_line * (float)syn_line);
        for (int i = 0; i < syn_n; ++i) {
            int ty = base + i * syn_line;
            if (ty + syn_line < clip.y || ty > clip.y + clip.h) continue;
            cat_draw_text(syn_f, syn_lines[i], rx + cat_scale(13), ty, ink);
        }

        if (prev_clip.w == 0 && prev_clip.h == 0) SDL_RenderSetClipRect(r, NULL);
        else                                      SDL_RenderSetClipRect(r, &prev_clip);
        y += syn_h;
    }

    /* System identity, bottom right, on the wallpaper with no panel of its own,
       tinted to match the list text. */
    int wy = sh - M - wm_slot;   /* inside the bottom margin, like every other zone */
    if (wordmark && wm_w > 0 && wm_h > 0) {
        /* Width first: a logo should span the column, and only give that up when
           its own proportions would push it past the slot's height. A wordmark
           is wide by nature, so most of them take the full width. */
        int dw = rw;
        int dh = (wm_h * dw + wm_w / 2) / wm_w;
        if (dh > wm_slot) {
            dh = wm_slot;
            dw = (wm_w * dh + wm_h / 2) / wm_h;
        }
        if (dh < 1) dh = 1;
        SDL_Rect dst = { rx + (rw - dw) / 2, wy + (wm_slot - dh) / 2, dw, dh };
        /* Tinted to the list's own ink. The art is authored white for exactly
           this, so the logo reads as part of the interface rather than as a
           sticker, and it stays legible when the underlay flips polarity. A
           .color wordmark carries its own colors and is drawn untinted. */
        SDL_SetTextureBlendMode(wordmark, SDL_BLENDMODE_BLEND);
        if (wm_color) SDL_SetTextureColorMod(wordmark, 255, 255, 255);
        else          SDL_SetTextureColorMod(wordmark, ink.r, ink.g, ink.b);
        SDL_SetTextureAlphaMod(wordmark, 255);
        SDL_RenderCopy(cat_get_renderer(), wordmark, NULL, &dst);
    } else if (system_name && system_name[0]) {
        static gg_fit_slot name_fit;
        int nw = 0;
        const char *fitted = gg_fit(&name_fit, body, system_name, rw, &nw);
        cat_draw_text(body, fitted, rx + (rw - nw) / 2,
                      wy + (wm_slot - TTF_FontHeight(body)) / 2,
                      (cat_draw_color){ 0xEE, 0xF4, 0xEA, 0xFF });
    }

}
