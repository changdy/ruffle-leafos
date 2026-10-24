#include "internal/db/db.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void exec_ok(sqlite3 *db, const char *sql) {
    char *error = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &error);
    if (rc != SQLITE_OK) fprintf(stderr, "%s\n", error ? error : sqlite3_errmsg(db));
    sqlite3_free(error);
    assert(rc == SQLITE_OK);
}

static int scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    int result = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return result;
}

static void expect_focus(sqlite3 *db, const char *expected) {
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE key='five_game_ids';",
                             -1, &stmt, NULL) == SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(strcmp((const char *)sqlite3_column_text(stmt, 0), expected) == 0);
    sqlite3_finalize(stmt);
}

static void seed(sqlite3 *db) {
    exec_ok(db,
        "INSERT INTO games(id,system,name,source_id,rom_relpath,rom_path,"
        "image_root_kind,image_relpath,image_path,last_played,playtime_s) VALUES"
        "(1,'PS','Parent','primary','PS/Parent.m3u','Roms/PS/Parent.m3u',"
        "'roms','PS/Parent.png','Roms/PS/Parent.png',123,456),"
        "(2,'PS','Disc','primary','PS/Disc.cue','Roms/PS/Disc.cue',NULL,NULL,NULL,234,567),"
        "(3,'PS','Shared','primary','PS/Shared.cue','Roms/PS/Shared.cue',NULL,NULL,NULL,345,678),"
        "(4,'PS','Other card','secondary_sd','PS/Disc.cue','/card/Roms/PS/Disc.cue',NULL,NULL,NULL,456,789),"
        "(5,'PS','Different case','primary','PS/disc.cue','Roms/PS/disc.cue',NULL,NULL,NULL,567,890);"
        "INSERT INTO apps(id,pak_dir,name) VALUES(1,'Apps/shared/Test.pak','Test');"
        "INSERT INTO favorites VALUES('game',1,11),('game',2,22),('game',3,33),('app',1,44);"
        "INSERT INTO recents VALUES('game',1,11,22),('game',2,22,33),('game',3,33,44),('app',1,44,55);"
        "INSERT INTO game_settings(game_id,key,value,updated_at) VALUES(1,'display_name','Parent title',1),"
        "(2,'core','test_core',2),(3,'core','shared_core',3);"
        "INSERT INTO settings VALUES('five_game_ids','2,1,3,4,5'),"
        "('five_game_active','1'),('five_game_lock','pin'),('five_game_pin_hash','keep'),"
        "('language','en_US');"
        "INSERT INTO hidden_roms VALUES('primary','PS/Parent.m3u',''),"
        "('primary','PS/Disc.cue',''),('primary','PS/Disc.cue','Disc 1'),"
        "('primary','PS/Disc.cue','Disc 2'),('primary','PS/Shared.cue',''),"
        "('secondary_sd','PS/Disc.cue',''),('primary','PS/disc.cue',''),"
        "('primary','PS/Absent.bin',''),('primary','PS/Unrelated.bin','');");
}

