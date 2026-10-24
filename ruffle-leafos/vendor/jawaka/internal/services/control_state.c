#include "internal/services/control_state.h"

#include <limits.h>
#include <stdint.h>
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

struct jw_svc_control_store {
    sqlite3 *db;
};

#define JW__CONTROL_STRINGIFY_INNER(value) #value
#define JW__CONTROL_STRINGIFY(value) JW__CONTROL_STRINGIFY_INNER(value)

static const char *const JW__CONTROL_SCHEMA_SQL =
    "CREATE TABLE IF NOT EXISTS service_control_state (\n"
    "  service_id TEXT PRIMARY KEY NOT NULL\n"
    "    CHECK (typeof(service_id) = 'text' AND\n"
    "           length(CAST(service_id AS BLOB)) BETWEEN 1 AND "
    JW__CONTROL_STRINGIFY(JW_SVC_CONTROL_ID_MAX) "),\n"
    "  start_with_leaf INTEGER NOT NULL DEFAULT 0\n"
    "    CHECK (start_with_leaf IN (0, 1)),\n"
    "  session_run INTEGER NOT NULL DEFAULT 0\n"
    "    CHECK (session_run IN (0, 1)),\n"
    "  last_transition_at_us INTEGER NOT NULL DEFAULT 0,\n"
    "  last_transition_reason TEXT NOT NULL DEFAULT ''\n"
    "    CHECK (typeof(last_transition_reason) = 'text' AND\n"
    "           length(CAST(last_transition_reason AS BLOB)) <= "
    JW__CONTROL_STRINGIFY(JW_SVC_CONTROL_REASON_MAX) "),\n"
    "  has_last_exit INTEGER NOT NULL DEFAULT 0\n"
    "    CHECK (has_last_exit IN (0, 1)),\n"
    "  last_exit_code INTEGER NOT NULL DEFAULT 0,\n"
    "  last_exit_at_us INTEGER NOT NULL DEFAULT 0,\n"
    "  restart_count INTEGER NOT NULL DEFAULT 0,\n"
    "  breaker_open INTEGER NOT NULL DEFAULT 0\n"
    "    CHECK (breaker_open IN (0, 1)),\n"
    "  backoff_failure_count INTEGER NOT NULL DEFAULT 0\n"
    "    CHECK (backoff_failure_count BETWEEN 0 AND "
    JW__CONTROL_STRINGIFY(JW_SVC_CONTROL_BACKOFF_TRACKED) "),\n"
    "  backoff_failure_time_1_us INTEGER NOT NULL DEFAULT 0,\n"
    "  backoff_failure_time_2_us INTEGER NOT NULL DEFAULT 0,\n"
    "  backoff_failure_time_3_us INTEGER NOT NULL DEFAULT 0,\n"
    "  backoff_failure_time_4_us INTEGER NOT NULL DEFAULT 0,\n"
    "  backoff_failure_time_5_us INTEGER NOT NULL DEFAULT 0,\n"
    "  installed_package_id TEXT NOT NULL DEFAULT ''\n"
    "    CHECK (typeof(installed_package_id) = 'text' AND\n"
    "           length(CAST(installed_package_id AS BLOB)) <= "
    JW__CONTROL_STRINGIFY(JW_SVC_CONTROL_PACKAGE_ID_MAX) "),\n"
    "  installed_package_version TEXT NOT NULL DEFAULT ''\n"
    "    CHECK (typeof(installed_package_version) = 'text' AND\n"
    "           length(CAST(installed_package_version AS BLOB)) <= "
    JW__CONTROL_STRINGIFY(JW_SVC_CONTROL_PACKAGE_VERSION_MAX) ")\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS service_control_migrations (\n"
    "  migration_id TEXT PRIMARY KEY NOT NULL\n"
    "    CHECK (typeof(migration_id) = 'text' AND\n"
    "           length(CAST(migration_id AS BLOB)) BETWEEN 1 AND "
    JW__CONTROL_STRINGIFY(JW_SVC_CONTROL_MIGRATION_ID_MAX) ")\n"
    ");\n"
    "PRAGMA user_version = 2;\n";

static const char *const JW__CONTROL_SCHEMA_PROBE_SQL =
    "SELECT start_with_leaf, session_run, last_transition_at_us,\n"
    "       last_transition_reason, has_last_exit, last_exit_code,\n"
    "       last_exit_at_us, restart_count, breaker_open,\n"
    "       backoff_failure_count, backoff_failure_time_1_us,\n"
    "       backoff_failure_time_2_us, backoff_failure_time_3_us,\n"
    "       backoff_failure_time_4_us, backoff_failure_time_5_us,\n"
    "       installed_package_id, installed_package_version\n"
    "  FROM service_control_state LIMIT 0;";

