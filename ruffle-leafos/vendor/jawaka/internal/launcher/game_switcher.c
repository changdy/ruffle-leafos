#include "internal/launcher/game_switcher.h"
#include "internal/core/title.h"

#include "internal/retroarch/states.h"
#include "internal/storage/sources.h"

#include "catastrophe.h"
#include "catastrophe_widgets.h"   /* cat_ui_feedback_emit */

#include <SDL2/SDL.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Slide animation: short ease-out so left/right feels snappy but not instant.
   Self-contained here (not tied to the launcher coverflow stylesheet fields) so
   the in-game overlay animates identically without depending on a theme. */
#define JW_SWITCHER_ANIM_MS 160u

/* Infinite-ring threshold. The render window draws the center tile +/- 2, so up
   to 5 tiles are on screen at once; below 5 entries a wrap would put the same
   tile on both sides of the window (a visible duplicate), so the carousel clamps
   at the ends instead of looping. At 5+ every visible slot is a distinct entry. */
#define JW_SWITCHER_LOOP_MIN 5

static bool jw__switcher_loops(const jw_game_switcher *sw) {
    return sw && sw->count >= JW_SWITCHER_LOOP_MIN;
}

typedef struct {
    const jw_storage_source *source;
    jw_state_thumb_index    *index;
} jw_switcher_thumb_source;

void jw_game_switcher_reset(jw_game_switcher *sw, bool in_game,
                            const char *sdcard_root, const char *states_dir) {
    if (!sw) {
        return;
    }
    memset(sw, 0, sizeof(*sw));
    sw->in_game = in_game;
    sw->current_index = -1;
    if (sdcard_root) {
        snprintf(sw->sdcard_root, sizeof(sw->sdcard_root), "%s", sdcard_root);
    }
    if (states_dir) {
        snprintf(sw->states_dir, sizeof(sw->states_dir), "%s", states_dir);
    }
}

static bool jw__switcher_set_thumb(jw_game_entry *entry, const char *thumb) {
    if (!entry || !thumb || !thumb[0]) {
        return false;
    }
    size_t tlen = strlen(thumb);
    if (tlen >= sizeof(entry->image_path)) {
        return false;
    }
    memcpy(entry->image_path, thumb, tlen + 1);
    return true;
}

static bool jw__switcher_find_thumb_in_index(jw_state_thumb_index *idx,
                                             jw_game_entry *entry) {
    if (!idx || !entry) {
        return false;
    }
    char thumb[PATH_MAX];
    if (!jw_state_thumb_index_find(idx, entry->rom_path, thumb, sizeof(thumb))) {
        return false;
    }
    return jw__switcher_set_thumb(entry, thumb);
}

void jw_game_switcher_resolve_thumbnails(jw_game_switcher *sw) {
    if (!sw || sw->count <= 0) {
        return;
    }

    jw_storage_source_list sources;
    bool have_sources = jw_storage_sources_resolve(sw->sdcard_root, &sources) == 0;
    jw_switcher_thumb_source thumbs[JW_STORAGE_MAX_SOURCES + 1];
    int thumb_count = 0;

    if (have_sources) {
        for (int i = 0; i < sources.count && thumb_count < JW_STORAGE_MAX_SOURCES; i++) {
            if (!sources.sources[i].states_path[0]) {
                continue;
            }
            jw_state_thumb_index *idx =
                jw_state_thumb_index_build(sources.sources[i].states_path);
            if (!idx) {
                continue;
            }
            thumbs[thumb_count].source = &sources.sources[i];
            thumbs[thumb_count].index = idx;
            thumb_count++;
        }
    }

    if (thumb_count == 0 && sw->states_dir[0]) {
        jw_state_thumb_index *idx = jw_state_thumb_index_build(sw->states_dir);
        if (idx) {
            thumbs[thumb_count].source = NULL;
            thumbs[thumb_count].index = idx;
            thumb_count++;
        }
    }

    if (thumb_count == 0) {
        return;
    }

    for (int i = 0; i < sw->count; i++) {
        jw_game_entry *entry = &sw->entries[i];
        bool found = false;

        if (have_sources) {
            const jw_storage_source *entry_source =
                jw_storage_sources_find_for_path(&sources, entry->rom_path);
            if (entry_source) {
                for (int t = 0; t < thumb_count; t++) {
                    if (thumbs[t].source != entry_source) {
                        continue;
                    }
                    found = jw__switcher_find_thumb_in_index(thumbs[t].index, entry);
                    break;
                }
            }
        }

        if (!found) {
            for (int t = 0; t < thumb_count; t++) {
                if (jw__switcher_find_thumb_in_index(thumbs[t].index, entry)) {
                    break;
                }
            }
        }
    }

    for (int t = 0; t < thumb_count; t++) {
        jw_state_thumb_index_free(thumbs[t].index);
    }
}

