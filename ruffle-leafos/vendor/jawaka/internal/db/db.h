#ifndef JW_DB_H
#define JW_DB_H

#include <stddef.h>
#include <sqlite3.h>

#define JW_GAME_SETTING_IMPORTED_DISPLAY_NAME "imported_display_name"
#define JW_GAME_SETTING_IMPORTED_DISPLAY_NAME_PROVIDER "imported_display_name_provider"

typedef struct {
    int game_count;
    int app_count;
    int system_count;
    char systems_summary[256];
    char sample_summary[256];
} jw_library_summary;

/* Aggregate library + playtime stats for the System menu's Info pages.
   Lifetime totals include hidden games; the title-bearing top list does not. */
typedef struct {
    char name[128];
    char system[64];
    long long playtime_s;
    long long last_played;
} jw_stat_game;

typedef struct {
    char system[64];
    int  game_count;
    long long playtime_s;
} jw_stat_system;

#define JW_STATS_TOP_MAX     8
#define JW_STATS_SYSTEM_MAX  64

typedef struct {
    long long total_playtime_s;   /* SUM(playtime_s) over all games            */
    int  games_played;       /* games with playtime_s > 0                 */
    int  game_count;
    int  app_count;
    int  favorite_count;     /* favorited games                           */
    int  art_covered;        /* games with a non-empty image_path         */
    long long last_played;        /* MAX(last_played), 0 if never              */
    jw_stat_game   top[JW_STATS_TOP_MAX];        /* most-played, time desc */
    int            top_count;
    jw_stat_system systems[JW_STATS_SYSTEM_MAX]; /* per-system, count desc */
    int            system_count;
} jw_library_stats;

typedef struct {
    char name[64];          /* system id / folder code, e.g. "FC" (used for DB queries) */
    char display_name[64];  /* full name for display, e.g. "Famicom"; filled by the launcher */
    int  game_count;
} jw_system_entry;

typedef struct {
    char name[256];
    char pak_dir[512];
    char icon[256];
    char platform[64];
    char pak_version[64];
    char min_jawaka_version[64];
    char min_leaf_version[64];
} jw_app_entry;

typedef struct {
    char store_id[128];
    char version[64];
    char platform[64];
    char source_id[32];
    char install_path[512];
    char artifact_sha256[80];
    char installed_at[64];
    char commit_token[33]; /* 128-bit lowercase hex; empty for legacy rows */
    int  app_present;
    char app_name[256];
    char app_pak_dir[512];
    char kind[8];          /* "app" or "theme"; always agrees with install_path */
} jw_pakrat_install;

typedef struct {
    int  id;
    char system[64];
    char name[256];
    char source_id[32];
    char rom_relpath[512];
    char image_root_kind[16];
    char image_relpath[512];
    char rom_path[512];
    char image_path[512];
    int  favorite;   /* 1 if present in favorites, else 0 */
    long long last_played;  /* unix seconds, 0 if never */
    int  playtime_s;        /* seconds accumulated in this game */
} jw_game_entry;

typedef enum {
    JW_SEARCH_GAME = 0,
    JW_SEARCH_APP
} jw_search_kind;

typedef struct {
    jw_search_kind kind;
    int id;
    char name[256];
    char system[64];
    char source_id[32];
    char rom_relpath[512];
    char image_root_kind[16];
    char image_relpath[512];
    char rom_path[512];
    char image_path[512];
    char pak_dir[512];
    char icon[256];
    int  favorite;   /* 1 if present in favorites, else 0 */
} jw_search_result;

int  jw_db_open(const char *path, sqlite3 **out);
int  jw_db_apply_schema(sqlite3 *db);
void jw_db_close(sqlite3 *db);

int  jw_db_reset_library(sqlite3 *db);
/* Non-destructive rescan helpers. scan_begin sets up per-scan "seen" tracking;
   stable game upserts preserve id and record (source_id,rom_relpath).
   Mark only successfully enumerated sources complete before scan_prune; rows
   belonging to unavailable/failed/unmarked sources are retained. App pruning
   is enabled only after the complete configured source set was enumerated. */
int  jw_db_scan_begin(sqlite3 *db);
int  jw_db_scan_source_complete(sqlite3 *db, const char *source_id);
int  jw_db_scan_apps_complete(sqlite3 *db);
/* Collapse duplicate library entries for one system: when the same title exists
   under both the canonical public folder and a legacy alias folder (both fold
   to one system), keep a single entry, preferring the copy whose rom_path is
   under canonical_rom_root (e.g. "Roms/NES"). Run after inserts, before prune,
   so prune's cascade cleans favorites/recents that referenced a dropped copy. */