static const char *const JW__CONTROL_MIGRATION_PROBE_SQL =
    "SELECT migration_id FROM service_control_migrations LIMIT 0;";

static void jw__control_set_reason(char *reason, size_t reason_size, const char *value) {
    if (!reason || reason_size == 0) {
        return;
    }
    size_t n = strlen(value);
    if (n >= reason_size) {
        n = reason_size - 1;
    }
    memcpy(reason, value, n);
    reason[n] = '\0';
}

/* The state fields passed here are fixed arrays, so inspecting at most
 * buf_size bytes cannot run beyond the caller's struct. */
static bool jw__control_has_terminator(const char *buf, size_t buf_size) {
    for (size_t i = 0; i < buf_size; i++) {
        if (buf[i] == '\0') {
            return true;
        }
    }
    return false;
}

static bool jw__control_read_user_version(sqlite3 *db, int *out_version) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL) !=
        SQLITE_OK) {
        return false;
    }

    bool ok = sqlite3_step(stmt) == SQLITE_ROW;
    if (ok) {
        *out_version = sqlite3_column_int(stmt, 0);
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK) {
        ok = false;
    }
    return ok;
}

static bool jw__control_apply_and_validate_schema(sqlite3 *db) {
    char *errmsg = NULL;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE;", NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
        return false;
    }

    int version = 0;
    bool ok = jw__control_read_user_version(db, &version) &&
              version >= 0 && version <= 2;
    if (ok &&
        sqlite3_exec(db, JW__CONTROL_SCHEMA_SQL, NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
        errmsg = NULL;
        ok = false;
    }

    sqlite3_stmt *probe = NULL;
    if (ok &&
        sqlite3_prepare_v2(db, JW__CONTROL_SCHEMA_PROBE_SQL, -1, &probe, NULL) !=
            SQLITE_OK) {
        ok = false;
    }
    if (probe && sqlite3_finalize(probe) != SQLITE_OK) {
        ok = false;
    }
    probe = NULL;
    if (ok &&
        sqlite3_prepare_v2(db, JW__CONTROL_MIGRATION_PROBE_SQL, -1, &probe,
                           NULL) != SQLITE_OK) {
        ok = false;
    }
    if (probe && sqlite3_finalize(probe) != SQLITE_OK) {
        ok = false;
    }

    if (ok) {
        if (sqlite3_exec(db, "COMMIT;", NULL, NULL, &errmsg) == SQLITE_OK) {
            return true;
        }
        sqlite3_free(errmsg);
        errmsg = NULL;
    }

    sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
    return false;
}

bool jw_svc_control_store_open(const char *db_path, jw_svc_control_store **out,
                                char *reason, size_t reason_size) {
    if (out) {
        *out = NULL;
    }
    if (!db_path || !db_path[0] || !out) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        if (db) {
            sqlite3_close(db);
        }
        jw__control_set_reason(reason, reason_size, "open-failed");
        return false;
    }
    if (sqlite3_busy_timeout(db, 2000) != SQLITE_OK ||
        !jw__control_apply_and_validate_schema(db)) {
        sqlite3_close(db);
        jw__control_set_reason(reason, reason_size, "open-failed");
        return false;
    }

    jw_svc_control_store *store = (jw_svc_control_store *)malloc(sizeof(*store));
    if (!store) {
        sqlite3_close(db);
        jw__control_set_reason(reason, reason_size, "open-failed");
        return false;
    }
    store->db = db;
    *out = store;
    return true;
}

void jw_svc_control_store_close(jw_svc_control_store *store) {
    if (!store) {
        return;
    }
    sqlite3_close(store->db);
    free(store);
}

bool jw_svc_control_store_clear_all_sessions(jw_svc_control_store *store,
                                              char *reason, size_t reason_size) {
    if (!store) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    char *errmsg = NULL;
    if (sqlite3_exec(store->db, "UPDATE service_control_state SET session_run = 0;",
                      NULL, NULL, &errmsg) != SQLITE_OK) {
        sqlite3_free(errmsg);
        jw__control_set_reason(reason, reason_size, "write-failed");
        return false;
    }
    return true;
}

