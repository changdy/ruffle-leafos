#ifndef JW_FOCUS_H
#define JW_FOCUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

/* 5-Game Mode ("focus mode") — the persistent state + recovery plumbing shared
   by the daemon (boot check) and the launcher/settings UI (later phases).
   See umrk-workspace/plans/Jawaka/five-game-mode.md for the full design.

   The mode curates up to five games and locks the device into a stripped focus
   screen. This module owns:
     - the persisted settings keys and their (de)serialization,
     - the salted PIN hash + verify,
     - the SD recovery lock file, and
     - the boot decision (enter focus vs. normal launcher, with the
       delete-the-lock-file escape hatch).
   It carries no UI and no device dependencies, so it is unit-testable natively. */

#define JW_FOCUS_MAX_GAMES 5

/* Persisted settings keys (rows in the settings table). Read/written via the
   jw_db_*_setting helpers. The chosen set persists even when the mode is off so
   re-entry can pre-fill the last choice. */
#define JW_FOCUS_KEY_ACTIVE    "five_game_active"
#define JW_FOCUS_KEY_IDS       "five_game_ids"
#define JW_FOCUS_KEY_LOCK      "five_game_lock"
#define JW_FOCUS_KEY_PIN_HASH  "five_game_pin_hash"
#define JW_FOCUS_KEY_STYLE     "five_game_style"
#define JW_FOCUS_KEY_WIFI_PREV "five_game_wifi_prev"

/* Recovery lock file, written at the SD root when the mode is active. Its
   presence gates a locked boot; deleting it from a computer releases the lock
   without a factory reset. FAT-safe, top-level, off the release-managed
   .system tree so it survives updates and is easy to find. */
#define JW_FOCUS_LOCK_FILENAME ".leaf-focus-lock"

typedef enum {
    JW_FOCUS_LOCK_NONE = 0,
    JW_FOCUS_LOCK_PIN,
} jw_focus_lock;

typedef enum {
    JW_FOCUS_STYLE_THEME = 0,
    JW_FOCUS_STYLE_BW,
} jw_focus_style;

typedef struct {
    bool           active;                     /* five_game_active */
    int            ids[JW_FOCUS_MAX_GAMES];    /* chosen game ids, in tile order */
    int            id_count;                   /* 0..JW_FOCUS_MAX_GAMES */
    jw_focus_lock  lock;                       /* five_game_lock */
    char           pin_hash[65];               /* five_game_pin_hash (hex or "") */
    jw_focus_style style;                      /* five_game_style */
    int            wifi_prev;                   /* radio state captured on enter:
                                                  1 on, 0 off, -1 unknown */
} jw_focus_config;

/* Boot decision returned by jw_focus_resolve_boot. */
typedef enum {
    JW_FOCUS_BOOT_NORMAL = 0,   /* mode off (or recovered) -> spawn the normal launcher */
    JW_FOCUS_BOOT_ENTER,        /* mode active + lock file present -> enter focus mode */
} jw_focus_boot_decision;

/* --- enum <-> string (for the settings values) --- */
jw_focus_lock  jw_focus_lock_parse(const char *s);   /* default NONE */
const char    *jw_focus_lock_name(jw_focus_lock v);  /* "none"|"pin" */
jw_focus_style jw_focus_style_parse(const char *s);  /* default THEME */
const char    *jw_focus_style_name(jw_focus_style v);/* "theme"|"bw" */

/* --- chosen-set (de)serialization --- */
/* Parse a CSV of positive game ids into ids[JW_FOCUS_MAX_GAMES]. Dedupes, drops
   non-numeric / non-positive tokens, and caps at JW_FOCUS_MAX_GAMES (extra ids
   are ignored). *out_count receives the number kept (0 on empty/NULL). */
static inline void jw_focus_ids_parse(const char *csv, int ids[JW_FOCUS_MAX_GAMES],
                        int *out_count) {
    int count = 0;
    for (int i = 0; i < JW_FOCUS_MAX_GAMES; i++) ids[i] = 0;

    const char *p = (csv && csv[0]) ? csv : NULL;
    while (p && *p && count < JW_FOCUS_MAX_GAMES) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        char *end = NULL;
        long v = strtol(p, &end, 10);
        if (end == p) { p++; continue; }   /* not a number: skip a char */
        p = end;
        if (v <= 0) continue;              /* ids are positive games.id values */
        bool dup = false;
        for (int i = 0; i < count; i++)
            if (ids[i] == (int)v) { dup = true; break; }
        if (dup) continue;
        ids[count++] = (int)v;
    }
    if (out_count) *out_count = count;
}

/* Serialize ids[0..count) as a CSV (e.g. "12,3,45"). Truncates safely. */
static inline void jw_focus_ids_to_csv(const int *ids, int count, char *out, size_t out_size) {
    size_t len = 0;
    if (out_size > 0) out[0] = '\0';
    if (count > JW_FOCUS_MAX_GAMES) count = JW_FOCUS_MAX_GAMES;
    for (int i = 0; i < count; i++) {
        int n = snprintf(out + len, out_size - len, "%s%d",
                         i > 0 ? "," : "", ids[i]);
        if (n < 0 || (size_t)n >= out_size - len) break;
        len += (size_t)n;
    }
}

/* --- PIN --- */
/* Hash a PIN into out_hex[65]. Salted, but a 4-digit PIN has only 10k values;
   this is a child deterrent, not real crypto (the SD-delete recovery is the
   real boundary). Never store the plaintext. */
void jw_focus_pin_hash(const char *pin, char out_hex[65]);
/* True when pin hashes to the stored hash. Empty stored hash -> never matches. */
bool jw_focus_pin_verify(const char *pin, const char *stored_hash);

/* --- SD recovery lock file --- */
/* Build "<sdcard_root>/.leaf-focus-lock" into out. Returns 0 on success. */
int  jw_focus_lock_path(const char *sdcard_root, char *out, size_t out_size);
bool jw_focus_lock_exists(const char *sdcard_root);
/* Write the lock file (a small JSON echo of the config; never the plaintext
   PIN). Returns 0 on success. */
int  jw_focus_lock_write(const char *sdcard_root, const jw_focus_config *cfg);
/* Remove the lock file. Returns 0 when it is gone (including already-absent). */
int  jw_focus_lock_remove(const char *sdcard_root);

/* --- DB config load/store --- */
/* Load the full config from the settings table. Missing keys take defaults;
   returns 0 (a fresh device with no keys is simply inactive). */
int  jw_focus_config_load(const char *db_path, jw_focus_config *out);

/* Resolve what to do at boot, performing the recovery clear as a side effect:
   - not active            -> NORMAL.
   - active + lock present  -> ENTER (out_cfg filled).
   - active + lock absent   -> recovery: clear five_game_active in the DB and
                               return NORMAL (the documented "delete the file"
                               escape hatch; the remembered set is kept).
   out_cfg may be NULL. */
jw_focus_boot_decision jw_focus_resolve_boot(const char *db_path,
                                             const char *sdcard_root,
                                             jw_focus_config *out_cfg);

#endif /* JW_FOCUS_H */
