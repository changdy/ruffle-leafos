#include "internal/db/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void fail(const char *message) {
    fprintf(stderr, "pinyin-search-test: %s\n", message);
    exit(1);
}

static void expect_one(const char *db_path, const char *query,
                       const char *want, int want_favorite) {
    jw_search_result results[8];
    int count = 0;
    if (jw_db_search_library(db_path, query, results, 8, &count) != 0 ||
        count != 1 || strcmp(results[0].name, want) != 0 ||
        results[0].favorite != want_favorite) {
        fprintf(stderr,
                "pinyin-search-test: query=%s count=%d name=%s favorite=%d\n",
                query, count, count > 0 ? results[0].name : "",
                count > 0 ? results[0].favorite : 0);
        exit(1);
    }
}

int main(void) {
    char db_path[] = "/tmp/jawaka-pinyin.XXXXXX";
    int fd = mkstemp(db_path);
    if (fd < 0) fail("mkstemp failed");
    close(fd);
    unlink(db_path);

    jw_search_result results[8];
    int count = -1;
    if (jw_db_search_library(db_path, "", results, 8, &count) != 0 ||
        count != 0 || access(db_path, F_OK) == 0 ||
        jw_db_search_library(db_path, " ", results, 8, &count) != 0 ||
        count != 0 || access(db_path, F_OK) == 0) {
        fail("blank query opened or searched the database");
    }

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        jw_db_scan_begin(db) != 0 ||
        jw_db_insert_game(db, "PS", "三国志 Beta", "Roms/PS/beta.cue", NULL) != 0 ||
        jw_db_insert_game(db, "NES", "超级马里奥", "Roms/NES/mario.nes", NULL) != 0 ||
        jw_db_insert_game(db, "PS", "ZZZ Base", "Roms/PS/alpha.cue", NULL) != 0 ||
        jw_db_insert_game(db, "PS", "SGZ Collection", "Roms/PS/sgz.cue", NULL) != 0 ||
        jw_db_insert_game(db, "PS", "Black Jewel Reborn", "Roms/PS/jewel.cue", NULL) != 0) {
        fail("could not prepare fixture database");
    }
    jw_db_close(db);

    expect_one(db_path, "sgz", "SGZ Collection", 0);
    if (jw_db_set_setting(db_path, "language", "zh_CN") != 0) {
        fail("could not enable Simplified Chinese");
    }

    jw_game_entry alpha;
    if (jw_db_get_game_by_rom_path(db_path, "Roms/PS/alpha.cue", &alpha) != 0 ||
        jw_db_set_game_setting(db_path, alpha.id, "display_name", "三国志 Alpha") != 0 ||
        jw_db_set_favorite(db_path, "game", alpha.id, 1) != 0) {
        fail("could not favorite deterministic-order fixture");
    }

    expect_one(db_path, "CJMLA", "超级马里奥", 0);

    count = 0;
    if (jw_db_search_library(db_path, "s g-z ", results, 2, &count) != 0 ||
        count != 2 || strcmp(results[0].name, "SGZ Collection") != 0 ||
        strcmp(results[1].name, "三国志 Alpha") != 0 ||
        !results[1].favorite) {
        fail("combined capped search was not normalized, deduplicated, or ordered");
    }

    count = 0;
    if (jw_db_search_library(db_path, " \t三国志 \r\n", results, 8, &count) != 0 ||
        count != 2 || strcmp(results[0].name, "三国志 Alpha") != 0 ||
        strcmp(results[1].name, "三国志 Beta") != 0) {
        fail("whitespace-padded literal Han search regressed");
    }

    expect_one(db_path, "三国志 Alpha", "三国志 Alpha", 1);
    expect_one(db_path, "Black Jewel", "Black Jewel Reborn", 0);

    unlink(db_path);
    puts("PASS pinyin-search-test");
    return 0;
}
