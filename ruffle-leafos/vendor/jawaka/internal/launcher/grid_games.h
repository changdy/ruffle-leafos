/* Grid View's games list.
 *
 * A scrolling list of names on the left and the selected game's cover on the
 * right, both over the theme's wallpaper. The list and the info sit on
 * translucent underlays; the cover does not, so the artwork reads as artwork
 * rather than as another widget.
 *
 * Geometry and drawing live here; the launcher keeps orchestration and hands
 * rows, art and metadata in through callbacks, mirroring coverflow.c and grid.c.
 * Nothing in here reads launcher state.
 *
 * Design record: umrk-workspace/plans/grid-view-and-user-themes.md. */
#ifndef JW_LAUNCHER_GRID_GAMES_H
#define JW_LAUNCHER_GRID_GAMES_H

#include "catastrophe.h"
#include "catastrophe_widgets.h"
#include <SDL2/SDL.h>
#include <stdbool.h>

#define JW_GRID_GAMES_MAX_META 6

/* One labelled fact about the selected game. The launcher formats the value,
   so this module never learns what a playtime or a release date is. */
typedef struct {
    const char *value;   /* drawn as-is; no label, the shape of the value says what it is */
    int         stars;   /* 0 = a text value; 1-10 = half-stars out of five */
    int         span;    /* chip width in columns of three; 0 and 1 both mean one */
} jw_grid_games_meta;

/* Everything a theme gets to decide about this view. The launcher fills it from
   the stylesheet today and from theme.json once themes carry colour. */
typedef struct {
    cat_draw_color underlay;        /* alpha carries the opacity */
    cat_draw_color ink;             /* text on the underlay; a==0 means derive it */
    cat_draw_color highlight;
    cat_draw_color highlight_text;
    int            shadow;          /* 0-255; 0 draws no shadow at all */
    int            radius;          /* panel corner radius, px */
} jw_grid_games_style;

typedef const char *(*jw_grid_games_name_fn)(void *ctx, int idx);
typedef SDL_Texture *(*jw_grid_games_art_fn)(void *ctx, int idx, int *w, int *h);

/* Draw the whole view. `top_bar` is the band reserved above both columns;
   the caller sizes it from the status cluster it is about to draw, so the
   columns clear it whatever scale that cluster ends up at.

   Draw the whole view. `meta` may be empty, in which case the cover takes the
   space the info would have used. `wordmark` may be NULL, and then
   `system_name` is drawn in its place. A `wm_color` wordmark is full-color art
   and is drawn as authored; otherwise it is tinted to the list's ink. */
void jw_grid_games_draw(const cat_list_state *ls, int count,
                        jw_grid_games_name_fn name_fn,
                        jw_grid_games_art_fn art_fn, void *ctx,
                        const jw_grid_games_meta *meta, int meta_count,
                        const char *synopsis,
                        SDL_Texture *wordmark, int wm_w, int wm_h, bool wm_color,
                        const char *system_name, int top_bar,
                        const jw_grid_games_style *style);

#endif
