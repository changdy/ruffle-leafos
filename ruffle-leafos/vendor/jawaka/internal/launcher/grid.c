#include "internal/launcher/grid.h"
#include <math.h>

#include "catastrophe.h"
#include <SDL2/SDL.h>
#include <stdio.h>

static int jw__grid_max(int a, int b) { return a > b ? a : b; }

/* Ease-out: motion starts at full speed on the press and settles at the end.
   Ease-in-out barely moves for its first third, which reads as input lag. */
static float jw__grid_ease_out(float t) {
    if (t <= 0.0f) return 0.0f;
    if (t >= 1.0f) return 1.0f;
    float u = 1.0f - t;
    return 1.0f - u * u * u;
}
static int jw__grid_min(int a, int b) { return a < b ? a : b; }

void jw_grid_layout(jw_grid *g, const cat_stylesheet_launcher *l,
                    int screen_w, int screen_h, int status_h) {
    if (!g || !l) return;
    int cols   = jw__grid_max(1, l->grid_cols);
    int rows   = jw__grid_max(1, l->grid_rows);
    int margin = cat_scale(l->grid_margin);
    int gutter = cat_scale(l->grid_gutter);

    /* Largest square satisfying both axes; the non-binding axis gets slack. */
    int w_fit = (screen_w - 2 * margin - (cols - 1) * gutter) / cols;
    int avail_h = screen_h - status_h;
    int h_fit = (avail_h - 2 * margin - (rows - 1) * gutter) / rows;
    int tile  = jw__grid_max(8, jw__grid_min(w_fit, h_fit));

    int block_w = cols * tile + (cols - 1) * gutter;
    int block_h = rows * tile + (rows - 1) * gutter;

    g->cols     = cols;
    g->rows     = rows;
    g->tile     = tile;
    g->gutter   = gutter;
    g->radius   = tile * l->grid_radius_pct / 100;
    g->status_h = status_h;
    g->x0       = (screen_w - block_w) / 2;
    /* Top-anchored: the first row sits half a gutter below the status band and
       the vertical slack goes to the bottom, per the plan. */
    g->y0       = status_h + gutter / 2;
    (void)block_h;
}

void jw_grid_reset(jw_grid *g) {
    if (!g) return;
    g->scroll_row    = 0;
    g->anim_from_row = 0.0f;
    g->anim_start_ms = 0;
    g->anim_active   = false;
    g->focus_prev    = -1;
    g->focus_anim_start_ms = 0;
}

static float jw__grid_scroll_now(const jw_grid *g, uint32_t now, uint32_t anim_ms) {
    if (!g->anim_active || anim_ms == 0) return (float)g->scroll_row;
    uint32_t el = now - g->anim_start_ms;
    if (el >= anim_ms) return (float)g->scroll_row;
    float t = jw__grid_ease_out((float)el / (float)anim_ms);
    return g->anim_from_row + ((float)g->scroll_row - g->anim_from_row) * t;
}

void jw_grid_step(jw_grid *g, cat_list_state *ls, int count, int dx, int dy,
                  uint32_t now, uint32_t anim_ms) {
    if (!g || !ls || count <= 0) return;
    int cols = g->cols, rows = g->rows;
    int idx  = ls->cursor;
    if (idx < 0 || idx >= count) idx = 0;

    if (dx != 0) {
        /* Linear: the grid is one list drawn in rows, wrapping at both ends. */
        idx = ((idx + dx) % count + count) % count;
    } else if (dy > 0) {
        int cand = idx + cols;
        if (cand < count) {
            idx = cand;
        } else if (idx / cols == (count - 1) / cols) {
            /* Last row: wrap to the same column in the first row. */
            int c = idx % cols;
            idx = (c < count) ? c : 0;
        } else {
            /* A row exists beneath but this column is empty in it. */
            idx = count - 1;
        }
    } else if (dy < 0) {
        int cand = idx - cols;
        if (cand >= 0) {
            idx = cand;
        } else {
            /* First row: wrap to the same column in the last row, else the last tile. */
            int last_start = ((count - 1) / cols) * cols;
            int cand2 = last_start + idx % cols;
            idx = (cand2 < count) ? cand2 : count - 1;
        }
    }
    if (idx != ls->cursor) {
        g->focus_prev = ls->cursor;
        g->focus_anim_start_ms = now;
    }
    ls->cursor = idx;

    int row        = idx / cols;
    int total_rows = (count + cols - 1) / cols;
    int max_scroll = jw__grid_max(0, total_rows - rows);
    int target     = g->scroll_row;
    if (row < target)              target = row;
    else if (row >= target + rows) target = row - rows + 1;
    if (target > max_scroll) target = max_scroll;
    if (target < 0) target = 0;

    if (target != g->scroll_row) {
        g->anim_from_row = jw__grid_scroll_now(g, now, anim_ms);
        g->scroll_row    = target;
        g->anim_start_ms = now;
        g->anim_active   = (anim_ms > 0);
    }
}

