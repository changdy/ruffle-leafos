#ifndef JW_SERVICES_CONTROL_STATE_H
#define JW_SERVICES_CONTROL_STATE_H

#include <stdbool.h>
#include <stddef.h>

/* SVC-1's per-service control state (contracts.md#svc-1--service-manifest;
 * A2's own "Ordered work" step 3, "Persistent and session control state,
 * including survival across daemon restart"): one row per service id,
 * holding persistent "Start with Leaf" intent, session Run/Stop, the last
 * transition and reason, the last exit status and time, restart count, a
 * backoff/circuit-breaker snapshot, and installed package identity.
 *
 * This module is its own small SQLite database, deliberately independent
 * of jawakad's existing library.db schema and migration system in
 * internal/db/db.c -- adding a table there means bumping a live
 * `PRAGMA user_version` migration that every existing Jawaka installation
 * would run through, which is a materially different risk than a new,
 * separate database file this module owns end to end. Folding this into
 * the shared schema, if ever wanted, is a deliberate later decision for
 * whoever wires this into jawakad, not something this module presumes.
 *
 * "Persistent" here means "survives a daemon restart AND a device
 * reboot": the caller opens this store against a path on durable
 * storage, and everything written to it stays until explicitly changed.
 * "Session" fields (currently just `session_run`) live in the same row
 * for simplicity but are NOT self-clearing -- contracts.md requires
 * session Run/Stop to reset on every daemon start, so the daemon owner's
 * startup sequence MUST call jw_svc_control_store_clear_all_sessions()
 * once before trusting any jw_svc_control_store_get() result. Opening an
 * additional handle (for example, in a CLI process) is not a daemon start
 * and must not clear session state.
 *
 * SQLite's normal file locking plus a 2000 ms busy timeout makes each
 * get(), put(), and clear operation atomic and safe with handles in
 * multiple processes. This API does not make a get/modify/put sequence
 * atomic across handles: concurrent whole-row writers are last-writer-
 * wins and can overwrite one another's fields. The integration layer
 * should designate one writer (normally jawakad, with CLIs using CTL-1)
 * or provide its own serialization. This store does not force WAL mode;
 * the default rollback journal is sufficient for these small operations
 * and avoids imposing WAL filesystem requirements on the caller's
 * eventual choice of durable path.
 *
 * This module does no process supervision, no manifest parsing, and
 * knows nothing about the other SVC-1 primitives' headers -- callers
 * translate between this struct and whatever the rest of the supervisor
 * uses (e.g. internal/services/backoff.h's jw_svc_backoff_state has the
 * same shape as the backoff_failure_times_us/backoff_failure_count/
 * breaker_open fields below by design, but this header does not include
 * backoff.h to stay independently branchable).
 */

#define JW_SVC_CONTROL_ID_MAX 255
#define JW_SVC_CONTROL_REASON_MAX 63
#define JW_SVC_CONTROL_PACKAGE_ID_MAX 127
#define JW_SVC_CONTROL_PACKAGE_VERSION_MAX 31
#define JW_SVC_CONTROL_BACKOFF_TRACKED 5
#define JW_SVC_CONTROL_MIGRATION_ID_MAX 127

typedef struct jw_svc_control_store jw_svc_control_store;

typedef struct {
    char service_id[JW_SVC_CONTROL_ID_MAX + 1];
} jw_svc_control_id;

typedef struct {
    /* Persistent: survives daemon restart and device reboot. */
    bool start_with_leaf;

    /* Session-only: MUST be cleared by
     * jw_svc_control_store_clear_all_sessions() on every daemon start.
     * Persisted in the same row purely for storage simplicity -- it is
     * not itself durable in the SVC-1 sense. */
    bool session_run;

    /* Persistent. 0 means "no transition recorded yet". */
    long long last_transition_at_us;
    char last_transition_reason[JW_SVC_CONTROL_REASON_MAX + 1];

    /* Persistent. has_last_exit distinguishes "never exited" from an
     * exit code of 0. */
    bool has_last_exit;
    int last_exit_code;
    long long last_exit_at_us;

    /* Persistent. */
    int restart_count;

    /* Persistent snapshot of the backoff/circuit-breaker primitive
     * (internal/services/backoff.h), so a daemon restart does not erase
     * evidence of an in-progress crash loop. Only the first
     * backoff_failure_count entries of backoff_failure_times_us are
     * valid, oldest first -- the same layout
     * jw_svc_backoff_state uses internally. */
    bool breaker_open;
    int backoff_failure_count;
    long long backoff_failure_times_us[JW_SVC_CONTROL_BACKOFF_TRACKED];

    /* Persistent. Empty string means "no package installed under this
     * service id" (a manifest-only/never-installed row). */
    char installed_package_id[JW_SVC_CONTROL_PACKAGE_ID_MAX + 1];
    char installed_package_version[JW_SVC_CONTROL_PACKAGE_VERSION_MAX + 1];
} jw_svc_control_state;

