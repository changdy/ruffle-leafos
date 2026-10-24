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
    int value = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return value;
}

static void expect_hidden(const char *path, int id, int expected) {
    int hidden = -1;
    assert(jw_db_is_game_hidden(path, id, &hidden) == 0);
    assert(hidden == expected);
}

static void expect_rom_hidden(const char *path, const char *source,
                                const char *rom, const char *member, int expected) {
    int hidden = -1;
    assert(jw_db_is_rom_hidden(path, source, rom, member, &hidden) == 0);
    assert(hidden == expected);
}

static void test_disc_visibility(sqlite3 *db, const char *path, const char *readonly) {
    exec_ok(db,
        "INSERT INTO games(id,system,name,source_id,rom_relpath,rom_path) VALUES"
        "(20,'PS','Parent','primary','PS/Parent.M3U','Roms/PS/Parent.M3U'),"
        "(21,'PS','Standalone disc','primary','PS/sub/Disc.cue','Roms/PS/sub/Disc.cue'),"
        "(22,'PS','Archive','primary','PS/Archive.zip','Roms/PS/Archive.zip'),"
        "(23,'PS','Other card','secondary_sd','PS/Parent.m3u8','/card/Roms/PS/Parent.m3u8');");
    assert(jw_db_set_game_setting(path, 20, "display_name", "A parent title") == 0);
    assert(jw_db_set_game_setting(path, 23, "imported_display_name", "Z other card") == 0);
    int before = 0, count = 0;
    assert(jw_db_count_hidden_roms(path, &before) == 0);
    expect_rom_hidden(path, "primary", "PS/sub/Disc.cue", "", 0);
    assert(jw_db_set_rom_hidden(path, "primary", "PS/sub/Disc.cue", "", 1) == 0);
    assert(jw_db_set_rom_hidden(path, "primary", "PS/sub/Disc.cue", "", 1) == 0);
    expect_hidden(path, 21, 1); /* Existing standalone disc row uses the same key. */
    expect_rom_hidden(path, "secondary_sd", "PS/sub/Disc.cue", "", 0);
    expect_rom_hidden(path, "primary", "PS/sub/disc.cue", "", 0);
    expect_rom_hidden(path, "primary", "PS/sub/Disc.cue", "member", 0);
    expect_hidden(path, 20, 0);
    assert(jw_db_set_game_hidden(path, 20, 1) == 0);
    assert(jw_db_set_game_hidden(path, 20, 0) == 0);
    expect_hidden(path, 21, 1); /* Parent choices never clear a disc preference. */

    /* Two logical discs in one archive do not hide each other or the archive. */
    assert(jw_db_set_rom_hidden(path, "primary", "PS/Archive.zip", "Disc 1.cue", 1) == 0);
    expect_rom_hidden(path, "primary", "PS/Archive.zip", "Disc 1.cue", 1);
    expect_rom_hidden(path, "primary", "PS/Archive.zip", "disc 1.cue", 0);
    expect_rom_hidden(path, "primary", "PS/Archive.zip", "Disc 2.cue", 0);
    expect_hidden(path, 22, 0);
    assert(jw_db_set_game_hidden(path, 22, 1) == 0);
    assert(jw_db_set_game_hidden(path, 22, 0) == 0);
    expect_rom_hidden(path, "primary", "PS/Archive.zip", "Disc 1.cue", 1);
    jw_game_entry games[16];
    assert(jw_db_list_games_for_system(path, "PS", games, 16, &count) == 0 && count == 3);
    for (int i = 0; i < count; ++i) assert(games[i].id != 21);

    /* Unindexed and missing discs remain clearable without artificial games. */
    assert(jw_db_set_rom_hidden(path, "primary", "PS/missing.cue", "", 1) == 0);
    assert(jw_db_count_hidden_roms(path, &count) == 0 && count == before + 3);
    assert(scalar(db, "SELECT COUNT(*) FROM games WHERE rom_relpath='PS/missing.cue'") == 0);
    jw_hidden_rom_entry entries[16];
    assert(jw_db_list_hidden_roms(path, entries, 16, &count) == 0 && count == before + 3);
    int found_member = 0, found_missing = 0, found_disc = 0;
    for (int i = 0; i < count; ++i) {
        jw_hidden_rom_entry *entry = &entries[i];
        if (strcmp(entry->game.rom_relpath, "PS/Archive.zip") == 0) {
            assert(entry->game.id == 22 && strcmp(entry->member, "Disc 1.cue") == 0);
            found_member++;
        } else if (strcmp(entry->game.rom_relpath, "PS/missing.cue") == 0) {
            assert(entry->game.id == 0 && !entry->member[0]);
            assert(strcmp(entry->game.name, "PS/missing.cue") == 0);
            assert(strcmp(entry->game.source_id, "primary") == 0);
            found_missing++;
        } else if (strcmp(entry->game.rom_relpath, "PS/sub/Disc.cue") == 0) {
            assert(entry->game.id == 21 && !entry->member[0]);
            found_disc++;
        }
    }
    assert(found_member == 1 && found_missing == 1 && found_disc == 1);
    assert(jw_db_list_hidden_roms(path, entries, 1, &count) == 0 && count == 1);
    assert(jw_db_set_rom_hidden(path, "primary", "PS/missing.cue", "", 0) == 0);
    assert(jw_db_set_rom_hidden(path, "primary", "PS/missing.cue", "", 0) == 0);
    expect_rom_hidden(path, "primary", "PS/missing.cue", "", 0);

    assert(jw_db_set_rom_hidden(readonly, "primary", "PS/new.cue", "", 1) == JW_DB_RC_READONLY);
    assert(jw_db_set_rom_hidden(readonly, "primary", "PS/sub/Disc.cue", "", 0) == JW_DB_RC_READONLY);
    assert(jw_db_set_rom_hidden(readonly, "primary", "PS/Archive.zip", "Disc 1.cue", 0) == JW_DB_RC_READONLY);
    expect_rom_hidden(readonly, "primary", "PS/Archive.zip", "Disc 1.cue", 1);
    expect_hidden(path, 21, 1);

    assert(jw_db_set_game_hidden(path, 20, 1) == 0);
    assert(jw_db_scan_begin(db) == 0);
    assert(jw_db_insert_game_stable(db, "PS", "Disc rescanned", "primary",
        "PS/sub/Disc.cue", "Roms/PS/sub/Disc.cue", NULL, NULL, NULL) == 0);
    assert(jw_db_scan_prune(db) == 0);
    expect_hidden(path, 21, 1);
    expect_rom_hidden(path, "primary", "PS/Archive.zip", "Disc 1.cue", 1);

    char too_long[513];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';
    assert(jw_db_set_rom_hidden(path, "primary", "PS/Disc.cue", NULL, 1) == -1);
    assert(jw_db_set_rom_hidden(path, "primary", too_long, "", 1) == -1);
    assert(jw_db_set_rom_hidden(path, "primary", "PS/Disc.cue", too_long, 1) == -1);
    assert(jw_db_set_rom_hidden(path, too_long, "PS/Disc.cue", "", 1) == -1);
}