int  jw_db_dedup_system_aliases(sqlite3 *db, const char *system, const char *canonical_rom_root);
int  jw_db_scan_prune(sqlite3 *db);
/* Exact physical identities confirmed removed (or already absent) by the daemon.
   The caller omits retained shared files and a surviving parent descriptor.
   This atomically drops matching games, all hidden member keys for these files,
   and orphaned metadata/Focus IDs. No scan seen-set is involved. Supports an
   existing transaction; returns JW_DB_RC_READONLY/BUSY/IOERR or -1 on failure. */
typedef struct {
    char source_id[32];
    char rom_relpath[512];
} jw_db_rom_key;
int  jw_db_reconcile_removed_roms(sqlite3 *db, const jw_db_rom_key *removed,
                                    size_t count);
int  jw_db_insert_game(sqlite3 *db, const char *system, const char *name, const char *rom_path, const char *image_path);
int  jw_db_insert_game_stable(sqlite3 *db, const char *system, const char *name,
                              const char *source_id, const char *rom_relpath,
                              const char *rom_path,
                              const char *image_root_kind,
                              const char *image_relpath,
                              const char *image_path);
int  jw_db_insert_app(sqlite3 *db, const char *pak_dir, const char *name,
                      const char *icon, const char *platform,
                      const char *pak_version,
                      const char *min_jawaka_version,
                      const char *min_leaf_version);

/* Optional source-provided display titles applied immediately after a library
   scan. rom_paths use the exact primary-relative / secondary-absolute form
   stored in games.rom_path. Manual display_name remains the highest-precedence
   user override; imported titles are the fallback ahead of the scanned stem. */
typedef struct {
    const char        *provider;
    const char        *title;
    const char *const *rom_paths;
    int                rom_path_count;
} jw_db_imported_title_group;

typedef struct {
    int groups;
    int paths;
    int matched;
    int applied;
    int unmatched;
} jw_db_imported_title_result;

int  jw_db_apply_imported_title_groups(sqlite3 *db,
                                       const jw_db_imported_title_group *groups,
                                       int group_count,
                                       jw_db_imported_title_result *out);
int  jw_db_read_summary(const char *db_path, jw_library_summary *out);
int  jw_db_read_stats(const char *db_path, jw_library_stats *out);
int  jw_db_list_systems(const char *db_path, jw_system_entry *out, int max_count, int *out_count);
int  jw_db_list_apps(const char *db_path, jw_app_entry *out, int max_count, int *out_count);
int  jw_db_count_games_for_system(const char *db_path, const char *system, int *out_count);
int  jw_db_list_games_for_system(const char *db_path, const char *system,
                                 jw_game_entry *out, int max_count, int *out_count);
int  jw_db_search_library(const char *db_path, const char *query,
                          jw_search_result *out, int max_count, int *out_count);

/* Visibility changes only the empty-member preference for the game's stable
   source/path key. Identity lookups, metadata and member preferences survive.
   Writes return JW_DB_RC_READONLY/BUSY/IOERR, or -1 for other errors.
   Hiding returns JW_DB_RC_NO_ROW if the game no longer exists; clearing an
   absent preference succeeds. */
int  jw_db_set_game_hidden(const char *db_path, int game_id, int hidden);
int  jw_db_is_game_hidden(const char *db_path, int game_id, int *out_hidden);
int  jw_db_clear_hidden_game(const char *db_path, const char *source_id,
                              const char *rom_relpath);
/* Hidden Games includes missing rows: id is 0, name falls back to rom_relpath,
   and source_id/rom_relpath remain available to clear the stored preference. */
int  jw_db_count_hidden_games(const char *db_path, int *out_count);
int  jw_db_list_hidden_games(const char *db_path, jw_game_entry *out,
                              int max_count, int *out_count);

typedef struct {
    jw_game_entry game;
    char member[512];
} jw_hidden_rom_entry;

/* Any stored key, including disc keys written before Manage Discs was removed;
   member is empty except for an explicit archive selector. No games row is
   required. An absent preference reads as visible, and writes return the same
   statuses as game visibility. No path case folding is done. */
int  jw_db_set_rom_hidden(const char *db_path, const char *source_id,
                           const char *rom_relpath, const char *member, int hidden);