/* Opens (creating if necessary) the control-state database at `db_path`.
 * On success, returns true and sets *out to a handle the caller must
 * later pass to jw_svc_control_store_close(). On failure, returns false
 * with a stable slug in `reason` (may be NULL to discard) and *out left
 * NULL:
 *   "invalid-arguments"  db_path or out is NULL, or db_path is empty
 *   "open-failed"        opening, initializing, or validating schema failed
 */
bool jw_svc_control_store_open(const char *db_path, jw_svc_control_store **out,
                                char *reason, size_t reason_size);

/* Closes `store` and releases it. A NULL store is a no-op. */
void jw_svc_control_store_close(jw_svc_control_store *store);

/* Resets session_run to false for every row in the store. A caller's
 * daemon-startup sequence must call this exactly once per daemon start,
 * before trusting any get() result, per SVC-1's requirement that session
 * Run/Stop does not survive a daemon restart. Opening an auxiliary handle
 * is not a reason to call it. Returns true on success; false with a slug
 * in `reason` ("invalid-arguments" for a NULL store, "write-failed" for a
 * SQL failure). */
bool jw_svc_control_store_clear_all_sessions(jw_svc_control_store *store,
                                              char *reason, size_t reason_size);

/* Reads the row for `service_id` into *out. Sets *out_found to false
 * (and *out to an all-zeroed state) when no row exists for that id --
 * this is not an error; a service that was never enabled or run simply
 * has no row yet. Returns false only on an actual failure to query, with
 * a slug in `reason` ("invalid-arguments" for a NULL store/service_id/
 * out/out_found, an empty service_id, or a service_id longer than
 * JW_SVC_CONTROL_ID_MAX; "read-failed" for a SQL failure or a row whose
 * stored types, ranges, or string lengths do not fit
 * jw_svc_control_state). */
bool jw_svc_control_store_get(jw_svc_control_store *store, const char *service_id,
                               jw_svc_control_state *out, bool *out_found,
                               char *reason, size_t reason_size);

/* Lists every retained control-state row in stable service-id order. This is
 * how the supervisor keeps a package-missing desired-state record visible to
 * CTL-1 and Settings -> Services after daemon restart. The returned heap array
 * belongs to the caller and must be released with
 * jw_svc_control_store_free_ids(). An empty store succeeds with *out_ids NULL
 * and *out_count 0. Returns false with "invalid-arguments",
 * "out-of-memory", or "read-failed". */
bool jw_svc_control_store_list_ids(jw_svc_control_store *store,
                                   jw_svc_control_id **out_ids,
                                   size_t *out_count,
                                   char *reason, size_t reason_size);
void jw_svc_control_store_free_ids(jw_svc_control_id *ids);

/* Writes (inserting or replacing) the entire row for `service_id`.
 * There is no partial-field update: callers read the current state,
 * modify the fields they own, and write the whole struct back, the same
 * whole-record pattern internal/services/reservation.c uses. Returns
 * true on success; false with a slug in `reason` ("invalid-arguments"
 * for a NULL store/service_id/state, an empty or oversized service_id
 * (longer than JW_SVC_CONTROL_ID_MAX), a backoff_failure_count outside
 * [0, JW_SVC_CONTROL_BACKOFF_TRACKED], or a string field with no NUL
 * within its fixed buffer; "write-failed" for a SQL failure). */
/* Removes a service's row entirely. Used by discovery reconciliation to shed
 * a record with nothing left to retain -- package gone, not enabled, no
 * session intent, no live or stale generation. Without this the store (and
 * CTL-1's `list`) would grow monotonically and the Settings -> Services screen
 * could never return to "absent on a clean system" after an uninstall.
 * Deleting a row that does not exist succeeds. */
bool jw_svc_control_store_delete(jw_svc_control_store *store,
                                 const char *service_id,
                                 char *reason, size_t reason_size);

bool jw_svc_control_store_put(jw_svc_control_store *store, const char *service_id,
                               const jw_svc_control_state *state,
                               char *reason, size_t reason_size);

/* Applies a one-time persistent enablement decision and records its migration
 * marker in the same SQLite transaction. If migration_id is already recorded,
 * no control row is changed and *out_applied is false. This is deliberately a
 * partial update: it changes only start_with_leaf, preserving any status,
 * package identity, backoff, and session fields already recorded for the
 * service. The marker remains even if the service row is later deleted, so a
 * restored legacy config cannot silently re-enable an uninstalled service.
 * Returns false with "invalid-arguments" or "migration-failed". */
bool jw_svc_control_store_apply_intent_migration(
    jw_svc_control_store *store, const char *migration_id,
    const char *service_id, bool start_with_leaf, bool *out_applied,
    char *reason, size_t reason_size);

/* Reads only the durable one-time marker. Used before consulting a legacy
 * source so a completed migration never depends on that source remaining
 * readable. Returns false with "invalid-arguments" or "read-failed". */
bool jw_svc_control_store_has_migration(
    jw_svc_control_store *store, const char *migration_id, bool *out_found,
    char *reason, size_t reason_size);

#endif
