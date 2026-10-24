#include "internal/db/relocation.h"

#include <stdio.h>
#include <string.h>

static void rel__error(char *out, size_t out_size, const char *message) {
    if (out && out_size) snprintf(out, out_size, "%s", message ? message : "");
}

static int rel__exec(sqlite3 *db, const char *sql) {
    return sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

static const char *rel__text(sqlite3_stmt *stmt, int column) {
    const unsigned char *value = sqlite3_column_text(stmt, column);
    return value ? (const char *)value : "";
}

static void rel__copy(char *out, size_t out_size, const char *value) {
    snprintf(out, out_size, "%s", value ? value : "");
}

static int rel__generation(sqlite3 *db, int *out) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT CAST(value AS INTEGER) FROM settings "
            "WHERE key='library.generation';", -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    int rc = sqlite3_step(stmt);
    *out = rc == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW || rc == SQLITE_DONE ? 0 : -1;
}

static int rel__set_generation(sqlite3 *db, int generation) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO settings(key,value) VALUES('library.generation',?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    char value[32];
    snprintf(value, sizeof(value), "%d", generation);
    sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int jw_db_relocation_status(sqlite3 *db, const char *operation_id,
                            jw_relocation_status *out) {
    if (!db || !operation_id || !operation_id[0] || !out) return JW_RELOCATION_ERROR;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT operation_id,state,expected_generation,"
            "mapping_generation,scan_ticket_generation,item_count "
            "FROM library_relocation_ops WHERE operation_id=?;",
            -1, &stmt, NULL) != SQLITE_OK) return JW_RELOCATION_ERROR;
    sqlite3_bind_text(stmt, 1, operation_id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    if (step == SQLITE_ROW) {
        memset(out, 0, sizeof(*out));
        rel__copy(out->operation_id, sizeof(out->operation_id), rel__text(stmt, 0));
        rel__copy(out->state, sizeof(out->state), rel__text(stmt, 1));
        out->expected_generation = sqlite3_column_int(stmt, 2);
        out->mapping_generation = sqlite3_column_int(stmt, 3);
        out->scan_ticket_generation = sqlite3_column_int(stmt, 4);
        out->item_count = sqlite3_column_int(stmt, 5);
    }
    sqlite3_finalize(stmt);
    return step == SQLITE_ROW ? JW_RELOCATION_OK :
           step == SQLITE_DONE ? JW_RELOCATION_NOT_FOUND : JW_RELOCATION_ERROR;
}

static int rel__existing_matches(sqlite3 *db, const char *operation_id,
                                 int expected_generation,
                                 const char *request_fingerprint) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT expected_generation,request_fingerprint "
            "FROM library_relocation_ops WHERE operation_id=?;",
            -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, operation_id, -1, SQLITE_TRANSIENT);
    int step = sqlite3_step(stmt);
    int match = step == SQLITE_ROW &&
        sqlite3_column_int(stmt, 0) == expected_generation &&
        strcmp(rel__text(stmt, 1), request_fingerprint ? request_fingerprint : "") == 0;
    sqlite3_finalize(stmt);
    return step == SQLITE_DONE ? 0 : match ? 1 : -1;
}