static bool jw__control_service_id_is_valid(const char *service_id) {
    if (!service_id) {
        return false;
    }
    for (size_t i = 0; i <= (size_t)JW_SVC_CONTROL_ID_MAX; i++) {
        if (service_id[i] == '\0') {
            return i > 0;
        }
    }
    return false;
}

static bool jw__control_column_int64(sqlite3_stmt *stmt, int column,
                                      long long *out) {
    if (sqlite3_column_type(stmt, column) != SQLITE_INTEGER) {
        return false;
    }
    *out = (long long)sqlite3_column_int64(stmt, column);
    return true;
}

static bool jw__control_column_int(sqlite3_stmt *stmt, int column, int *out) {
    if (sqlite3_column_type(stmt, column) != SQLITE_INTEGER) {
        return false;
    }
    sqlite3_int64 value = sqlite3_column_int64(stmt, column);
    if (value < (sqlite3_int64)INT_MIN || value > (sqlite3_int64)INT_MAX) {
        return false;
    }
    *out = (int)value;
    return true;
}

static bool jw__control_column_bool(sqlite3_stmt *stmt, int column, bool *out) {
    int value = 0;
    if (!jw__control_column_int(stmt, column, &value) ||
        (value != 0 && value != 1)) {
        return false;
    }
    *out = value != 0;
    return true;
}

static bool jw__control_copy_text_column(sqlite3_stmt *stmt, int column,
                                          char *out, size_t out_size) {
    if (out_size == 0 || sqlite3_column_type(stmt, column) != SQLITE_TEXT) {
        return false;
    }

    const unsigned char *text = sqlite3_column_text(stmt, column);
    int byte_count = sqlite3_column_bytes(stmt, column);
    if (byte_count < 0) {
        return false;
    }
    if (byte_count == 0) {
        /* Some SQLite builds may return NULL for a zero-byte TEXT value.
         * It is still the valid empty string because the SQL type was
         * checked above; SQL NULL is rejected. */
        if (!text &&
            sqlite3_errcode(sqlite3_db_handle(stmt)) == SQLITE_NOMEM) {
            return false;
        }
        out[0] = '\0';
        return true;
    }
    if (!text) {
        return false;
    }

    size_t text_size = (size_t)byte_count;
    if (text_size >= out_size || memchr(text, '\0', text_size) != NULL) {
        return false;
    }
    memcpy(out, text, text_size);
    out[text_size] = '\0';
    return true;
}

bool jw_svc_control_store_get(jw_svc_control_store *store, const char *service_id,
                               jw_svc_control_state *out, bool *out_found,
                               char *reason, size_t reason_size) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (out_found) {
        *out_found = false;
    }
    if (!store || !out || !out_found || !jw__control_service_id_is_valid(service_id)) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    static const char *const kSql =
        "SELECT start_with_leaf, session_run, last_transition_at_us,\n"
        "       last_transition_reason, has_last_exit, last_exit_code,\n"
        "       last_exit_at_us, restart_count, breaker_open,\n"
        "       backoff_failure_count, backoff_failure_time_1_us,\n"
        "       backoff_failure_time_2_us, backoff_failure_time_3_us,\n"
        "       backoff_failure_time_4_us, backoff_failure_time_5_us,\n"
        "       installed_package_id, installed_package_version\n"
        "  FROM service_control_state WHERE service_id = ?1;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, kSql, -1, &stmt, NULL) != SQLITE_OK) {
        jw__control_set_reason(reason, reason_size, "read-failed");
        return false;
    }
    if (sqlite3_bind_text(stmt, 1, service_id, -1, SQLITE_TRANSIENT) !=
        SQLITE_OK) {
        sqlite3_finalize(stmt);
        jw__control_set_reason(reason, reason_size, "read-failed");
        return false;
    }

    int step = sqlite3_step(stmt);
    if (step == SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return true; /* out_found already false, out already zeroed */
    }
    if (step != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        jw__control_set_reason(reason, reason_size, "read-failed");
        return false;
    }

    jw_svc_control_state result;
    memset(&result, 0, sizeof(result));
    if (!jw__control_column_bool(stmt, 0, &result.start_with_leaf) ||
        !jw__control_column_bool(stmt, 1, &result.session_run) ||
        !jw__control_column_int64(stmt, 2, &result.last_transition_at_us) ||
        !jw__control_copy_text_column(stmt, 3, result.last_transition_reason,
                                       sizeof(result.last_transition_reason)) ||
        !jw__control_column_bool(stmt, 4, &result.has_last_exit) ||
        !jw__control_column_int(stmt, 5, &result.last_exit_code) ||
        !jw__control_column_int64(stmt, 6, &result.last_exit_at_us) ||
        !jw__control_column_int(stmt, 7, &result.restart_count) ||
        !jw__control_column_bool(stmt, 8, &result.breaker_open) ||
        !jw__control_column_int(stmt, 9, &result.backoff_failure_count) ||
        result.backoff_failure_count < 0 ||
        result.backoff_failure_count > JW_SVC_CONTROL_BACKOFF_TRACKED) {
        sqlite3_finalize(stmt);
        jw__control_set_reason(reason, reason_size, "read-failed");
        return false;
    }
    for (int i = 0; i < JW_SVC_CONTROL_BACKOFF_TRACKED; i++) {
        if (!jw__control_column_int64(stmt, 10 + i,
                                      &result.backoff_failure_times_us[i])) {
            sqlite3_finalize(stmt);
            jw__control_set_reason(reason, reason_size, "read-failed");
            return false;
        }
    }
    if (!jw__control_copy_text_column(stmt, 15, result.installed_package_id,
                                       sizeof(result.installed_package_id)) ||
        !jw__control_copy_text_column(stmt, 16, result.installed_package_version,
                                       sizeof(result.installed_package_version))) {
        sqlite3_finalize(stmt);
        jw__control_set_reason(reason, reason_size, "read-failed");
        return false;
    }

    sqlite3_finalize(stmt);
    *out = result;
    *out_found = true;
    return true;
}

