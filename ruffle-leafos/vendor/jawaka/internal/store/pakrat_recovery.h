#ifndef JW_STORE_PAKRAT_RECOVERY_H
#define JW_STORE_PAKRAT_RECOVERY_H

#include "internal/db/db.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* ── Filesystem helpers shared between pakrat.c and pakrat_recovery.c ──────
   Internal to internal/store/ (jw__ prefix); declared here so the daemon can
   link recovery without pulling in the download/zip half of pakrat.c. */

int  jw__pakrat_copy(char *out, size_t out_size, const char *value);
int  jw__pakrat_join2(char *out, size_t out_size, const char *a, const char *b);
int  jw__pakrat_join3(char *out, size_t out_size, const char *a, const char *b,
                      const char *c);
bool jw__pakrat_path_exists(const char *path);
bool jw__pakrat_is_dir(const char *path);
bool jw__pakrat_is_regular_file(const char *path);
int  jw__pakrat_mkdir_p(const char *path, mode_t mode);
int  jw__pakrat_mkdir_parent(const char *path);
int  jw__pakrat_remove_tree(const char *path);
bool jw__pakrat_safe_name(const char *name);
bool jw__pakrat_safe_rel_path(const char *path);

/* Appends one timestamped line to <state_dir>/store/logs/pakrat.log. No-op
   when state_dir is empty or the log cannot be opened. */
void jw__pakrat_log(const char *state_dir, const char *fmt, ...);

/* Resolves an install_path to its absolute target under sdcard_root: an
   Apps-namespace path ("mlp1/X.pak" or "Apps/mlp1/X.pak") to Apps/, a theme's
   "Themes/<id>" to Themes/<id> (pakrat_kind.h). */
int  jw__pakrat_target_path(const char *sdcard_root, const char *install_path,
                            char *out, size_t out_size);
/* Sibling transition path next to target: <parent>/.pakrat-<kind>-<store_id>.
   Kinds in use: "stage", "rollback", and "origin" (a marker file, not a tree). */
int  jw__pakrat_target_sibling_path(const char *target, const char *store_id,
                                    const char *kind, char *out,
                                    size_t out_size);

/* Records the Apps-relative install_path a rollback sibling was moved aside
   from, so recovery can restore it even when no install row exists yet (an
   adopted install crashes before its first record is written). Written next to
   the target before the move-aside rename, then removed with the rollback. */
int  jw__pakrat_write_origin_marker(const char *target, const char *store_id,
                                    const char *install_path);
int  jw__pakrat_read_origin_marker(const char *apps_dir, const char *store_id,
                                   char *out, size_t out_size);
void jw__pakrat_clear_origin_marker(const char *target, const char *store_id);

#define JW_PAKRAT_COMMIT_TOKEN_HEX_LEN 32
#define JW_PAKRAT_COMMIT_TOKEN_BUF (JW_PAKRAT_COMMIT_TOKEN_HEX_LEN + 1)

typedef struct {
    char store_id[128];
    char version[64];
    char artifact_sha256[80];
    char token[JW_PAKRAT_COMMIT_TOKEN_BUF];
} jw_pakrat_commit_marker;

/* Reserved per-tree commit identity. Archives are forbidden from supplying
   this file; the installer writes and fsyncs it before the first promotion
   rename. Recovery accepts a promoted tree as committed only when every field
   and the install-record token match exactly. */
int jw__pakrat_write_commit_marker(const char *pak_dir,
                                   const jw_pakrat_commit_marker *marker);
int jw__pakrat_read_commit_marker(const char *pak_dir,
                                  jw_pakrat_commit_marker *out);

/* Filesystem-wide durability barrier for the Apps filesystem containing path.
   Linux/MLP1 uses syncfs(2); desktop fallback uses fsync on the opened path. */
int jw__pakrat_sync_filesystem(const char *path);

typedef struct {
    char platform[64];
    char pak_version[64];
    char min_leaf_version[64];
    /* CONTENT-1: a pure content pak legitimately ships no launch.sh, so the
       install-time entry-point check has to know the difference between a
       content pak and a broken one. */
    int has_provides;
} jw__pakrat_manifest;

/* The single JSON path for Pak Rat manifest reads. Loads
   <pak_dir>/<manifest_rel> (must be a safe relative path), requires it to
   parse as a JSON object, and copies out the string fields Pak Rat relies on;
   absent or non-string fields become "". Returns 0 on success. */
int  jw__pakrat_read_manifest(const char *pak_dir, const char *manifest_rel,
                              jw__pakrat_manifest *out);

/* A store theme's identity: theme.json "id" and "version" (loosely read; the
   package was held to THEME-1 before it was ever promoted). Returns 0 when
   both are present strings. */
int  jw__pakrat_read_theme_identity(const char *theme_dir, char *id,
                                    size_t id_size, char *version,
                                    size_t version_size);

/* ── Install-transition recovery ─────────────────────────────────────────── */

typedef struct {
    char platform[64];
    char sdcard_root[PATH_MAX];
    char state_dir[PATH_MAX];
    char db_path[PATH_MAX];
} jw_pakrat_recovery_context;

enum {
    JW_PAKRAT_RECOVERY_FAILED = -1,
    JW_PAKRAT_RECOVERY_OK = 0,
    JW_PAKRAT_RECOVERY_REPAIR_REQUIRED = -2,
};

/* Reconcile one install target's transition siblings. |install| is the current
   ownership record, or NULL when the store id has none. Stale staging trees
   are always discarded. target absent + rollback present restores the
   rollback. target + rollback present is decided by the record: the promoted
   tree counts as committed only when its pak.json, reserved commit marker,
   entry point, and install-record token all match; anything else is an
   uncommitted promote and rolls back to the sibling that was already running.
   Returns 1 (without mutation) when the owning Apps source is not positively
   mounted, JW_PAKRAT_RECOVERY_OK on reconciliation,
   JW_PAKRAT_RECOVERY_REPAIR_REQUIRED when an inconsistent live tree has no
   rollback, and JW_PAKRAT_RECOVERY_FAILED on another recovery error. */
int  jw__pakrat_reconcile_transition(const jw_pakrat_recovery_context *ctx,
                                     const char *store_id,
                                     const char *install_path,
                                     const jw_pakrat_install *install);

/* Recovery entry point, called before package discovery on every daemon start
   and from the Pak Rat rescan path. Reconciles every recorded install, then
   sweeps the platform and shared Apps dirs, and the primary card's Themes
   dir, for .pakrat-stage-* and .pakrat-rollback-* trees with no install row (an interrupted first install
   leaves no row). A REPAIR_REQUIRED result is stable and must not be retried
   on a timer; an explicit repair or ownership change is required. */
int  jw_pakrat_recover_installs(const jw_pakrat_recovery_context *ctx);

#endif
