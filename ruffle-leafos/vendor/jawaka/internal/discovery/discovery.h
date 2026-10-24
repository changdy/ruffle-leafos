#ifndef JW_DISCOVERY_H
#define JW_DISCOVERY_H

#include <sqlite3.h>
#include <stddef.h>

typedef struct {
    int game_count;
    int app_count;
    int system_count;
} jw_scan_result;

int jw_scan_library(sqlite3 *db, const char *sdcard_root, jw_scan_result *out);

/* Enumerate CONTENT-1 contributors across platform/shared lanes and perform
   the producer-side refresh. Called at daemon startup and at scan start. */
int jw_discovery_catalog_refresh(const char *sdcard_root,
                                 char *generation,
                                 size_t generation_size,
                                 char *reason,
                                 size_t reason_size);

#endif