int main(void) {
    char path[] = "/tmp/jawaka-deletion-db.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    sqlite3 *db = NULL;
    assert(jw_db_open(path, &db) == 0);
    assert(jw_db_apply_schema(db) == 0);
    seed(db);

    /* Preview inventory includes hidden entries and exact artwork, with no cap. */
    jw_game_entry *games = NULL;
    size_t count = 0;
    assert(jw_db_list_indexed_games(path, &games, &count) == 0 && count == 5);
    assert(games[0].id == 1 && strcmp(games[0].name, "Parent title") == 0);
    assert(strcmp(games[0].image_root_kind, "roms") == 0);
    assert(strcmp(games[0].image_relpath, "PS/Parent.png") == 0);
    assert(strcmp(games[0].image_path, "Roms/PS/Parent.png") == 0);
    assert(games[0].favorite && games[0].playtime_s == 456);
    assert(strcmp(games[3].source_id, "secondary_sd") == 0);
    free(games);

    const jw_db_rom_key partial[] = {
        {"primary", "PS/Disc.cue"}, {"primary", "PS/Absent.bin"},
        {"primary", "PS/Disc.cue"} /* Physical deduplication is not required here. */
    };
    sqlite3 *readonly = NULL;
    assert(sqlite3_open_v2(path, &readonly, SQLITE_OPEN_READONLY, NULL) == SQLITE_OK);
    assert(jw_db_reconcile_removed_roms(readonly, partial, 3) == JW_DB_RC_READONLY);
    sqlite3_close(readonly);
    assert(scalar(db, "SELECT COUNT(*) FROM games") == 5);
    assert(scalar(db, "SELECT COUNT(*) FROM hidden_roms") == 9);

    /* Metadata failure rolls back every removed row, FTS row and preference. */
    exec_ok(db, "CREATE TRIGGER fail_focus BEFORE UPDATE OF value ON settings "
                "WHEN old.key='five_game_ids' BEGIN SELECT RAISE(ABORT,'test failure'); END;");
    assert(jw_db_reconcile_removed_roms(db, partial, 3) == -1);
    assert(scalar(db, "SELECT COUNT(*) FROM games") == 5);
    assert(scalar(db, "SELECT COUNT(*) FROM hidden_roms") == 9);
    assert(scalar(db, "SELECT COUNT(*) FROM games_fts WHERE games_fts MATCH 'Disc'") == 1);
    expect_focus(db, "2,1,3,4,5");
    exec_ok(db, "DROP TRIGGER fail_focus;");

    /* Validation is complete before writes, even with an earlier valid key. */
    const jw_db_rom_key invalid[] = {{"primary", "PS/Disc.cue"}, {"primary", "../other"}};
    assert(jw_db_reconcile_removed_roms(db, invalid, 2) == -1);
    assert(scalar(db, "SELECT COUNT(*) FROM games") == 5);
    assert(jw_db_reconcile_removed_roms(db, NULL, 0) == 0);

    /* A partial filesystem failure leaves the parent's row and metadata intact. */
    assert(jw_db_reconcile_removed_roms(db, partial, 3) == 0);
    assert(scalar(db, "SELECT COUNT(*) FROM games") == 4);
    assert(scalar(db, "SELECT COUNT(*) FROM games WHERE id IN (1,3,4,5)") == 4);
    assert(scalar(db, "SELECT COUNT(*) FROM games_fts WHERE games_fts MATCH 'Disc'") == 0);
    assert(scalar(db, "SELECT COUNT(*) FROM hidden_roms") == 5);
    assert(scalar(db, "SELECT COUNT(*) FROM hidden_roms WHERE rom_relpath='PS/Disc.cue'") == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM hidden_roms WHERE rom_relpath='PS/Unrelated.bin'") == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM game_settings WHERE game_id=1 AND value='Parent title'") == 1);
    assert(scalar(db, "SELECT playtime_s FROM games WHERE id=1") == 456);
    assert(scalar(db, "SELECT COUNT(*) FROM favorites WHERE kind='game' AND target_id=2") == 0);
    assert(scalar(db, "SELECT COUNT(*) FROM recents WHERE kind='game' AND target_id=2") == 0);
    assert(scalar(db, "SELECT COUNT(*) FROM game_settings WHERE game_id=2") == 0);
    assert(scalar(db, "SELECT COUNT(*) FROM favorites WHERE kind='app' AND target_id=1") == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM settings WHERE key='five_game_lock' AND value='pin'") == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM settings WHERE key='five_game_pin_hash' AND value='keep'") == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM settings WHERE key='five_game_active' AND value='1'") == 1);
    expect_focus(db, "1,3,4,5");

    /* A caller's outer transaction still owns the final commit. */
    const jw_db_rom_key root = {"primary", "PS/Parent.m3u"};
    exec_ok(db, "BEGIN;");
    assert(jw_db_reconcile_removed_roms(db, &root, 1) == 0);
    expect_focus(db, "3,4,5");
    exec_ok(db, "ROLLBACK;");
    expect_focus(db, "1,3,4,5");
    assert(jw_db_reconcile_removed_roms(db, &root, 1) == 0);
    assert(scalar(db, "SELECT COUNT(*) FROM games WHERE id=1") == 0);
    assert(scalar(db, "SELECT COUNT(*) FROM games WHERE id=3") == 1);
    expect_focus(db, "3,4,5");

    /* A later scan also cleans Focus, retaining the missing card and preferences. */
    assert(jw_db_scan_begin(db) == 0);
    assert(jw_db_scan_source_complete(db, "primary") == 0);
    assert(jw_db_insert_game_stable(db, "PS", "Shared", "primary", "PS/Shared.cue",
        "Roms/PS/Shared.cue", NULL, NULL, NULL) == 0);
    exec_ok(db, "CREATE TRIGGER fail_focus BEFORE UPDATE OF value ON settings "
                "WHEN old.key='five_game_ids' BEGIN SELECT RAISE(ABORT,'test failure'); END;");
    assert(jw_db_scan_prune(db) == -1);
    assert(scalar(db, "SELECT COUNT(*) FROM games WHERE id=5") == 1);
    expect_focus(db, "3,4,5");
    exec_ok(db, "DROP TRIGGER fail_focus;");
    assert(jw_db_scan_prune(db) == 0);
    expect_focus(db, "3,4");
    assert(scalar(db, "SELECT COUNT(*) FROM games") == 2);
    assert(scalar(db, "SELECT COUNT(*) FROM hidden_roms WHERE rom_relpath='PS/disc.cue'") == 1);
    assert(scalar(db, "SELECT COUNT(*) FROM game_settings WHERE game_id=3 AND value='shared_core'") == 1);
    /* Reusing a pruned numeric ID cannot silently restore a Focus choice. */
    exec_ok(db, "INSERT INTO games(id,system,name,source_id,rom_relpath,rom_path) VALUES"
                "(5,'PS','Replacement','primary','PS/New.cue','Roms/PS/New.cue');");
    expect_focus(db, "3,4");

    jw_db_close(db);
    unlink(path);
    puts("deletion DB tests passed");
    return 0;
}