int main(void) {
    /* URI read-only mode exercises a real SQLite readonly connection even
       when a test runner can bypass filesystem permission bits. */
    assert(sqlite3_config(SQLITE_CONFIG_URI, 1) == SQLITE_OK);
    char path[] = "/tmp/jawaka-visibility.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    sqlite3 *db = NULL;
    assert(jw_db_open(path, &db) == 0);
    assert(jw_db_apply_schema(db) == 0);
    exec_ok(db,
        "INSERT INTO games(id,system,name,source_id,rom_relpath,rom_path) VALUES"
        "(1,'NES','Alpha','primary','NES/alpha.nes','Roms/NES/alpha.nes'),"
        "(2,'NES','Zulu','primary','NES/zulu.nes','Roms/NES/zulu.nes'),"
        "(3,'PS','Only game','primary','PS/only.cue','Roms/PS/only.cue'),"
        "(4,'NES','Alpha copy','secondary_sd','NES/alpha.nes','/card/Roms/NES/alpha.nes'),"
        "(5,'PS','超级马里奥','primary','PS/chinese.cue','Roms/PS/chinese.cue');"
        "INSERT INTO hidden_roms VALUES('primary','NES/alpha.nes','member');"
        "INSERT INTO settings VALUES('five_game_ids','1,2'),('language','zh_CN');");
    assert(jw_db_set_favorite(path, "game", 1, 1) == 0);
    assert(jw_db_set_favorite(path, "game", 2, 1) == 0);
    assert(jw_db_record_play_by_id(path, 1, 1000) == 0);
    assert(jw_db_record_play_by_id(path, 2, 10) == 0);
    exec_ok(db, "UPDATE recents SET last_opened=100 WHERE target_id=2;"
                "UPDATE recents SET last_opened=200 WHERE target_id=1;");
    assert(jw_db_set_game_setting(path, 1, "display_name", "Alpha HiddenSuffix") == 0);
    assert(jw_db_set_game_setting(path, 2, "display_name", "Zulu VisibleSuffix") == 0);
    expect_hidden(path, 1, 0); /* An archive member never hides the whole game. */
    assert(jw_db_set_game_hidden(path, 1, 1) == 0);
    assert(jw_db_set_game_hidden(path, 1, 1) == 0);
    assert(jw_db_set_game_hidden(path, 3, 1) == 0);
    assert(jw_db_set_game_hidden(path, 5, 1) == 0);
    assert(jw_db_set_game_hidden(path, 999, 1) == JW_DB_RC_NO_ROW);
    expect_hidden(path, 1, 1);
    expect_hidden(path, 4, 0); /* Same path on the other card stays independent. */

    jw_system_entry systems[4];
    jw_game_entry games[8], game;
    jw_search_result search[8];
    int count = 0;
    assert(jw_db_list_systems(path, systems, 4, &count) == 0 && count == 1);
    assert(strcmp(systems[0].name, "NES") == 0 && systems[0].game_count == 2);
    assert(jw_db_count_games_for_system(path, "NES", &count) == 0 && count == 2);
    assert(jw_db_count_games_for_system(path, "PS", &count) == 0 && count == 0);
    assert(jw_db_list_games_for_system(path, "NES", games, 1, &count) == 0);
    assert(count == 1 && games[0].id == 4);
    assert(jw_db_list_favorite_games(path, games, 1, &count) == 0);
    assert(count == 1 && games[0].id == 2);
    assert(jw_db_list_recent_games(path, games, 1, &count) == 0);
    assert(count == 1 && games[0].id == 2);
    assert(jw_db_search_library(path, "alpha", search, 1, &count) == 0);
    assert(count == 1 && search[0].id == 4); /* FTS filtering precedes LIMIT. */
    assert(jw_db_search_library(path, "suffix", search, 1, &count) == 0);
    assert(count == 1 && search[0].id == 2); /* Override LIKE fallback. */
    assert(jw_db_search_library(path, "CJMLA", search, 8, &count) == 0 && count == 0);
    assert(jw_db_search_library(path, "超级", search, 8, &count) == 0 && count == 0);

    jw_library_stats stats;
    jw_library_summary summary;
    assert(jw_db_read_stats(path, &stats) == 0);
    assert(stats.game_count == 5 && stats.total_playtime_s == 1010);
    assert(stats.favorite_count == 2 && stats.top_count == 1);
    assert(strcmp(stats.top[0].name, "Zulu VisibleSuffix") == 0);
    assert(jw_db_read_summary(path, &summary) == 0);
    assert(summary.game_count == 2 && summary.system_count == 1);
    assert(strstr(summary.sample_summary, "HiddenSuffix") == NULL);

    /* Focus, boot resume and a running game can still resolve hidden IDs/paths. */
    assert(jw_db_get_game_by_id(path, 1, &game) == 0 && game.favorite);
    assert(game.playtime_s == 1000);
    assert(jw_db_get_game_by_rom_path(path, "Roms/NES/alpha.nes", &game) == 0);
    assert(jw_db_get_game_by_source_relpath(path, "primary", "NES/alpha.nes", &game) == 0);
    assert(scalar(db, "SELECT COUNT(*) FROM settings WHERE key='five_game_ids' AND value='1,2'") == 1);
    assert(jw_db_count_hidden_games(path, &count) == 0 && count == 3);
    assert(jw_db_list_hidden_games(path, games, 8, &count) == 0 && count == 3);
    assert(games[0].id == 1 && games[0].favorite && games[0].playtime_s == 1000);

    char readonly[256];
    snprintf(readonly, sizeof(readonly), "file:%s?mode=ro", path);
    assert(jw_db_set_game_hidden(readonly, 2, 1) == JW_DB_RC_READONLY);
    assert(jw_db_set_game_hidden(readonly, 1, 0) == JW_DB_RC_READONLY);
    assert(jw_db_clear_hidden_game(readonly, "primary", "NES/alpha.nes") == JW_DB_RC_READONLY);
    expect_hidden(path, 1, 1);
    expect_hidden(path, 2, 0);

    /* Resetting overrides and scanning the same identity keep Hide and history. */
    assert(jw_db_delete_game_setting(path, 1, "display_name") == 0);
    assert(jw_db_scan_begin(db) == 0);
    assert(jw_db_insert_game_stable(db, "NES", "Alpha rescanned", "primary",
        "NES/alpha.nes", "Roms/NES/alpha.nes", NULL, NULL, NULL) == 0);
    assert(jw_db_scan_prune(db) == 0); /* No completed sources: retain every row. */
    expect_hidden(path, 1, 1);
    assert(jw_db_get_game_by_id(path, 1, &game) == 0 && game.playtime_s == 1000);
    assert(jw_db_clear_hidden_game(path, "primary", "NES/alpha.nes") == 0);
    expect_hidden(path, 1, 0);
    assert(scalar(db, "SELECT COUNT(*) FROM hidden_roms WHERE member='member'") == 1);
    assert(jw_db_list_favorite_games(path, games, 8, &count) == 0 && count == 2);
    assert(jw_db_list_recent_games(path, games, 1, &count) == 0 && games[0].id == 1);

    assert(jw_db_set_game_hidden(path, 1, 1) == 0);
    assert(jw_db_set_game_hidden(path, 2, 1) == 0);
    assert(jw_db_set_game_hidden(path, 4, 1) == 0);
    assert(jw_db_list_systems(path, systems, 4, &count) == 0 && count == 0);
    assert(jw_db_read_summary(path, &summary) == 0 && summary.game_count == 0);
    assert(jw_db_read_stats(path, &stats) == 0 && stats.game_count == 5 && stats.top_count == 0);
    assert(jw_db_count_hidden_games(path, &count) == 0 && count == 5);

    /* Pruning missing files preserves clearable preferences, and a returning
       source/path inherits Hide without an artificial game row. */
    assert(jw_db_scan_begin(db) == 0);
    assert(jw_db_scan_source_complete(db, "primary") == 0);
    assert(jw_db_scan_prune(db) == 0);
    assert(jw_db_list_hidden_games(path, games, 8, &count) == 0 && count == 5);
    int orphans = 0;
    for (int i = 0; i < count; ++i) {
        if (games[i].id == 0) {
            assert(strcmp(games[i].name, games[i].rom_relpath) == 0);
            assert(strcmp(games[i].source_id, "primary") == 0);
            orphans++;
        }
    }
    assert(orphans == 4);
    assert(jw_db_clear_hidden_game(path, "primary", "PS/only.cue") == 0);
    assert(jw_db_count_hidden_games(path, &count) == 0 && count == 4);
    assert(jw_db_scan_begin(db) == 0);
    assert(jw_db_insert_game_stable(db, "NES", "Returned", "primary",
        "NES/alpha.nes", "Roms/NES/alpha.nes", NULL, NULL, NULL) == 0);
    assert(jw_db_get_game_by_source_relpath(path, "primary", "NES/alpha.nes", &game) == 0);
    expect_hidden(path, game.id, 1);
    assert(jw_db_set_game_hidden(path, game.id, 0) == 0);
    assert(jw_db_set_game_hidden(path, game.id, 0) == 0);
    expect_hidden(path, game.id, 0);
    assert(scalar(db, "SELECT COUNT(*) FROM hidden_roms WHERE member='member'") == 1);

    test_disc_visibility(db, path, readonly);
    jw_db_close(db);
    unlink(path);
    puts("visibility tests passed");
    return 0;
}