int  jw_db_is_rom_hidden(const char *db_path, const char *source_id,
                          const char *rom_relpath, const char *member, int *out_hidden);
/* Every stored key, including archive members and unindexed or missing discs.
   game.id is 0 without a matching row; game.name then falls back to the path. */
int  jw_db_count_hidden_roms(const char *db_path, int *out_count);
int  jw_db_list_hidden_roms(const char *db_path, jw_hidden_rom_entry *out,
                             int max_count, int *out_count);

/* Complete indexed identities, including hidden and unavailable sources.
   Deletion uses it to stop artwork scrapes for the files it removes.
   Caller frees *out; failure leaves *out NULL and *out_count zero. */
int  jw_db_list_indexed_games(const char *db_path, jw_game_entry **out,
                                size_t *out_count);

/* Favorites. kind is "game" or "app"; target_id is the games/apps id.
   set_favorite adds (on != 0) or removes (on == 0); it is idempotent.
   list_favorite_games returns favorited games in case-insensitive alphabetical
   order by display name (added_at is only a tiebreaker). */
int  jw_db_set_favorite(const char *db_path, const char *kind, int target_id, int on);
int  jw_db_list_favorite_games(const char *db_path, jw_game_entry *out,
                               int max_count, int *out_count);

/* Recents + playtime. record_play_by_id is authoritative for active sessions;
   the path variant remains a compatibility wrapper. Both bump cumulative
   playtime_s + last_played and upsert the recents row.
   list_recent_games returns games most-recently-opened first. */
int  jw_db_record_play(const char *db_path, const char *rom_path, int duration_s);
int  jw_db_record_play_by_id(const char *db_path, int game_id, int duration_s);
int  jw_db_list_recent_games(const char *db_path, jw_game_entry *out,
                             int max_count, int *out_count);
/* Drop a single play-history row (kind 'game'/'app'). Does not touch the
   game/app itself or its favorite; the game just leaves the Recents list.
   Idempotent — removing an absent row succeeds. */
int  jw_db_remove_recent(const char *db_path, const char *kind, int target_id);

/* Scoped content actions/settings. Identity lookups deliberately include hidden
   games (Focus selections, boot resume and running sessions use them).
   Game settings are keyed by stable games.id;
   system settings are keyed by canonical Jawaka system id. Empty values should
   normally be deleted by callers rather than stored. */
int  jw_db_get_game_by_rom_path(const char *db_path, const char *rom_path,
                                jw_game_entry *out);
/* The row a card's scan keyed by (source_id, rom_relpath). Unlike rom_path,
   which holds a second card's mount path from its last scan, this still finds
   the game after the two MLP1 cards swap mount points. 0 when found. */
int  jw_db_get_game_by_source_relpath(const char *db_path, const char *source_id,
                                      const char *rom_relpath, jw_game_entry *out);
/* Resolve a stable games.id to a full entry. Returns 0 when found, -1 when no
   row matched (e.g. a picked game whose ROM was removed) or on error. */
int  jw_db_get_game_by_id(const char *db_path, int game_id, jw_game_entry *out);
int  jw_db_get_game_setting(const char *db_path, int game_id,
                            const char *key, char *out, size_t out_size);
/* Scraped facts about one game. Stored as game_settings rows under an "ss."
   prefix rather than columns: they are sparse, they arrive long after the row
   does, and a key the scraper stops sending should not leave a dead column. */
typedef struct {
    char genre[96];
    char developer[96];
    char publisher[96];
    char players[16];
    char rating[8];       /* ScreenScraper's note, 0-20 */
    char year[8];
    char synopsis[1200];
} jw_game_meta;

/* Write whatever fields are non-empty, resolving the game by rom_path in one
   statement. Empty fields are left alone rather than blanked, so a rescrape that
   returns less than the last one does not erase what is already known. */
int  jw_db_set_game_meta(const char *db_path, const char *rom_path,
                         const jw_game_meta *meta);

/* Read one game's scraped facts. Missing keys come back as empty strings. */
int  jw_db_get_game_meta(const char *db_path, int game_id, jw_game_meta *out);

int  jw_db_set_game_setting(const char *db_path, int game_id,
                            const char *key, const char *value);
int  jw_db_delete_game_setting(const char *db_path, int game_id,
                               const char *key);
int  jw_db_get_system_setting(const char *db_path, const char *system,
                              const char *key, char *out, size_t out_size);
int  jw_db_set_system_setting(const char *db_path, const char *system,
                              const char *key, const char *value);