int jw_game_switcher_load(jw_game_switcher *sw, const char *db_path) {
    if (!sw) {
        return -1;
    }
    sw->count = 0;
    int rc = db_path ? jw_db_list_recent_games(db_path, sw->entries,
                                               JW_GAME_SWITCHER_MAX - 1,
                                               &sw->count)
                     : -1;
    if (rc != 0) {
        sw->count = 0;
    }
    if (sw->cursor >= sw->count) {
        sw->cursor = sw->count > 0 ? sw->count - 1 : 0;
    }
    sw->current_index = -1;
    sw->anim_active = false;
    return rc;
}

static const char *jw__switcher_basename(const char *path) {
    if (!path) {
        return "";
    }
    const char *slash = strrchr(path, '/');
    return slash && slash[1] ? slash + 1 : path;
}

static bool jw__switcher_resolve_rom(const jw_game_switcher *sw,
                                     const char *path,
                                     char *out,
                                     size_t out_size) {
    if (!path || !path[0] || !out || out_size == 0) {
        return false;
    }

    char candidate[PATH_MAX];
    if (path[0] == '/') {
        snprintf(candidate, sizeof(candidate), "%s", path);
    } else if (sw && sw->sdcard_root[0]) {
        if (snprintf(candidate, sizeof(candidate), "%s/%s",
                     sw->sdcard_root, path) >= (int)sizeof(candidate)) {
            return false;
        }
    } else {
        return false;
    }

    char resolved[PATH_MAX];
    if (!realpath(candidate, resolved)) {
        return false;
    }
    if (snprintf(out, out_size, "%s", resolved) >= (int)out_size) {
        return false;
    }
    return true;
}

/* A recents entry is the running game when the system and resolved ROM path
   match. Basename matching made sibling folders with the same ROM filename look
   like the current game, so only exact path identity is accepted now. */
static bool jw__switcher_same_game(const jw_game_switcher *sw,
                                   const jw_game_entry *entry,
                                   const char *system, const char *rom_path) {
    if (!entry || !system || !rom_path) {
        return false;
    }
    if (entry->system[0] && system[0] && strcmp(entry->system, system) != 0) {
        return false;
    }
    char entry_abs[PATH_MAX];
    char current_abs[PATH_MAX];
    if (jw__switcher_resolve_rom(sw, entry->rom_path, entry_abs, sizeof(entry_abs)) &&
        jw__switcher_resolve_rom(sw, rom_path, current_abs, sizeof(current_abs))) {
        return strcmp(entry_abs, current_abs) == 0;
    }
    return strcmp(entry->rom_path, rom_path) == 0;
}