static SDL_Texture *jw__grid_scratch(jw_grid *g, int size) {
    if (g->scratch_size != size) {
        for (int i = 0; i < g->scratch_count; i++)
            if (g->scratch[i]) { SDL_DestroyTexture(g->scratch[i]); g->scratch[i] = NULL; }
        g->scratch_count = 0;
        g->scratch_next  = 0;
        g->scratch_size  = size;
    }
    int i = g->scratch_next;
    if (i >= JW_GRID_SCRATCH_MAX) i = 0;
    if (!g->scratch[i]) {
        SDL_Renderer *r = cat_get_renderer();
        g->scratch[i] = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                          SDL_TEXTUREACCESS_TARGET, size, size);
        if (!g->scratch[i]) return NULL;
        SDL_SetTextureBlendMode(g->scratch[i], SDL_BLENDMODE_BLEND);
        if (i + 1 > g->scratch_count) g->scratch_count = i + 1;
    }
    g->scratch_next = (i + 1) % JW_GRID_SCRATCH_MAX;
    return g->scratch[i];
}

static void jw__grid_draw_tile(jw_grid *g, int x, int y, int size, SDL_Texture *art,
                               int aw, int ah, SDL_Texture *label,
                               int bw, cat_draw_color bc, int radius) {
    int inner = g->tile;
    if (inner <= 0 || size <= 0) return;
    SDL_Renderer *r = cat_get_renderer();
    SDL_Texture *scratch = jw__grid_scratch(g, inner);
    if (!scratch) return;

    /* `size` exceeds the cell when the tile is focused; centre the growth. */
    int off = (size - g->tile) / 2;
    int sx = x - off, sy = y - off;
    int bwc = bw < 0 ? 0 : (bw > inner / 4 ? inner / 4 : bw);
    /* Border and radius are authored against the base tile and scale with it, so
       a focused tile grows whole rather than growing a fixed-width ring. */
    int bws = bwc * size / inner;
    if (bws < 1 && bwc > 0) bws = 1;
    int rad_out = radius * size / inner;

    /* The ring goes down first and the art lands inside it. Composing the art
       full-bleed and the ring on top would need a ring primitive; this way a
       filled rounded rect is all it takes, and the art still reaches the edge. */
    cat_draw_rounded_rect(sx, sy, size, size, rad_out, bc);

    SDL_Texture *prev = SDL_GetRenderTarget(r);
    SDL_Rect prev_vp, prev_clip;
    SDL_bool had_clip = SDL_RenderIsClipEnabled(r);
    SDL_RenderGetViewport(r, &prev_vp);
    SDL_RenderGetClipRect(r, &prev_clip);
    SDL_SetRenderTarget(r, scratch);
    /* Viewport and clip carry across a target switch. Left as they were, a fill
       or clear covers only part of the tile and the rest keeps whatever the GPU
       memory held — which is where the torn tiles came from. */
    SDL_RenderSetViewport(r, NULL);
    SDL_RenderSetClipRect(r, NULL);

    /* Cover every pixel of the target explicitly rather than trusting a clear:
       the plate is opaque, so this both clears and lays the backing down in one
       pass, and an explicit rect cannot be trimmed by a stale viewport. */
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_NONE);

    /* Plate behind the art: art with transparency (the cut-out console icons)
       reads as a tile rather than as a hole in the wallpaper. Opaque, the theme
       background lifted a little toward white. */
    cat_draw_color pbg = cat_get_theme()->background;
    SDL_Rect whole = { 0, 0, inner, inner };
    SDL_SetRenderDrawColor(r, (Uint8)(pbg.r + (255 - pbg.r) * 16 / 255),
                              (Uint8)(pbg.g + (255 - pbg.g) * 16 / 255),
                              (Uint8)(pbg.b + (255 - pbg.b) * 16 / 255), 255);
    SDL_RenderFillRect(r, &whole);
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    if (art && aw > 0 && ah > 0) {
        /* Cover-fit: the art fills the tile edge to edge and the mask below
           rounds it to the tile's own corners, so square art sits in the tile
           exactly. Off-square art is centred and cropped, never distorted. */
        float sc = (float)inner / (float)(aw < ah ? aw : ah);
        int dw = (int)(aw * sc + 0.5f), dh = (int)(ah * sc + 0.5f);
        SDL_Rect dst = { (inner - dw) / 2, (inner - dh) / 2, dw, dh };
        SDL_RenderCopy(r, art, NULL, &dst);
    }
    if (label) {
        /* Full-tile overlay: the author positioned the wordmark within a tile-
           sized canvas, so it maps onto the tile edge to edge. */
        SDL_Rect full = { 0, 0, inner, inner };
        SDL_RenderCopy(r, label, NULL, &full);
    }

    /* Round the composite off to the shape the ring leaves for it. Where the
       renderer rejects the custom blend the corners simply stay square. */
    cat_mask_rounded_rect(0, 0, inner, inner, jw__grid_max(0, radius - bwc), CAT_CORNER_ALL);

    SDL_SetRenderTarget(r, prev);
    SDL_RenderSetViewport(r, &prev_vp);
    if (had_clip) SDL_RenderSetClipRect(r, &prev_clip);
    else          SDL_RenderSetClipRect(r, NULL);
    /* Land the composition before the texture is sampled: SDL batches draws, and
       a batch that both writes and reads a target texture is where stale pixels
       creep in. */
    SDL_RenderFlush(r);
    SDL_Rect out = { sx + bws, sy + bws, size - 2 * bws, size - 2 * bws };
    SDL_RenderCopy(r, scratch, NULL, &out);
}

