#include "internal/db/db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *message) {
    fprintf(stderr, "imported-title-test: %s\n", message);
    exit(1);
}

static void expect_name(const char *db_path, const char *rom_path,
                        const char *want) {
    jw_game_entry game;
    if (jw_db_get_game_by_rom_path(db_path, rom_path, &game) != 0 ||
        strcmp(game.name, want) != 0) {
        fprintf(stderr, "imported-title-test: %s name=%s want=%s\n",
                rom_path, game.name, want);
        exit(1);
    }
}

/* A game with builds for two systems can send both ROMs in one group. When a
   scanned name only repeats the title (ignoring case and surrounding space),
   the suffix adds nothing, so the plain title is stored. Other matches in the
   same group keep their suffix. */
static void check_redundant_suffix(void) {
    char db_path[] = "/tmp/jawaka-imported-title-suffix.XXXXXX";
    int fd = mkstemp(db_path);
    if (fd < 0) fail("mkstemp failed");
    close(fd);
    unlink(db_path);

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        jw_db_scan_begin(db) != 0 ||
        jw_db_insert_game(db, "GB", "Glory Hunters", "Roms/GB/Glory Hunters.gb", NULL) != 0 ||
        jw_db_insert_game(db, "GBA", "glory hunters ", "Roms/GBA/glory hunters .gba", NULL) != 0 ||
        jw_db_insert_game(db, "PS", "Saga", "Roms/PS/Saga.cue", NULL) != 0 ||
        jw_db_insert_game(db, "PS", "Saga (Disc 2)", "Roms/PS/Saga (Disc 2).cue", NULL) != 0) {
        fail("could not prepare suffix fixture database");
    }

    const char *glory_paths[] = {"Roms/GB/Glory Hunters.gb", "Roms/GBA/glory hunters .gba"};
    const char *saga_paths[] = {"Roms/PS/Saga.cue", "Roms/PS/Saga (Disc 2).cue"};
    jw_db_imported_title_group groups[] = {
        {.provider = "org.umrk.itchio", .title = "Glory Hunters",
         .rom_paths = glory_paths, .rom_path_count = 2},
        {.provider = "org.umrk.itchio", .title = "Saga",
         .rom_paths = saga_paths, .rom_path_count = 2},
    };
    jw_db_imported_title_result result;
    if (jw_db_apply_imported_title_groups(db, groups, 2, &result) != 0 ||
        result.matched != 4 || result.applied != 4 || result.unmatched != 0) {
        fail("unexpected suffix fixture result");
    }
    jw_db_close(db);

    expect_name(db_path, "Roms/GB/Glory Hunters.gb", "Glory Hunters");
    expect_name(db_path, "Roms/GBA/glory hunters .gba", "Glory Hunters");
    expect_name(db_path, "Roms/PS/Saga.cue", "Saga");
    expect_name(db_path, "Roms/PS/Saga (Disc 2).cue", "Saga — Saga (Disc 2)");
    unlink(db_path);
}