void jw_game_switcher_set_current(jw_game_switcher *sw, const char *db_path,
                                  const char *system,
                                  const char *rom_path, const char *name,
                                  const char *image_path) {
    if (!sw || !rom_path || !rom_path[0]) {
        return;
    }

    /* Session paths are absolute; primary-card DB paths are SD-relative. */
    jw_game_entry game;
    int found = jw_db_get_game_by_rom_path(db_path, rom_path, &game);
    size_t root_len = strlen(sw->sdcard_root);
    if (found != 0 && root_len > 0 &&
        strncmp(rom_path, sw->sdcard_root, root_len) == 0 &&
        rom_path[root_len] == '/') {
        found = jw_db_get_game_by_rom_path(db_path, rom_path + root_len + 1, &game);
    }
    int hidden = 0;
    /* A pruned row may still have a hidden preference. Only inject known
       visible games; B resumes the session independently of this tile. */
    if (found != 0 || jw_db_is_game_hidden(db_path, game.id, &hidden) != 0 || hidden) {
        return;
    }

    for (int i = 0; i < sw->count; i++) {
        if (jw__switcher_same_game(sw, &sw->entries[i], system, rom_path)) {
            sw->current_index = i;
            sw->cursor = i;
            sw->anim_active = false;
            return;
        }
    }

    /* Not in Recents yet: insert a synthetic entry at the front and start there.
       id < 0 marks it un-removable (Y is refused on the running game). */
    if (sw->count >= JW_GAME_SWITCHER_MAX) {
        sw->count = JW_GAME_SWITCHER_MAX - 1; /* drop the oldest to make room */
    }
    for (int i = sw->count; i > 0; i--) {
        sw->entries[i] = sw->entries[i - 1];
    }
    sw->count++;

    jw_game_entry *cur = &sw->entries[0];
    memset(cur, 0, sizeof(*cur));
    cur->id = -1;
    cur->favorite = 0;
    snprintf(cur->system, sizeof(cur->system), "%s", system ? system : "");
    snprintf(cur->rom_path, sizeof(cur->rom_path), "%s", rom_path);
    snprintf(cur->name, sizeof(cur->name), "%s",
             name && name[0] ? name : jw__switcher_basename(rom_path));
    if (image_path) {
        snprintf(cur->image_path, sizeof(cur->image_path), "%s", image_path);
    }

    sw->current_index = 0;
    sw->cursor = 0;
    sw->anim_active = false;
}

bool jw_game_switcher_select(jw_game_switcher *sw, const char *system,
                             const char *rom_path) {
    if (!sw || !rom_path || !rom_path[0]) {
        return false;
    }
    for (int i = 0; i < sw->count; i++) {
        if (jw__switcher_same_game(sw, &sw->entries[i], system, rom_path)) {
            sw->cursor = i;
            sw->anim_active = false;
            return true;
        }
    }
    return false;
}

void jw_game_switcher_set_current_texture(jw_game_switcher *sw, SDL_Texture *tex) {
    if (!sw) {
        return;
    }
    sw->current_tex = tex; /* borrowed; caller retains ownership */
}

/* Current visual displacement of the strip from the (already-updated) cursor, in
   tile units. 0 at rest; eases from anim_from_offset to 0 over a slide. The tile
   at slot k (logical cursor + k) sits at screen distance (k - this) tiles. */
static float jw__switcher_visual_offset(const jw_game_switcher *sw) {
    if (!sw->anim_active) {
        return 0.0f;
    }
    uint32_t elapsed = SDL_GetTicks() - sw->anim_start_ms;
    if (elapsed >= JW_SWITCHER_ANIM_MS) {
        return 0.0f;
    }
    float t = (float)elapsed / (float)JW_SWITCHER_ANIM_MS;
    float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t); /* ease-out cubic */
    return sw->anim_from_offset * (1.0f - eased);
}