int jw_db_relocation_prepare(sqlite3 *db, const char *operation_id,
                             int expected_generation,
                             const char *request_fingerprint,
                             jw_relocation_item *items, int item_count,
                             jw_relocation_status *out,
                             char *error, size_t error_size) {
    if (!db || !operation_id || !operation_id[0] ||
        strlen(operation_id) >= JW_RELOCATION_OPERATION_ID_MAX ||
        !items || item_count <= 0 || item_count > JW_RELOCATION_MAX_ITEMS) {
        rel__error(error, error_size, "invalid relocation batch");
        return JW_RELOCATION_ERROR;
    }
    int existing = rel__existing_matches(db, operation_id, expected_generation,
                                         request_fingerprint);
    if (existing > 0) return jw_db_relocation_status(db, operation_id, out);
    if (existing < 0) {
        rel__error(error, error_size, "operation id reused with different input");
        return JW_RELOCATION_CONFLICT;
    }
    int generation = 0;
    if (rel__generation(db, &generation) != 0 || generation != expected_generation) {
        rel__error(error, error_size, "stale library generation");
        return JW_RELOCATION_STALE;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM library_relocation_ops "
            "WHERE state NOT IN ('aborted','finished') LIMIT 1;",
            -1, &stmt, NULL) != SQLITE_OK) return JW_RELOCATION_ERROR;
    int busy = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    if (busy) {
        rel__error(error, error_size, "another relocation is active");
        return JW_RELOCATION_BUSY;
    }

    for (int i = 0; i < item_count; i++) {
        for (int j = 0; j < i; j++) {
            if ((strcmp(items[i].old_identity.source_id, items[j].old_identity.source_id) == 0 &&
                 strcmp(items[i].old_identity.rom_relpath, items[j].old_identity.rom_relpath) == 0) ||
                (strcmp(items[i].new_identity.source_id, items[j].new_identity.source_id) == 0 &&
                 strcmp(items[i].new_identity.rom_relpath, items[j].new_identity.rom_relpath) == 0)) {
                rel__error(error, error_size, "duplicate relocation locator");
                return JW_RELOCATION_CONFLICT;
            }
        }
        if (sqlite3_prepare_v2(db,
                "SELECT id,rom_path,COALESCE(image_root_kind,''),"
                "COALESCE(image_relpath,''),COALESCE(image_path,'') "
                "FROM games WHERE source_id=? AND rom_relpath=?;",
                -1, &stmt, NULL) != SQLITE_OK) return JW_RELOCATION_ERROR;
        sqlite3_bind_text(stmt, 1, items[i].old_identity.source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, items[i].old_identity.rom_relpath, -1, SQLITE_TRANSIENT);
        int step = sqlite3_step(stmt);
        if (step != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            rel__error(error, error_size, "source game row missing");
            return JW_RELOCATION_NOT_FOUND;
        }
        items[i].game_id = sqlite3_column_int(stmt, 0);
        rel__copy(items[i].old_rom_path, sizeof(items[i].old_rom_path), rel__text(stmt, 1));
        if (strcmp(items[i].old_identity.image_root_kind, rel__text(stmt, 2)) != 0 ||
            strcmp(items[i].old_identity.image_relpath, rel__text(stmt, 3)) != 0) {
            sqlite3_finalize(stmt);
            rel__error(error, error_size, "conflicting image locator");
            return JW_RELOCATION_CONFLICT;
        }
        rel__copy(items[i].old_image_path, sizeof(items[i].old_image_path), rel__text(stmt, 4));
        sqlite3_finalize(stmt);

        if (sqlite3_prepare_v2(db,
                "SELECT id FROM games WHERE source_id=? AND rom_relpath=?;",
                -1, &stmt, NULL) != SQLITE_OK) return JW_RELOCATION_ERROR;
        sqlite3_bind_text(stmt, 1, items[i].new_identity.source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, items[i].new_identity.rom_relpath, -1, SQLITE_TRANSIENT);
        step = sqlite3_step(stmt);
        int destination_id = step == SQLITE_ROW ? sqlite3_column_int(stmt, 0) : 0;
        sqlite3_finalize(stmt);
        if (destination_id && destination_id != items[i].game_id) {
            rel__error(error, error_size, "destination locator already exists");
            return JW_RELOCATION_CONFLICT;
        }
    }

    if (rel__exec(db, "BEGIN IMMEDIATE;") != 0) return JW_RELOCATION_ERROR;
    int rc = JW_RELOCATION_ERROR;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO library_relocation_ops("
            "operation_id,state,expected_generation,mapping_generation,"
            "scan_ticket_generation,item_count,request_fingerprint,updated_at)"
            "VALUES(?,'prepared',?,0,0,?,?,strftime('%s','now'));",
            -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(stmt, 1, operation_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, expected_generation);
    sqlite3_bind_int(stmt, 3, item_count);
    sqlite3_bind_text(stmt, 4, request_fingerprint ? request_fingerprint : "",
                      -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) { sqlite3_finalize(stmt); goto done; }
    sqlite3_finalize(stmt);
    stmt = NULL;

    const char *insert_item =
        "INSERT INTO library_relocation_items("
        "operation_id,ordinal,game_id,old_source_id,old_rom_relpath,"
        "old_image_root_kind,old_image_relpath,old_rom_path,old_image_path,"
        "new_source_id,new_rom_relpath,new_image_root_kind,new_image_relpath,"
        "new_rom_path,new_image_path,old_source_snapshot,new_source_snapshot)"
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?);";
    if (sqlite3_prepare_v2(db, insert_item, -1, &stmt, NULL) != SQLITE_OK) goto done;
    for (int i = 0; i < item_count; i++) {
        jw_relocation_item *v = &items[i];
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, operation_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 2, i);
        sqlite3_bind_int(stmt, 3, v->game_id);
        sqlite3_bind_text(stmt, 4, v->old_identity.source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, v->old_identity.rom_relpath, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, v->old_identity.image_root_kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, v->old_identity.image_relpath, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 8, v->old_rom_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 9, v->old_image_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 10, v->new_identity.source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 11, v->new_identity.rom_relpath, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 12, v->new_identity.image_root_kind, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 13, v->new_identity.image_relpath, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 14, v->new_rom_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 15, v->new_image_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 16, v->old_source_snapshot, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 17, v->new_source_snapshot, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) goto done;
    }
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (rel__exec(db, "COMMIT;") != 0) return JW_RELOCATION_ERROR;
    return jw_db_relocation_status(db, operation_id, out);
