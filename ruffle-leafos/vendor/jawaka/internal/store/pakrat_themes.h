/* Pak Rat theme policy that THEME-1 itself cannot know: which folder names the
 * running release owns, and how many folders the launcher will list. */
#ifndef JW_STORE_PAKRAT_THEMES_H
#define JW_STORE_PAKRAT_THEMES_H

#include <stdbool.h>
#include <stddef.h>

#define JW_PAKRAT_BUNDLED_THEMES_FILE "bundled-themes.txt"

/* <platform_root>/../../releases/<release_id>/bundled-themes.txt, with the
   release id from <state_dir>/release.json. Returns 0 with the path, 1 when no
   release id is recorded (a development card), -1 on invalid input or
   unreadable metadata. */
int jw_pakrat_bundled_themes_path(const char *platform_root,
                                  const char *state_dir,
                                  char *out, size_t out_size);

/* Whether list_path names `name`, one folder per line as the release installer
   reads it ('#' comments and blank lines skipped), ignoring case: the card is
   FAT32. Returns 1 listed, 0 not listed or no such file, -1 unreadable. */
int jw_pakrat_theme_name_listed(const char *list_path, const char *name);

/* A store theme may not take a folder a Leaf release replaces wholesale on
   every install: THEME-1's reserved names, and whatever the running release's
   bundled-themes.txt lists. Returns 1 reserved, 0 free, -1 when the release's
   list exists but cannot be read (the caller refuses rather than guesses). */
int jw_pakrat_theme_name_reserved(const char *platform_root,
                                  const char *state_dir, const char *name);

/* Folders directly under <sdcard_root>/Themes that the launcher would consider
   (not hidden, a directory). 0 when Themes/ does not exist, -1 on an error. */
int jw_pakrat_theme_folder_count(const char *sdcard_root);

/* The launcher lists at most JW_USER_THEME_MAX themes, so a new folder past
   that would install and silently never appear. Replacing an existing folder
   never adds one. */
bool jw_pakrat_theme_slots_full(int folder_count, bool replaces_existing);

#endif