int main(void) {
    check_redundant_suffix();

    char db_path[] = "/tmp/jawaka-imported-title.XXXXXX";
    int fd = mkstemp(db_path);
    if (fd < 0) fail("mkstemp failed");
    close(fd);
    unlink(db_path);

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        jw_db_scan_begin(db) != 0 ||
        jw_db_insert_game(db, "PS", "game", "Roms/PS/game.cue", NULL) != 0 ||
        jw_db_insert_game(db, "PICO8", "cart-a", "Roms/PICO8/cart-a.p8", NULL) != 0 ||
        jw_db_insert_game(db, "PICO8", "cart-b", "Roms/PICO8/cart-b.p8", NULL) != 0) {
        fail("could not prepare fixture database");
    }

    const char *ps_paths[] = {"Roms/PS/game.cue", "Roms/PS/track.bin"};
    const char *p8_paths[] = {"Roms/PICO8/cart-a.p8", "Roms/PICO8/cart-b.p8"};
    jw_db_imported_title_group groups[] = {
        {.provider = "org.umrk.itchio", .title = "Black Jewel Reborn",
         .rom_paths = ps_paths, .rom_path_count = 2},
        {.provider = "org.umrk.itchio", .title = "Pocket Collection",
         .rom_paths = p8_paths, .rom_path_count = 2},
    };
    jw_db_imported_title_result result;
    if (jw_db_apply_imported_title_groups(db, groups, 2, &result) != 0 ||
        result.groups != 2 || result.paths != 4 || result.matched != 3 ||
        result.applied != 3 || result.unmatched != 1) {
        fail("unexpected imported-title result");
    }
    jw_db_close(db);

    expect_name(db_path, "Roms/PS/game.cue", "Black Jewel Reborn");
    expect_name(db_path, "Roms/PICO8/cart-a.p8", "Pocket Collection — cart-a");
    expect_name(db_path, "Roms/PICO8/cart-b.p8", "Pocket Collection — cart-b");

    jw_game_entry ps;
    if (jw_db_get_game_by_rom_path(db_path, "Roms/PS/game.cue", &ps) != 0 ||
        jw_db_set_game_setting(db_path, ps.id, "display_name", "My Manual Name") != 0) {
        fail("manual override setup failed");
    }
    expect_name(db_path, "Roms/PS/game.cue", "My Manual Name");

    jw_game_entry listed[8];
    int listed_count = 0;
    if (jw_db_list_games_for_system(db_path, "PS", listed, 8, &listed_count) != 0 ||
        listed_count != 1 || strcmp(listed[0].name, "My Manual Name") != 0) {
        fail("library list ignored manual precedence");
    }
    if (jw_db_set_favorite(db_path, "game", ps.id, 1) != 0 ||
        jw_db_list_favorite_games(db_path, listed, 8, &listed_count) != 0 ||
        listed_count != 1 || strcmp(listed[0].name, "My Manual Name") != 0) {
        fail("favorites ignored effective title");
    }
    if (jw_db_record_play(db_path, "Roms/PS/game.cue", 10) != 0 ||
        jw_db_list_recent_games(db_path, listed, 8, &listed_count) != 0 ||
        listed_count != 1 || strcmp(listed[0].name, "My Manual Name") != 0) {
        fail("recents ignored effective title");
    }
    if (jw_db_delete_game_setting(db_path, ps.id, "display_name") != 0) {
        fail("manual override reset failed");
    }
    expect_name(db_path, "Roms/PS/game.cue", "Black Jewel Reborn");

    jw_library_stats stats;
    if (jw_db_read_stats(db_path, &stats) != 0 || stats.top_count < 1 ||
        strcmp(stats.top[0].name, "Black Jewel Reborn") != 0) {
        fail("library stats ignored imported title");
    }
    jw_library_summary summary;
    if (jw_db_read_summary(db_path, &summary) != 0 ||
        strstr(summary.sample_summary, "Black Jewel Reborn") == NULL) {
        fail("library summary ignored imported title");
    }

    jw_search_result search[8];
    int search_count = 0;
    if (jw_db_search_library(db_path, "Black Jewel", search, 8, &search_count) != 0 ||
        search_count < 1 || strcmp(search[0].name, "Black Jewel Reborn") != 0 ||
        !search[0].favorite) {
        fail("search ignored imported title or favorite state");
    }

    /* A normal scan upsert keeps the stable game id and imported settings. */
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        jw_db_scan_begin(db) != 0 ||
        jw_db_insert_game(db, "PS", "game", "Roms/PS/game.cue", NULL) != 0 ||
        jw_db_insert_game(db, "PICO8", "cart-a", "Roms/PICO8/cart-a.p8", NULL) != 0 ||
        jw_db_insert_game(db, "PICO8", "cart-b", "Roms/PICO8/cart-b.p8", NULL) != 0 ||
        jw_db_scan_source_complete(db, "primary") != 0 ||
        jw_db_scan_prune(db) != 0) {
        fail("rescan fixture failed");
    }
    jw_db_close(db);
    expect_name(db_path, "Roms/PS/game.cue", "Black Jewel Reborn");

    /* Removing the ROM prunes its settings through the existing FK cascade. */
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        jw_db_scan_begin(db) != 0 ||
        jw_db_insert_game(db, "PICO8", "cart-a", "Roms/PICO8/cart-a.p8", NULL) != 0 ||
        jw_db_insert_game(db, "PICO8", "cart-b", "Roms/PICO8/cart-b.p8", NULL) != 0 ||
        jw_db_scan_source_complete(db, "primary") != 0 ||
        jw_db_scan_prune(db) != 0) {
        fail("removal scan failed");
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT COUNT(*) FROM game_settings WHERE game_id = ?;",
            -1, &stmt, NULL) != SQLITE_OK) {
        fail("cleanup query failed");
    }
    sqlite3_bind_int(stmt, 1, ps.id);
    if (sqlite3_step(stmt) != SQLITE_ROW || sqlite3_column_int(stmt, 0) != 0) {
        fail("removed game retained imported metadata");
    }
    sqlite3_finalize(stmt);
    jw_db_close(db);
    unlink(db_path);

    puts("PASS imported-title-test");
    return 0;
}
