#include "internal/db/db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(sqlite3 *db, const char *message) {
    fprintf(stderr, "schema-v6-test: %s%s%s\n", message,
            db ? ": " : "", db ? sqlite3_errmsg(db) : "");
    exit(1);
}

static void exec_ok(sqlite3 *db, const char *sql) {
    char *error = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &error) != SQLITE_OK) {
        fprintf(stderr, "schema-v6-test SQL: %s\n", error ? error : "unknown");
        sqlite3_free(error);
        exit(1);
    }
}

static long long scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK ||
        sqlite3_step(stmt) != SQLITE_ROW) {
        fail(db, sql);
    }
    long long value = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

/* Released Jawaka refuses any user_version above 6, so additive tables must
   never raise it. */
static int reject_version_change(void *context, int action, const char *name,
                                 const char *value, const char *database, const char *trigger) {
    (void)context; (void)database; (void)trigger;
    return action == SQLITE_PRAGMA && name && value && strcmp(name, "user_version") == 0
        ? SQLITE_DENY : SQLITE_OK;
}

static void check_unreleased_v7_and_read_only(void) {
    /* Development builds stamped hidden_roms as v7; it returns to v6 intact. */
    char dir[] = "/tmp/jawaka-schema-v7.XXXXXX", path[256];
    if (!mkdtemp(dir)) fail(NULL, "v7 mkdtemp");
    snprintf(path, sizeof(path), "%s/library.db", dir);
    sqlite3 *db = NULL;
    if (jw_db_open(path, &db) != 0 || jw_db_apply_schema(db) != 0) fail(db, "v7 fixture");
    exec_ok(db, "INSERT INTO hidden_roms VALUES('primary','GB/Hidden.gb','');"
                "PRAGMA user_version=7;");
    if (jw_db_apply_schema(db) != 0 || scalar(db, "PRAGMA user_version") != 6 ||
        scalar(db, "SELECT COUNT(*) FROM hidden_roms") != 1) {
        fail(db, "unreleased v7 library did not return to v6");
    }
    if (jw_db_scan_begin(db) != 0 ||
        jw_db_insert_game(db, "GB", "Visible", "Roms/GB/Visible.gb", NULL) != 0 ||
        jw_db_insert_game(db, "GB", "Hidden", "Roms/GB/Hidden.gb", NULL) != 0 ||
        jw_db_scan_prune(db) != 0) {
        fail(db, "read-only fixture games");
    }
    /* What an older release leaves behind: the same v6 library without hidden_roms. */
    exec_ok(db, "DROP TABLE hidden_roms;");
    sqlite3_close(db);
    if (chmod(path, 0444) != 0 || chmod(dir, 0555) != 0) fail(NULL, "chmod read-only");
    jw_system_entry systems[4];
    jw_game_entry games[4];
    int count = 0, hidden = 0;
    if (access(path, W_OK) == 0) {
        puts("schema-v6-test: read-only checks skipped for a privileged user");
    } else if (jw_db_list_systems(path, systems, 4, &count) != 0 || count != 1 ||
               jw_db_list_games_for_system(path, "GB", games, 4, &count) != 0 || count != 2 ||
               jw_db_is_rom_hidden(path, "primary", "GB/Hidden.gb", "", &hidden) != 0 || hidden ||
               jw_db_set_rom_hidden(path, "primary", "GB/Hidden.gb", "", 1) != JW_DB_RC_READONLY) {
        fail(NULL, "a read-only library from an older release must still browse");
    }
    if (chmod(dir, 0755) != 0 || chmod(path, 0644) != 0) fail(NULL, "chmod writable");
    /* The first writable open adds the table; the version stays readable by older releases. */
    if (jw_db_list_games_for_system(path, "GB", games, 4, &count) != 0 || count != 2 ||
        jw_db_open(path, &db) != 0 ||
        scalar(db, "SELECT COUNT(*) FROM main.sqlite_master WHERE name='hidden_roms'") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM temp.sqlite_master WHERE name='hidden_roms'") != 0 ||
        scalar(db, "PRAGMA user_version") != 6) {
        fail(db, "writable open did not add the visibility table");
    }
    sqlite3_close(db);
    unlink(path);
    rmdir(dir);
}

