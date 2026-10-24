#include "internal/db/db.h"
#include "internal/discovery/discovery.h"

#include <sqlite3.h>
#include <stdio.h>

static int jw__print_games(sqlite3 *db) {
    static const char *sql =
        "SELECT system, name, rom_path, COALESCE(image_path, '') "
        "FROM games ORDER BY system, rom_path;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *system = sqlite3_column_text(stmt, 0);
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        const unsigned char *rom_path = sqlite3_column_text(stmt, 2);
        const unsigned char *image_path = sqlite3_column_text(stmt, 3);
        printf("game\t%s\t%s\t%s\t%s\n",
               system ? (const char *)system : "",
               name ? (const char *)name : "",
               rom_path ? (const char *)rom_path : "",
               image_path ? (const char *)image_path : "");
    }

    sqlite3_finalize(stmt);
    return 0;
}

static int jw__print_apps(sqlite3 *db) {
    static const char *sql =
        "SELECT name,pak_dir,platform,COALESCE(icon,''),"
        "COALESCE(min_leaf_version,'') "
        "FROM apps ORDER BY pak_dir;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "prepare failed: %s\n", sqlite3_errmsg(db));
        return -1;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 0);
        const unsigned char *pak_dir = sqlite3_column_text(stmt, 1);
        const unsigned char *platform = sqlite3_column_text(stmt, 2);
        const unsigned char *icon = sqlite3_column_text(stmt, 3);
        const unsigned char *min_leaf_version =
            sqlite3_column_text(stmt, 4);
        printf("app\t%s\t%s\t%s\t%s\t%s\n",
               name ? (const char *)name : "",
               pak_dir ? (const char *)pak_dir : "",
               platform ? (const char *)platform : "",
               icon ? (const char *)icon : "",
               min_leaf_version ? (const char *)min_leaf_version : "");
    }

    sqlite3_finalize(stmt);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <sdcard-root> <db-path>\n", argv[0]);
        return 2;
    }

    sqlite3 *db = NULL;
    if (jw_db_open(argv[2], &db) != 0 || !db) {
        fprintf(stderr, "could not open db: %s\n", argv[2]);
        return 1;
    }

    int rc = 0;
    jw_scan_result result;
    if (jw_db_apply_schema(db) != 0 ||
        jw_scan_library(db, argv[1], &result) != 0) {
        fprintf(stderr, "scan failed\n");
        rc = 1;
    } else {
        printf("summary\tgames=%d\tsystems=%d\tapps=%d\n",
               result.game_count, result.system_count, result.app_count);
        if (jw__print_games(db) != 0) {
            rc = 1;
        }
        if (jw__print_apps(db) != 0) {
            rc = 1;
        }
    }

    jw_db_close(db);
    return rc;
}