bool jw_svc_control_store_list_ids(jw_svc_control_store *store,
                                   jw_svc_control_id **out_ids,
                                   size_t *out_count,
                                   char *reason, size_t reason_size) {
    if (out_ids) *out_ids = NULL;
    if (out_count) *out_count = 0;
    if (!store || !out_ids || !out_count) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db,
                           "SELECT service_id FROM service_control_state "
                           "ORDER BY service_id;",
                           -1, &stmt, NULL) != SQLITE_OK) {
        jw__control_set_reason(reason, reason_size, "read-failed");
        return false;
    }

    jw_svc_control_id *ids = NULL;
    size_t count = 0;
    bool ok = true;
    int step = SQLITE_DONE;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (sqlite3_column_type(stmt, 0) != SQLITE_TEXT) {
            ok = false;
            break;
        }
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        int bytes = sqlite3_column_bytes(stmt, 0);
        if (!text || bytes <= 0 || bytes > JW_SVC_CONTROL_ID_MAX ||
            memchr(text, '\0', (size_t)bytes) != NULL ||
            count == SIZE_MAX / sizeof(*ids)) {
            ok = false;
            break;
        }
        jw_svc_control_id *grown =
            realloc(ids, (count + 1u) * sizeof(*ids));
        if (!grown) {
            free(ids);
            sqlite3_finalize(stmt);
            jw__control_set_reason(reason, reason_size, "out-of-memory");
            return false;
        }
        ids = grown;
        memcpy(ids[count].service_id, text, (size_t)bytes);
        ids[count].service_id[bytes] = '\0';
        count++;
    }
    if (step != SQLITE_DONE) {
        ok = false;
    }
    if (sqlite3_finalize(stmt) != SQLITE_OK) {
        ok = false;
    }
    if (!ok) {
        free(ids);
        jw__control_set_reason(reason, reason_size, "read-failed");
        return false;
    }
    *out_ids = ids;
    *out_count = count;
    return true;
}

void jw_svc_control_store_free_ids(jw_svc_control_id *ids) {
    free(ids);
}

bool jw_svc_control_store_delete(jw_svc_control_store *store,
                                 const char *service_id,
                                 char *reason, size_t reason_size) {
    if (!store || !jw__control_service_id_is_valid(service_id)) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    static const char *const kSql =
        "DELETE FROM service_control_state WHERE service_id = ?1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, kSql, -1, &stmt, NULL) != SQLITE_OK) {
        jw__control_set_reason(reason, reason_size, "write-failed");
        return false;
    }
    if (sqlite3_bind_text(stmt, 1, service_id, -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        jw__control_set_reason(reason, reason_size, "write-failed");
        return false;
    }
    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step != SQLITE_DONE) {
        jw__control_set_reason(reason, reason_size, "write-failed");
        return false;
    }
    return true;
}