void jw_game_switcher_move(jw_game_switcher *sw, int delta) {
    if (!sw || delta == 0) return;
    if (sw->count <= 0) {
        cat_ui_feedback_emit(CAT_UI_EDGE);   /* nothing to move between */
        return;
    }
    bool loops = jw__switcher_loops(sw);

    int next;
    if (loops) {
        next = ((sw->cursor + delta) % sw->count + sw->count) % sw->count;
    } else {
        next = sw->cursor + delta;
        if (next < 0) next = 0;
        if (next > sw->count - 1) next = sw->count - 1;
    }
    if (next == sw->cursor) {
        cat_ui_feedback_emit(CAT_UI_EDGE);   /* clamped at an end */
        return;
    }
    cat_ui_feedback_emit(CAT_UI_MOVED);

    /* Visual step = how many tiles the strip slides on screen. Normally next -
       cursor, but a seam wrap (e.g. last -> first) is logically a big index jump
       yet visually a single tile in the press direction, so use delta there. */
    bool wrapped_seam = loops &&
                        ((delta > 0 && next < sw->cursor) ||
                         (delta < 0 && next > sw->cursor));
    int vstep = wrapped_seam ? delta : (next - sw->cursor);

    /* Carry any in-flight offset so a mid-slide press composes smoothly. */
    sw->anim_from_offset = jw__switcher_visual_offset(sw) - (float)vstep;
    sw->anim_start_ms = SDL_GetTicks();
    sw->anim_active = true;
    sw->cursor = next;
}

const jw_game_entry *jw_game_switcher_selected(const jw_game_switcher *sw) {
    if (!sw || sw->count <= 0 || sw->cursor < 0 || sw->cursor >= sw->count) {
        return NULL;
    }
    return &sw->entries[sw->cursor];
}

bool jw_game_switcher_selected_is_current(const jw_game_switcher *sw) {
    return sw && sw->current_index >= 0 && sw->cursor == sw->current_index;
}

bool jw_game_switcher_is_empty(const jw_game_switcher *sw) {
    return !sw || sw->count <= 0;
}

bool jw_game_switcher_remove_selected(jw_game_switcher *sw) {
    if (!sw || sw->count <= 0 || sw->cursor < 0 || sw->cursor >= sw->count) {
        return false;
    }
    if (jw_game_switcher_selected_is_current(sw)) {
        return false; /* never drop the running game from the overlay */
    }

    int removed = sw->cursor;
    for (int i = removed; i < sw->count - 1; i++) {
        sw->entries[i] = sw->entries[i + 1];
    }
    sw->count--;

    if (sw->current_index > removed) {
        sw->current_index--;
    }
    if (sw->cursor >= sw->count) {
        sw->cursor = sw->count > 0 ? sw->count - 1 : 0;
    }
    sw->anim_active = false;
    return true;
}

/* ── Rendering ─────────────────────────────────────────────────────────── */

static SDL_Texture *jw__switcher_cover(const jw_game_switcher *sw,
                                       const jw_game_entry *entry,
                                       int *out_w, int *out_h) {
    if (!entry || !entry->image_path[0]) {
        return NULL;
    }
    /* Sized to hold sdcard_root + '/' + image_path without truncation. */
    char abs[PATH_MAX + sizeof(entry->image_path) + 1];
    if (entry->image_path[0] == '/') {
        snprintf(abs, sizeof(abs), "%s", entry->image_path);
    } else {
        snprintf(abs, sizeof(abs), "%s/%s", sw->sdcard_root, entry->image_path);
    }

    int w = 0, h = 0;
    SDL_Texture *tex = cat_cache_get(abs, &w, &h);
    if (!tex) {
        tex = cat_load_image(abs);
        if (!tex) {
            return NULL;
        }
        if (SDL_QueryTexture(tex, NULL, NULL, &w, &h) != 0 || w <= 0 || h <= 0) {
            SDL_DestroyTexture(tex);
            return NULL;
        }
        cat_cache_put(abs, tex, w, h);
    }
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return tex;
}