done:
    if (stmt) sqlite3_finalize(stmt);
    rel__exec(db, "ROLLBACK;");
    return rc;
}

int jw_db_relocation_load_items(sqlite3 *db, const char *operation_id,
                                jw_relocation_item *out, int max_items,
                                int *out_count) {
    if (!db || !operation_id || !out || max_items <= 0 || !out_count)
        return JW_RELOCATION_ERROR;
    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "SELECT game_id,old_source_id,old_rom_relpath,old_image_root_kind,"
        "old_image_relpath,old_rom_path,old_image_path,new_source_id,"
        "new_rom_relpath,new_image_root_kind,new_image_relpath,new_rom_path,"
        "new_image_path,old_source_snapshot,new_source_snapshot "
        "FROM library_relocation_items WHERE operation_id=? ORDER BY ordinal;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return JW_RELOCATION_ERROR;
    sqlite3_bind_text(stmt, 1, operation_id, -1, SQLITE_TRANSIENT);
    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= max_items) { sqlite3_finalize(stmt); return JW_RELOCATION_ERROR; }
        jw_relocation_item *v = &out[count++];
        memset(v, 0, sizeof(*v));
        v->game_id = sqlite3_column_int(stmt, 0);
        rel__copy(v->old_identity.source_id, sizeof(v->old_identity.source_id), rel__text(stmt, 1));
        rel__copy(v->old_identity.rom_relpath, sizeof(v->old_identity.rom_relpath), rel__text(stmt, 2));
        rel__copy(v->old_identity.image_root_kind, sizeof(v->old_identity.image_root_kind), rel__text(stmt, 3));
        rel__copy(v->old_identity.image_relpath, sizeof(v->old_identity.image_relpath), rel__text(stmt, 4));
        rel__copy(v->old_rom_path, sizeof(v->old_rom_path), rel__text(stmt, 5));
        rel__copy(v->old_image_path, sizeof(v->old_image_path), rel__text(stmt, 6));
        rel__copy(v->new_identity.source_id, sizeof(v->new_identity.source_id), rel__text(stmt, 7));
        rel__copy(v->new_identity.rom_relpath, sizeof(v->new_identity.rom_relpath), rel__text(stmt, 8));
        rel__copy(v->new_identity.image_root_kind, sizeof(v->new_identity.image_root_kind), rel__text(stmt, 9));
        rel__copy(v->new_identity.image_relpath, sizeof(v->new_identity.image_relpath), rel__text(stmt, 10));
        rel__copy(v->new_rom_path, sizeof(v->new_rom_path), rel__text(stmt, 11));
        rel__copy(v->new_image_path, sizeof(v->new_image_path), rel__text(stmt, 12));
        rel__copy(v->old_source_snapshot, sizeof(v->old_source_snapshot), rel__text(stmt, 13));
        rel__copy(v->new_source_snapshot, sizeof(v->new_source_snapshot), rel__text(stmt, 14));
    }
    sqlite3_finalize(stmt);
    *out_count = count;
    return count > 0 ? JW_RELOCATION_OK : JW_RELOCATION_NOT_FOUND;
}