int  jw_db_delete_system_setting(const char *db_path, const char *system,
                                 const char *key);

typedef struct {
    const char *key;
    char       *out;
    size_t      out_size;
    int         found;
} jw_db_setting_query;

/* Set a game's image_path by rom_path (scrape worker: art landed outside a
   scan). Returns 0 on success, 1 when no game row matched, -2 on a transient
   lock (SQLITE_BUSY/LOCKED, caller may retry), -3 when the database is
   read-only (SQLITE_READONLY or its extended codes), -4 on SQLITE_IOERR, -1 on
   any other error. An I/O error alone does not prove read-only storage. */
#define JW_DB_RC_NO_ROW 1
#define JW_DB_RC_BUSY (-2)
#define JW_DB_RC_READONLY (-3)
#define JW_DB_RC_IOERR (-4)
int  jw_db_set_game_image(const char *db_path, const char *rom_path,
                          const char *image_path);

/* Atomically increment an integer-valued setting, creating it at 1 when
   absent. Safe against concurrent bumps from the daemon main loop and the
   scrape worker (single UPSERT statement). */
int  jw_db_increment_setting(const char *db_path, const char *key);

int  jw_db_get_setting(const char *db_path, const char *key,
                        char *out, size_t out_size);
int  jw_db_get_settings(const char *db_path, jw_db_setting_query *queries,
                        int count);
/* Narrow read for time-sensitive daemon paths: a read-only connection with no
   busy wait and no schema work. Returns 0 with the value, 1 when the key or
   the settings table is absent, and -1 when the database is missing, locked,
   or unreadable -- the caller picks its own safe default. */
int  jw_db_read_setting_nowait(const char *db_path, const char *key,
                               char *out, size_t out_size);

/* RetroAchievements account (producer side of standalone-ra-account-v1).
   The account is the three settings keys retroachievements_user,
   retroachievements_pass and retroachievements_revision. The revision is a
   positive decimal that increments inside the same checked write transaction
   as every successful account save or clear; a clear stores empty credentials
   and retains the new revision. A credential is raw UTF-8 of at most
   JW_RA_USERNAME_MAX / JW_RA_PASSWORD_MAX bytes (excluding NUL) and may not
   contain NUL, CR or LF. */
#define JW_RA_USERNAME_MAX 63
#define JW_RA_PASSWORD_MAX 127
#define JW_RA_REVISION_MAX 4611686018427387904LL /* 2^62 */

typedef enum {
    /* No credentials and no revision: the account was never saved. */
    JW_RA_ACCOUNT_NEVER_CONFIGURED = 0,
    /* Complete, validated credentials. revision > 0, or 0 for a legacy pair
       stored before revisions existed; the caller initializes that once with
       jw_db_ensure_ra_account_revision() before handing the account out. */
    JW_RA_ACCOUNT_CONFIGURED,
    /* Credentials cleared; the retained revision proves the sign-out. */
    JW_RA_ACCOUNT_SIGNED_OUT,
    /* Stored values are oversized, malformed or an incomplete pair, or the
       revision is malformed. Not sign-out: durable data stays for repair. */
    JW_RA_ACCOUNT_INVALID,
    /* The settings database could not be read. Also not sign-out. */
    JW_RA_ACCOUNT_UNREADABLE,
} jw_ra_account_state;

typedef struct {
    jw_ra_account_state state;
    char user[JW_RA_USERNAME_MAX + 1];  /* valid only for CONFIGURED */
    char pass[JW_RA_PASSWORD_MAX + 1];
    long long revision;                 /* CONFIGURED and SIGNED_OUT only */
} jw_ra_account;

/* Read all three account keys in one checked read transaction, validate them
   and classify the result. Never returns truncated credentials as valid:
   oversized or malformed stored values come back INVALID. */
int  jw_db_resolve_ra_account(const char *db_path, jw_ra_account *out);

/* Replace the saved account in one checked write transaction, bumping the
   revision (0/absent/malformed restarts at 1; >= JW_RA_REVISION_MAX fails
   instead of wrapping). On any failure the prior account is unchanged and -1
   is returned; *revision_out is left alone. The caller validates input first
   with jw_ra_credentials_check() and pre-validates again inside here. */
int  jw_db_save_ra_account(const char *db_path, const char *user,
                           const char *pass, long long *revision_out);

/* Sign out: store empty credentials and the next revision in one checked
   write transaction. */
int  jw_db_clear_ra_account(const char *db_path, long long *revision_out);

