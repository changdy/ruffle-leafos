#ifndef JW_STORE_PAKRAT_TXN_H
#define JW_STORE_PAKRAT_TXN_H

/* A full library scan holds the DB write lock for 6-9 seconds. A package
   mutation's commit point has to outlast that rather than report a failure for
   work it already did -- jw_db_open's 2s default is sized for interactive
   writes (a favourite, a recent), where waiting longer would feel like a hang.
   Generous on purpose: the alternative is telling a user their uninstall
   failed after the package is already gone from disk. */
#define JW_PAKRAT_COMMIT_BUSY_TIMEOUT_MS 30000

#include "internal/db/db.h"
#include "internal/services/manifest.h"
#include "internal/store/pakrat.h"

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <sqlite3.h>

#define JW_PAKRAT_TXN_TARGET_MAX 511
#define JW_PAKRAT_TXN_DISPLAY_MAX 255
#define JW_PAKRAT_TXN_SOURCE_ID_MAX 31
#define JW_PAKRAT_TXN_OPERATION_MAX 63

typedef struct {
    char store_id[128];
    char install_path[JW_PAKRAT_TXN_TARGET_MAX + 1];
    char package_id[JW_SVC_ID_BUF];
    char display_name[JW_PAKRAT_TXN_DISPLAY_MAX + 1];
    bool has_service;
    char service_id[JW_SVC_ID_BUF];
    char *state_root;
    char *revoke_on_uninstall[JW_SVC_MAX_STATE_LIST];
    int revoke_count;
    char *retained_roots[JW_SVC_MAX_STATE_LIST];
    int retained_count;
} jw_pakrat_txn_metadata;

typedef struct {
    char source_id[JW_PAKRAT_TXN_SOURCE_ID_MAX + 1];
    jw_pakrat_txn_metadata metadata;
} jw_pakrat_pending_uninstall;

typedef struct {
    int fd;
    char operation_id[JW_PAKRAT_TXN_OPERATION_MAX + 1];
    char package_id[JW_SVC_ID_BUF];
    char target_path[JW_PAKRAT_TXN_TARGET_MAX + 1];
} jw_pakrat_mutation_lock;

typedef struct {
    char source_id[JW_PAKRAT_TXN_SOURCE_ID_MAX + 1];
    char root[PATH_MAX];
    bool source_present;
    bool size_known;
    unsigned long long size_bytes;
} jw_pakrat_retained_item;

typedef struct jw_pakrat_uninstall_info {
    jw_pakrat_txn_metadata metadata;
    jw_pakrat_retained_item *items;
    int item_count;
} jw_pakrat_uninstall_info;

void jw_pakrat_txn_metadata_destroy(jw_pakrat_txn_metadata *metadata);
void jw_pakrat_pending_uninstall_destroy(jw_pakrat_pending_uninstall *pending);
void jw_pakrat_uninstall_info_destroy(jw_pakrat_uninstall_info *info);

/* Reads and fully validates one candidate/live pak manifest. A pak without a
   service object is valid metadata with has_service=false; state declarations
   are cached only from a fully validated service manifest. */
int jw_pakrat_txn_inspect_manifest(const char *pak_dir,
                                   const char *manifest_rel,
                                   const char *userdata_root,
                                   const char *expected_package_id,
                                   const char *install_path,
                                   jw_pakrat_txn_metadata *out,
                                   char *reason, size_t reason_size);

/* A theme has no pak.json and no service: its metadata is its identity and
   display name, with nothing to revoke or retain. */
int jw_pakrat_txn_theme_metadata(const char *store_id,
                                 const char *install_path,
                                 const char *display_name,
                                 jw_pakrat_txn_metadata *out);

int jw_pakrat_txn_metadata_upsert_db(sqlite3 *db,
                                     const jw_pakrat_txn_metadata *metadata);
int jw_pakrat_txn_metadata_get(const char *db_path, const char *store_id,
                               jw_pakrat_txn_metadata *out);

int jw_pakrat_txn_pending_persist(const char *db_path,
                                  const char *source_id,
                                  const jw_pakrat_txn_metadata *metadata);
int jw_pakrat_txn_pending_get(const char *db_path, const char *store_id,
                              jw_pakrat_pending_uninstall *out);
int jw_pakrat_txn_pending_list(const char *db_path,
                               jw_pakrat_pending_uninstall **out,
                               int *out_count);

/* Attach the service-control database before BEGIN. The clear helper is then
   part of the caller's existing SQLite transaction. */
int jw_pakrat_txn_attach_control_db(sqlite3 *db, const char *state_dir);
int jw_pakrat_txn_clear_service_control_db(sqlite3 *db,
                                           const char *service_id);

/* Complete an already-durable uninstall intent. The caller must hold TXN-1's
   mutation lock and have proven service quiescence. For a theme, the final
   commit also sets the "user_theme" setting to None when it names the removed
   folder, and reports that through ctx->outcome. */
int jw_pakrat_txn_complete_uninstall(const jw_pakrat_context *ctx,
                                     const jw_pakrat_pending_uninstall *pending);

int jw_pakrat_txn_inventory_retained(const jw_pakrat_context *ctx,
                                     const jw_pakrat_txn_metadata *metadata,
                                     jw_pakrat_uninstall_info *out);
int jw_pakrat_txn_remove_retained(const jw_pakrat_context *ctx,
                                  const jw_pakrat_txn_metadata *metadata);

int jw_pakrat_mutation_lock_acquire(const char *runtime_dir,
                                    const char *operation_id,
                                    const char *package_id,
                                    const char *target_path,
                                    jw_pakrat_mutation_lock *out,
                                    char *reason, size_t reason_size);
void jw_pakrat_mutation_lock_release(jw_pakrat_mutation_lock *lock);
bool jw_pakrat_mutation_lock_is_held(const char *runtime_dir,
                                     const char *operation_id,
                                     const char *package_id,
                                     const char *target_path);

bool jw_pakrat_txn_target_path_valid(const char *path);

#endif
