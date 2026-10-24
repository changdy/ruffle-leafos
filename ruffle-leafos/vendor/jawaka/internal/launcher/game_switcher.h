#ifndef JW_LAUNCHER_GAME_SWITCHER_H
#define JW_LAUNCHER_GAME_SWITCHER_H

#include "internal/db/db.h"

#include <limits.h>
#include <stdbool.h>
#include <stdint.h>

/* JW_MAX_RECENTS (64, defined in the launcher) plus one slot for the in-game
   current-game injection that may not have reached Recents yet. */
#define JW_GAME_SWITCHER_MAX 65

/* Shared recents/resume carousel used by both the launcher (Select opens a
   dedicated switcher view) and the in-game overlay (Menu + Select pauses the
   game and overlays the same carousel). The module owns the entry list,
   selection, and slide animation, and renders the carousel art/title/system
   into a caller-provided content rect. The caller owns the backdrop (launcher:
   clear + status bar; in-game: paused still + scrim) and the footer hints,
   which differ per context. */
typedef struct {
    jw_game_entry entries[JW_GAME_SWITCHER_MAX];
    int           count;
    int           cursor;          /* logical selected index */
    bool          in_game;         /* true = in-game overlay, false = launcher view */
    int           current_index;   /* index of the running game, or -1 when none */
    /* Borrowed (not owned) texture used as the current game's tile art — the
       in-game overlay passes its live paused-frame still here so the current
       tile shows a real screenshot instantly. NULL falls back to cover/thumb. */
    struct SDL_Texture *current_tex;
    char          sdcard_root[PATH_MAX];
    char          states_dir[PATH_MAX]; /* RetroArch States/ root for thumbnails */
    /* Carousel slide animation. The cursor jumps to the new selection at once;
       anim_from_offset is the strip's visual displacement (in tile units) at the
       start of the slide, relative to the new cursor, and eases to 0. Tracking a
       relative offset (rather than an absolute target index) makes every move a
       single-tile slide in the press direction — including the seam crossing of
       the infinite wrap, which no longer rewinds across the whole strip. */
    float         anim_from_offset;
    uint32_t      anim_start_ms;
    bool          anim_active;
} jw_game_switcher;

/* Reset to an empty carousel. in_game selects the overlay footer/behavior;
   sdcard_root resolves relative cover-art paths; states_dir (may be NULL/empty)
   is the RetroArch States/ root searched for savestate thumbnails. */
void jw_game_switcher_reset(jw_game_switcher *sw, bool in_game,
                            const char *sdcard_root, const char *states_dir);

/* Load the recents list from the library DB (most-recently-played first).
   Returns 0 on success. Safe to call repeatedly to refresh after a removal. */
int  jw_game_switcher_load(jw_game_switcher *sw, const char *db_path);

/* Inject the currently running game (in-game overlay only) and start the
   carousel on it. If a recents entry already matches (same system + ROM file)
   that entry becomes the current one; otherwise a synthetic entry is inserted
   at the front. Only known visible library games are injected; the caller's
   independent Resume action stays available. Synthetic entries carry id < 0. */
void jw_game_switcher_set_current(jw_game_switcher *sw, const char *db_path,
                                  const char *system,
                                  const char *rom_path, const char *name,
                                  const char *image_path);

/* Move the cursor to the entry matching system + ROM path without marking it as
   the running game (current_index is untouched, so Y removal still works). Used
   when reopening the launcher straight into the switcher seeded on a game.
   Returns true when a matching entry was found and selected. */
bool jw_game_switcher_select(jw_game_switcher *sw, const char *system,
                             const char *rom_path);

/* Provide a borrowed texture (not owned by the switcher) to render as the
   current game's tile art, overriding cover/savestate-thumb art for that tile.
   The in-game overlay passes its captured paused-frame still so the current tile
   shows the live game instantly. Pass NULL to clear. The caller keeps ownership
   and must outlive the switcher's use of it. */
void jw_game_switcher_set_current_texture(jw_game_switcher *sw,
                                          struct SDL_Texture *tex);

/* Prefer a savestate thumbnail over the cover for any entry that has one. Uses
   the resolved storage sources when available so recents from secondary cards
   search their own States/ root first. Call after load (and set_current) so
   every entry is considered. */
void jw_game_switcher_resolve_thumbnails(jw_game_switcher *sw);

/* Move the selection by delta (callers pass +/-1), animating a single-tile
   slide. With 5+ entries the carousel is an infinite ring: stepping past either
   end wraps seamlessly to the other in the press direction. Below that the seam
   would show the same tile on both sides of the window, so it clamps instead. */
void jw_game_switcher_move(jw_game_switcher *sw, int delta);

/* The selected entry, or NULL when the carousel is empty. */
const jw_game_entry *jw_game_switcher_selected(const jw_game_switcher *sw);

/* True when the selection is the running game (in-game overlay). */
bool jw_game_switcher_selected_is_current(const jw_game_switcher *sw);

bool jw_game_switcher_is_empty(const jw_game_switcher *sw);

/* Drop the selected entry from the in-memory list after its Recents row was
   deleted by the caller. Refuses (returns false) when the selection is the
   running game. Keeps the cursor and current_index in range. */
bool jw_game_switcher_remove_selected(jw_game_switcher *sw);

/* Draw the carousel (cover art + title + system) within the content rect.
   Renders a quiet centered empty state when there are no entries, and requests
   an animation frame while a slide is in flight. */
void jw_game_switcher_render(jw_game_switcher *sw, int x, int y, int w, int h);

#endif /* JW_LAUNCHER_GAME_SWITCHER_H */