static void jw__switcher_draw_image_fit(SDL_Texture *tex, int tex_w, int tex_h,
                                        int x, int y, int w, int h,
                                        uint8_t alpha) {
    if (!tex || tex_w <= 0 || tex_h <= 0 || w <= 0 || h <= 0) {
        return;
    }
    int draw_w = w;
    int draw_h = (tex_h * draw_w) / tex_w;
    if (draw_h > h) {
        draw_h = h;
        draw_w = (tex_w * draw_h) / tex_h;
    }
    int draw_x = x + (w - draw_w) / 2;
    int draw_y = y + (h - draw_h) / 2;
    SDL_SetTextureAlphaMod(tex, alpha);
    cat_draw_image(tex, draw_x, draw_y, draw_w, draw_h);
    SDL_SetTextureAlphaMod(tex, 255);
}

/* Draw a texture filling the square tile, center-cropped to the tile's aspect.
   Used for the live paused-frame current tile so a landscape frame fills the
   tile cleanly instead of letterboxing. */
static void jw__switcher_draw_tex_fill(SDL_Texture *tex, int x, int y, int box,
                                       uint8_t alpha) {
    if (!tex || box <= 0) {
        return;
    }
    int tw = 0, th = 0;
    if (SDL_QueryTexture(tex, NULL, NULL, &tw, &th) != 0 || tw <= 0 || th <= 0) {
        return;
    }
    int side = tw < th ? tw : th; /* square crop source for the square tile */
    SDL_Rect src = { (tw - side) / 2, (th - side) / 2, side, side };
    SDL_Rect dst = { x, y, box, box };
    SDL_SetTextureAlphaMod(tex, alpha);
    SDL_RenderCopy(cat_get_renderer(), tex, &src, &dst);
    SDL_SetTextureAlphaMod(tex, 255);
}

/* One carousel tile: cover art when available, otherwise a neutral placeholder
   card with the system code centered so missing art still reads as a game. */
static void jw__switcher_draw_tile(const jw_game_switcher *sw,
                                   const jw_game_entry *entry,
                                   int cx, int cy, int size, uint8_t alpha,
                                   bool is_current) {
    ap_theme *theme = cat_get_theme();
    int box = size;
    int x = cx - box / 2;
    int y = cy - box / 2;

    /* The running game's tile prefers the live paused-frame still (instant, a
       real screenshot) over cover art / savestate thumbnail. */
    if (is_current && sw->current_tex) {
        jw__switcher_draw_tex_fill(sw->current_tex, x, y, box, alpha);
        return;
    }

    int tw = 0, th = 0;
    SDL_Texture *tex = jw__switcher_cover(sw, entry, &tw, &th);
    if (tex) {
        jw__switcher_draw_image_fit(tex, tw, th, x, y, box, box, alpha);
        return;
    }

    /* No art: a 4:3 placeholder card (centered in the square slot) rather than a
       full square, so its height matches a fit 4:3 screenshot instead of towering
       over the real art and making everything else look small. */
    int ph = box * 3 / 4;
    int py = cy - ph / 2;
    ap_color card = theme->highlight;
    card.a = (uint8_t)((int)card.a * alpha / 255);
    cat_draw_rounded_rect(x, py, box, ph, cat_scale(10), card);

    if (entry && entry->system[0]) {
        TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
        int code_w = cat_measure_text(small, entry->system);
        ap_color tc = theme->text;
        tc.a = alpha;
        cat_draw_text(small, entry->system, x + (box - code_w) / 2,
                      py + ph / 2 - TTF_FontHeight(small) / 2, tc);
    }
}