bool jw_grid_draw(jw_grid *g, const cat_stylesheet_launcher *l,
                  const cat_list_state *ls, int count,
                  jw_grid_icon_fn icon_fn, jw_grid_icon_fn label_fn, void *ctx,
                  uint32_t now, uint32_t anim_ms,
                  cat_draw_color border_override, cat_draw_color focus_override) {
    if (!g || !l || !ls || count <= 0) return false;
    g->scratch_next = 0;   /* one texture per tile per frame, reused next frame */
    int cols = g->cols, rows = g->rows, pitch = g->tile + g->gutter;
    float scroll = jw__grid_scroll_now(g, now, anim_ms);
    bool animating = g->anim_active && (now - g->anim_start_ms) < anim_ms;
    if (!animating) g->anim_active = false;

    cat_draw_color border = border_override.a ? border_override
                                              : cat_color_to_sdl(l->grid_border_color);
    /* Sentinel alpha 0 = the theme's selection colour (accent is the footer
       pill background in cat_theme, not the highlight). */
    cat_draw_color focus  = focus_override.a ? focus_override
                          : (CAT_COLOR_A(l->grid_focus_border_color) == 0)
                                ? cat_get_theme()->highlight
                                : cat_color_to_sdl(l->grid_focus_border_color);
    int bw_norm  = cat_scale(l->grid_border_w);
    int bw_focus = cat_scale(l->grid_focus_border_w);

    /* No renderer clip here: a clip rect stays in force across every render-
       target switch (the tile scratch, and cat_draw_image_rounded_ex's own),
       which clipped the clears and let stale texture rows through. The caller
       draws the status bar after the tiles, so it floats over anything that
       passes beneath it during a scroll. */

    /* Focus scale tween, shared duration with the row scroll. */
    float grow = (float)jw__grid_max(100, l->grid_focus_scale_pct) / 100.0f - 1.0f;
    float ft = 1.0f;
    if (anim_ms > 0 && g->focus_prev >= 0) {
        uint32_t el = now - g->focus_anim_start_ms;
        ft = el >= anim_ms ? 1.0f : jw__grid_ease_out((float)el / (float)anim_ms);
    }
    if (ft >= 1.0f) g->focus_prev = -1;
    else animating = true;
    int focus_idx = ls->cursor;

    /* Only the page's rows draw: at rest exactly `rows` of them, so no partial
       row peeks in below; mid-scroll one more, the row sliding in or out. */
    int total_rows = (count + cols - 1) / cols;
    int first = jw__grid_max(0, (int)floorf(scroll));
    int last  = jw__grid_min(total_rows - 1, (int)ceilf(scroll) + rows - 1);
    /* Pass 1: every tile but the focused one, so the focused tile can overlap
       its neighbours when it grows. The previously focused tile eases back. */
    for (int pass = 0; pass < 2; pass++) {
        for (int row = first; row <= last; row++) {
            int y = g->y0 + (int)(((float)row - scroll) * (float)pitch);
            for (int c = 0; c < cols; c++) {
                int idx = row * cols + c;
                if (idx >= count) break;
                bool focused = (idx == focus_idx);
                if ((pass == 0) == focused) continue;
                int x = g->x0 + c * pitch;
                int aw = 0, ah = 0;
                SDL_Texture *art = icon_fn ? icon_fn(ctx, idx, &aw, &ah) : NULL;
                int lw = 0, lh = 0;
                SDL_Texture *label = label_fn ? label_fn(ctx, idx, &lw, &lh) : NULL;
                float sc = 1.0f;
                if (focused)                     sc = 1.0f + grow * ft;
                else if (idx == g->focus_prev)   sc = 1.0f + grow * (1.0f - ft);
                int size = (int)((float)g->tile * sc);
                /* Border and radius are composed at the base tile size; the blit
                   scales them with the tile, which is what "the whole tile grows"
                   means. Nothing here pre-scales them. */
                jw__grid_draw_tile(g, x, y, size, art, aw, ah, label,
                                   focused ? bw_focus : bw_norm,
                                   focused ? focus : border, g->radius);
            }
        }
    }

    return animating;
}