int jw_db_relocation_refresh_items(sqlite3 *db, const char *operation_id,
                                   const jw_relocation_item *items,
                                   int item_count) {
    if (!db || !operation_id || !items || item_count <= 0) return -1;
    if (rel__exec(db, "BEGIN IMMEDIATE;") != 0) return -1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE library_relocation_items SET "
            "old_rom_path=?,old_image_path=?,new_rom_path=?,new_image_path=?,"
            "old_source_snapshot=?,new_source_snapshot=? "
            "WHERE operation_id=? AND ordinal=? AND game_id=?;",
            -1, &stmt, NULL) != SQLITE_OK) {
        rel__exec(db, "ROLLBACK;");
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < item_count; i++) {
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, items[i].old_rom_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, items[i].old_image_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, items[i].new_rom_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, items[i].new_image_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, items[i].old_source_snapshot, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, items[i].new_source_snapshot, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 7, operation_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 8, i);
        sqlite3_bind_int(stmt, 9, items[i].game_id);
        if (sqlite3_step(stmt) != SQLITE_DONE || sqlite3_changes(db) != 1) {
            rc = -1;
            break;
        }
    }
    sqlite3_finalize(stmt);
    if (rc == 0 && rel__exec(db, "COMMIT;") == 0) return 0;
    rel__exec(db, "ROLLBACK;");
    return -1;
}

