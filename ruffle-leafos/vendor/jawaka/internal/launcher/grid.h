/* Grid View — a vertical-scrolling grid of rounded system tiles.
 *
 * Geometry, cursor movement and tile drawing live here; the launcher keeps
 * orchestration (which items, what activating one does) and hands icons in
 * through a callback, mirroring the coverflow module. Nothing in here reads
 * launcher state.
 *
 * Design record: umrk-workspace/plans/grid-view-and-user-themes.md. */
#ifndef JW_LAUNCHER_GRID_H
#define JW_LAUNCHER_GRID_H

#include "catastrophe.h"
#include "catastrophe_widgets.h"
#include <SDL2/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#define JW_GRID_SCRATCH_MAX 16

typedef struct {
    /* Derived geometry (px). Recomputed by jw_grid_layout; never authored. */
    int cols, rows;
    int tile;            /* square tile edge */
    int gutter;
    int radius;
    int x0, y0;          /* top-left of the first tile */
    int status_h;        /* band reserved above the grid for the status icons */

    /* Row scroll. scroll_row is the logical top row; the tween eases toward it. */
    int      scroll_row;
    float    anim_from_row;
    uint32_t anim_start_ms;
    bool     anim_active;

    /* Focus tween: the newly focused tile grows and the previous one shrinks
       over the same duration as a row scroll. */
    int      focus_prev;          /* -1 = none */
    uint32_t focus_anim_start_ms;

    /* Off-screen composition targets, sized to the tile. A pool rather than one
       texture: on this GPU a target texture written and then sampled again in
       the same frame comes back holding another tile's pixels, so each tile in a
       frame gets its own, used round-robin. */
    SDL_Texture *scratch[JW_GRID_SCRATCH_MAX];
    int          scratch_count;
    int          scratch_next;
    int          scratch_size;
} jw_grid;

/* Same shape as jw_cf_icon_fn: return a texture for item idx (or NULL while it
   is still decoding) and its natural size. */
typedef SDL_Texture *(*jw_grid_icon_fn)(void *ctx, int idx, int *tw, int *th);

/* Derive tile size and origin from the stylesheet tunables and the screen.
   Tiles are the largest square that fits BOTH the column and row constraint,
   then the block is centred horizontally and placed below the status band. */
void jw_grid_layout(jw_grid *g, const cat_stylesheet_launcher *l,
                    int screen_w, int screen_h, int status_h);

/* Reset scroll and animation (keeps the scratch texture). */
void jw_grid_reset(jw_grid *g);

/* Move the cursor. dx walks the grid as one linear list with wrap; dy moves
   by a row, and Down from a column with no tile beneath lands on the last
   tile so a press never does nothing. Scrolls the window to keep the cursor
   visible and starts the row tween. */
void jw_grid_step(jw_grid *g, cat_list_state *ls, int count, int dx, int dy,
                  uint32_t now, uint32_t anim_ms);

/* Draw every visible tile. Returns true while the scroll tween is running so
   the caller keeps requesting frames. */
/* label_fn may be NULL. A label is a full-tile overlay authored at the tile's
   own size, drawn over the composited art on the same rect. */
/* `border` and `focus_ring` override the stylesheet's tile colours when their
   alpha is non-zero; a theme that sets neither keeps Leaf's own. */
bool jw_grid_draw(jw_grid *g, const cat_stylesheet_launcher *l,
                  const cat_list_state *ls, int count,
                  jw_grid_icon_fn icon_fn, jw_grid_icon_fn label_fn, void *ctx,
                  uint32_t now, uint32_t anim_ms,
                  cat_draw_color border_override, cat_draw_color focus_override);

/* Bottom-left item count for the focused tile: a controller silhouette and the
   number, mirroring the status cluster's inset on the opposite corner. It sits
   in the slack below the last row, so it never overlaps a tile. `count` below
   zero draws nothing (an entry that counts nothing).
   `glyph` is Leaf's controller or apps mark, drawn tinted to `color`; NULL falls
   back to Catastrophe's atlas gamepad. `reserve_w` is the width of the widest
   mark: every glyph is right-aligned inside it and the number starts after it,
   so neither the number nor the marks' right edges move when the focused tile
   changes from a system to Apps. Pass 0 to reserve exactly this glyph. */
void jw_grid_draw_count(const jw_grid *g, int count, int screen_h,
                        cat_draw_color color,
                        SDL_Texture *glyph, int glyph_w, int glyph_h, int reserve_w);

#endif