/* One-time initialization of a legacy saved pair's missing revision, using
   the same checked-transaction discipline as a save. Returns 0 and the
   current (or newly assigned) revision; 1 when there is no legacy pair to
   initialize; -1 on failure. A malformed stored revision is left alone and
   reported as 1 (the resolver classifies the pair INVALID). */
int  jw_db_ensure_ra_account_revision(const char *db_path,
                                      long long *revision_out);

/* The one resolve every launch handoff uses (the standalone-ra-account-v1
   snapshot and the RetroArch per-launch config): jw_db_resolve_ra_account plus
   the one-time legacy revision initialization. It never returns CONFIGURED
   without a revision in 1..JW_RA_REVISION_MAX, because the contract forbids
   fabricating one:
     - the store cannot be read, or the initialization cannot read or write
       it                                                    -> UNREADABLE
     - the stored rows are malformed, including a revision row that is
       present but not a canonical positive integer          -> INVALID
   Any result other than CONFIGURED carries no credentials. Always fills
   *out; never fails. */
void jw_db_resolve_ra_account_handoff(const char *db_path, jw_ra_account *out);

typedef enum {
    JW_RA_CREDENTIALS_OK = 0,
    JW_RA_CREDENTIALS_INCOMPLETE,  /* missing or empty half of the pair */
    JW_RA_CREDENTIALS_TOO_LONG,    /* exceeds the byte limits */
    JW_RA_CREDENTIALS_BAD_CHARS,   /* NUL/CR/LF or invalid UTF-8 */
} jw_ra_credentials_check;

jw_ra_credentials_check jw_ra_credentials_check_values(const char *user,
                                                       const char *pass);

int  jw_db_set_setting(const char *db_path, const char *key, const char *value);
/* Write multiple key/value settings in a single open + transaction. Far cheaper
   than N jw_db_set_setting() calls, each of which re-opens the DB and re-applies
   the schema. keys[i]/values[i] are paired; count is the number of pairs. */
int  jw_db_set_settings(const char *db_path, const char *const *keys,
                        const char *const *values, int count);
int  jw_db_get_theme_name(const char *db_path, char *out, size_t out_size);

/* Rumble is two switches and one strength: UI rumble (every interface buzz,
   cursor ticks included; default off), game rumble (the motor handed to
   emulators; default on), and a 0-100 strength shared by both (default 65).
   Both the daemon and Settings read them here so they cannot disagree.

   The first load after the old master-switch settings rewrites them once:
   master off turns both switches off, otherwise UI rumble takes the old
   Cursor Movement value and game rumble keeps its own. Returns 0 when the
   database was read; *out holds defaults either way. */
typedef struct {
    int ui;        /* 0/1 */
    int game;      /* 0/1 */
    int strength;  /* 0-100 */
} jw_rumble_settings;

#define JW_RUMBLE_DEFAULT_STRENGTH 65

int  jw_db_load_rumble_settings(const char *db_path, jw_rumble_settings *out);

/* Pak Rat store ownership. apps remains scan truth; pakrat_installs records
   packages installed/updated through the store and intentionally survives
   library rescans. install_path is the Apps-namespace path for an app, e.g.
   "mlp1/SDLReader.pak" or "shared/RetroArch.pak", and "Themes/<id>" for a
   theme; the row's kind column is derived from it. */
int  jw_db_pakrat_upsert_install(const char *db_path, const char *store_id,
                                 const char *version, const char *platform,
                                 const char *install_path,
                                 const char *artifact_sha256,
                                 const char *installed_at,
                                 const char *commit_token);
/* Same write on an already-open connection. The caller owns transaction and
   commit ordering; P1 uses this to publish rebuilt discovery state and the
   token-bearing install record in one SQLite commit after syncfs. */
int  jw_db_pakrat_upsert_install_db(sqlite3 *db, const char *store_id,
                                    const char *version,
                                    const char *platform,
                                    const char *source_id,
                                    const char *install_path,
                                    const char *artifact_sha256,
                                    const char *installed_at,
                                    const char *commit_token);
int  jw_db_pakrat_remove_install(const char *db_path, const char *store_id);
/* Returns 0 when found, 1 when no matching store_id exists, -1 on error. */
int  jw_db_pakrat_get_install(const char *db_path, const char *store_id,
                              jw_pakrat_install *out);
int  jw_db_pakrat_list_installs(const char *db_path, jw_pakrat_install *out,
                                int max_count, int *out_count);

#endif