static int rel__move(sqlite3 *db, const char *operation_id, int forward,
                     jw_relocation_status *out, char *error, size_t error_size) {
    jw_relocation_status status;
    int sr = jw_db_relocation_status(db, operation_id, &status);
    if (sr != JW_RELOCATION_OK) return sr;
    const char *target_state = forward ? "committed" : "reverted";
    if (strcmp(status.state, target_state) == 0) {
        if (out) *out = status;
        return JW_RELOCATION_OK;
    }
    if ((forward && strcmp(status.state, "prepared") != 0) ||
        (!forward && strcmp(status.state, "committed") != 0)) {
        rel__error(error, error_size, "operation is in the wrong state");
        return JW_RELOCATION_BAD_STATE;
    }
    if (rel__exec(db, "BEGIN IMMEDIATE;") != 0) return JW_RELOCATION_ERROR;
    sqlite3_stmt *select = NULL;
    sqlite3_stmt *update = NULL;
    sqlite3_stmt *visibility = NULL;
    int rc = JW_RELOCATION_ERROR;
    const char *select_sql =
        "SELECT game_id,old_source_id,old_rom_relpath,old_image_root_kind,"
        "old_image_relpath,old_rom_path,old_image_path,new_source_id,"
        "new_rom_relpath,new_image_root_kind,new_image_relpath,new_rom_path,"
        "new_image_path FROM library_relocation_items "
        "WHERE operation_id=? ORDER BY ordinal;";
    const char *update_sql =
        "UPDATE games SET source_id=?,rom_relpath=?,image_root_kind=NULLIF(?,''),"
        "image_relpath=NULLIF(?,''),rom_path=?,image_path=NULLIF(?,'') "
        "WHERE id=? AND source_id=? AND rom_relpath=?;";
    const char *visibility_sql =
        "UPDATE OR REPLACE main.hidden_roms SET source_id=?,rom_relpath=? "
        "WHERE source_id=? AND rom_relpath=?;";
    if (sqlite3_prepare_v2(db, select_sql, -1, &select, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, update_sql, -1, &update, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, visibility_sql, -1, &visibility, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(select, 1, operation_id, -1, SQLITE_TRANSIENT);
    while (sqlite3_step(select) == SQLITE_ROW) {
        int base = forward ? 7 : 1;
        sqlite3_reset(update);
        sqlite3_bind_text(update, 1, rel__text(select, base), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, 2, rel__text(select, base + 1), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, 3, rel__text(select, base + 2), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, 4, rel__text(select, base + 3), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, 5, rel__text(select, base + 4), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, 6, rel__text(select, base + 5), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(update, 7, sqlite3_column_int(select, 0));
        int expected_base = forward ? 1 : 7;
        sqlite3_bind_text(update, 8, rel__text(select, expected_base), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(update, 9, rel__text(select, expected_base + 1), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(update) != SQLITE_DONE || sqlite3_changes(db) != 1) {
            rel__error(error, error_size, "reserved game row changed");
            goto done;
        }
        sqlite3_reset(visibility);
        sqlite3_bind_text(visibility, 1, rel__text(select, base), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(visibility, 2, rel__text(select, base + 1), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(visibility, 3, rel__text(select, expected_base), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(visibility, 4, rel__text(select, expected_base + 1), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(visibility) != SQLITE_DONE) goto done;
    }
    int generation = 0;
    if (rel__generation(db, &generation) != 0 ||
        rel__set_generation(db, generation + 1) != 0) goto done;
    sqlite3_stmt *op = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE library_relocation_ops SET state=?,mapping_generation=?,"
            "updated_at=strftime('%s','now') WHERE operation_id=?;",
            -1, &op, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(op, 1, target_state, -1, SQLITE_STATIC);
    sqlite3_bind_int(op, 2, generation + 1);
    sqlite3_bind_text(op, 3, operation_id, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(op) != SQLITE_DONE) { sqlite3_finalize(op); goto done; }
    sqlite3_finalize(op);
    if (rel__exec(db, "COMMIT;") != 0) goto done;
    sqlite3_finalize(select);
    sqlite3_finalize(update);
    sqlite3_finalize(visibility);
    return jw_db_relocation_status(db, operation_id, out);
done:
    if (select) sqlite3_finalize(select);
    if (update) sqlite3_finalize(update);
    if (visibility) sqlite3_finalize(visibility);
    rel__exec(db, "ROLLBACK;");
    return rc;
}

int jw_db_relocation_commit(sqlite3 *db, const char *operation_id,
                            jw_relocation_status *out,
                            char *error, size_t error_size) {
    return rel__move(db, operation_id, 1, out, error, error_size);
}

int jw_db_relocation_revert(sqlite3 *db, const char *operation_id,
                            jw_relocation_status *out,
                            char *error, size_t error_size) {
    return rel__move(db, operation_id, 0, out, error, error_size);
}

static int rel__state(sqlite3 *db, const char *operation_id,
                      const char *from_a, const char *from_b, const char *to,
                      int ticket, jw_relocation_status *out) {
    jw_relocation_status status;
    int rc = jw_db_relocation_status(db, operation_id, &status);
    if (rc != JW_RELOCATION_OK) return rc;
    if (strcmp(status.state, to) == 0) { if (out) *out = status; return 0; }
    if (strcmp(status.state, from_a) != 0 &&
        (!from_b || strcmp(status.state, from_b) != 0)) return JW_RELOCATION_BAD_STATE;
    sqlite3_stmt *stmt = NULL;
    const char *sql = ticket
        ? "UPDATE library_relocation_ops SET state=?,scan_ticket_generation=?,"
          "updated_at=strftime('%s','now') WHERE operation_id=?;"
        : "UPDATE library_relocation_ops SET state=?,updated_at=strftime('%s','now') "
          "WHERE operation_id=?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return JW_RELOCATION_ERROR;
    sqlite3_bind_text(stmt, 1, to, -1, SQLITE_STATIC);
    int p = 2;
    if (ticket) sqlite3_bind_int(stmt, p++, ticket);
    sqlite3_bind_text(stmt, p, operation_id, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : JW_RELOCATION_ERROR;
    sqlite3_finalize(stmt);
    return rc == 0 ? jw_db_relocation_status(db, operation_id, out) : rc;
}

int jw_db_relocation_abort(sqlite3 *db, const char *operation_id,
                           jw_relocation_status *out) {
    return rel__state(db, operation_id, "prepared", NULL, "aborted", 0, out);
}

int jw_db_relocation_finish(sqlite3 *db, const char *operation_id,
                            jw_relocation_status *out) {
    jw_relocation_status status;
    int rc = jw_db_relocation_status(db, operation_id, &status);
    if (rc != 0) return rc;
    if (strcmp(status.state, "finishing") == 0 ||
        strcmp(status.state, "finished") == 0) {
        if (out) *out = status;
        return 0;
    }
    int generation = 0;
    if (rel__generation(db, &generation) != 0) return JW_RELOCATION_ERROR;
    int ticket = generation + 1;
    if (ticket <= status.mapping_generation) ticket = status.mapping_generation + 1;
    return rel__state(db, operation_id, "committed", "reverted",
                      "finishing", ticket, out);
}

int jw_db_relocation_note_scan(sqlite3 *db, int generation) {
    if (!db || generation <= 0 || rel__exec(db, "BEGIN IMMEDIATE;") != 0) return -1;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(db,
            "INSERT INTO settings(key,value) VALUES('library.last_scan_generation',?) "
            "ON CONFLICT(key) DO UPDATE SET value=excluded.value;",
            -1, &stmt, NULL) != SQLITE_OK) goto done;
    char value[32];
    snprintf(value, sizeof(value), "%d", generation);
    sqlite3_bind_text(stmt, 1, value, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto done;
    sqlite3_finalize(stmt);
    stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "UPDATE library_relocation_ops SET state='finished',"
            "updated_at=strftime('%s','now') WHERE state='finishing' "
            "AND scan_ticket_generation<=? AND mapping_generation<?;",
            -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_int(stmt, 1, generation);
    sqlite3_bind_int(stmt, 2, generation);
    if (sqlite3_step(stmt) != SQLITE_DONE) goto done;
    sqlite3_finalize(stmt);
    stmt = NULL;
    rc = rel__exec(db, "COMMIT;");
    return rc;
done:
    if (stmt) sqlite3_finalize(stmt);
    rel__exec(db, "ROLLBACK;");
    return rc;
}

int jw_db_relocation_game_reserved(sqlite3 *db, int game_id) {
    if (!db || game_id <= 0) return 1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM library_relocation_items i "
            "JOIN library_relocation_ops o USING(operation_id) "
            "WHERE i.game_id=? AND o.state IN ('prepared','committed','reverted') "
            "LIMIT 1;", -1, &stmt, NULL) != SQLITE_OK) return 1;
    sqlite3_bind_int(stmt, 1, game_id);
    int reserved = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return reserved;
}

int jw_db_relocation_key_reserved(sqlite3 *db, const char *source_id,
                                  const char *rom_relpath) {
    if (!db || !source_id || !rom_relpath) return 1;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT 1 FROM library_relocation_items i "
            "JOIN library_relocation_ops o USING(operation_id) "
            "WHERE o.state IN ('prepared','committed','reverted') AND "
            "((i.old_source_id=? AND i.old_rom_relpath=?) OR "
            "(i.new_source_id=? AND i.new_rom_relpath=?)) LIMIT 1;",
            -1, &stmt, NULL) != SQLITE_OK) return 1;
    sqlite3_bind_text(stmt, 1, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, rom_relpath, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, rom_relpath, -1, SQLITE_TRANSIENT);
    int reserved = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    return reserved;
}
