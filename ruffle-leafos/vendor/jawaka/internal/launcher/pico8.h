#ifndef JW_PICO8_H
#define JW_PICO8_H

#include "internal/storage/sources.h"
#include <sqlite3.h>

#define JW_PICO8_CORE "pico8_native"
#define JW_PICO8_PROVIDER "mlp1/PICO8.pak"
#define JW_PICO8_ENTRY "launch-cart.sh"
#define JW_PICO8_CAPABILITY "native-pico8-v1"
#define JW_PICO8_EXIT_CONFIRM_MS 4000

typedef struct {
    char runtime[JW_STORAGE_PATH_MAX];
    char home[JW_STORAGE_PATH_MAX];
    char root[JW_STORAGE_PATH_MAX];
    char desktop[JW_STORAGE_PATH_MAX];
} jw_pico8_paths;

/* Curated behavior selection, not publisher authentication. */
bool jw_pico8_core_matches(const char *id, const char *provider, const char *entry);
bool jw_pico8_app_matches(const jw_storage_source *primary, const char *pak_abs);
bool jw_pico8_resolve_paths(const jw_storage_source_list *sources,
                            const jw_storage_source *rom_source,
                            jw_pico8_paths *out);
/* Always clear stale per-launch values for unrelated children. */
void jw_pico8_export(const jw_pico8_paths *paths, bool protected_session);
/* Executes only the curated wrapper's read-only --check, never PICO-8.
   One two-second budget, including descendants; no post-session retry. */
bool jw_pico8_preflight(const char *wrapper, const jw_pico8_paths *paths);
/* First/expired tap arms a short confirmation window; a second consumes it. */
bool jw_pico8_exit_confirmed(long long *deadline, long long now);

/* Signal for a confirmed exit: graceful first, force after two seconds. */
int jw_pico8_exit_signal(long long *requested, long long now);

/* Apply importer metadata after a successful ordinary library scan. */
int jw_pico8_apply_library(sqlite3 *db, const char *report);

#endif