void jw_game_switcher_render(jw_game_switcher *sw, int x, int y, int w, int h) {
    if (!sw) {
        return;
    }
    ap_theme *theme = cat_get_theme();

    if (jw_game_switcher_is_empty(sw)) {
        TTF_Font *body = cat_get_font(CAT_FONT_MEDIUM);
        const char *msg = "No recent games yet";
        int mw = cat_measure_text(body, msg);
        cat_draw_text(body, msg, x + (w - mw) / 2,
                      y + h / 2 - TTF_FontHeight(body) / 2, theme->hint);
        return;
    }

    TTF_Font *title_font = cat_get_font(CAT_FONT_EXTRA_LARGE);
    TTF_Font *small_font = cat_get_font(CAT_FONT_SMALL);

    /* Vertical budget: the square center tile, then the title + system caption
       below it. Grow the tile to fill the height left after that text block (the
       old fixed h*52% cap left a band of dead space under the tiles), and centre
       the whole composition so any leftover splits top/bottom. */
    int title_h = TTF_FontHeight(title_font);
    int sys_h   = TTF_FontHeight(small_font);
    int below_h = cat_scale(18) + title_h + cat_scale(6) + sys_h; /* matches title_y + system layout */
    int top_pad = cat_scale(10);

    int center_size = w * 46 / 100;
    int max_by_h = h - below_h - top_pad * 2;
    if (center_size > max_by_h) {
        center_size = max_by_h;
    }
    if (center_size < cat_scale(80)) {
        center_size = cat_scale(80);
    }
    int side_size = center_size * 64 / 100;
    int spacing = center_size * 78 / 100;
    int cx0 = x + w / 2;
    int block_h = center_size + below_h;
    int cy = y + (h - block_h) / 2 + center_size / 2;

    float voff = jw__switcher_visual_offset(sw);
    if (sw->anim_active) {
        cat_request_frame();
        if (SDL_GetTicks() - sw->anim_start_ms >= JW_SWITCHER_ANIM_MS) {
            sw->anim_active = false;
        }
    }

    bool loops = jw__switcher_loops(sw);

    /* Walk slots around the cursor. The window is center +/- 2 tiles; slots
       -3..+3 cover it for any voff in [-1, 1]. Each slot maps to a logical entry
       by wrapping (infinite ring) or is skipped past the ends (clamp). Sides
       first, center last so the active tile overlaps its neighbors. */
    for (int pass = 0; pass < 2; pass++) {
        for (int k = -3; k <= 3; k++) {
            int idx = sw->cursor + k;
            if (loops) {
                idx = (idx % sw->count + sw->count) % sw->count;
            } else if (idx < 0 || idx >= sw->count) {
                continue;
            }

            float dist = (float)k - voff;
            float adist = fabsf(dist);
            if (adist > 2.0f) {
                continue;
            }
            bool is_center = adist < 0.5f;
            if (pass == 0 && is_center) continue;
            if (pass == 1 && !is_center) continue;

            float c = 1.0f - fminf(adist, 1.0f); /* 1 at center, 0 at the sides */
            int size_px = (int)((1.0f - c) * (float)side_size + c * (float)center_size);
            uint8_t alpha = (uint8_t)((1.0f - c) * 110.0f + c * 255.0f);
            int cx = cx0 + (int)(dist * (float)spacing);
            jw__switcher_draw_tile(sw, &sw->entries[idx], cx, cy, size_px, alpha,
                                   idx == sw->current_index);
        }
    }

    /* Title + system for the logical (target) selection. */
    const jw_game_entry *sel = &sw->entries[sw->cursor];
    char title[256];
    snprintf(title, sizeof(title), "%s", sel->name);
    jw_clean_rom_title(title);   /* drop (U)/[!]/(En,Fr) filename tags */
    int text_w = w * 86 / 100;
    int title_y = cy + center_size / 2 + cat_scale(18);
    int tw = cat_measure_text(title_font, title);
    if (tw > text_w) {
        cat_draw_text_ellipsized(title_font, title, x + (w - text_w) / 2,
                                 title_y, theme->text, text_w);
    } else {
        cat_draw_text(title_font, title, x + (w - tw) / 2, title_y,
                      theme->text);
    }

    if (sel->system[0]) {
        int sy = title_y + TTF_FontHeight(title_font) + cat_scale(6);
        int sysw = cat_measure_text(small_font, sel->system);
        cat_draw_text(small_font, sel->system, x + (w - sysw) / 2, sy,
                      theme->hint);
    }
}
