#include "internal/db/db.h"
#include "internal/db/relocation.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int query_int(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

static void seed(sqlite3 *db) {
    assert(sqlite3_exec(db,
        "INSERT INTO settings(key,value) VALUES('library.generation','5');"
        "INSERT INTO games(id,system,name,source_id,rom_relpath,rom_path,"
        "image_root_kind,image_relpath,image_path) VALUES("
        "42,'PORTS','Bully','primary','PORTS/Bully.sh','Roms/PORTS/Bully.sh',"
        "'images','PORTS/Bully.png','Images/PORTS/Bully.png');"
        "INSERT INTO hidden_roms VALUES"
        "('primary','PORTS/Bully.sh',''),"
        "('primary','PORTS/Bully.sh','member'),"
        "('primary','PORTS/Other.sh','');",
        NULL, NULL, NULL) == SQLITE_OK);
}

int main(void) {
    char path[] = "/tmp/jawaka-relocation-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    sqlite3 *db = NULL;
    assert(jw_db_open(path, &db) == 0);
    assert(jw_db_apply_schema(db) == 0);
    seed(db);

    jw_relocation_item item;
    memset(&item, 0, sizeof(item));
    snprintf(item.old_identity.source_id, sizeof(item.old_identity.source_id), "primary");
    snprintf(item.old_identity.rom_relpath, sizeof(item.old_identity.rom_relpath),
             "PORTS/Bully.sh");
    snprintf(item.old_identity.image_root_kind,
             sizeof(item.old_identity.image_root_kind), "images");
    snprintf(item.old_identity.image_relpath, sizeof(item.old_identity.image_relpath),
             "PORTS/Bully.png");
    snprintf(item.new_identity.source_id, sizeof(item.new_identity.source_id),
             "secondary_sd");
    snprintf(item.new_identity.rom_relpath, sizeof(item.new_identity.rom_relpath),
             "PORTS/Bully.sh");
    snprintf(item.new_identity.image_root_kind,
             sizeof(item.new_identity.image_root_kind), "images");
    snprintf(item.new_identity.image_relpath, sizeof(item.new_identity.image_relpath),
             "PORTS/Bully.png");
    snprintf(item.new_rom_path, sizeof(item.new_rom_path),
             "/media/sdcard1/Roms/PORTS/Bully.sh");
    snprintf(item.new_image_path, sizeof(item.new_image_path),
             "/media/sdcard1/Images/PORTS/Bully.png");
    snprintf(item.old_source_snapshot, sizeof(item.old_source_snapshot), "primary-fs");
    snprintf(item.new_source_snapshot, sizeof(item.new_source_snapshot), "secondary-fs");

    jw_relocation_status status;
    char error[256];
    jw_relocation_item duplicates[2] = {item, item};
    assert(jw_db_relocation_prepare(db, "duplicate-destination", 5, "dupes",
                                    duplicates, 2, &status,
                                    error, sizeof(error)) ==
           JW_RELOCATION_CONFLICT);
    assert(query_int(db, "SELECT count(*) FROM library_relocation_ops;") == 0);
    assert(jw_db_relocation_prepare(db, "move-1", 4, "stale", &item, 1,
                                    &status, error, sizeof(error)) ==
           JW_RELOCATION_STALE);
    assert(jw_db_relocation_prepare(db, "move-1", 5, "request-a", &item, 1,
                                    &status, error, sizeof(error)) == 0);
    assert(strcmp(status.state, "prepared") == 0);
    assert(item.game_id == 42);
    assert(jw_db_relocation_prepare(db, "move-1", 5, "request-a", &item, 1,
                                    &status, error, sizeof(error)) == 0);
    assert(jw_db_relocation_prepare(db, "move-1", 5, "request-b", &item, 1,
                                    &status, error, sizeof(error)) ==
           JW_RELOCATION_CONFLICT);
    assert(jw_db_relocation_game_reserved(db, 42));
    assert(jw_db_relocation_key_reserved(db, "primary", "PORTS/Bully.sh"));
    assert(jw_db_relocation_key_reserved(db, "secondary_sd", "PORTS/Bully.sh"));

    assert(jw_db_scan_begin(db) == 0);
    assert(jw_db_scan_source_complete(db, "primary") == 0);
    assert(jw_db_insert_game_stable(db, "PORTS", "wrong", "primary",
                                    "PORTS/Bully.sh", "wrong", "images",
                                    "PORTS/Bully.png", "wrong") == 0);
    assert(jw_db_scan_prune(db) == 0);
    assert(query_int(db, "SELECT count(*) FROM games WHERE id=42 AND name='Bully';") == 1);

    /* A failure after the mapping and visibility updates rolls back both. */
    assert(sqlite3_exec(db,
        "CREATE TRIGGER fail_move BEFORE UPDATE OF state ON library_relocation_ops "
        "BEGIN SELECT RAISE(ABORT,'injected relocation failure'); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(jw_db_relocation_commit(db, "move-1", &status, error, sizeof(error)) ==
           JW_RELOCATION_ERROR);
    assert(query_int(db, "SELECT count(*) FROM games WHERE id=42 AND source_id='primary';") == 1);
    assert(query_int(db, "SELECT count(*) FROM hidden_roms WHERE source_id='primary' "
                         "AND rom_relpath='PORTS/Bully.sh';") == 2);
    assert(query_int(db, "SELECT CAST(value AS INTEGER) FROM settings "
                         "WHERE key='library.generation';") == 5);
    assert(sqlite3_exec(db, "DROP TRIGGER fail_move;", NULL, NULL, NULL) == SQLITE_OK);

    assert(jw_db_relocation_commit(db, "move-1", &status, error, sizeof(error)) == 0);
    assert(strcmp(status.state, "committed") == 0);
    assert(status.mapping_generation == 6);
    assert(query_int(db, "SELECT CAST(value AS INTEGER) FROM settings "
                         "WHERE key='library.generation';") == 6);
    assert(query_int(db, "SELECT count(*) FROM games WHERE id=42 AND "
                         "source_id='secondary_sd';") == 1);
    assert(query_int(db, "SELECT count(*) FROM hidden_roms WHERE source_id='secondary_sd' "
                         "AND rom_relpath='PORTS/Bully.sh';") == 2);
    assert(query_int(db, "SELECT count(*) FROM hidden_roms WHERE source_id='primary' "
                         "AND rom_relpath='PORTS/Bully.sh';") == 0);
    assert(query_int(db, "SELECT count(*) FROM hidden_roms WHERE source_id='primary' "
                         "AND rom_relpath='PORTS/Other.sh';") == 1);
    assert(jw_db_relocation_commit(db, "move-1", &status, error, sizeof(error)) == 0);
    assert(status.mapping_generation == 6);

    jw_db_close(db);
    assert(jw_db_open(path, &db) == 0);
    assert(jw_db_apply_schema(db) == 0);
    assert(jw_db_relocation_game_reserved(db, 42));
    assert(sqlite3_exec(db,
        "CREATE TRIGGER fail_move BEFORE UPDATE OF state ON library_relocation_ops "
        "BEGIN SELECT RAISE(ABORT,'injected revert failure'); END;",
        NULL, NULL, NULL) == SQLITE_OK);
    assert(jw_db_relocation_revert(db, "move-1", &status, error, sizeof(error)) ==
           JW_RELOCATION_ERROR);
    assert(query_int(db, "SELECT count(*) FROM games WHERE id=42 AND source_id='secondary_sd';") == 1);
    assert(query_int(db, "SELECT count(*) FROM hidden_roms WHERE source_id='secondary_sd' "
                         "AND rom_relpath='PORTS/Bully.sh';") == 2);
    assert(query_int(db, "SELECT CAST(value AS INTEGER) FROM settings "
                         "WHERE key='library.generation';") == 6);
    assert(sqlite3_exec(db, "DROP TRIGGER fail_move;", NULL, NULL, NULL) == SQLITE_OK);
    assert(jw_db_relocation_revert(db, "move-1", &status, error, sizeof(error)) == 0);
    assert(strcmp(status.state, "reverted") == 0);
    assert(status.mapping_generation == 7);
    assert(query_int(db, "SELECT count(*) FROM hidden_roms WHERE source_id='primary' "
                         "AND rom_relpath='PORTS/Bully.sh';") == 2);
    assert(query_int(db, "SELECT count(*) FROM hidden_roms WHERE source_id='secondary_sd';") == 0);
    assert(jw_db_relocation_revert(db, "move-1", &status, error, sizeof(error)) == 0);
    assert(status.mapping_generation == 7);

    assert(jw_db_relocation_finish(db, "move-1", &status) == 0);
    assert(strcmp(status.state, "finishing") == 0);
    assert(status.scan_ticket_generation == 8);
    assert(!jw_db_relocation_game_reserved(db, 42));
    assert(jw_db_relocation_note_scan(db, 7) == 0);
    assert(jw_db_relocation_status(db, "move-1", &status) == 0);
    assert(strcmp(status.state, "finishing") == 0);
    assert(jw_db_relocation_note_scan(db, 8) == 0);
    assert(jw_db_relocation_status(db, "move-1", &status) == 0);
    assert(strcmp(status.state, "finished") == 0);

    jw_db_close(db);
    unlink(path);
    puts("relocation tests passed");
    return 0;
}
