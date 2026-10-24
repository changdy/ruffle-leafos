#ifndef JW_RETROARCH_STATES_H
#define JW_RETROARCH_STATES_H

#include <stdbool.h>
#include <stddef.h>

#define JW_RA_AUTO_STATE_SLOT (-1)
#define JW_RA_GAME_SWITCHER_STATE_SLOT 99

/* Resolve a validated States/<core_folder> namespace. core_folder must be one
   path component (normally the catalog's config_folder); traversal and nested
   paths fail closed. */
bool jw_ra_core_states_dir(const char *states_dir, const char *core_folder,
                           char *out, size_t out_size);

/* RetroArch savestate thumbnail lookup.

   RetroArch writes a PNG beside each savestate (e.g. <rom>.state1.png, and
   <rom>.state.auto.png for the autosave) and may sort states into per-core
   subfolders under the States/ root (States/<Core Name>/<rom>.state1.png). The
   in-game menu's Save/Load preview and the recents/resume switcher carousel
   both surface these, so the lookup lives here as a shared helper. */

/* Slot-specific lookup, for callers that know the exact slot (the in-game
   Save/Load preview). slot < 0 = the auto slot, 0 = ".state", N = ".stateN".
   Searches states_dir flat, then one level of per-core subfolders. Writes an
   existing path to out and returns true, else false. */
bool jw_ra_find_slot_thumb(const char *states_dir, const char *rom_path,
                           int slot, char *out, size_t out_size);

/* Slot-specific savestate lookup. slot < 0 = the auto slot, 0 = ".state",
   N = ".stateN". Searches states_dir flat, then one level of per-core
   subfolders. Writes an existing path to out and returns true, else false. */
bool jw_ra_find_slot_state(const char *states_dir, const char *rom_path,
                           int slot, char *out, size_t out_size);

/* Core-isolated lookup for launch/resume paths. core_folder is required and
   only States/<core_folder> is searched; flat files and other core folders are
   deliberately ignored. This prevents a core override from loading a state
   produced by another libretro core. */
/* Build (without requiring existence) the path RetroArch uses for a slot in
   the States/<core_folder> namespace: <stem>.stateN, or its ".png" thumbnail
   when thumb is true. Same slot numbering as the lookups. Fails closed on an
   unsafe core_folder or truncation. */
bool jw_ra_slot_state_path_for_core(const char *states_dir,
                                    const char *core_folder,
                                    const char *rom_path, int slot, bool thumb,
                                    char *out, size_t out_size);

bool jw_ra_find_slot_state_for_core(const char *states_dir,
                                    const char *core_folder,
                                    const char *rom_path,
                                    int slot, char *out, size_t out_size);

/* Find the state the switcher should resume. preferred_slot wins when present;
   otherwise the newest existing state for the ROM is selected. Writes the
   resolved slot and path when found. */
bool jw_ra_find_resume_state(const char *states_dir, const char *rom_path,
                             int preferred_slot, int *out_slot,
                             char *out, size_t out_size);

bool jw_ra_find_resume_state_for_core(const char *states_dir,
                                      const char *core_folder,
                                      const char *rom_path,
                                      int preferred_slot, int *out_slot,
                                      char *out, size_t out_size);

/* Thumbnail index: scans a States/ root once (flat + one level of subfolders)
   so the carousel can look up many ROMs without re-walking the tree per game. */
typedef struct jw_state_thumb_index jw_state_thumb_index;

jw_state_thumb_index *jw_state_thumb_index_build(const char *states_dir);
void jw_state_thumb_index_free(jw_state_thumb_index *idx);

/* Find any savestate thumbnail for the ROM (matched by filename stem; the slot
   is unknown). Prefers the game-switcher slot, then the newest thumbnail. Writes
   the path to out and returns true when one exists. */
bool jw_state_thumb_index_find(const jw_state_thumb_index *idx,
                               const char *rom_path, char *out, size_t out_size);

/* ── Slot enumeration + switcher-slot promotion (in-game slot picker) ──────── */

typedef struct {
    int  slot;   /* JW_RA_AUTO_STATE_SLOT (auto), 0..N, or the game-switcher slot */
    long mtime;  /* unix mtime of the savestate file */
} jw_ra_slot_info;

/* Enumerate existing savestates for the ROM (flat layout + one level of per-core
   subfolders). Fills out[] with up to max {slot, mtime} entries and writes the
   count to *count. Includes the auto slot and the game-switcher slot when they
   exist; duplicates across flat/subfolder keep the newest mtime. The caller
   decides how to present (e.g. hide the switcher slot). Returns true on success. */
bool jw_ra_list_slots(const char *states_dir, const char *rom_path,
                      jw_ra_slot_info *out, int max, int *count);

/* Promote the game-switcher quicksave into a permanent numbered slot: copies
   <rom>.state<99> (and its .png) to the lowest free slot in 0..9 (overwriting
   the oldest numbered slot when all are taken), in the same directory the source
   lives. Writes the chosen slot to *out_slot. Returns false when no switcher
   save exists or the copy fails. */
bool jw_ra_promote_switcher_slot(const char *states_dir, const char *rom_path,
                                 int *out_slot);

#endif /* JW_RETROARCH_STATES_H */