/* The gamepad is Catastrophe's atlas icon, tinted through the same colour-mod
   path the status icons use. Its d-pad and buttons are transparent holes, so
   one colour is the whole glyph and it needs no second tone of its own. */
void jw_grid_draw_count(const jw_grid *g, int count, int screen_h,
                        cat_draw_color color,
                        SDL_Texture *glyph, int glyph_w, int glyph_h, int reserve_w) {
    if (!g || count < 0) return;
    TTF_Font *font = cat_get_font(CAT_FONT_SMALL);
    if (!font) return;

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", count);

    int th = TTF_FontHeight(font);
    /* A themed pad is drawn at the number's height, whatever it was authored
       at; the atlas glyph has one native size and is drawn at it. */
    bool have_glyph = glyph && glyph_w > 0 && glyph_h > 0;
    /* A mark that already fits the line draws 1:1. These are small hard-edged
       glyphs, and 24 px of art stretched to a 37 px line box is a blurred 1.5x
       upscale for no gain -- at native size the mark matches the height of the
       digits' own ink. Anything taller than the line is scaled down to fit. */
    int icon_h, icon_w;
    if (have_glyph) {
        icon_h = glyph_h <= th ? glyph_h : th;
        icon_w = glyph_h <= th ? glyph_w : (glyph_w * icon_h + glyph_h / 2) / glyph_h;
    } else {
        icon_h = cat_device_icon_px();   /* 0 = no atlas either */
        icon_w = icon_h;
    }
    int icon = icon_h;
    int gap  = icon_h > 0 ? cat_scale(6) : 0;
    int block_h = th > icon ? th : icon;
    /* The column the mark sits in. Wider than the mark whenever another mark is
       wider, which is what keeps the number still. */
    int slot_w = reserve_w > icon_w ? reserve_w : icon_w;

    /* The slack the layout left below the last row. Clamped both ways so a
       dense grid that leaves almost none still draws on screen. */
    int bottom = g->y0 + g->rows * (g->tile + g->gutter) - g->gutter;
    int y = bottom + (screen_h - bottom - block_h) / 2;
    if (y + block_h > screen_h) y = screen_h - block_h;
    if (y < bottom)             y = bottom;
    int x = cat_scale(16);

    if (have_glyph) {
        /* Tint through the same colour-mod path the atlas icons use, so the
           glyph and the status cluster stay the same colour. Right-aligned in
           the slot: a narrower mark indents from the left rather than dragging
           the number left with it. */
        SDL_Rect dst = { x + slot_w - icon_w, y + (block_h - icon_h) / 2, icon_w, icon_h };
        SDL_SetTextureBlendMode(glyph, SDL_BLENDMODE_BLEND);
        SDL_SetTextureColorMod(glyph, color.r, color.g, color.b);
        SDL_SetTextureAlphaMod(glyph, color.a);
        SDL_RenderCopy(cat_get_renderer(), glyph, NULL, &dst);
    } else if (icon > 0) {
        cat_draw_device_icon(CAT_DEVICE_ICON_CONTROLLER,
                             x + slot_w - icon, y + (block_h - icon) / 2, color);
    }
    cat_draw_text(font, buf, x + slot_w + gap, y + (block_h - th) / 2, color);
}
