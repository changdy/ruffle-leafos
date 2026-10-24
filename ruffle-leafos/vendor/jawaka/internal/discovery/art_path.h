#ifndef JW_DISCOVERY_ART_PATH_H
#define JW_DISCOVERY_ART_PATH_H

#include "internal/retroarch/catalog.h"
#include "internal/storage/sources.h"

#include <stddef.h>

/* Box-art lookup shared by the library scan and the scraper's missing-art
   check, so both agree on what counts as a game's art.

   Accepted: <stem>.png, <stem>.jpg, <stem>.jpeg, with the extension matched
   case-insensitively. Within a folder PNG beats JPG beats JPEG; within a
   format the all-lower spelling wins, then all-upper, then mixed spellings in
   bytewise order. The stem is compared byte for byte, so on a case-sensitive
   card (ext4, or the MLP1's utf8 vfat mount) "Game" never borrows "game.jpg";
   where a folder's filesystem folds case, the stem is compared without case,
   matching what opening the file by name would find. The result is the
   spelling that exists on disk and never depends on directory order.

   Folders are read with readdir and matched in memory. Probing spellings with
   stat was far slower on FAT, where every miss reads the whole directory: a
   cold scan of 1500 JPEG covers in one folder took 40 s instead of 8 s. */

/* A cache of art folders, each read once on first use. It never notices files
   added or removed after that read, so keep one only for a single pass over
   the library (a scan, one missing-art count) and free it afterwards. Not
   thread-safe. */
typedef struct jw_art_index jw_art_index;

/* Returns NULL when out of memory; every lookup accepts a NULL index and then
   reads the folder just for that call. */
jw_art_index *jw_art_index_new(void);
void jw_art_index_free(jw_art_index *index);

/* Find the art for `stem` in `dir`. On success writes the matched filename
   to out_name and returns 0. Returns 1 when there is no art (including a
   missing folder), -1 when the name does not fit. */
int jw_art_find(jw_art_index *index, const char *dir, const char *stem,
                char *out_name, size_t out_name_size);

/* The art stem for a ROM filename: its extension is stripped, and so is an
   inner content extension behind an archive or content extension
   ("Sonic.md.zip" -> "Sonic"). A NULL system strips only the last extension. */
void jw_art_stem_for_rom(const jw_ra_system *system, const char *filename,
                         char *out, size_t out_size);

/* Search a ROM's art locations on its storage source in order:
     <source root>/<image_root>/     (canonical, skipped when image_root is NULL)
     Images/<physical_folder>/
     Roms/<physical_folder>/Imgs/
   Extensions are tried within a location before moving to the next, so a JPEG
   in an earlier location beats a PNG in a later one. On success fills image_abs
   and the source-relative image_rel ("Images/GBA/Game.jpg") from the same
   matched filename and returns 0; returns 1 when nothing matches. */
int jw_art_find_for_rom(jw_art_index *index,
                        const jw_storage_source *source,
                        const char *image_root,
                        const char *physical_folder,
                        const char *stem,
                        char *image_abs, size_t image_abs_size,
                        char *image_rel, size_t image_rel_size);

#endif