int main(void) {
    char fresh[] = "/tmp/jawaka-schema-fresh.XXXXXX";
    int fresh_fd = mkstemp(fresh);
    if (fresh_fd < 0) fail(NULL, "fresh mkstemp");
    close(fresh_fd);
    unlink(fresh);
    sqlite3 *db = NULL;
    if (jw_db_open(fresh, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        scalar(db, "PRAGMA user_version") != 6 ||
        scalar(db, "SELECT COUNT(*) FROM pragma_table_info('games') "
                   "WHERE name IN ('source_id','rom_relpath','image_root_kind',"
                   "'image_relpath')") != 4 ||
        scalar(db, "SELECT COUNT(*) FROM pragma_table_info('apps') "
                   "WHERE name='min_leaf_version'") != 1) {
        fail(db, "fresh v6 schema");
    }
    if (scalar(db, "SELECT COUNT(*) FROM pragma_table_info('hidden_roms') "
                   "WHERE name IN ('source_id','rom_relpath','member') "
                   "AND \"notnull\"=1 AND pk>0") != 3) {
        fail(db, "fresh visibility schema");
    }
    if (scalar(db, "SELECT COUNT(*) FROM pragma_table_info('pakrat_installs') "
                   "WHERE name='commit_token' AND \"notnull\"=0") != 1) {
        fail(db, "fresh pakrat commit token column");
    }
    if (scalar(db, "SELECT COUNT(*) FROM pragma_table_info('pakrat_installs') "
                   "WHERE name='source_id' AND \"notnull\"=1") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                   "AND name IN ('pakrat_service_metadata',"
                   "'pakrat_pending_uninstalls')") != 2) {
        fail(db, "fresh pakrat transaction schema");
    }
    /* The install root travels in install_path; kind is derived from it. */
    if (scalar(db, "SELECT COUNT(*) FROM pragma_table_info('pakrat_installs') "
                   "WHERE name='kind' AND \"notnull\"=1") != 1 ||
        jw_db_pakrat_upsert_install_db(
            db, "neon-nights", "1.0.0", "any", "primary", "Themes/neon-nights",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            NULL, NULL) != 0 ||
        jw_db_pakrat_upsert_install_db(
            db, "org.umrk.app", "1.0.0", "mlp1", "primary", "mlp1/App.pak",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            NULL, NULL) != 0 ||
        scalar(db, "SELECT COUNT(*) FROM pakrat_installs WHERE "
                   "(store_id='neon-nights' AND kind='theme') OR "
                   "(store_id='org.umrk.app' AND kind='app')") != 2) {
        fail(db, "fresh pakrat kind column");
    }
    sqlite3_close(db);
    unlink(fresh);

    char current[] = "/tmp/jawaka-schema-current-v6.XXXXXX";
    int current_fd = mkstemp(current);
    if (current_fd < 0) fail(NULL, "current v6 mkstemp");
    close(current_fd);
    if (sqlite3_open(current, &db) != SQLITE_OK) fail(db, "current v6 open");
    exec_ok(db,
        "PRAGMA user_version=6;"
        "CREATE TABLE apps("
        "id INTEGER PRIMARY KEY,pak_dir TEXT NOT NULL UNIQUE,"
        "name TEXT NOT NULL,icon TEXT,platform TEXT,pak_version TEXT,"
        "min_jawaka_version TEXT);"
        "CREATE TABLE pakrat_installs("
        "store_id TEXT PRIMARY KEY,version TEXT NOT NULL,"
        "platform TEXT NOT NULL,install_path TEXT NOT NULL,"
        "artifact_sha256 TEXT NOT NULL,installed_at TEXT NOT NULL);"
        "INSERT INTO pakrat_installs VALUES("
        "'org.umrk.legacy','1.0.0','mlp1','mlp1/Legacy.pak',"
        "'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa',"
        "'2026-07-29T00:00:00Z');"
        "INSERT INTO apps(pak_dir,name,pak_version,min_jawaka_version)"
        "VALUES('Apps/mlp1/Existing.pak','Existing','1.0.0','0.0.1');");
    sqlite3_set_authorizer(db, reject_version_change, NULL);
    if (jw_db_apply_schema(db) != 0 ||
        jw_db_apply_schema(db) != 0 ||
        scalar(db, "PRAGMA user_version") != 6 ||
        scalar(db, "SELECT COUNT(*) FROM hidden_roms") != 0 ||
        scalar(db, "SELECT COUNT(*) FROM pragma_table_info('apps') "
                   "WHERE name='min_leaf_version'") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM pragma_table_info('pakrat_installs') "
                   "WHERE name='commit_token' AND \"notnull\"=0") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM pragma_table_info('pakrat_installs') "
                   "WHERE name='source_id' AND \"notnull\"=1") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM sqlite_master WHERE type='table' "
                   "AND name IN ('pakrat_service_metadata',"
                   "'pakrat_pending_uninstalls')") != 2 ||
        scalar(db, "SELECT COUNT(*) FROM apps "
                   "WHERE pak_dir='Apps/mlp1/Existing.pak' "
                   "AND pak_version='1.0.0'") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM pakrat_installs "
                   "WHERE store_id='org.umrk.legacy' AND commit_token IS NULL "
                   "AND source_id='primary' AND kind='app'") != 1 ||
        jw_db_scan_begin(db) != 0 ||
        jw_db_insert_app(db, "Apps/mlp1/Existing.pak", "Existing", "",
                         "mlp1", "1.0.1", "0.0.1", "0.7.0") != 0 ||
        scalar(db, "SELECT COUNT(*) FROM apps "
                   "WHERE pak_version='1.0.1' "
                   "AND min_leaf_version='0.7.0'") != 1) {
        fail(db, "current v6 additive migration");
    }
    sqlite3_set_authorizer(db, NULL, NULL);
    sqlite3_close(db);
    unlink(current);

    char path[] = "/tmp/jawaka-schema-v6.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) fail(NULL, "mkstemp");
    close(fd);
    unlink(path);

    if (sqlite3_open(path, &db) != SQLITE_OK) fail(db, "open");
    exec_ok(db,
        "PRAGMA user_version=5;"
        "CREATE TABLE games(id INTEGER PRIMARY KEY,system TEXT NOT NULL,"
        "name TEXT NOT NULL,rom_path TEXT NOT NULL UNIQUE,image_path TEXT,"
        "last_played INTEGER,playtime_s INTEGER NOT NULL DEFAULT 0);"
        "CREATE TABLE favorites(kind TEXT,target_id INTEGER,added_at INTEGER,"
        "PRIMARY KEY(kind,target_id));"
        "CREATE TABLE recents(kind TEXT,target_id INTEGER,last_opened INTEGER,"
        "duration_s INTEGER,PRIMARY KEY(kind,target_id));"
        "CREATE TABLE game_settings(game_id INTEGER,key TEXT,value TEXT,"
        "updated_at INTEGER,PRIMARY KEY(game_id,key),"
        "FOREIGN KEY(game_id) REFERENCES games(id) ON DELETE CASCADE);"
        "INSERT INTO games VALUES"
        "(10,'NES','old-a','/mnt/sdcard/Roms/NES/game.zip',"
        "'/mnt/sdcard/Images/NES/game.png',1000,5),"
        "(20,'NES','old-b','/media/sdcard1/Roms/NES/game.zip',"
        "'/media/sdcard1/Images/NES/game.png',2000,7),"
        "(30,'PORTS','source-art','Roms/PORTS/source.sh',"
        "'.catalog/PORTS/source.png',NULL,0),"
        "(40,'PORTS','bad-art','Roms/PORTS/bad.sh',"
        "'/unknown/art/bad.png',NULL,0);"
        "INSERT INTO favorites VALUES('game',20,50);"
        "INSERT INTO recents VALUES('game',10,100,1),('game',20,200,2);"
        "INSERT INTO game_settings VALUES"
        "(10,'display_name','Older',10),(20,'display_name','Newer',20);");

    if (jw_db_apply_schema(db) != 0) fail(db, "migration");
    if (scalar(db, "PRAGMA user_version") != 6 ||
        scalar(db, "SELECT COUNT(*) FROM hidden_roms") != 0) fail(db, "wrong version");
    if (scalar(db, "SELECT COUNT(*) FROM games") != 3 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE id=10") != 1 ||
        scalar(db, "SELECT playtime_s FROM games WHERE id=10") != 12 ||
        scalar(db, "SELECT last_played FROM games WHERE id=10") != 2000 ||
        scalar(db, "SELECT COUNT(*) FROM favorites WHERE target_id=10") != 1 ||
        scalar(db, "SELECT last_opened FROM recents WHERE target_id=10") != 200 ||
        scalar(db, "SELECT COUNT(*) FROM game_settings "
                   "WHERE game_id=10 AND value='Newer'") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE source_id='secondary_sd' "
                   "AND rom_relpath='NES/game.zip'") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE id=30 "
                   "AND image_root_kind='source' "
                   "AND image_relpath='.catalog/PORTS/source.png'") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE id=40 "
                   "AND image_root_kind IS NULL AND image_relpath IS NULL") != 1) {
        fail(db, "state was not merged");
    }
    sqlite3_close(db);

    char backup[4096];
    snprintf(backup, sizeof(backup), "%s.v5-backup", path);
    if (sqlite3_open_v2(backup, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK ||
        scalar(db, "PRAGMA user_version") != 5 ||
        scalar(db, "SELECT COUNT(*) FROM games") != 4) {
        fail(db, "backup invalid");
    }
    sqlite3_close(db);

    if (jw_db_open(path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        jw_db_scan_begin(db) != 0 ||
        jw_db_insert_game_stable(db, "NES", "moved", "secondary_sd",
            "NES/game.zip", "/media/sdcard1/Roms/NES/game.zip",
            "images", "NES/game.png",
            "/media/sdcard1/Images/NES/game.png") != 0 ||
        jw_db_insert_game_stable(db, "NES", "primary", "primary",
            "NES/primary.zip", "Roms/NES/primary.zip", NULL, NULL, NULL) != 0 ||
        jw_db_scan_source_complete(db, "primary") != 0 ||
        jw_db_scan_prune(db) != 0) {
        fail(db, "source-scoped prune setup");
    }
    if (scalar(db, "SELECT COUNT(*) FROM games WHERE source_id='secondary_sd'") != 1 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE source_id='primary'") != 1 ||
        scalar(db, "SELECT id FROM games WHERE source_id='secondary_sd'") != 10 ||
        scalar(db, "SELECT playtime_s FROM games WHERE id=10") != 12 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE id=10 AND "
                   "rom_path='/media/sdcard1/Roms/NES/game.zip'") != 1) {
        fail(db, "unscanned source was pruned");
    }
    if (jw_db_record_play_by_id(path, 10, 3) != 0 ||
        scalar(db, "SELECT playtime_s FROM games WHERE id=10") != 15) {
        fail(db, "id-based play accounting");
    }
    if (jw_db_scan_begin(db) != 0 ||
        jw_db_scan_source_complete(db, "primary") != 0 ||
        jw_db_scan_prune(db) != 0 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE source_id='primary'") != 0 ||
        scalar(db, "SELECT COUNT(*) FROM games WHERE source_id='secondary_sd'") != 1) {
        fail(db, "completed source was not pruned");
    }
    sqlite3_close(db);

    char unsafe[] = "/tmp/jawaka-schema-unsafe.XXXXXX";
    fd = mkstemp(unsafe);
    if (fd < 0) fail(NULL, "unsafe mkstemp");
    close(fd);
    if (sqlite3_open(unsafe, &db) != SQLITE_OK) fail(db, "unsafe open");
    exec_ok(db,
        "PRAGMA user_version=5;"
        "CREATE TABLE games(id INTEGER PRIMARY KEY,system TEXT NOT NULL,"
        "name TEXT NOT NULL,rom_path TEXT NOT NULL UNIQUE,image_path TEXT,"
        "last_played INTEGER,playtime_s INTEGER NOT NULL DEFAULT 0);"
        "INSERT INTO games VALUES"
        "(1,'NES','unsafe','/unknown/card/Roms/NES/game.zip',NULL,NULL,0);");
    if (jw_db_apply_schema(db) == 0 ||
        scalar(db, "PRAGMA user_version") != 5 ||
        scalar(db, "SELECT COUNT(*) FROM games") != 1) {
        fail(db, "unsafe migration was not rolled back");
    }
    sqlite3_close(db);

    char future[] = "/tmp/jawaka-schema-future.XXXXXX";
    fd = mkstemp(future);
    if (fd < 0 || sqlite3_open(future, &db) != SQLITE_OK) fail(db, "future open");
    close(fd);
    exec_ok(db, "PRAGMA user_version=8;");
    if (jw_db_apply_schema(db) == 0) fail(db, "future schema accepted");
    sqlite3_close(db);

    check_unreleased_v7_and_read_only();

    unlink(future);
    unlink(unsafe);
    unlink(backup);
    unlink(path);
    puts("PASS schema-v6-test");
    return 0;
}