bool jw_svc_control_store_put(jw_svc_control_store *store, const char *service_id,
                               const jw_svc_control_state *state,
                               char *reason, size_t reason_size) {
    if (!store || !state || !jw__control_service_id_is_valid(service_id)) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    if (state->backoff_failure_count < 0 ||
        state->backoff_failure_count > JW_SVC_CONTROL_BACKOFF_TRACKED) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    if (!jw__control_has_terminator(state->last_transition_reason,
                                     sizeof(state->last_transition_reason)) ||
        !jw__control_has_terminator(state->installed_package_id,
                                     sizeof(state->installed_package_id)) ||
        !jw__control_has_terminator(state->installed_package_version,
                                     sizeof(state->installed_package_version))) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    static const char *const kSql =
        "INSERT INTO service_control_state (\n"
        "  service_id, start_with_leaf, session_run, last_transition_at_us,\n"
        "  last_transition_reason, has_last_exit, last_exit_code,\n"
        "  last_exit_at_us, restart_count, breaker_open,\n"
        "  backoff_failure_count, backoff_failure_time_1_us,\n"
        "  backoff_failure_time_2_us, backoff_failure_time_3_us,\n"
        "  backoff_failure_time_4_us, backoff_failure_time_5_us,\n"
        "  installed_package_id, installed_package_version\n"
        ") VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14, ?15, ?16, ?17, ?18)\n"
        "ON CONFLICT(service_id) DO UPDATE SET\n"
        "  start_with_leaf = excluded.start_with_leaf,\n"
        "  session_run = excluded.session_run,\n"
        "  last_transition_at_us = excluded.last_transition_at_us,\n"
        "  last_transition_reason = excluded.last_transition_reason,\n"
        "  has_last_exit = excluded.has_last_exit,\n"
        "  last_exit_code = excluded.last_exit_code,\n"
        "  last_exit_at_us = excluded.last_exit_at_us,\n"
        "  restart_count = excluded.restart_count,\n"
        "  breaker_open = excluded.breaker_open,\n"
        "  backoff_failure_count = excluded.backoff_failure_count,\n"
        "  backoff_failure_time_1_us = excluded.backoff_failure_time_1_us,\n"
        "  backoff_failure_time_2_us = excluded.backoff_failure_time_2_us,\n"
        "  backoff_failure_time_3_us = excluded.backoff_failure_time_3_us,\n"
        "  backoff_failure_time_4_us = excluded.backoff_failure_time_4_us,\n"
        "  backoff_failure_time_5_us = excluded.backoff_failure_time_5_us,\n"
        "  installed_package_id = excluded.installed_package_id,\n"
        "  installed_package_version = excluded.installed_package_version;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db, kSql, -1, &stmt, NULL) != SQLITE_OK) {
        jw__control_set_reason(reason, reason_size, "write-failed");
        return false;
    }

    if (sqlite3_bind_text(stmt, 1, service_id, -1, SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 2, state->start_with_leaf ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 3, state->session_run ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, state->last_transition_at_us) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 5, state->last_transition_reason, -1,
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 6, state->has_last_exit ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 7, state->last_exit_code) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 8, state->last_exit_at_us) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 9, state->restart_count) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 10, state->breaker_open ? 1 : 0) != SQLITE_OK ||
        sqlite3_bind_int(stmt, 11, state->backoff_failure_count) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        jw__control_set_reason(reason, reason_size, "write-failed");
        return false;
    }
    for (int i = 0; i < JW_SVC_CONTROL_BACKOFF_TRACKED; i++) {
        long long value = i < state->backoff_failure_count
                              ? state->backoff_failure_times_us[i]
                              : 0;
        if (sqlite3_bind_int64(stmt, 12 + i, value) != SQLITE_OK) {
            sqlite3_finalize(stmt);
            jw__control_set_reason(reason, reason_size, "write-failed");
            return false;
        }
    }
    if (sqlite3_bind_text(stmt, 17, state->installed_package_id, -1,
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 18, state->installed_package_version, -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        jw__control_set_reason(reason, reason_size, "write-failed");
        return false;
    }

    int step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (step != SQLITE_DONE) {
        jw__control_set_reason(reason, reason_size, "write-failed");
        return false;
    }
    return true;
}

static bool jw__control_migration_id_is_valid(const char *migration_id) {
    if (!migration_id) {
        return false;
    }
    for (size_t i = 0; i <= (size_t)JW_SVC_CONTROL_MIGRATION_ID_MAX; i++) {
        if (migration_id[i] == '\0') {
            return i > 0;
        }
    }
    return false;
}

