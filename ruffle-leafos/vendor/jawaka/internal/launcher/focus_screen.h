#ifndef JW_LAUNCHER_FOCUS_SCREEN_H
#define JW_LAUNCHER_FOCUS_SCREEN_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>
#include <stdbool.h>

/* 5-Game Mode focus screen — the stripped, locked home for a curated set of up
   to five games. Rounded-square box-art tiles laid out 2-up / 3-down (reading
   order), centered, nothing else on screen but a corner battery. No box art ->
   the tile shows the game title centered. Selected tile = brighter, thicker
   accent border. Two styles: Theme (accent-colored borders on the theme
   background) and Black & white (stark white-on-black).

   This module is pure render: the launcher resolves the chosen ids to tiles
   (title + box-art texture) and reads the battery, then calls the render fn.
   No DB, IPC, or texture-cache dependency lives here. See
   umrk-workspace/plans/Jawaka/five-game-mode.md.

   Naming: public API is `jw_focus_screen_*`; module-private statics `jw__focus_*`. */

#define JW_FOCUS_SCREEN_MAX_TILES 5

/* One resolved tile. art == NULL draws the title-fallback square instead. */
typedef struct {
    const char  *title;   /* display name (fallback + accessibility) */
    SDL_Texture *art;      /* box art, or NULL for the title fallback */
    int          art_w;    /* native art size (for CONTAIN fit); 0 when art==NULL */
    int          art_h;
} jw_focus_tile;

/* Battery snapshot for the always-on corner glyph. percent is 0..100; charging
   draws the bolt; a monochrome glyph with bar fill, no color-coding. */
typedef struct {
    int  percent;
    bool charging;
} jw_focus_battery;

/* Compute the on-screen rect for tile `idx` of `count` (1..5) at the current
   screen size. Layout: bottom row holds up to 3, the top row the remainder
   (0..2), both centered; squares sized to fit. Returns false for a bad index. */
bool jw_focus_screen_tile_rect(int idx, int count, SDL_Rect *out);

/* Render one full focus-screen frame: clears to the style background, draws each
   tile (art CONTAIN-fit or title fallback) with the selection highlight on
   `cursor`, and the corner battery. `bt_pip` draws a small Bluetooth indicator
   next to the battery (a headset is paired but disconnected). The caller
   presents. */
void jw_focus_screen_render(const jw_focus_tile *tiles, int count, int cursor,
                            bool bw, jw_focus_battery battery, bool bt_pip);

#define JW_FOCUS_PIN_LEN 4

/* One legend row: a left and (optional) right button hint, each a key + label
   (e.g. "L1" / "Reboot"). NULL key = empty cell. */
typedef struct {
    const char *lkey, *llabel;
    const char *rkey, *rlabel;
    bool        center;   /* draw the (single) left cell centered, full width */
} jw_focus_hint_row;

#define JW_FOCUS_HINT_ROWS 5

/* The MENU unlock overlay (drawn over the dimmed grid). pin_mode true = the
   4-digit PIN entry; false = a simple Exit? confirm (lock == none). */
typedef struct {
    bool        pin_mode;
    const int  *pin;        /* JW_FOCUS_PIN_LEN digits 0..9 (pin_mode only) */
    int         pin_slot;    /* active slot 0..JW_FOCUS_PIN_LEN-1 */
    bool        error;       /* last attempt was wrong -> show a retry note */
    const char *error_text;  /* retry note; NULL = English fallback */
    const char *title;       /* e.g. "Enter PIN to exit" / "Exit 5-Game Mode?" */
    const char *confirm;     /* when set (e.g. "Reboot?"), show a plain A/B
                                confirm instead of the exit / PIN UI */
    const char *confirm_hint; /* confirm-mode footer; NULL = "B: Back  A: Confirm" */
    jw_focus_hint_row rows[JW_FOCUS_HINT_ROWS];  /* 2-column button legend */
    int         row_count;
} jw_focus_unlock_view;

/* Dim the whole screen and draw the unlock panel on top. Call AFTER
   jw_focus_screen_render (which draws the grid), BEFORE presenting. */
void jw_focus_screen_render_unlock(bool bw, const jw_focus_unlock_view *v);

/* Draw a "KEY: Value   KEY: Value" hint line centered at cx, "KEY:" tokens in
   key_color and values in val_color (segments split on runs of 3+ spaces, so a
   value may contain a double space). The shared two-tone hint style so key/value
   colors match across the focus UI. */
void jw_focus_draw_hint_kv(TTF_Font *f, const char *s, int cx, int y,
                           SDL_Color key_color, SDL_Color val_color);

#endif /* JW_LAUNCHER_FOCUS_SCREEN_H */