bool jw_svc_control_store_has_migration(
    jw_svc_control_store *store, const char *migration_id, bool *out_found,
    char *reason, size_t reason_size) {
    if (out_found) {
        *out_found = false;
    }
    if (!store || !out_found ||
        !jw__control_migration_id_is_valid(migration_id)) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(store->db,
                           "SELECT 1 FROM service_control_migrations "
                           "WHERE migration_id = ?1;",
                           -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 1, migration_id, -1,
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        jw__control_set_reason(reason, reason_size, "read-failed");
        return false;
    }
    int step = sqlite3_step(stmt);
    bool ok = step == SQLITE_ROW || step == SQLITE_DONE;
    bool found = step == SQLITE_ROW;
    if (sqlite3_finalize(stmt) != SQLITE_OK) {
        ok = false;
    }
    if (!ok) {
        jw__control_set_reason(reason, reason_size, "read-failed");
        return false;
    }
    *out_found = found;
    return true;
}

bool jw_svc_control_store_apply_intent_migration(
    jw_svc_control_store *store, const char *migration_id,
    const char *service_id, bool start_with_leaf, bool *out_applied,
    char *reason, size_t reason_size) {
    if (out_applied) {
        *out_applied = false;
    }
    if (!store || !out_applied ||
        !jw__control_migration_id_is_valid(migration_id) ||
        !jw__control_service_id_is_valid(service_id)) {
        jw__control_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    char *errmsg = NULL;
    sqlite3_stmt *stmt = NULL;
    bool ok = sqlite3_exec(store->db, "BEGIN IMMEDIATE;", NULL, NULL,
                           &errmsg) == SQLITE_OK;
    sqlite3_free(errmsg);
    errmsg = NULL;

    if (ok &&
        sqlite3_prepare_v2(store->db,
                           "SELECT 1 FROM service_control_migrations "
                           "WHERE migration_id = ?1;",
                           -1, &stmt, NULL) != SQLITE_OK) {
        ok = false;
    }
    if (ok && sqlite3_bind_text(stmt, 1, migration_id, -1,
                                SQLITE_TRANSIENT) != SQLITE_OK) {
        ok = false;
    }
    int step = ok ? sqlite3_step(stmt) : SQLITE_ERROR;
    bool already_applied = step == SQLITE_ROW;
    if (ok && step != SQLITE_ROW && step != SQLITE_DONE) {
        ok = false;
    }
    if (stmt && sqlite3_finalize(stmt) != SQLITE_OK) {
        ok = false;
    }
    stmt = NULL;

    if (ok && !already_applied &&
        sqlite3_prepare_v2(
            store->db,
            "INSERT INTO service_control_state (service_id, start_with_leaf) "
            "VALUES (?1, ?2) "
            "ON CONFLICT(service_id) DO UPDATE SET "
            "start_with_leaf = excluded.start_with_leaf;",
            -1, &stmt, NULL) != SQLITE_OK) {
        ok = false;
    }
    if (ok && !already_applied &&
        (sqlite3_bind_text(stmt, 1, service_id, -1, SQLITE_TRANSIENT) !=
             SQLITE_OK ||
         sqlite3_bind_int(stmt, 2, start_with_leaf ? 1 : 0) != SQLITE_OK ||
         sqlite3_step(stmt) != SQLITE_DONE)) {
        ok = false;
    }
    if (stmt && sqlite3_finalize(stmt) != SQLITE_OK) {
        ok = false;
    }
    stmt = NULL;

    if (ok && !already_applied &&
        sqlite3_prepare_v2(
            store->db,
            "INSERT INTO service_control_migrations (migration_id) "
            "VALUES (?1);",
            -1, &stmt, NULL) != SQLITE_OK) {
        ok = false;
    }
    if (ok && !already_applied &&
        (sqlite3_bind_text(stmt, 1, migration_id, -1, SQLITE_TRANSIENT) !=
             SQLITE_OK ||
         sqlite3_step(stmt) != SQLITE_DONE)) {
        ok = false;
    }
    if (stmt && sqlite3_finalize(stmt) != SQLITE_OK) {
        ok = false;
    }

    if (ok && sqlite3_exec(store->db, "COMMIT;", NULL, NULL, &errmsg) ==
                  SQLITE_OK) {
        sqlite3_free(errmsg);
        *out_applied = !already_applied;
        return true;
    }
    sqlite3_free(errmsg);
    sqlite3_exec(store->db, "ROLLBACK;", NULL, NULL, NULL);
    jw__control_set_reason(reason, reason_size, "migration-failed");
    return false;
}
