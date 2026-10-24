#include "internal/db/db.h"
#include "internal/search/pinyin.h"
#include "internal/db/relocation.h"
#include "internal/focus/focus.h"
#include "internal/storage/sources.h"

#include <ctype.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>

#define JW_DB_SCHEMA_VERSION 6
/* Development builds briefly stamped the additive hidden_roms table as v7. No
   release shipped it, and every released Jawaka refuses a newer version, so
   such a library returns to v6. */
#define JW_DB_UNRELEASED_VERSION 7

#define JW_VISIBLE_GAME(table) \
    "NOT EXISTS (SELECT 1 FROM hidden_roms h WHERE h.source_id=" table ".source_id " \
    "AND h.rom_relpath=" table ".rom_relpath AND h.member='') "

/* An additive v6 table: older releases open the library and ignore it. */
#define JW_HIDDEN_TABLE_SQL(schema) \
    "CREATE TABLE IF NOT EXISTS " schema "hidden_roms (" \
    "source_id TEXT NOT NULL,rom_relpath TEXT NOT NULL," \
    "member TEXT NOT NULL DEFAULT ''," \
    "PRIMARY KEY(source_id,rom_relpath,member));"

static void jw__fill_game_entry(sqlite3_stmt *stmt, jw_game_entry *out);
static int jw__write_error(sqlite3 *db);

static const char *kSchemaSql =
    "PRAGMA foreign_keys = ON;\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS games (\n"
    "    id          INTEGER PRIMARY KEY,\n"
    "    system      TEXT    NOT NULL,\n"
    "    name        TEXT    NOT NULL,\n"
    "    source_id   TEXT    NOT NULL,\n"
    "    rom_relpath TEXT    NOT NULL,\n"
    "    rom_path    TEXT    NOT NULL,\n"
    "    image_root_kind TEXT CHECK (image_root_kind IN ('images','roms','source')),\n"
    "    image_relpath TEXT,\n"
    "    image_path  TEXT,\n"
    "    last_played INTEGER,\n"
    "    playtime_s  INTEGER NOT NULL DEFAULT 0,\n"
    "    UNIQUE (source_id, rom_relpath)\n"
    ");\n"
    "CREATE INDEX IF NOT EXISTS games_rom_path_idx ON games(rom_path);\n"
    "CREATE INDEX IF NOT EXISTS games_image_path_idx ON games(image_path);\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS apps (\n"
    "    id                  INTEGER PRIMARY KEY,\n"
    "    pak_dir             TEXT NOT NULL UNIQUE,\n"
    "    name                TEXT NOT NULL,\n"
    "    icon                TEXT,\n"
    "    platform            TEXT,\n"
    "    pak_version         TEXT,\n"
    "    min_jawaka_version  TEXT,\n"
    "    min_leaf_version    TEXT\n"
    ");\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS favorites (\n"
    "    kind       TEXT NOT NULL CHECK (kind IN ('game','app')),\n"
    "    target_id  INTEGER NOT NULL,\n"
    "    added_at   INTEGER NOT NULL,\n"
    "    PRIMARY KEY (kind, target_id)\n"
    ");\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS recents (\n"
    "    kind        TEXT NOT NULL CHECK (kind IN ('game','app')),\n"
    "    target_id   INTEGER NOT NULL,\n"
    "    last_opened INTEGER NOT NULL,\n"
    "    duration_s  INTEGER NOT NULL DEFAULT 0,\n"
    "    PRIMARY KEY (kind, target_id)\n"
    ");\n"
    "\n"
    "CREATE VIRTUAL TABLE IF NOT EXISTS games_fts USING fts5(\n"
    "    name, system, content='games', content_rowid='id'\n"
    ");\n"
    "\n"
    "CREATE TRIGGER IF NOT EXISTS games_ai AFTER INSERT ON games BEGIN\n"
    "    INSERT INTO games_fts(rowid, name, system)\n"
    "        VALUES (new.id, new.name, new.system);\n"
    "END;\n"
    "\n"
    "CREATE TRIGGER IF NOT EXISTS games_ad AFTER DELETE ON games BEGIN\n"
    "    INSERT INTO games_fts(games_fts, rowid, name, system)\n"
    "        VALUES ('delete', old.id, old.name, old.system);\n"
    "END;\n"
    "\n"
    "CREATE TRIGGER IF NOT EXISTS games_au AFTER UPDATE ON games BEGIN\n"
    "    INSERT INTO games_fts(games_fts, rowid, name, system)\n"
    "        VALUES ('delete', old.id, old.name, old.system);\n"
    "    INSERT INTO games_fts(rowid, name, system)\n"
    "        VALUES (new.id, new.name, new.system);\n"
    "END;\n"
    "\n"
    "CREATE VIRTUAL TABLE IF NOT EXISTS apps_fts USING fts5(\n"
    "    name, pak_dir, content='apps', content_rowid='id'\n"
    ");\n"
    "\n"
    "CREATE TRIGGER IF NOT EXISTS apps_ai AFTER INSERT ON apps BEGIN\n"
    "    INSERT INTO apps_fts(rowid, name, pak_dir)\n"
    "        VALUES (new.id, new.name, new.pak_dir);\n"
    "END;\n"
    "\n"
    "CREATE TRIGGER IF NOT EXISTS apps_ad AFTER DELETE ON apps BEGIN\n"
    "    INSERT INTO apps_fts(apps_fts, rowid, name, pak_dir)\n"
    "        VALUES ('delete', old.id, old.name, old.pak_dir);\n"
    "END;\n"
    "\n"
    "CREATE TRIGGER IF NOT EXISTS apps_au AFTER UPDATE ON apps BEGIN\n"
    "    INSERT INTO apps_fts(apps_fts, rowid, name, pak_dir)\n"
    "        VALUES ('delete', old.id, old.name, old.pak_dir);\n"
    "    INSERT INTO apps_fts(rowid, name, pak_dir)\n"
    "        VALUES (new.id, new.name, new.pak_dir);\n"
    "END;\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS settings (\n"
    "    key   TEXT PRIMARY KEY,\n"
    "    value TEXT NOT NULL\n"
    ");\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS system_settings (\n"
    "    system     TEXT NOT NULL,\n"
    "    key        TEXT NOT NULL,\n"
    "    value      TEXT NOT NULL,\n"
    "    updated_at INTEGER NOT NULL,\n"
    "    PRIMARY KEY (system, key)\n"
    ");\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS game_settings (\n"
    "    game_id    INTEGER NOT NULL,\n"
    "    key        TEXT NOT NULL,\n"
    "    value      TEXT NOT NULL,\n"
    "    updated_at INTEGER NOT NULL,\n"
    "    PRIMARY KEY (game_id, key),\n"
    "    FOREIGN KEY (game_id) REFERENCES games(id) ON DELETE CASCADE\n"
    ");\n"
    "\n"
    "CREATE TABLE IF NOT EXISTS pakrat_installs (\n"
    "    store_id        TEXT PRIMARY KEY,\n"
    "    version         TEXT NOT NULL,\n"
    "    platform        TEXT NOT NULL,\n"
    "    source_id       TEXT NOT NULL DEFAULT 'primary',\n"
    "    install_path    TEXT NOT NULL,\n"
    "    artifact_sha256 TEXT NOT NULL,\n"
    "    installed_at    TEXT NOT NULL,\n"
    "    commit_token    TEXT,\n"
    "    kind            TEXT NOT NULL DEFAULT 'app' CHECK (kind IN ('app','theme'))\n"
    ");\n"
    "\n"
    "CREATE INDEX IF NOT EXISTS pakrat_installs_install_path_idx\n"
    "    ON pakrat_installs(install_path);\n"
    "PRAGMA user_version = 6;\n";

static const char *kRelocationSchemaSql =
    "PRAGMA foreign_keys = ON;\n"
    "CREATE TABLE IF NOT EXISTS library_relocation_ops (\n"
    " operation_id TEXT PRIMARY KEY,\n"
    " state TEXT NOT NULL CHECK(state IN "
    "('prepared','committed','reverted','finishing','finished','aborted')),\n"
    " expected_generation INTEGER NOT NULL,\n"
    " mapping_generation INTEGER NOT NULL DEFAULT 0,\n"
    " scan_ticket_generation INTEGER NOT NULL DEFAULT 0,\n"
    " item_count INTEGER NOT NULL,\n"
    " request_fingerprint TEXT NOT NULL,\n"
    " updated_at INTEGER NOT NULL\n"
    ");\n"
    "CREATE TABLE IF NOT EXISTS library_relocation_items (\n"
    " operation_id TEXT NOT NULL, ordinal INTEGER NOT NULL, game_id INTEGER NOT NULL,\n"
    " old_source_id TEXT NOT NULL, old_rom_relpath TEXT NOT NULL,\n"
    " old_image_root_kind TEXT NOT NULL, old_image_relpath TEXT NOT NULL,\n"
    " old_rom_path TEXT NOT NULL, old_image_path TEXT NOT NULL,\n"
    " new_source_id TEXT NOT NULL, new_rom_relpath TEXT NOT NULL,\n"
    " new_image_root_kind TEXT NOT NULL, new_image_relpath TEXT NOT NULL,\n"
    " new_rom_path TEXT NOT NULL, new_image_path TEXT NOT NULL,\n"
    " old_source_snapshot TEXT NOT NULL, new_source_snapshot TEXT NOT NULL,\n"
    " PRIMARY KEY(operation_id,ordinal),\n"
    " FOREIGN KEY(operation_id) REFERENCES library_relocation_ops(operation_id)"
    " ON DELETE CASCADE\n"
    ");\n"
    "CREATE INDEX IF NOT EXISTS library_relocation_items_game_idx "
    "ON library_relocation_items(game_id);\n";

static int jw__exec(sqlite3 *db, const char *sql) {
    char *err = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &err);
    if (rc != SQLITE_OK) {
        if (err) {
            fprintf(stderr, "sqlite exec failed: %s\n", err);
            sqlite3_free(err);
        } else {
            fprintf(stderr, "sqlite exec failed: %s\n", sqlite3_errmsg(db));
        }
        return -1;
    }
    return 0;
}

static int jw__schema_version(sqlite3 *db, int *out_version) {
    sqlite3_stmt *stmt = NULL;
    if (!db || !out_version) {
        return -1;
    }
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out_version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return rc == SQLITE_ROW ? 0 : -1;
}

static int jw__table_has_column(sqlite3 *db, const char *table,
                                const char *column, int *out_present) {
    if (!db || !table || !table[0] || !column || !column[0] ||
        !out_present) {
        return -1;
    }
    *out_present = 0;
    char sql[256];
    if (snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table) >=
        (int)sizeof(sql)) {
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (name && strcmp((const char *)name, column) == 0) {
            *out_present = 1;
            break;
        }
    }
    sqlite3_finalize(stmt);
    return step == SQLITE_ROW || step == SQLITE_DONE ? 0 : -1;
}

static int jw__ensure_apps_min_leaf_version(sqlite3 *db) {
    int present = 0;
    if (jw__table_has_column(db, "apps", "min_leaf_version",
                             &present) != 0) {
        return -1;
    }
    if (present) {
        return 0;
    }

    char *error = NULL;
    int rc = sqlite3_exec(
        db, "ALTER TABLE apps ADD COLUMN min_leaf_version TEXT;",
        NULL, NULL, &error);
    if (rc == SQLITE_OK) {
        sqlite3_free(error);
        return 0;
    }

    /* Two launcher processes can discover the absent column concurrently.
       If the other connection won the ALTER race, the end state is already
       correct and this call remains idempotent. */
    int now_present = 0;
    if (jw__table_has_column(db, "apps", "min_leaf_version",
                             &now_present) == 0 &&
        now_present) {
        sqlite3_free(error);
        return 0;
    }
    fprintf(stderr, "sqlite apps min_leaf_version migration failed: %s\n",
            error ? error : sqlite3_errmsg(db));
    sqlite3_free(error);
    return -1;
}

static int jw__ensure_pakrat_commit_token(sqlite3 *db) {
    if (jw__exec(
            db,
            "CREATE TABLE IF NOT EXISTS pakrat_installs ("
            "store_id TEXT PRIMARY KEY,version TEXT NOT NULL,"
            "platform TEXT NOT NULL,source_id TEXT NOT NULL DEFAULT 'primary',"
            "install_path TEXT NOT NULL,"
            "artifact_sha256 TEXT NOT NULL,installed_at TEXT NOT NULL,"
            "commit_token TEXT,"
            "kind TEXT NOT NULL DEFAULT 'app' CHECK(kind IN ('app','theme')));"
            "CREATE INDEX IF NOT EXISTS pakrat_installs_install_path_idx "
            "ON pakrat_installs(install_path);") != 0) {
        return -1;
    }
    static const struct {
        const char *column;
        const char *alter_sql;
    } columns[] = {
        {"commit_token",
         "ALTER TABLE pakrat_installs ADD COLUMN commit_token TEXT;"},
        {"source_id",
         "ALTER TABLE pakrat_installs ADD COLUMN source_id TEXT NOT NULL "
         "DEFAULT 'primary';"},
        /* Pak Rat themes. Every row written before this column existed is an
           app or content pak, which is what the default says. */
        {"kind",
         "ALTER TABLE pakrat_installs ADD COLUMN kind TEXT NOT NULL "
         "DEFAULT 'app' CHECK(kind IN ('app','theme'));"},
    };
    for (size_t i = 0; i < sizeof(columns) / sizeof(columns[0]); i++) {
        int present = 0;
        if (jw__table_has_column(db, "pakrat_installs", columns[i].column,
                                 &present) != 0) {
            return -1;
        }
        if (present) {
            continue;
        }
        char *error = NULL;
        int rc = sqlite3_exec(db, columns[i].alter_sql, NULL, NULL, &error);
        int now_present = 0;
        if (rc != SQLITE_OK &&
            (jw__table_has_column(db, "pakrat_installs", columns[i].column,
                                  &now_present) != 0 ||
             !now_present)) {
            fprintf(stderr, "sqlite pakrat %s migration failed: %s\n",
                    columns[i].column,
                    error ? error : sqlite3_errmsg(db));
            sqlite3_free(error);
            return -1;
        }
        sqlite3_free(error);
    }
    return jw__exec(
        db,
        "CREATE TABLE IF NOT EXISTS pakrat_service_metadata ("
        "store_id TEXT PRIMARY KEY,install_path TEXT NOT NULL,"
        "package_id TEXT NOT NULL,display_name TEXT NOT NULL,"
        "has_service INTEGER NOT NULL CHECK(has_service IN (0,1)),"
        "service_id TEXT NOT NULL,state_root TEXT NOT NULL,"
        "revoke_json TEXT NOT NULL,retained_json TEXT NOT NULL,"
        "validated_at TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS pakrat_pending_uninstalls ("
        "store_id TEXT PRIMARY KEY,source_id TEXT NOT NULL,"
        "install_path TEXT NOT NULL,package_id TEXT NOT NULL,"
        "display_name TEXT NOT NULL,"
        "has_service INTEGER NOT NULL CHECK(has_service IN (0,1)),"
        "service_id TEXT NOT NULL,state_root TEXT NOT NULL,"
        "revoke_json TEXT NOT NULL,retained_json TEXT NOT NULL,"
        "created_at TEXT NOT NULL);");
}

static int jw__backup_before_v6(sqlite3 *db) {
    const char *path = sqlite3_db_filename(db, "main");
    if (!path || !path[0] || strcmp(path, ":memory:") == 0) {
        return 0;
    }
    if (sqlite3_wal_checkpoint_v2(db, "main", SQLITE_CHECKPOINT_FULL,
                                  NULL, NULL) != SQLITE_OK) {
        return -1;
    }
    char backup_path[PATH_MAX];
    char temp_path[PATH_MAX];
    if (snprintf(backup_path, sizeof(backup_path), "%s.v5-backup", path) >=
            (int)sizeof(backup_path) ||
        snprintf(temp_path, sizeof(temp_path), "%s.tmp", backup_path) >=
            (int)sizeof(temp_path)) {
        return -1;
    }
    unlink(temp_path);
    sqlite3 *dest = NULL;
    if (sqlite3_open(temp_path, &dest) != SQLITE_OK) {
        if (dest) sqlite3_close(dest);
        return -1;
    }
    sqlite3_backup *backup = sqlite3_backup_init(dest, "main", db, "main");
    int rc = backup ? sqlite3_backup_step(backup, -1) : SQLITE_ERROR;
    if (backup) {
        int finish_rc = sqlite3_backup_finish(backup);
        if (rc == SQLITE_DONE) rc = finish_rc;
    }
    if (sqlite3_close(dest) != SQLITE_OK || rc != SQLITE_OK) {
        unlink(temp_path);
        return -1;
    }
    int fd = open(temp_path, O_RDONLY);
    if (fd < 0 || fsync(fd) != 0) {
        if (fd >= 0) close(fd);
        unlink(temp_path);
        return -1;
    }
    close(fd);
    if (rename(temp_path, backup_path) != 0) {
        unlink(temp_path);
        return -1;
    }
    char directory[PATH_MAX];
    if (snprintf(directory, sizeof(directory), "%s", backup_path) >=
        (int)sizeof(directory)) {
        return -1;
    }
    char *slash = strrchr(directory, '/');
    if (slash) {
        *slash = '\0';
        if (!directory[0]) snprintf(directory, sizeof(directory), "%s", "/");
    } else {
        snprintf(directory, sizeof(directory), "%s", ".");
    }
    int dir_fd = open(directory, O_RDONLY);
    if (dir_fd < 0 || fsync(dir_fd) != 0) {
        if (dir_fd >= 0) close(dir_fd);
        return -1;
    }
    close(dir_fd);
    return 0;
}

static const char *jw__relative_under_root(const char *path, const char *root) {
    if (!path || !root || !path[0] || !root[0]) return NULL;
    size_t len = strlen(root);
    return strncmp(path, root, len) == 0 && path[len] == '/' ? path + len + 1 : NULL;
}

static const char *jw__legacy_mlp1_rom_relative(const char *path) {
    static const char *roots[] = {
        "/mnt/sdcard/Roms/",
        "/media/sdcard1/Roms/",
    };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        size_t len = strlen(roots[i]);
        if (strncmp(path, roots[i], len) == 0) return path + len;
    }
    return NULL;
}

static const jw_storage_source *jw__unique_source_for_legacy_rom(
    const jw_storage_source_list *sources, const char *path,
    const char **out_relative) {
    const jw_storage_source *match = NULL;
    const char *relative = NULL;
    if (!sources || !path || path[0] != '/') return NULL;
    for (int i = 0; i < sources->count; i++) {
        const jw_storage_source *source = &sources->sources[i];
        const char *candidate = jw__relative_under_root(path, source->roms_path);
        if (!candidate && source->root_abs[0] &&
            strcmp(source->root_abs, source->root) != 0) {
            char roms_abs[JW_STORAGE_PATH_MAX];
            if (snprintf(roms_abs, sizeof(roms_abs), "%s/Roms",
                         source->root_abs) < (int)sizeof(roms_abs)) {
                candidate = jw__relative_under_root(path, roms_abs);
            }
        }
        if (candidate) {
            if (match) return NULL;
            match = source;
            relative = candidate;
        }
    }
    if (match && out_relative) *out_relative = relative;
    return match;
}

static void jw__legacy_image_identity(const char *image_path,
                                      const jw_storage_source *source,
                                      char *kind, size_t kind_size,
                                      char *relative, size_t relative_size) {
    kind[0] = '\0';
    relative[0] = '\0';
    if (!image_path || !image_path[0]) return;
    const char *rel = NULL;
    const char *resolved_kind = NULL;
    if (strncmp(image_path, "Images/", 7) == 0) {
        resolved_kind = "images";
        rel = image_path + 7;
    } else if (strncmp(image_path, "Roms/", 5) == 0) {
        resolved_kind = "roms";
        rel = image_path + 5;
    } else if (image_path[0] != '/') {
        resolved_kind = "source";
        rel = image_path;
    } else if (source &&
               (rel = jw__relative_under_root(image_path,
                                              source->images_path)) != NULL) {
        resolved_kind = "images";
    } else if (source &&
               (rel = jw__relative_under_root(image_path,
                                              source->roms_path)) != NULL) {
        resolved_kind = "roms";
    } else if (source &&
               (rel = jw__relative_under_root(image_path, source->root)) != NULL) {
        resolved_kind = "source";
    } else if ((rel = strstr(image_path, "/Images/")) != NULL) {
        resolved_kind = "images";
        rel += 8;
    } else if ((rel = strstr(image_path, "/Roms/")) != NULL) {
        resolved_kind = "roms";
        rel += 6;
    }
    if (resolved_kind && rel && jw_storage_relative_path_valid(rel)) {
        snprintf(kind, kind_size, "%s", resolved_kind);
        snprintf(relative, relative_size, "%s", rel);
    }
}

static int jw__prepare_v6_identity_map(sqlite3 *db) {
    if (jw__exec(db,
        "DROP TABLE IF EXISTS temp._v6_identity;"
        "CREATE TEMP TABLE _v6_identity("
        "old_id INTEGER PRIMARY KEY,source_id TEXT NOT NULL,"
        "rom_relpath TEXT NOT NULL,image_root_kind TEXT,image_relpath TEXT);") != 0) {
        return -1;
    }
    jw_storage_source_list sources;
    bool have_sources = jw_storage_sources_resolve(NULL, &sources) == 0;
    sqlite3_stmt *read = NULL;
    sqlite3_stmt *write = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT id,rom_path,COALESCE(image_path,'') FROM games ORDER BY id;",
            -1, &read, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "INSERT INTO _v6_identity VALUES(?,?,?,?,?);",
            -1, &write, NULL) != SQLITE_OK) {
        sqlite3_finalize(read);
        sqlite3_finalize(write);
        return -1;
    }
    int status = 0;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(read)) == SQLITE_ROW) {
        int old_id = sqlite3_column_int(read, 0);
        const char *rom_path = (const char *)sqlite3_column_text(read, 1);
        const char *image_path = (const char *)sqlite3_column_text(read, 2);
        const char *source_id = NULL;
        const char *rom_relpath = NULL;
        const jw_storage_source *source = NULL;
        if (rom_path && strncmp(rom_path, "Roms/", 5) == 0) {
            source_id = "primary";
            rom_relpath = rom_path + 5;
            if (have_sources) source = jw_storage_sources_find_by_id(&sources, source_id);
        } else if (rom_path &&
                   (rom_relpath = jw__legacy_mlp1_rom_relative(rom_path)) != NULL) {
            source_id = "secondary_sd";
            if (have_sources) source = jw_storage_sources_find_by_id(&sources, source_id);
        } else if (rom_path && have_sources &&
                   (source = jw__unique_source_for_legacy_rom(
                       &sources, rom_path, &rom_relpath)) != NULL) {
            source_id = source->id;
        }
        if (!source_id || !rom_relpath ||
            !jw_storage_relative_path_valid(rom_relpath)) {
            status = -1;
            break;
        }
        char image_kind[16];
        char image_relative[JW_STORAGE_PATH_MAX];
        jw__legacy_image_identity(image_path, source, image_kind,
                                  sizeof(image_kind), image_relative,
                                  sizeof(image_relative));
        sqlite3_reset(write);
        sqlite3_bind_int(write, 1, old_id);
        sqlite3_bind_text(write, 2, source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(write, 3, rom_relpath, -1, SQLITE_TRANSIENT);
        if (image_kind[0]) {
            sqlite3_bind_text(write, 4, image_kind, -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(write, 5, image_relative, -1, SQLITE_TRANSIENT);
        } else {
            sqlite3_bind_null(write, 4);
            sqlite3_bind_null(write, 5);
        }
        if (sqlite3_step(write) != SQLITE_DONE) {
            status = -1;
            break;
        }
    }
    if (step != SQLITE_DONE && status == 0) status = -1;
    sqlite3_finalize(read);
    sqlite3_finalize(write);
    return status;
}

static int jw__migrate_to_v6(sqlite3 *db) {
    static const char *begin_sql =
        "PRAGMA foreign_keys = OFF;"
        "BEGIN IMMEDIATE;"
        "DROP TRIGGER IF EXISTS games_ai;"
        "DROP TRIGGER IF EXISTS games_ad;"
        "DROP TRIGGER IF EXISTS games_au;"
        "DROP TABLE IF EXISTS games_fts;"
        "CREATE TABLE games_v6 ("
        " id INTEGER PRIMARY KEY,"
        " system TEXT NOT NULL,"
        " name TEXT NOT NULL,"
        " source_id TEXT NOT NULL,"
        " rom_relpath TEXT NOT NULL,"
        " rom_path TEXT NOT NULL,"
        " image_root_kind TEXT CHECK (image_root_kind IN ('images','roms','source')),"
        " image_relpath TEXT,"
        " image_path TEXT,"
        " last_played INTEGER,"
        " playtime_s INTEGER NOT NULL DEFAULT 0,"
        " UNIQUE(source_id, rom_relpath)"
        ");"
        "INSERT OR IGNORE INTO games_v6 "
        "(id,system,name,source_id,rom_relpath,rom_path,image_root_kind,image_relpath,"
        " image_path,last_played,playtime_s) "
        "SELECT g.id,g.system,g.name,i.source_id,i.rom_relpath,g.rom_path,"
        "i.image_root_kind,i.image_relpath,g.image_path,g.last_played,g.playtime_s "
        "FROM games g JOIN _v6_identity i ON i.old_id=g.id ORDER BY g.id;"
        "CREATE TEMP TABLE _game_id_map AS "
        "SELECT i.old_id,new.id AS new_id FROM _v6_identity i "
        "JOIN games_v6 new ON new.source_id=i.source_id "
        "AND new.rom_relpath=i.rom_relpath;"
        "UPDATE games_v6 SET "
        " playtime_s=(SELECT COALESCE(SUM(g.playtime_s),0) FROM games g "
        "             JOIN _game_id_map m ON m.old_id=g.id WHERE m.new_id=games_v6.id),"
        " last_played=(SELECT MAX(g.last_played) FROM games g "
        "              JOIN _game_id_map m ON m.old_id=g.id WHERE m.new_id=games_v6.id);";
    static const char *finish_sql =
        "CREATE TEMP TABLE _game_settings_v6 AS "
        "SELECT m.new_id AS game_id,s.key,s.value,s.updated_at FROM game_settings s "
        "JOIN _game_id_map m ON m.old_id=s.game_id "
        "WHERE NOT EXISTS (SELECT 1 FROM game_settings newer "
        " JOIN _game_id_map nm ON nm.old_id=newer.game_id "
        " WHERE nm.new_id=m.new_id AND newer.key=s.key "
        " AND (newer.updated_at>s.updated_at OR "
        "     (newer.updated_at=s.updated_at AND newer.game_id>s.game_id)));"
        "DELETE FROM game_settings;"
        "INSERT INTO game_settings SELECT * FROM _game_settings_v6;"
        "INSERT OR IGNORE INTO favorites(kind,target_id,added_at) "
        "SELECT 'game',m.new_id,MAX(f.added_at) FROM favorites f "
        "JOIN _game_id_map m ON m.old_id=f.target_id WHERE f.kind='game' "
        "GROUP BY m.new_id;"
        "DELETE FROM favorites WHERE kind='game' AND target_id NOT IN "
        "(SELECT DISTINCT new_id FROM _game_id_map);"
        "DELETE FROM favorites WHERE kind='game' AND target_id IN "
        "(SELECT old_id FROM _game_id_map WHERE old_id!=new_id);"
        "CREATE TEMP TABLE _recents_v6 AS "
        "SELECT 'game' kind,m.new_id target_id,r.last_opened,r.duration_s "
        "FROM recents r "
        "JOIN _game_id_map m ON m.old_id=r.target_id WHERE r.kind='game' "
        "AND NOT EXISTS (SELECT 1 FROM recents newer "
        " JOIN _game_id_map nm ON nm.old_id=newer.target_id "
        " WHERE newer.kind='game' AND nm.new_id=m.new_id "
        " AND (newer.last_opened>r.last_opened OR "
        "     (newer.last_opened=r.last_opened AND newer.target_id>r.target_id)));"
        "DELETE FROM recents WHERE kind='game';"
        "INSERT INTO recents SELECT * FROM _recents_v6;"
        "DROP TABLE games;"
        "ALTER TABLE games_v6 RENAME TO games;"
        "CREATE INDEX games_rom_path_idx ON games(rom_path);"
        "CREATE INDEX games_image_path_idx ON games(image_path);"
        "CREATE VIRTUAL TABLE games_fts USING fts5("
        " name,system,content='games',content_rowid='id');"
        "CREATE TRIGGER games_ai AFTER INSERT ON games BEGIN "
        " INSERT INTO games_fts(rowid,name,system) VALUES(new.id,new.name,new.system); END;"
        "CREATE TRIGGER games_ad AFTER DELETE ON games BEGIN "
        " INSERT INTO games_fts(games_fts,rowid,name,system) "
        " VALUES('delete',old.id,old.name,old.system); END;"
        "CREATE TRIGGER games_au AFTER UPDATE ON games BEGIN "
        " INSERT INTO games_fts(games_fts,rowid,name,system) "
        " VALUES('delete',old.id,old.name,old.system);"
        " INSERT INTO games_fts(rowid,name,system) VALUES(new.id,new.name,new.system); END;"
        "INSERT INTO games_fts(games_fts) VALUES('rebuild');"
        "PRAGMA user_version = 6;"
        "COMMIT;"
        "PRAGMA foreign_keys = ON;";
    if (jw__prepare_v6_identity_map(db) != 0 ||
        jw__backup_before_v6(db) != 0) {
        return -1;
    }
    if (jw__exec(db, begin_sql) != 0 || jw__exec(db, finish_sql) != 0) {
        sqlite3_exec(db, "ROLLBACK;", NULL, NULL, NULL);
        sqlite3_exec(db, "PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
        return -1;
    }
    sqlite3_stmt *check = NULL;
    if (sqlite3_prepare_v2(db, "PRAGMA foreign_key_check;", -1, &check, NULL) != SQLITE_OK) {
        return -1;
    }
    int rc = sqlite3_step(check);
    sqlite3_finalize(check);
    return rc == SQLITE_DONE ? 0 : -1;
}

/* A library from an older release gains hidden_roms on its first writable
   open. On a card that is already read-only this connection gets an empty
   temporary table instead, so browse and visibility reads still work. */
static int jw__visibility_table(sqlite3 *db) {
    if (sqlite3_exec(db, JW_HIDDEN_TABLE_SQL("main.") "DROP TABLE IF EXISTS temp.hidden_roms;",
                     NULL, NULL, NULL) == SQLITE_OK) return 0;
    if (sqlite3_db_readonly(db, "main") != 1) return -1;
    return sqlite3_exec(db, JW_HIDDEN_TABLE_SQL("temp."), NULL, NULL, NULL) == SQLITE_OK ? 0 : -1;
}

/* Only a library that predates the table needs it here; a fresh or unrelated
   file has no games table and is left to jw_db_apply_schema. This runs before
   the busy timeout, so a writer holding the library (a scan) never adds a wait
   to the caller's own query; that only happens on a writable card, where
   jawakad's startup schema has already added the table. */
static void jw__ensure_visibility_table(sqlite3 *db) {
    sqlite3_stmt *stmt = NULL;
    bool missing = sqlite3_prepare_v2(db, "SELECT EXISTS(SELECT 1 FROM main.sqlite_master "
                                      "WHERE type='table' AND name='games') AND NOT EXISTS("
                                      "SELECT 1 FROM main.sqlite_master WHERE type='table' "
                                      "AND name='hidden_roms');", -1, &stmt, NULL) == SQLITE_OK &&
                   sqlite3_step(stmt) == SQLITE_ROW && sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    if (missing) (void)jw__visibility_table(db);
}

int jw_db_open(const char *path, sqlite3 **out) {
    if (!path || !out) {
        return -1;
    }
    if (sqlite3_open(path, out) != SQLITE_OK) {
        return -1;
    }
    /* Launcher, menu, and daemon each open their own connection on the same
       SQLite file. Without a busy timeout a write that collides with another
       connection's lock (e.g. a favorite/recent write during a daemon scan)
       fails immediately with SQLITE_BUSY. Wait briefly instead. */
    jw__ensure_visibility_table(*out);
    sqlite3_busy_timeout(*out, 2000);
    return 0;
}

/* Additive tables at the current version: no games-schema change and no
   migration/backup cycle, so every released Jawaka still opens the library. */
static int jw__apply_additive(sqlite3 *db) {
    return jw__ensure_apps_min_leaf_version(db) == 0 &&
                   jw__ensure_pakrat_commit_token(db) == 0 &&
                   jw__exec(db, kRelocationSchemaSql) == 0
               ? jw__visibility_table(db)
               : -1;
}

int jw_db_apply_schema(sqlite3 *db) {
    if (!db) {
        return -1;
    }
    int version = 0;
    if (jw__schema_version(db, &version) != 0) {
        return -1;
    }
    if (version == JW_DB_UNRELEASED_VERSION) {
        if (jw__exec(db, "PRAGMA user_version = 6;") != 0) {
            return -1;
        }
        version = JW_DB_SCHEMA_VERSION;
    }
    if (version > JW_DB_SCHEMA_VERSION) {
        return -1;
    }
    if (version == JW_DB_SCHEMA_VERSION) {
        return jw__apply_additive(db);
    }
    if (version > 0 && jw__migrate_to_v6(db) != 0) {
        return -1;
    }
    if (version == 0 && jw__exec(db, "PRAGMA foreign_keys = ON;") != 0) {
        return -1;
    }
    return jw__exec(db, kSchemaSql) == 0 ? jw__apply_additive(db) : -1;
}

void jw_db_close(sqlite3 *db) {
    if (db) {
        sqlite3_close(db);
    }
}

int jw_db_reset_library(sqlite3 *db) {
    if (!db) {
        return -1;
    }
    return jw__exec(db,
        "INSERT INTO games_fts(games_fts) VALUES('rebuild');"
        "INSERT INTO apps_fts(apps_fts) VALUES('rebuild');"
        "DELETE FROM apps; DELETE FROM games;");
}

/* Records a path in the per-scan "seen" temp table so jw_db_scan_prune can
   later remove only the rows whose ROM/pak vanished from disk. The temp table
   is created by jw_db_scan_begin. */
static int jw__mark_seen(sqlite3 *db, const char *seen_sql, const char *path) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, seen_sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

static int jw__mark_game_seen(sqlite3 *db, const char *source_id,
                              const char *rom_relpath) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db,
            "INSERT OR IGNORE INTO _seen_games(source_id,rom_relpath) VALUES(?,?);",
            -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, rom_relpath, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int jw_db_insert_game_stable(sqlite3 *db, const char *system, const char *name,
                             const char *source_id, const char *rom_relpath,
                             const char *rom_path,
                             const char *image_root_kind,
                             const char *image_relpath,
                             const char *image_path) {
    static const char *sql =
        "INSERT INTO games(system,name,source_id,rom_relpath,rom_path,"
        " image_root_kind,image_relpath,image_path) "
        "VALUES(?,?,?,?,?,?,?,?) "
        "ON CONFLICT(source_id,rom_relpath) DO UPDATE SET "
        "system=excluded.system,name=excluded.name,rom_path=excluded.rom_path,"
        "image_root_kind=excluded.image_root_kind,"
        "image_relpath=excluded.image_relpath,image_path=excluded.image_path;";
    sqlite3_stmt *stmt = NULL;

    bool source_id_valid = source_id && source_id[0];
    for (const unsigned char *p = (const unsigned char *)source_id;
         source_id_valid && *p; p++) {
        if (!(isalnum(*p) || *p == '_' || *p == '-')) source_id_valid = false;
    }
    bool have_image_identity =
        image_root_kind && image_root_kind[0] && image_relpath && image_relpath[0];
    bool image_identity_valid =
        !have_image_identity ||
        ((strcmp(image_root_kind, "images") == 0 ||
          strcmp(image_root_kind, "roms") == 0 ||
          strcmp(image_root_kind, "source") == 0) &&
         jw_storage_relative_path_valid(image_relpath));
    if (!db || !system || !name || !source_id_valid ||
        !jw_storage_relative_path_valid(rom_relpath) ||
        !rom_path || !rom_path[0] ||
        ((image_root_kind && image_root_kind[0]) !=
         (image_relpath && image_relpath[0])) ||
        !image_identity_valid) {
        return -1;
    }

    /* A relocation reservation owns both exact locators. The scan must leave
       the stable row untouched until finish releases the reservation. */
    if (jw_db_relocation_key_reserved(db, source_id, rom_relpath)) {
        return 0;
    }

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_text(stmt, 1, system, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, rom_relpath, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, rom_path, -1, SQLITE_TRANSIENT);
    if (image_root_kind && image_root_kind[0])
        sqlite3_bind_text(stmt, 6, image_root_kind, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 6);
    if (image_relpath && image_relpath[0])
        sqlite3_bind_text(stmt, 7, image_relpath, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 7);
    if (image_path && image_path[0])
        sqlite3_bind_text(stmt, 8, image_path, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_null(stmt, 8);

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    if (rc != 0) {
        return -1;
    }

    return jw__mark_game_seen(db, source_id, rom_relpath);
}

int jw_db_insert_game(sqlite3 *db, const char *system, const char *name,
                      const char *rom_path, const char *image_path) {
    if (!rom_path || !rom_path[0]) {
        return -1;
    }
    const char *source_id = "primary";
    const char *rom_relpath = rom_path;
    const char *rom_marker = strstr(rom_path, "/Roms/");
    if (strncmp(rom_path, "Roms/", 5) == 0) {
        rom_relpath = rom_path + 5;
    } else if (rom_marker) {
        source_id = "secondary_sd";
        rom_relpath = rom_marker + 6;
    }
    const char *image_root_kind = NULL;
    const char *image_relpath = NULL;
    if (image_path && strncmp(image_path, "Images/", 7) == 0) {
        image_root_kind = "images";
        image_relpath = image_path + 7;
    } else if (image_path && (rom_marker = strstr(image_path, "/Images/"))) {
        image_root_kind = "images";
        image_relpath = rom_marker + 8;
    } else if (image_path && strncmp(image_path, "Roms/", 5) == 0) {
        image_root_kind = "roms";
        image_relpath = image_path + 5;
    } else if (image_path && (rom_marker = strstr(image_path, "/Roms/"))) {
        image_root_kind = "roms";
        image_relpath = rom_marker + 6;
    }
    return jw_db_insert_game_stable(db, system, name, source_id, rom_relpath,
                                    rom_path, image_root_kind, image_relpath,
                                    image_path);
}

typedef struct {
    int   game_id;
    char *scanned_name;
} jw__imported_title_match;

/* True when a scanned name only repeats the imported title, ignoring
   surrounding whitespace and ASCII case. Such a suffix adds nothing. */
static bool jw__imported_title_repeats(const char *title, const char *scanned) {
    while (isspace((unsigned char)*title)) title++;
    while (isspace((unsigned char)*scanned)) scanned++;
    size_t title_len = strlen(title);
    size_t scanned_len = strlen(scanned);
    while (title_len > 0 && isspace((unsigned char)title[title_len - 1])) title_len--;
    while (scanned_len > 0 && isspace((unsigned char)scanned[scanned_len - 1])) scanned_len--;
    if (title_len != scanned_len) return false;
    for (size_t i = 0; i < title_len; i++) {
        if (tolower((unsigned char)title[i]) != tolower((unsigned char)scanned[i]))
            return false;
    }
    return true;
}

static int jw__upsert_game_setting(sqlite3_stmt *stmt, int game_id,
                                   const char *key, const char *value) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_int(stmt, 1, game_id);
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, value, -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
}

int jw_db_apply_imported_title_groups(sqlite3 *db,
                                      const jw_db_imported_title_group *groups,
                                      int group_count,
                                      jw_db_imported_title_result *out) {
    static const char *lookup_sql =
        "SELECT id, name FROM games WHERE rom_path = ? LIMIT 1;";
    static const char *upsert_sql =
        "INSERT INTO game_settings (game_id, key, value, updated_at) "
        "VALUES (?, ?, ?, strftime('%s','now')) "
        "ON CONFLICT(game_id, key) DO UPDATE SET "
        "value = excluded.value, updated_at = excluded.updated_at;";
    sqlite3_stmt *lookup = NULL;
    sqlite3_stmt *upsert = NULL;
    jw_db_imported_title_result result = {0};
    int status = -1;

    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!db || group_count < 0 || (group_count > 0 && !groups)) {
        return -1;
    }
    if (group_count == 0) {
        return 0;
    }
    if (sqlite3_prepare_v2(db, lookup_sql, -1, &lookup, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, upsert_sql, -1, &upsert, NULL) != SQLITE_OK ||
        sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        goto done;
    }

    for (int i = 0; i < group_count; i++) {
        const jw_db_imported_title_group *group = &groups[i];
        if (!group->provider || !group->provider[0] || !group->title ||
            !group->title[0] || !group->rom_paths || group->rom_path_count <= 0) {
            goto rollback;
        }
        result.groups++;
        result.paths += group->rom_path_count;

        jw__imported_title_match *matches =
            calloc((size_t)group->rom_path_count, sizeof(*matches));
        if (!matches) {
            goto rollback;
        }
        int match_count = 0;
        for (int p = 0; p < group->rom_path_count; p++) {
            const char *rom_path = group->rom_paths[p];
            if (!rom_path || !rom_path[0]) {
                free(matches);
                goto rollback;
            }
            sqlite3_reset(lookup);
            sqlite3_clear_bindings(lookup);
            sqlite3_bind_text(lookup, 1, rom_path, -1, SQLITE_TRANSIENT);
            int rc = sqlite3_step(lookup);
            if (rc == SQLITE_DONE) {
                result.unmatched++;
                continue;
            }
            if (rc != SQLITE_ROW) {
                free(matches);
                goto rollback;
            }
            const unsigned char *name = sqlite3_column_text(lookup, 1);
            matches[match_count].game_id = sqlite3_column_int(lookup, 0);
            matches[match_count].scanned_name = strdup(name ? (const char *)name : "");
            if (!matches[match_count].scanned_name) {
                for (int m = 0; m < match_count; m++) free(matches[m].scanned_name);
                free(matches);
                goto rollback;
            }
            match_count++;
            result.matched++;
        }

        for (int m = 0; m < match_count; m++) {
            const char *imported = group->title;
            char *composed = NULL;
            if (match_count > 1 &&
                !jw__imported_title_repeats(group->title,
                                            matches[m].scanned_name)) {
                size_t needed = strlen(group->title) + strlen(matches[m].scanned_name) + 6u;
                composed = malloc(needed);
                if (!composed) {
                    for (int n = 0; n < match_count; n++) free(matches[n].scanned_name);
                    free(matches);
                    goto rollback;
                }
                snprintf(composed, needed, "%s — %s", group->title,
                         matches[m].scanned_name);
                imported = composed;
            }
            if (jw__upsert_game_setting(upsert, matches[m].game_id,
                                        JW_GAME_SETTING_IMPORTED_DISPLAY_NAME,
                                        imported) != 0 ||
                jw__upsert_game_setting(upsert, matches[m].game_id,
                                        JW_GAME_SETTING_IMPORTED_DISPLAY_NAME_PROVIDER,
                                        group->provider) != 0) {
                free(composed);
                for (int n = 0; n < match_count; n++) free(matches[n].scanned_name);
                free(matches);
                goto rollback;
            }
            free(composed);
            result.applied++;
        }
        for (int m = 0; m < match_count; m++) free(matches[m].scanned_name);
        free(matches);
    }

    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        goto rollback;
    }
    status = 0;
    goto done;

rollback:
    (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    status = -1;

done:
    sqlite3_finalize(lookup);
    sqlite3_finalize(upsert);
    if (status == 0 && out) {
        *out = result;
    }
    return status;
}

int jw_db_insert_app(sqlite3 *db, const char *pak_dir, const char *name,
                     const char *icon, const char *platform,
                     const char *pak_version,
                     const char *min_jawaka_version,
                     const char *min_leaf_version) {
    /* Upsert keyed on the UNIQUE pak_dir, mirroring jw_db_insert_game, so an
       app keeps its id across rescans. */
    static const char *sql =
        "INSERT INTO apps (pak_dir, name, icon, platform, pak_version, "
        "min_jawaka_version, min_leaf_version) "
        "VALUES (?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(pak_dir) DO UPDATE SET "
        "name = excluded.name, icon = excluded.icon, platform = excluded.platform, "
        "pak_version = excluded.pak_version, "
        "min_jawaka_version = excluded.min_jawaka_version, "
        "min_leaf_version = excluded.min_leaf_version;";
    sqlite3_stmt *stmt = NULL;

    if (!db || !pak_dir || !name) {
        return -1;
    }

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_text(stmt, 1, pak_dir, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, name, -1, SQLITE_TRANSIENT);
    if (icon && icon[0]) sqlite3_bind_text(stmt, 3, icon, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 3);
    if (platform && platform[0]) sqlite3_bind_text(stmt, 4, platform, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 4);
    if (pak_version && pak_version[0]) sqlite3_bind_text(stmt, 5, pak_version, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 5);
    if (min_jawaka_version && min_jawaka_version[0]) sqlite3_bind_text(stmt, 6, min_jawaka_version, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 6);
    if (min_leaf_version && min_leaf_version[0]) sqlite3_bind_text(stmt, 7, min_leaf_version, -1, SQLITE_TRANSIENT); else sqlite3_bind_null(stmt, 7);

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    if (rc != 0) {
        return -1;
    }

    return jw__mark_seen(db, "INSERT OR IGNORE INTO _seen_apps (pak_dir) VALUES (?);", pak_dir);
}

int jw_db_scan_begin(sqlite3 *db) {
    if (!db) {
        return -1;
    }
    /* Per-connection temp tables track which ROMs/paks the current scan saw.
       Recreated-then-cleared each scan; dropped implicitly when the daemon
       connection closes. */
    return jw__exec(db,
        "CREATE TEMP TABLE IF NOT EXISTS _seen_games ("
        " source_id TEXT NOT NULL, rom_relpath TEXT NOT NULL,"
        " PRIMARY KEY(source_id,rom_relpath));"
        "CREATE TEMP TABLE IF NOT EXISTS _scanned_game_sources (source_id TEXT PRIMARY KEY);"
        "CREATE TEMP TABLE IF NOT EXISTS _seen_apps (pak_dir TEXT PRIMARY KEY);"
        "CREATE TEMP TABLE IF NOT EXISTS _scan_apps_complete ("
        " ok INTEGER PRIMARY KEY CHECK(ok=1));"
        "DELETE FROM _seen_games;"
        "DELETE FROM _scanned_game_sources;"
        "DELETE FROM _seen_apps;"
        "DELETE FROM _scan_apps_complete;");
}

int jw_db_scan_source_complete(sqlite3 *db, const char *source_id) {
    return (db && source_id && source_id[0])
        ? jw__mark_seen(db,
            "INSERT OR IGNORE INTO _scanned_game_sources(source_id) VALUES(?);",
            source_id)
        : -1;
}

int jw_db_scan_apps_complete(sqlite3 *db) {
    return db
        ? jw__exec(db, "INSERT OR IGNORE INTO _scan_apps_complete(ok) VALUES(1);")
        : -1;
}

int jw_db_dedup_system_aliases(sqlite3 *db, const char *system, const char *canonical_rom_root) {
    /* Drop a legacy-alias-folder copy of a title only when a copy under the
       canonical public folder also exists, so the library shows it once and
       prefers the canonical folder. Before deleting the alias copy, its launcher
       metadata (favorites, recents, per-game settings, playtime, last-played) is
       transferred to the canonical survivor so existing libraries don't lose it.
       Deliberately surgical: copies that share the canonical folder name across
       storage sources (e.g. the same Roms/GBA game on two SD cards) are NOT
       collapsed, and an alias-folder copy with no canonical twin is kept so the
       game still appears. The AFTER DELETE FTS trigger keeps the search index in
       sync; jw_db_scan_prune (run next) sweeps any leftover orphaned rows. */
    static const char *setup_sql =
        "CREATE TEMP TABLE IF NOT EXISTS _dedup_candidates ("
        "    id INTEGER PRIMARY KEY, name TEXT, source_key TEXT, is_canonical INTEGER"
        ");"
        "CREATE TEMP TABLE IF NOT EXISTS _dedup_map (loser INTEGER PRIMARY KEY, winner INTEGER);"
        "DELETE FROM _dedup_candidates;"
        "DELETE FROM _dedup_map;";
    static const char *fill_candidates_sql =
        "INSERT INTO _dedup_candidates(id, name, source_key, is_canonical) "
        "SELECT id, name, source_id, "
        "       CASE WHEN rom_relpath LIKE ?2 ESCAPE '\\' THEN 1 ELSE 0 END "
        "  FROM games WHERE system = ?1 "
        "AND NOT EXISTS (SELECT 1 FROM library_relocation_items ri "
        " JOIN library_relocation_ops ro USING(operation_id) "
        " WHERE ri.game_id=games.id "
        " AND ro.state IN ('prepared','committed','reverted'));";
    static const char *fill_map_sql =
        "INSERT INTO _dedup_map(loser, winner) "
        "SELECT l.id, "
        "       (SELECT w.id FROM _dedup_candidates w "
        "          WHERE w.name = l.name AND w.source_key = l.source_key AND w.is_canonical = 1 "
        "          ORDER BY w.id LIMIT 1) "
        "  FROM _dedup_candidates l "
        " WHERE l.is_canonical = 0 "
        "   AND EXISTS (SELECT 1 FROM _dedup_candidates c "
        "                WHERE c.name = l.name AND c.source_key = l.source_key AND c.is_canonical = 1);";
    /* Transfer metadata to the survivor, then delete the losers. UPDATE OR IGNORE
       keeps the canonical row's own metadata when both rows had it; the leftover
       loser rows are removed by the final DELETE and the prune cascade. */
    static const char *transfer_sql =
        "UPDATE OR IGNORE favorites SET target_id = "
        "    (SELECT winner FROM _dedup_map WHERE loser = favorites.target_id) "
        "  WHERE kind = 'game' AND target_id IN (SELECT loser FROM _dedup_map);"
        "UPDATE OR IGNORE recents SET target_id = "
        "    (SELECT winner FROM _dedup_map WHERE loser = recents.target_id) "
        "  WHERE kind = 'game' AND target_id IN (SELECT loser FROM _dedup_map);"
        "UPDATE OR IGNORE game_settings SET game_id = "
        "    (SELECT winner FROM _dedup_map WHERE loser = game_settings.game_id) "
        "  WHERE game_id IN (SELECT loser FROM _dedup_map);"
        "UPDATE games SET "
        "    playtime_s = playtime_s + COALESCE("
        "        (SELECT SUM(l.playtime_s) FROM games l JOIN _dedup_map m ON l.id = m.loser "
        "          WHERE m.winner = games.id), 0), "
        "    last_played = NULLIF(MAX(COALESCE(last_played, 0), COALESCE("
        "        (SELECT MAX(l.last_played) FROM games l JOIN _dedup_map m ON l.id = m.loser "
        "          WHERE m.winner = games.id), 0)), 0) "
        "  WHERE id IN (SELECT winner FROM _dedup_map);"
        "DELETE FROM games WHERE id IN (SELECT loser FROM _dedup_map);";
    sqlite3_stmt *stmt = NULL;
    char escaped_root[512];
    char pattern[512];
    int rc;

    if (!db || !system || !system[0] || !canonical_rom_root || !canonical_rom_root[0]) {
        return -1;
    }
    /* Escape LIKE wildcards in the ROM-root-relative path so a folder containing '%' or '_' does
       not over-match (paired with ESCAPE '\' on the LIKE clause). Backslash is
       escaped first so it doesn't double-escape the wildcards added after it. */
    const char *canonical_relative =
        strncmp(canonical_rom_root, "Roms/", 5) == 0
            ? canonical_rom_root + 5
            : canonical_rom_root;
    size_t ei = 0;
    for (const char *p = canonical_relative; *p; p++) {
        if (*p == '\\' || *p == '%' || *p == '_') {
            if (ei + 2 >= sizeof(escaped_root)) return -1;
            escaped_root[ei++] = '\\';
        } else if (ei + 1 >= sizeof(escaped_root)) {
            return -1;
        }
        escaped_root[ei++] = *p;
    }
    escaped_root[ei] = '\0';
    if ((size_t)snprintf(pattern, sizeof(pattern), "%s/%%", escaped_root) >= sizeof(pattern)) {
        return -1;
    }
    /* Per-call map of dropped alias rows -> their canonical survivor. The
       source key is the stable logical source id, so aliases never collapse
       across cards even when their materialized mount paths change. */
    if (jw__exec(db, setup_sql) != 0) {
        return -1;
    }
    if (sqlite3_prepare_v2(db, fill_candidates_sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }
    sqlite3_bind_text(stmt, 1, system, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, pattern, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    if (rc != 0) {
        return -1;
    }
    if (jw__exec(db, fill_map_sql) != 0) {
        return -1;
    }
    return jw__exec(db, transfer_sql);
}

static int jw__cleanup_orphans(sqlite3 *db) {
    if (jw__exec(db,
        "DELETE FROM game_settings WHERE game_id NOT IN (SELECT id FROM games);"
        "DELETE FROM favorites WHERE kind='game' AND target_id NOT IN (SELECT id FROM games);"
        "DELETE FROM favorites WHERE kind='app' AND target_id NOT IN (SELECT id FROM apps);"
        "DELETE FROM recents WHERE kind='game' AND target_id NOT IN (SELECT id FROM games);"
        "DELETE FROM recents WHERE kind='app' AND target_id NOT IN (SELECT id FROM apps);") != 0)
        return -1;

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE key=?;",
                          -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, JW_FOCUS_KEY_IDS, -1, SQLITE_STATIC);
    int step = sqlite3_step(stmt);
    int ids[JW_FOCUS_MAX_GAMES], count = 0;
    if (step == SQLITE_ROW)
        jw_focus_ids_parse((const char *)sqlite3_column_text(stmt, 0), ids, &count);
    sqlite3_finalize(stmt);
    if (step != SQLITE_ROW && step != SQLITE_DONE) return -1;
    if (count == 0) return 0;

    if (sqlite3_prepare_v2(db, "SELECT 1 FROM games WHERE id=?;",
                          -1, &stmt, NULL) != SQLITE_OK) return -1;
    int kept = 0;
    for (int i = 0; i < count; ++i) {
        sqlite3_reset(stmt);
        sqlite3_bind_int(stmt, 1, ids[i]);
        step = sqlite3_step(stmt);
        if (step == SQLITE_ROW) ids[kept++] = ids[i];
        else if (step != SQLITE_DONE) {
            sqlite3_finalize(stmt);
            return -1;
        }
    }
    sqlite3_finalize(stmt);
    if (kept == count) return 0;

    char csv[64];
    jw_focus_ids_to_csv(ids, kept, csv, sizeof(csv));
    if (sqlite3_prepare_v2(db, "UPDATE settings SET value=? WHERE key=?;",
                          -1, &stmt, NULL) != SQLITE_OK) return -1;
    sqlite3_bind_text(stmt, 1, csv, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, JW_FOCUS_KEY_IDS, -1, SQLITE_STATIC);
    step = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return step == SQLITE_DONE ? 0 : -1;
}

/* A savepoint also keeps pruning atomic inside a caller's scan transaction. */
static int jw__finish_rom_cleanup(sqlite3 *db, int rc) {
    if (rc == 0 && jw__cleanup_orphans(db) != 0) rc = jw__write_error(db);
    if (rc == 0) {
        if (jw__exec(db, "RELEASE jawaka_rom_cleanup;") == 0) return 0;
        rc = jw__write_error(db);
    }
    sqlite3_exec(db, "ROLLBACK TO jawaka_rom_cleanup; RELEASE jawaka_rom_cleanup;",
                 NULL, NULL, NULL);
    return rc;
}

int jw_db_scan_prune(sqlite3 *db) {
    if (!db) return -1;
    if (jw__exec(db, "SAVEPOINT jawaka_rom_cleanup;") != 0)
        return jw__write_error(db);
    /* FTS triggers remove search rows. Missing-source rows and all visibility
       preferences remain available until the source returns or is forgotten. */
    int rc = jw__exec(db,
        "DELETE FROM games "
        "WHERE source_id IN (SELECT source_id FROM _scanned_game_sources) "
        "AND NOT EXISTS ("
        " SELECT 1 FROM library_relocation_items ri "
        " JOIN library_relocation_ops ro USING(operation_id) "
        " WHERE ro.state IN ('prepared','committed','reverted') "
        " AND (ri.game_id=games.id OR "
        "      (ri.old_source_id=games.source_id AND ri.old_rom_relpath=games.rom_relpath) OR "
        "      (ri.new_source_id=games.source_id AND ri.new_rom_relpath=games.rom_relpath))"
        ") "
        "AND NOT EXISTS (SELECT 1 FROM _seen_games seen "
        " WHERE seen.source_id=games.source_id "
        " AND seen.rom_relpath=games.rom_relpath);"
        "DELETE FROM apps WHERE EXISTS(SELECT 1 FROM _scan_apps_complete) "
        "AND pak_dir NOT IN (SELECT pak_dir FROM _seen_apps);");
    return jw__finish_rom_cleanup(db, rc == 0 ? 0 : jw__write_error(db));
}

int jw_db_reconcile_removed_roms(sqlite3 *db, const jw_db_rom_key *removed,
                                  size_t count) {
    if (!db || (!removed && count)) return -1;
    for (size_t i = 0; i < count; ++i) {
        if (!removed[i].source_id[0] ||
            strnlen(removed[i].source_id, sizeof(removed[i].source_id)) == sizeof(removed[i].source_id) ||
            strnlen(removed[i].rom_relpath, sizeof(removed[i].rom_relpath)) == sizeof(removed[i].rom_relpath) ||
            !jw_storage_relative_path_valid(removed[i].rom_relpath)) return -1;
    }
    if (count == 0) return 0;
    if (jw__exec(db, "SAVEPOINT jawaka_rom_cleanup;") != 0)
        return jw__write_error(db);
    sqlite3_stmt *games = NULL, *hidden = NULL;
    int ok = 0;
    if (sqlite3_prepare_v2(db,
            "DELETE FROM games WHERE source_id=? AND rom_relpath=?;",
            -1, &games, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db,
            "DELETE FROM main.hidden_roms WHERE source_id=? AND rom_relpath=?;",
            -1, &hidden, NULL) != SQLITE_OK) goto done;
    for (size_t i = 0; i < count; ++i) {
        sqlite3_reset(games);
        sqlite3_reset(hidden);
        sqlite3_bind_text(games, 1, removed[i].source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(games, 2, removed[i].rom_relpath, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(hidden, 1, removed[i].source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(hidden, 2, removed[i].rom_relpath, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(games) != SQLITE_DONE || sqlite3_step(hidden) != SQLITE_DONE)
            goto done;
    }
    ok = 1;
done:;
    int rc = ok ? 0 : jw__write_error(db);
    sqlite3_finalize(games);
    sqlite3_finalize(hidden);
    return jw__finish_rom_cleanup(db, rc);
}

static int jw__query_int(sqlite3 *db, const char *sql, int *out) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    int value = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    *out = value;
    return 0;
}

static int jw__query_string(sqlite3 *db, const char *sql, char *out, size_t out_size) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    out[0] = '\0';
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text) {
            snprintf(out, out_size, "%s", text);
        }
    }
    sqlite3_finalize(stmt);
    return 0;
}

int jw_db_read_stats(const char *db_path, jw_library_stats *out) {
    if (!db_path || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    int rc = 0;
    if (jw__query_int(db, "SELECT COUNT(*) FROM games;", &out->game_count) != 0)
        rc = -1;
    if (jw__query_int(db, "SELECT COUNT(*) FROM apps;", &out->app_count) != 0)
        rc = -1;
    if (jw__query_int(db, "SELECT COUNT(*) FROM favorites WHERE kind = 'game';",
                      &out->favorite_count) != 0)
        rc = -1;
    if (jw__query_int(db, "SELECT COUNT(*) FROM games WHERE playtime_s > 0;",
                      &out->games_played) != 0)
        rc = -1;
    if (jw__query_int(db,
        "SELECT COUNT(*) FROM games WHERE image_path IS NOT NULL AND image_path <> '';",
        &out->art_covered) != 0)
        rc = -1;

    sqlite3_stmt *st = NULL;

    /* Totals (64-bit: a unix timestamp overflows int32 near 2038). */
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(SUM(playtime_s), 0), COALESCE(MAX(last_played), 0) FROM games;",
            -1, &st, NULL) == SQLITE_OK) {
        if (sqlite3_step(st) == SQLITE_ROW) {
            out->total_playtime_s = (long long)sqlite3_column_int64(st, 0);
            out->last_played      = (long long)sqlite3_column_int64(st, 1);
        }
        sqlite3_finalize(st);
        st = NULL;
    } else {
        rc = -1;
    }

    /* Most-played games (only those actually played). */
    if (sqlite3_prepare_v2(db,
            "SELECT COALESCE(NULLIF(manual.value, ''), NULLIF(imported.value, ''), g.name), "
            "       g.system, g.playtime_s, COALESCE(g.last_played, 0) "
            "FROM games g "
            "LEFT JOIN game_settings manual "
            "  ON manual.game_id = g.id AND manual.key = 'display_name' "
            "LEFT JOIN game_settings imported "
            "  ON imported.game_id = g.id AND imported.key = 'imported_display_name' "
            "WHERE g.playtime_s > 0 AND " JW_VISIBLE_GAME("g")
            "ORDER BY g.playtime_s DESC, "
            "         COALESCE(NULLIF(manual.value, ''), NULLIF(imported.value, ''), g.name) "
            "LIMIT ?;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, JW_STATS_TOP_MAX);
        while (sqlite3_step(st) == SQLITE_ROW && out->top_count < JW_STATS_TOP_MAX) {
            jw_stat_game *g = &out->top[out->top_count++];
            const unsigned char *nm = sqlite3_column_text(st, 0);
            const unsigned char *sy = sqlite3_column_text(st, 1);
            snprintf(g->name,   sizeof(g->name),   "%s", nm ? (const char *)nm : "");
            snprintf(g->system, sizeof(g->system), "%s", sy ? (const char *)sy : "");
            g->playtime_s  = (long long)sqlite3_column_int64(st, 2);
            g->last_played = (long long)sqlite3_column_int64(st, 3);
        }
        sqlite3_finalize(st);
        st = NULL;
    } else {
        rc = -1;
    }

    /* Per-system breakdown, most-stocked first. The row count doubles as the
       distinct-system count for the Library page. */
    if (sqlite3_prepare_v2(db,
            "SELECT system, COUNT(*), COALESCE(SUM(playtime_s), 0) FROM games "
            "GROUP BY system ORDER BY COUNT(*) DESC, system LIMIT ?;",
            -1, &st, NULL) == SQLITE_OK) {
        sqlite3_bind_int(st, 1, JW_STATS_SYSTEM_MAX);
        while (sqlite3_step(st) == SQLITE_ROW && out->system_count < JW_STATS_SYSTEM_MAX) {
            jw_stat_system *s = &out->systems[out->system_count++];
            const unsigned char *sy = sqlite3_column_text(st, 0);
            snprintf(s->system, sizeof(s->system), "%s", sy ? (const char *)sy : "");
            s->game_count = sqlite3_column_int(st, 1);
            s->playtime_s = (long long)sqlite3_column_int64(st, 2);
        }
        sqlite3_finalize(st);
        st = NULL;
    } else {
        rc = -1;
    }

    jw_db_close(db);
    return rc == 0 ? 0 : -1;
}

int jw_db_list_systems(const char *db_path, jw_system_entry *out, int max_count, int *out_count) {
    if (!db_path || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    static const char *sql =
        "SELECT system, COUNT(*) FROM games WHERE " JW_VISIBLE_GAME("games")
        "GROUP BY system ORDER BY system LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_int(stmt, 1, max_count);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_count < max_count) {
        const unsigned char *name = sqlite3_column_text(stmt, 0);
        int count = sqlite3_column_int(stmt, 1);
        if (name) {
            snprintf(out[*out_count].name, sizeof(out[*out_count].name), "%s", name);
            out[*out_count].game_count = count;
            (*out_count)++;
        }
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}

int jw_db_read_summary(const char *db_path, jw_library_summary *out) {
    if (!db_path || !out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    int rc = 0;
    rc |= jw__query_int(db, "SELECT COUNT(*) FROM games WHERE "
                       JW_VISIBLE_GAME("games") ";", &out->game_count);
    rc |= jw__query_int(db, "SELECT COUNT(*) FROM apps;", &out->app_count);
    rc |= jw__query_int(db, "SELECT COUNT(DISTINCT system) FROM games WHERE "
                       JW_VISIBLE_GAME("games") ";", &out->system_count);
    rc |= jw__query_string(db,
        "SELECT group_concat(system, ', ') FROM ("
        "SELECT DISTINCT system FROM games WHERE " JW_VISIBLE_GAME("games")
        "ORDER BY system LIMIT 4"
        ");",
        out->systems_summary, sizeof(out->systems_summary));
    rc |= jw__query_string(db,
        "SELECT group_concat(name, ', ') FROM ("
        "SELECT COALESCE(NULLIF(manual.value, ''), NULLIF(imported.value, ''), g.name) AS name "
        "FROM games g "
        "LEFT JOIN game_settings manual "
        "  ON manual.game_id = g.id AND manual.key = 'display_name' "
        "LEFT JOIN game_settings imported "
        "  ON imported.game_id = g.id AND imported.key = 'imported_display_name' "
        "WHERE " JW_VISIBLE_GAME("g") "ORDER BY name LIMIT 4"
        ");",
        out->sample_summary, sizeof(out->sample_summary));

    if (!out->systems_summary[0]) {
        snprintf(out->systems_summary, sizeof(out->systems_summary), "%s", "none");
    }
    if (!out->sample_summary[0]) {
        jw__query_string(db,
            "SELECT group_concat(name, ', ') FROM ("
            "SELECT name FROM apps ORDER BY name LIMIT 4"
            ");",
            out->sample_summary, sizeof(out->sample_summary));
    }
    if (!out->sample_summary[0]) {
        snprintf(out->sample_summary, sizeof(out->sample_summary), "%s", "none");
    }

    jw_db_close(db);
    return rc == 0 ? 0 : -1;
}

int jw_db_get_setting(const char *db_path, const char *key,
                       char *out, size_t out_size) {
    if (!db_path || !key || !out || out_size == 0) return -1;
    out[0] = '\0';

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;

    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql = "SELECT value FROM settings WHERE key = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text) snprintf(out, out_size, "%s", text);
    }
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}

int jw_db_read_setting_nowait(const char *db_path, const char *key,
                              char *out, size_t out_size) {
    if (!db_path || !db_path[0] || !key || !out || out_size == 0) return -1;
    out[0] = '\0';

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 0);

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, "SELECT value FROM settings WHERE key = ?;",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        /* A database without a settings table has no value to report. */
        bool no_table = rc == SQLITE_ERROR &&
                        strstr(sqlite3_errmsg(db), "no such table") != NULL;
        sqlite3_close(db);
        return no_table ? 1 : -1;
    }
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    int result;
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        snprintf(out, out_size, "%s", text ? (const char *)text : "");
        result = 0;
    } else {
        result = rc == SQLITE_DONE ? 1 : -1;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return result;
}

int jw_db_get_settings(const char *db_path, jw_db_setting_query *queries,
                       int count) {
    if (!db_path || !queries || count < 0) return -1;

    for (int i = 0; i < count; i++) {
        queries[i].found = 0;
        if (queries[i].out && queries[i].out_size > 0)
            queries[i].out[0] = '\0';
    }
    if (count == 0) return 0;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;

    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql = "SELECT value FROM settings WHERE key = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    /* A multi-key reader expects one coherent snapshot: wrap the SELECT loop
       in a checked read transaction so a concurrent account save cannot split
       the result between the old and new values. */
    if (sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        jw_db_close(db);
        return -1;
    }

    int ok = 1;
    for (int i = 0; i < count; i++) {
        if (!queries[i].key || !queries[i].out || queries[i].out_size == 0) {
            continue;
        }

        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        sqlite3_bind_text(stmt, 1, queries[i].key, -1, SQLITE_TRANSIENT);

        int rc = sqlite3_step(stmt);
        if (rc == SQLITE_ROW) {
            const unsigned char *text = sqlite3_column_text(stmt, 0);
            if (text) {
                snprintf(queries[i].out, queries[i].out_size, "%s", text);
                queries[i].found = 1;
            }
        } else if (rc != SQLITE_DONE) {
            ok = 0;
            break;
        }
    }

    /* COMMIT vs ROLLBACK is immaterial for a pure read; both end it. A failed
       end still means the snapshot was coherent, so it is not an error here. */
    sqlite3_exec(db, ok ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return ok ? 0 : -1;
}

int jw_db_set_game_image(const char *db_path, const char *rom_path,
                         const char *image_path) {
    if (!db_path || !rom_path) return -1;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;

    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql =
        "UPDATE games SET image_path=?,"
        "image_root_kind=CASE "
        " WHEN ? LIKE 'Images/%' OR instr(?,'/Images/')>0 THEN 'images' "
        " WHEN ? LIKE 'Roms/%' OR instr(?,'/Roms/')>0 THEN 'roms' "
        " ELSE NULL END,"
        "image_relpath=CASE "
        " WHEN ? LIKE 'Images/%' THEN substr(?,8) "
        " WHEN instr(?,'/Images/')>0 THEN substr(?,instr(?,'/Images/')+8) "
        " WHEN ? LIKE 'Roms/%' THEN substr(?,6) "
        " WHEN instr(?,'/Roms/')>0 THEN substr(?,instr(?,'/Roms/')+6) "
        " ELSE NULL END WHERE rom_path=?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    if (image_path && image_path[0]) {
        sqlite3_bind_text(stmt, 1, image_path, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 1);
    }
    const char *image = image_path ? image_path : "";
    for (int i = 2; i <= 15; i++) {
        sqlite3_bind_text(stmt, i, image, -1, SQLITE_TRANSIENT);
    }
    sqlite3_bind_text(stmt, 16, rom_path, -1, SQLITE_TRANSIENT);

    int step_rc = sqlite3_step(stmt);
    int rc;
    if (step_rc == SQLITE_DONE) {
        rc = (sqlite3_changes(db) == 0) ? JW_DB_RC_NO_ROW : 0;
    } else if (step_rc == SQLITE_BUSY || step_rc == SQLITE_LOCKED) {
        rc = JW_DB_RC_BUSY; /* transient lock: caller may retry (vs -1 hard failure). */
    } else if ((sqlite3_extended_errcode(db) & 0xff) == SQLITE_READONLY) {
        rc = JW_DB_RC_READONLY;
    } else if ((sqlite3_extended_errcode(db) & 0xff) == SQLITE_IOERR) {
        rc = JW_DB_RC_IOERR;
    } else {
        rc = -1;
    }
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_increment_setting(const char *db_path, const char *key) {
    if (!db_path || !key) return -1;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;

    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql =
        "INSERT INTO settings (key, value) VALUES (?, '1') "
        "ON CONFLICT(key) DO UPDATE SET value = CAST(value AS INTEGER) + 1;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_set_setting(const char *db_path, const char *key, const char *value) {
    if (!db_path || !key || !value) return -1;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;

    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql =
        "INSERT INTO settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, key,   -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_set_settings(const char *db_path, const char *const *keys,
                       const char *const *values, int count) {
    if (!db_path || !keys || !values || count <= 0) return -1;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;

    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql =
        "INSERT INTO settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    if (sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        jw_db_close(db);
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < count; i++) {
        if (!keys[i] || !values[i]) { rc = -1; break; }
        sqlite3_bind_text(stmt, 1, keys[i],   -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, values[i], -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) rc = -1;
        sqlite3_reset(stmt);
        sqlite3_clear_bindings(stmt);
        if (rc != 0) break;
    }
    /* A failed COMMIT (busy/disk-full/ro-flip) leaves nothing durably written,
       so surface it as an error rather than reporting success. */
    if (rc == 0) {
        if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) rc = -1;
    } else {
        sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

/* ------------------------------------------------------------------ */
/* RetroAchievements account (producer of standalone-ra-account-v1)   */
/* ------------------------------------------------------------------ */

static const char *JW_RA_USER_KEY = "retroachievements_user";
static const char *JW_RA_PASS_KEY = "retroachievements_pass";
static const char *JW_RA_REVISION_KEY = "retroachievements_revision";

/* Strict UTF-8: rejects overlong forms, surrogates and values past U+10FFFF.
   A stored credential that is not valid UTF-8 is INVALID, never repaired. */
static bool jw__ra_utf8_valid(const char *s) {
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        unsigned long cp;
        int len;
        if (*p < 0x80) { p++; continue; }
        if ((*p & 0xE0) == 0xC0)      { len = 2; cp = *p & 0x1F; }
        else if ((*p & 0xF0) == 0xE0) { len = 3; cp = *p & 0x0F; }
        else if ((*p & 0xF8) == 0xF0) { len = 4; cp = *p & 0x07; }
        else return false;
        for (int i = 1; i < len; i++) {
            if ((p[i] & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (len == 2 && cp < 0x80) return false;          /* overlong */
        if (len == 3 && cp < 0x800) return false;         /* overlong */
        if (len == 4 && cp < 0x10000) return false;       /* overlong */
        if (cp >= 0xD800 && cp <= 0xDFFF) return false;   /* surrogate */
        if (cp > 0x10FFFF) return false;
        p += len;
    }
    return true;
}

jw_ra_credentials_check jw_ra_credentials_check_values(const char *user,
                                                       const char *pass) {
    if (!user || !pass || !user[0] || !pass[0])
        return JW_RA_CREDENTIALS_INCOMPLETE;
    if (strlen(user) > JW_RA_USERNAME_MAX || strlen(pass) > JW_RA_PASSWORD_MAX)
        return JW_RA_CREDENTIALS_TOO_LONG;
    for (const char *s = user; *s; s++)
        if (*s == '\r' || *s == '\n') return JW_RA_CREDENTIALS_BAD_CHARS;
    for (const char *s = pass; *s; s++)
        if (*s == '\r' || *s == '\n') return JW_RA_CREDENTIALS_BAD_CHARS;
    if (!jw__ra_utf8_valid(user) || !jw__ra_utf8_valid(pass))
        return JW_RA_CREDENTIALS_BAD_CHARS;
    return JW_RA_CREDENTIALS_OK;
}

/* Parse a stored revision: digits only, 1..JW_RA_REVISION_MAX. Returns 0 and
   writes *out on success; -1 when absent, malformed or out of range. */
static int jw__ra_revision_parse(const char *text, long long *out) {
    if (!text || !text[0]) return -1;
    long long v = 0;
    for (const char *p = text; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
        int digit = *p - '0';
        if (v > (JW_RA_REVISION_MAX - digit) / 10) return -1;
        v = v * 10 + digit;
    }
    if (v < 1 || v > JW_RA_REVISION_MAX) return -1;
    *out = v;
    return 0;
}

/* One settings row, read byte-exactly: *text gets the value (truncated to
   text_size, which callers size beyond the contract limits on purpose),
   *bytes gets the true stored length, and the return value is 1 when the row
   exists, 0 when absent, -1 on error. A length mismatch against the C string
   means an embedded NUL. */
static int jw__ra_read_raw(sqlite3_stmt *stmt, const char *key,
                           char *text, size_t text_size, int *bytes) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        text[0] = '\0';
        *bytes = 0;
        return 0;
    }
    if (rc != SQLITE_ROW) {
        text[0] = '\0';
        *bytes = 0;
        return -1;
    }
    const unsigned char *t = sqlite3_column_text(stmt, 0);
    int n = sqlite3_column_bytes(stmt, 0);
    if (!t) { t = (const unsigned char *)""; n = 0; }
    snprintf(text, text_size, "%.*s", (int)text_size - 1, (const char *)t);
    *bytes = n;
    return 1;
}

int jw_db_resolve_ra_account(const char *db_path, jw_ra_account *out) {
    if (!db_path || !out) return -1;
    out->state = JW_RA_ACCOUNT_UNREADABLE;
    out->user[0] = '\0';
    out->pass[0] = '\0';
    out->revision = 0;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql = "SELECT value FROM settings WHERE key = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    /* One checked read transaction: the three keys are one coherent snapshot
       even while another writer saves a new account. */
    if (sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
        sqlite3_finalize(stmt);
        jw_db_close(db);
        return -1;
    }
    /* +8 headroom so embedded NUL / oversize detection works on the copy. */
    char user[JW_RA_USERNAME_MAX + 8];
    char pass[JW_RA_PASSWORD_MAX + 8];
    char rev_text[32];
    int user_bytes = 0, pass_bytes = 0;
    int have_user = jw__ra_read_raw(stmt, JW_RA_USER_KEY, user, sizeof(user), &user_bytes);
    int have_pass = jw__ra_read_raw(stmt, JW_RA_PASS_KEY, pass, sizeof(pass), &pass_bytes);
    int have_rev  = jw__ra_read_raw(stmt, JW_RA_REVISION_KEY, rev_text, sizeof(rev_text), &(int){0});
    int read_ok = (have_user >= 0 && have_pass >= 0 && have_rev >= 0);
    int end_rc = sqlite3_exec(db, read_ok ? "COMMIT" : "ROLLBACK",
                              NULL, NULL, NULL);
    sqlite3_finalize(stmt);
    jw_db_close(db);
    if (!read_ok || end_rc != SQLITE_OK) return -1;

    long long revision = 0;
    int revision_ok = (have_rev == 1) &&
                      (jw__ra_revision_parse(rev_text, &revision) == 0);
    /* A present revision row that cannot be parsed makes any state verdict
       unreliable: malformed is not missing. */
    int revision_bad = (have_rev == 1) && !revision_ok;

    int user_present = (have_user == 1) && user[0];
    int pass_present = (have_pass == 1) && pass[0];

    if ((have_user == 1 && (int)strlen(user) != user_bytes) ||
        (have_pass == 1 && (int)strlen(pass) != pass_bytes)) {
        /* Embedded NUL: the column bytes disagree with the C string. */
        out->state = JW_RA_ACCOUNT_INVALID;
        return 0;
    }
    if (user_present && pass_present) {
        if (user_bytes > JW_RA_USERNAME_MAX || pass_bytes > JW_RA_PASSWORD_MAX ||
            jw_ra_credentials_check_values(user, pass) != JW_RA_CREDENTIALS_OK ||
            revision_bad) {
            out->state = JW_RA_ACCOUNT_INVALID;
            return 0;
        }
        out->state = JW_RA_ACCOUNT_CONFIGURED;
        snprintf(out->user, sizeof(out->user), "%s", user);
        snprintf(out->pass, sizeof(out->pass), "%s", pass);
        out->revision = revision_ok ? revision : 0; /* 0: legacy, init once */
        return 0;
    }
    if (user_present != pass_present) {
        out->state = JW_RA_ACCOUNT_INVALID; /* incomplete pair */
        return 0;
    }
    if (revision_bad) {
        out->state = JW_RA_ACCOUNT_INVALID;
        return 0;
    }
    if (revision_ok) {
        out->state = JW_RA_ACCOUNT_SIGNED_OUT;
        out->revision = revision;
        return 0;
    }
    out->state = JW_RA_ACCOUNT_NEVER_CONFIGURED;
    return 0;
}

/* Read the raw revision inside an already-open checked write transaction.
   Returns 1 with *cur set when a valid stored revision exists, 0 when it is
   absent or malformed (the caller restarts at 1), -1 on read error. */
static int jw__ra_read_current_revision(sqlite3 *db, sqlite3_stmt *stmt,
                                        long long *cur) {
    char rev_text[32];
    int have = jw__ra_read_raw(stmt, JW_RA_REVISION_KEY,
                               rev_text, sizeof(rev_text), &(int){0});
    (void)db;
    if (have < 0) return -1;
    if (have == 1 && jw__ra_revision_parse(rev_text, cur) == 0) return 1;
    return 0;
}

static int jw__ra_upsert(sqlite3_stmt *stmt, const char *key, const char *value) {
    sqlite3_reset(stmt);
    sqlite3_clear_bindings(stmt);
    sqlite3_bind_text(stmt, 1, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value, -1, SQLITE_TRANSIENT);
    return sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
}

/* Shared body of save/clear: BEGIN IMMEDIATE, read the current revision,
   compute the next one (failing on the contractual ceiling instead of
   wrapping), upsert user/pass/revision, COMMIT. Returns 0 with the new
   revision in *revision_out; -1 with nothing committed on any failure. */
static int jw__ra_write_account(const char *db_path, const char *user,
                                const char *pass, long long *revision_out) {
    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *select_sql = "SELECT value FROM settings WHERE key = ?;";
    static const char *upsert_sql =
        "INSERT INTO settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    sqlite3_stmt *select_stmt = NULL;
    sqlite3_stmt *upsert_stmt = NULL;
    if (sqlite3_prepare_v2(db, select_sql, -1, &select_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, upsert_sql, -1, &upsert_stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(select_stmt);
        sqlite3_finalize(upsert_stmt);
        jw_db_close(db);
        return -1;
    }

    int rc = -1;
    /* IMMEDIATE: the read of the old revision and its increment are one
       checked write transaction, so a concurrent save can never double-use
       the same revision. */
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        goto out;
    long long cur = 0;
    if (jw__ra_read_current_revision(db, select_stmt, &cur) < 0)
        goto rollback;
    if (cur >= JW_RA_REVISION_MAX) goto rollback; /* overflow: fail, no wrap */
    long long next = cur + 1;
    char rev_canonical[32];
    snprintf(rev_canonical, sizeof(rev_canonical), "%lld", next);

    if (jw__ra_upsert(upsert_stmt, JW_RA_USER_KEY, user) != 0) goto rollback;
    if (jw__ra_upsert(upsert_stmt, JW_RA_PASS_KEY, pass) != 0) goto rollback;
    if (jw__ra_upsert(upsert_stmt, JW_RA_REVISION_KEY, rev_canonical) != 0)
        goto rollback;
    /* A failed COMMIT (busy/disk-full/ro-flip) leaves nothing durably
       written, so surface it as an error rather than reporting success. */
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
        goto rollback;
    *revision_out = next;
    rc = 0;
    goto out;
rollback:
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
out:
    sqlite3_finalize(select_stmt);
    sqlite3_finalize(upsert_stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_save_ra_account(const char *db_path, const char *user,
                          const char *pass, long long *revision_out) {
    if (!db_path || !revision_out) return -1;
    if (jw_ra_credentials_check_values(user, pass) != JW_RA_CREDENTIALS_OK)
        return -1;
    return jw__ra_write_account(db_path, user, pass, revision_out);
}

int jw_db_clear_ra_account(const char *db_path, long long *revision_out) {
    if (!db_path || !revision_out) return -1;
    return jw__ra_write_account(db_path, "", "", revision_out);
}

int jw_db_ensure_ra_account_revision(const char *db_path,
                                     long long *revision_out) {
    if (!db_path || !revision_out) return -1;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *select_sql = "SELECT value FROM settings WHERE key = ?;";
    static const char *upsert_sql =
        "INSERT INTO settings (key, value) VALUES (?, ?) "
        "ON CONFLICT(key) DO UPDATE SET value = excluded.value;";
    sqlite3_stmt *select_stmt = NULL;
    sqlite3_stmt *upsert_stmt = NULL;
    if (sqlite3_prepare_v2(db, select_sql, -1, &select_stmt, NULL) != SQLITE_OK ||
        sqlite3_prepare_v2(db, upsert_sql, -1, &upsert_stmt, NULL) != SQLITE_OK) {
        sqlite3_finalize(select_stmt);
        sqlite3_finalize(upsert_stmt);
        jw_db_close(db);
        return -1;
    }

    int result = -1;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK)
        goto out;
    {
        char user[JW_RA_USERNAME_MAX + 8];
        char pass[JW_RA_PASSWORD_MAX + 8];
        int have_user = jw__ra_read_raw(select_stmt, JW_RA_USER_KEY,
                                        user, sizeof(user), &(int){0});
        int have_pass = jw__ra_read_raw(select_stmt, JW_RA_PASS_KEY,
                                        pass, sizeof(pass), &(int){0});
        long long cur = 0;
        int rev_state = jw__ra_read_current_revision(db, select_stmt, &cur);
        if (have_user < 0 || have_pass < 0 || rev_state < 0)
            goto rollback;
        if (have_user != 1 || !user[0] || have_pass != 1 || !pass[0]) {
            /* No legacy pair: nothing to initialize. */
            if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                goto rollback;
            result = 1;
            goto out;
        }
        if (rev_state == 1) {
            /* Another writer already initialized it, or it never lacked one. */
            if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                goto rollback;
            *revision_out = cur;
            result = 0;
            goto out;
        }
        char rev_text[32];
        int have_rev = jw__ra_read_raw(select_stmt, JW_RA_REVISION_KEY,
                                       rev_text, sizeof(rev_text), &(int){0});
        if (have_rev < 0) goto rollback;
        if (have_rev == 1) {
            /* Present but malformed: do not guess a repair; the resolver
               reports the pair INVALID until the next explicit save. */
            if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                goto rollback;
            result = 1;
            goto out;
        }
        if (jw__ra_upsert(upsert_stmt, JW_RA_REVISION_KEY, "1") != 0)
            goto rollback;
        if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
            goto rollback;
        *revision_out = 1;
        result = 0;
    }
    goto out;
rollback:
    sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
out:
    sqlite3_finalize(select_stmt);
    sqlite3_finalize(upsert_stmt);
    jw_db_close(db);
    return result;
}

/* Replace whatever *out holds with a credential-free verdict. */
static void jw__ra_account_verdict(jw_ra_account *out,
                                   jw_ra_account_state state) {
    memset(out->user, 0, sizeof(out->user));
    memset(out->pass, 0, sizeof(out->pass));
    out->revision = 0;
    out->state = state;
}

void jw_db_resolve_ra_account_handoff(const char *db_path, jw_ra_account *out) {
    if (!out) return;
    if (jw_db_resolve_ra_account(db_path, out) != 0) {
        jw__ra_account_verdict(out, JW_RA_ACCOUNT_UNREADABLE);
        return;
    }
    if (out->state != JW_RA_ACCOUNT_CONFIGURED || out->revision > 0) return;

    /* A legacy pair: the counter must exist before the first handoff. */
    long long revision = 0;
    int ensured = jw_db_ensure_ra_account_revision(db_path, &revision);
    if (ensured < 0) {
        /* Could not read or write the counter: never guess one. */
        jw__ra_account_verdict(out, JW_RA_ACCOUNT_UNREADABLE);
        return;
    }
    /* A save can commit before or after revision initialization. Never attach
       its counter to credentials from the earlier legacy read: read the whole
       account again, even when initialization succeeded. */
    if (jw_db_resolve_ra_account(db_path, out) != 0) {
        jw__ra_account_verdict(out, JW_RA_ACCOUNT_UNREADABLE);
        return;
    }
    if (out->state == JW_RA_ACCOUNT_CONFIGURED && out->revision <= 0)
        jw__ra_account_verdict(out, JW_RA_ACCOUNT_INVALID);
}

int jw_db_load_rumble_settings(const char *db_path, jw_rumble_settings *out) {
    if (!out) return -1;
    out->ui = 0;
    out->game = 1;
    out->strength = JW_RUMBLE_DEFAULT_STRENGTH;
    if (!db_path) return -1;

    char ui[8], game[8], strength[16], master[8], nav[8];
    jw_db_setting_query q[] = {
        { "rumble_ui",       ui,       sizeof(ui),       0 },
        { "rumble_game",     game,     sizeof(game),     0 },
        { "rumble_strength", strength, sizeof(strength), 0 },
        { "rumble_enabled",  master,   sizeof(master),   0 },
        { "rumble_nav",      nav,      sizeof(nav),      0 },
    };
    if (jw_db_get_settings(db_path, q, (int)(sizeof(q) / sizeof(q[0]))) != 0)
        return -1;

    if (q[2].found && strength[0]) {
        int s = atoi(strength);
        out->strength = s < 0 ? 0 : (s > 100 ? 100 : s);
    }
    int master_off = q[3].found && strcmp(master, "0") == 0;

    if (q[0].found && ui[0]) {
        out->ui = strcmp(ui, "0") != 0;
        out->game = !(q[1].found && strcmp(game, "0") == 0);
        return 0;
    }

    /* First load since the master switch went away. Write the result so the
       old keys are consulted exactly once: a later Game Rumble change must not
       be overridden by a master switch the user can no longer see. */
    out->ui = !master_off && q[4].found && strcmp(nav, "1") == 0;
    out->game = !master_off && !(q[1].found && strcmp(game, "0") == 0);
    const char *keys[] = { "rumble_ui", "rumble_game" };
    const char *values[] = { out->ui ? "1" : "0", out->game ? "1" : "0" };
    (void)jw_db_set_settings(db_path, keys, values, 2);
    return 0;
}

int jw_db_get_theme_name(const char *db_path, char *out, size_t out_size) {
    return jw_db_get_setting(db_path, "theme_name", out, out_size);
}

static int jw__pakrat_commit_token_valid(const char *token) {
    if (!token || !token[0]) {
        return 1;
    }
    if (strlen(token) != 32u) {
        return 0;
    }
    for (size_t i = 0; i < 32u; i++) {
        if (!((token[i] >= '0' && token[i] <= '9') ||
              (token[i] >= 'a' && token[i] <= 'f'))) {
            return 0;
        }
    }
    return 1;
}

int jw_db_pakrat_upsert_install_db(sqlite3 *db, const char *store_id,
                                   const char *version,
                                   const char *platform,
                                   const char *source_id,
                                   const char *install_path,
                                   const char *artifact_sha256,
                                   const char *installed_at,
                                   const char *commit_token) {
    if (!db || !store_id || !store_id[0] || !version || !version[0] ||
        !platform || !platform[0] || !source_id || !source_id[0] ||
        !install_path || !install_path[0] ||
        !artifact_sha256 || !artifact_sha256[0] ||
        !jw__pakrat_commit_token_valid(commit_token)) {
        return -1;
    }
    /* The kind is the install root, which install_path names (pakrat_kind.h):
       deriving it here keeps the two from ever disagreeing. */
    const char *kind =
        strncmp(install_path, "Themes/", 7) == 0 ? "theme" : "app";
    static const char *sql =
        "INSERT INTO pakrat_installs "
        "(store_id, version, platform, source_id, install_path, artifact_sha256, installed_at, commit_token, kind) "
        "VALUES (?, ?, ?, ?, ?, ?, COALESCE(NULLIF(?, ''), strftime('%Y-%m-%dT%H:%M:%SZ','now')), ?, ?) "
        "ON CONFLICT(store_id) DO UPDATE SET "
        "version = excluded.version, "
        "platform = excluded.platform, "
        "source_id = excluded.source_id, "
        "install_path = excluded.install_path, "
        "artifact_sha256 = excluded.artifact_sha256, "
        "installed_at = excluded.installed_at, "
        "commit_token = excluded.commit_token, "
        "kind = excluded.kind;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    sqlite3_bind_text(stmt, 1, store_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, version, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, platform, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 5, install_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 6, artifact_sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 7, installed_at ? installed_at : "", -1, SQLITE_TRANSIENT);
    if (commit_token && commit_token[0]) {
        sqlite3_bind_text(stmt, 8, commit_token, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_null(stmt, 8);
    }
    sqlite3_bind_text(stmt, 9, kind, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int jw_db_pakrat_upsert_install(const char *db_path, const char *store_id,
                                const char *version, const char *platform,
                                const char *install_path,
                                const char *artifact_sha256,
                                const char *installed_at,
                                const char *commit_token) {
    if (!db_path) {
        return -1;
    }
    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }
    int rc = jw_db_pakrat_upsert_install_db(
        db, store_id, version, platform, "primary", install_path, artifact_sha256,
        installed_at, commit_token);
    jw_db_close(db);
    return rc;
}

int jw_db_pakrat_remove_install(const char *db_path, const char *store_id) {
    if (!db_path || !store_id || !store_id[0]) {
        return -1;
    }

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, "DELETE FROM pakrat_installs WHERE store_id = ?;",
                          -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, store_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

static void jw__pakrat_copy_column(sqlite3_stmt *stmt, int column,
                                   char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    const unsigned char *text = sqlite3_column_text(stmt, column);
    if (text) {
        snprintf(out, out_size, "%s", text);
    }
}

static void jw__pakrat_fill_install(sqlite3_stmt *stmt, jw_pakrat_install *out) {
    jw__pakrat_copy_column(stmt, 0, out->store_id, sizeof(out->store_id));
    jw__pakrat_copy_column(stmt, 1, out->version, sizeof(out->version));
    jw__pakrat_copy_column(stmt, 2, out->platform, sizeof(out->platform));
    jw__pakrat_copy_column(stmt, 3, out->source_id, sizeof(out->source_id));
    jw__pakrat_copy_column(stmt, 4, out->install_path, sizeof(out->install_path));
    jw__pakrat_copy_column(stmt, 5, out->artifact_sha256, sizeof(out->artifact_sha256));
    jw__pakrat_copy_column(stmt, 6, out->installed_at, sizeof(out->installed_at));
    jw__pakrat_copy_column(stmt, 7, out->commit_token, sizeof(out->commit_token));
    out->app_present = sqlite3_column_int(stmt, 8);
    jw__pakrat_copy_column(stmt, 9, out->app_name, sizeof(out->app_name));
    jw__pakrat_copy_column(stmt, 10, out->app_pak_dir, sizeof(out->app_pak_dir));
    jw__pakrat_copy_column(stmt, 11, out->kind, sizeof(out->kind));
}

static const char *kPakratInstallJoinSql =
    "SELECT p.store_id, p.version, p.platform, p.source_id, p.install_path, "
    "p.artifact_sha256, p.installed_at, p.commit_token, "
    "CASE WHEN a.id IS NULL THEN 0 ELSE 1 END AS app_present, "
    "COALESCE(a.name, ''), COALESCE(a.pak_dir, ''), p.kind "
    "FROM pakrat_installs p "
    "LEFT JOIN apps a ON a.pak_dir = p.install_path "
    "    OR a.pak_dir = ('Apps/' || p.install_path) ";

int jw_db_pakrat_get_install(const char *db_path, const char *store_id,
                             jw_pakrat_install *out) {
    if (!db_path || !store_id || !store_id[0] || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    char sql[1024];
    snprintf(sql, sizeof(sql), "%s WHERE p.store_id = ? LIMIT 1;", kPakratInstallJoinSql);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, store_id, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        jw__pakrat_fill_install(stmt, out);
        rc = 0;
    } else {
        rc = rc == SQLITE_DONE ? 1 : -1;
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_pakrat_list_installs(const char *db_path, jw_pakrat_install *out,
                               int max_count, int *out_count) {
    if (!db_path || !out || max_count <= 0 || !out_count) {
        return -1;
    }
    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    char sql[1024];
    snprintf(sql, sizeof(sql), "%s ORDER BY COALESCE(a.name, p.store_id) LIMIT ?;",
             kPakratInstallJoinSql);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_int(stmt, 1, max_count);
    int step_rc = SQLITE_ROW;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW && *out_count < max_count) {
        jw__pakrat_fill_install(stmt, &out[*out_count]);
        (*out_count)++;
    }

    int rc = (step_rc == SQLITE_DONE || *out_count >= max_count) ? 0 : -1;
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_list_apps(const char *db_path, jw_app_entry *out, int max_count, int *out_count) {
    if (!db_path || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql =
        "SELECT name,pak_dir,icon,platform,pak_version,"
        "min_jawaka_version,min_leaf_version "
        "FROM apps ORDER BY name LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_int(stmt, 1, max_count);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_count < max_count) {
        const unsigned char *name = sqlite3_column_text(stmt, 0);
        const unsigned char *pak_dir = sqlite3_column_text(stmt, 1);
        const unsigned char *icon = sqlite3_column_text(stmt, 2);
        const unsigned char *platform = sqlite3_column_text(stmt, 3);
        const unsigned char *pak_version = sqlite3_column_text(stmt, 4);
        const unsigned char *min_jawaka_version =
            sqlite3_column_text(stmt, 5);
        const unsigned char *min_leaf_version =
            sqlite3_column_text(stmt, 6);
        int i = *out_count;
        if (name) snprintf(out[i].name, sizeof(out[i].name), "%s", name);
        if (pak_dir) snprintf(out[i].pak_dir, sizeof(out[i].pak_dir), "%s", pak_dir);
        if (icon) snprintf(out[i].icon, sizeof(out[i].icon), "%s", icon);
        if (platform) {
            snprintf(out[i].platform, sizeof(out[i].platform), "%s",
                     platform);
        }
        if (pak_version) {
            snprintf(out[i].pak_version, sizeof(out[i].pak_version), "%s",
                     pak_version);
        }
        if (min_jawaka_version) {
            snprintf(out[i].min_jawaka_version,
                     sizeof(out[i].min_jawaka_version), "%s",
                     min_jawaka_version);
        }
        if (min_leaf_version) {
            snprintf(out[i].min_leaf_version,
                     sizeof(out[i].min_leaf_version), "%s",
                     min_leaf_version);
        }
        (*out_count)++;
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}

int jw_db_count_games_for_system(const char *db_path, const char *system, int *out_count) {
    if (!db_path || !system || !out_count) {
        return -1;
    }

    *out_count = 0;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql = "SELECT COUNT(*) FROM games WHERE system = ? AND "
        JW_VISIBLE_GAME("games") ";";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, system, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        *out_count = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc == SQLITE_ROW ? 0 : -1;
}

int jw_db_list_games_for_system(const char *db_path, const char *system,
                                jw_game_entry *out, int max_count, int *out_count) {
    if (!db_path || !system || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql =
        "SELECT g.id, g.system, COALESCE(NULLIF(gs.value, ''), NULLIF(ig.value, ''), g.name), "
        "g.source_id,g.rom_relpath,g.rom_path,COALESCE(g.image_root_kind,''),"
        "COALESCE(g.image_relpath,''),COALESCE(g.image_path,''), "
        "EXISTS(SELECT 1 FROM favorites f WHERE f.kind = 'game' AND f.target_id = g.id), "
        "COALESCE(g.last_played,0), g.playtime_s "
        "FROM games g "
        "LEFT JOIN game_settings gs ON gs.game_id = g.id AND gs.key = 'display_name' "
        "LEFT JOIN game_settings ig ON ig.game_id = g.id AND ig.key = 'imported_display_name' "
        "WHERE g.system = ? AND " JW_VISIBLE_GAME("g")
        "ORDER BY COALESCE(NULLIF(gs.value, ''), NULLIF(ig.value, ''), g.name) LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_text(stmt, 1, system, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, max_count);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_count < max_count) {
        jw__fill_game_entry(stmt, &out[*out_count]);
        (*out_count)++;
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}


/* The "ss." prefix keeps scraped facts distinct from settings a user sets, so a
   rescrape can replace them without touching display_name or a core override. */
static const struct { const char *key; size_t off; size_t len; } kGameMetaFields[] = {
    { "ss.genre",     offsetof(jw_game_meta, genre),     sizeof(((jw_game_meta *)0)->genre) },
    { "ss.developer", offsetof(jw_game_meta, developer), sizeof(((jw_game_meta *)0)->developer) },
    { "ss.publisher", offsetof(jw_game_meta, publisher), sizeof(((jw_game_meta *)0)->publisher) },
    { "ss.players",   offsetof(jw_game_meta, players),   sizeof(((jw_game_meta *)0)->players) },
    { "ss.rating",    offsetof(jw_game_meta, rating),    sizeof(((jw_game_meta *)0)->rating) },
    { "ss.year",      offsetof(jw_game_meta, year),      sizeof(((jw_game_meta *)0)->year) },
    { "ss.synopsis",  offsetof(jw_game_meta, synopsis),  sizeof(((jw_game_meta *)0)->synopsis) },
};

int jw_db_set_game_meta(const char *db_path, const char *rom_path,
                        const jw_game_meta *meta) {
    if (!db_path || !rom_path || !meta) return -1;
    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;
    if (jw_db_apply_schema(db) != 0) { jw_db_close(db); return -1; }

    static const char *sql =
        "INSERT INTO game_settings (game_id, key, value, updated_at) "
        "SELECT id, ?2, ?3, strftime('%s','now') FROM games WHERE rom_path = ?1 "
        "ON CONFLICT(game_id, key) DO UPDATE SET value = ?3, "
        "  updated_at = strftime('%s','now');";

    int wrote = 0;
    for (size_t i = 0; i < sizeof(kGameMetaFields) / sizeof(kGameMetaFields[0]); i++) {
        const char *val = (const char *)meta + kGameMetaFields[i].off;
        if (!val[0]) continue;                 /* absent: leave what is stored */
        sqlite3_stmt *st = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) continue;
        sqlite3_bind_text(st, 1, rom_path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, kGameMetaFields[i].key, -1, SQLITE_STATIC);
        sqlite3_bind_text(st, 3, val, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_DONE) wrote++;
        sqlite3_finalize(st);
    }
    jw_db_close(db);
    return wrote > 0 ? 0 : -1;
}

int jw_db_get_game_meta(const char *db_path, int game_id, jw_game_meta *out) {
    if (!db_path || !out || game_id <= 0) return -1;
    memset(out, 0, sizeof(*out));
    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) return -1;

    static const char *sql =
        "SELECT key, value FROM game_settings WHERE game_id = ? AND key LIKE 'ss.%';";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }
    sqlite3_bind_int(st, 1, game_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        const char *k = (const char *)sqlite3_column_text(st, 0);
        const char *v = (const char *)sqlite3_column_text(st, 1);
        if (!k || !v) continue;
        for (size_t i = 0; i < sizeof(kGameMetaFields) / sizeof(kGameMetaFields[0]); i++) {
            if (strcmp(k, kGameMetaFields[i].key) != 0) continue;
            char *dst = (char *)out + kGameMetaFields[i].off;
            snprintf(dst, kGameMetaFields[i].len, "%s", v);
            break;
        }
    }
    sqlite3_finalize(st);
    jw_db_close(db);
    return 0;
}

int jw_db_set_favorite(const char *db_path, const char *kind, int target_id, int on) {
    if (!db_path || !kind || target_id <= 0) {
        return -1;
    }

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    static const char *add_sql =
        "INSERT OR IGNORE INTO favorites (kind, target_id, added_at) "
        "VALUES (?, ?, strftime('%s','now'));";
    static const char *del_sql =
        "DELETE FROM favorites WHERE kind = ? AND target_id = ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, on ? add_sql : del_sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, target_id);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_remove_recent(const char *db_path, const char *kind, int target_id) {
    if (!db_path || !kind || target_id <= 0) {
        return -1;
    }

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    static const char *del_sql =
        "DELETE FROM recents WHERE kind = ? AND target_id = ?;";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, del_sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, target_id);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

static void jw__fill_game_entry(sqlite3_stmt *stmt, jw_game_entry *out) {
    memset(out, 0, sizeof(*out));
    out->id = sqlite3_column_int(stmt, 0);
    jw__pakrat_copy_column(stmt, 1, out->system, sizeof(out->system));
    jw__pakrat_copy_column(stmt, 2, out->name, sizeof(out->name));
    jw__pakrat_copy_column(stmt, 3, out->source_id, sizeof(out->source_id));
    jw__pakrat_copy_column(stmt, 4, out->rom_relpath, sizeof(out->rom_relpath));
    jw__pakrat_copy_column(stmt, 5, out->rom_path, sizeof(out->rom_path));
    jw__pakrat_copy_column(stmt, 6, out->image_root_kind,
                           sizeof(out->image_root_kind));
    jw__pakrat_copy_column(stmt, 7, out->image_relpath,
                           sizeof(out->image_relpath));
    jw__pakrat_copy_column(stmt, 8, out->image_path, sizeof(out->image_path));
    out->favorite = sqlite3_column_int(stmt, 9);
    /* Appended columns: a caller whose SELECT predates them still fills
       everything above, and these stay zero. */
    if (sqlite3_column_count(stmt) > 11) {
        out->last_played = sqlite3_column_int64(stmt, 10);
        out->playtime_s  = sqlite3_column_int(stmt, 11);
    }
}

static int jw__write_error(sqlite3 *db) {
    int rc = sqlite3_extended_errcode(db) & 0xff;
    if (rc == SQLITE_READONLY) return JW_DB_RC_READONLY;
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) return JW_DB_RC_BUSY;
    if (rc == SQLITE_IOERR) return JW_DB_RC_IOERR;
    return -1;
}

static int jw__write_visibility(const char *db_path, const char *sql,
                                int game_id, const char *source_id,
                                const char *rom_relpath, const char *member,
                                int require_row) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0) goto done;
    /* A read-only library may only have the temporary stand-in table. */
    if (sqlite3_db_readonly(db, "main") == 1) { rc = JW_DB_RC_READONLY; goto done; }
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) goto done;
    if (source_id) {
        sqlite3_bind_text(stmt, 1, source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, rom_relpath, -1, SQLITE_TRANSIENT);
        if (member) sqlite3_bind_text(stmt, 3, member, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_int(stmt, 1, game_id);
    }
    if (sqlite3_step(stmt) == SQLITE_DONE) {
        rc = require_row && sqlite3_changes(db) == 0 ? JW_DB_RC_NO_ROW : 0;
    }
done:
    if (rc == -1 && db) rc = jw__write_error(db);
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_set_game_hidden(const char *db_path, int game_id, int hidden) {
    if (!db_path || game_id <= 0) return -1;
    const char *sql = hidden
        ? "INSERT INTO main.hidden_roms(source_id,rom_relpath,member) "
          "SELECT source_id,rom_relpath,'' FROM games WHERE id=? "
          "ON CONFLICT(source_id,rom_relpath,member) DO UPDATE SET member='';"
        : "DELETE FROM main.hidden_roms WHERE member='' AND (source_id,rom_relpath) "
          "IN (SELECT source_id,rom_relpath FROM games WHERE id=?);";
    return jw__write_visibility(db_path, sql, game_id, NULL, NULL, NULL, hidden != 0);
}

int jw_db_clear_hidden_game(const char *db_path, const char *source_id,
                             const char *rom_relpath) {
    if (!db_path || !source_id || !source_id[0] ||
        !rom_relpath || !rom_relpath[0]) return -1;
    return jw__write_visibility(db_path,
        "DELETE FROM main.hidden_roms WHERE source_id=? AND rom_relpath=? AND member='';",
        0, source_id, rom_relpath, NULL, 0);
}

static int jw__valid_visibility_key(const char *source_id, const char *rom_relpath,
                                     const char *member) {
    return source_id && source_id[0] &&
        strlen(source_id) < sizeof(((jw_game_entry *)0)->source_id) &&
        rom_relpath && rom_relpath[0] &&
        strlen(rom_relpath) < sizeof(((jw_game_entry *)0)->rom_relpath) &&
        member && strlen(member) < sizeof(((jw_hidden_rom_entry *)0)->member);
}

int jw_db_set_rom_hidden(const char *db_path, const char *source_id,
                          const char *rom_relpath, const char *member, int hidden) {
    if (!db_path || !jw__valid_visibility_key(source_id, rom_relpath, member)) return -1;
    const char *sql = hidden
        ? "INSERT INTO main.hidden_roms(source_id,rom_relpath,member) VALUES(?,?,?) "
          "ON CONFLICT(source_id,rom_relpath,member) DO NOTHING;"
        : "DELETE FROM main.hidden_roms WHERE source_id=? AND rom_relpath=? AND member=?;";
    return jw__write_visibility(db_path, sql, 0, source_id, rom_relpath, member, 0);
}

int jw_db_is_rom_hidden(const char *db_path, const char *source_id,
                         const char *rom_relpath, const char *member, int *out_hidden) {
    if (!db_path || !out_hidden ||
        !jw__valid_visibility_key(source_id, rom_relpath, member)) return -1;
    *out_hidden = 0;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        sqlite3_prepare_v2(db, "SELECT EXISTS(SELECT 1 FROM hidden_roms "
            "WHERE source_id=? AND rom_relpath=? AND member=?);",
            -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_text(stmt, 1, source_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, rom_relpath, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, member, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out_hidden = sqlite3_column_int(stmt, 0);
        rc = 0;
    }
done:
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_is_game_hidden(const char *db_path, int game_id, int *out_hidden) {
    if (!db_path || game_id <= 0 || !out_hidden) return -1;
    *out_hidden = 0;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        sqlite3_prepare_v2(db, "SELECT NOT " JW_VISIBLE_GAME("g")
            "FROM games g WHERE id=?;", -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_int(stmt, 1, game_id);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        *out_hidden = sqlite3_column_int(stmt, 0);
        rc = 0;
    }
done:
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

static int jw__count_visibility_rows(const char *db_path, const char *sql,
                                      int *out_count) {
    if (!db_path || !out_count) return -1;
    *out_count = 0;
    sqlite3 *db = NULL;
    int rc = -1;
    if (jw_db_open(db_path, &db) == 0 && jw_db_apply_schema(db) == 0) {
        rc = jw__query_int(db, sql, out_count);
    }
    jw_db_close(db);
    return rc;
}

int jw_db_count_hidden_games(const char *db_path, int *out_count) {
    return jw__count_visibility_rows(db_path,
        "SELECT COUNT(*) FROM hidden_roms WHERE member='';", out_count);
}

int jw_db_count_hidden_roms(const char *db_path, int *out_count) {
    return jw__count_visibility_rows(db_path,
        "SELECT COUNT(*) FROM hidden_roms;", out_count);
}

static int jw__list_hidden_roms(const char *db_path, jw_game_entry *games,
                                jw_hidden_rom_entry *roms, int max_count,
                                int *out_count) {
    if (!db_path || (!games && !roms) || max_count <= 0 || !out_count) return -1;
    *out_count = 0;
    if (games) memset(games, 0, sizeof(games[0]) * (size_t)max_count);
    if (roms) memset(roms, 0, sizeof(roms[0]) * (size_t)max_count);
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    const char *sql =
        "SELECT COALESCE(g.id,0),COALESCE(g.system,''),"
        "COALESCE(NULLIF(gs.value,''),NULLIF(ig.value,''),g.name,h.rom_relpath),"
        "h.source_id,h.rom_relpath,COALESCE(g.rom_path,''),"
        "COALESCE(g.image_root_kind,''),COALESCE(g.image_relpath,''),"
        "COALESCE(g.image_path,''),"
        "EXISTS(SELECT 1 FROM favorites f WHERE f.kind='game' AND f.target_id=g.id),"
        "COALESCE(g.last_played,0),COALESCE(g.playtime_s,0),h.member "
        "FROM hidden_roms h LEFT JOIN games g "
        "ON g.source_id=h.source_id AND g.rom_relpath=h.rom_relpath "
        "LEFT JOIN game_settings gs ON gs.game_id=g.id AND gs.key='display_name' "
        "LEFT JOIN game_settings ig ON ig.game_id=g.id AND ig.key='imported_display_name' "
        "WHERE (?1 OR h.member='') "
        "ORDER BY 3 COLLATE NOCASE,h.source_id,h.rom_relpath,h.member LIMIT ?2;";
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) goto done;
    sqlite3_bind_int(stmt, 1, roms != NULL);
    sqlite3_bind_int(stmt, 2, max_count);
    int step_rc;
    while ((step_rc = sqlite3_step(stmt)) == SQLITE_ROW && *out_count < max_count) {
        int i = (*out_count)++;
        jw__fill_game_entry(stmt, games ? &games[i] : &roms[i].game);
        if (roms) jw__pakrat_copy_column(stmt, 12, roms[i].member, sizeof(roms[i].member));
    }
    rc = step_rc == SQLITE_DONE ? 0 : -1;
done:
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_list_hidden_games(const char *db_path, jw_game_entry *out,
                             int max_count, int *out_count) {
    return jw__list_hidden_roms(db_path, out, NULL, max_count, out_count);
}

int jw_db_list_hidden_roms(const char *db_path, jw_hidden_rom_entry *out,
                            int max_count, int *out_count) {
    return jw__list_hidden_roms(db_path, NULL, out, max_count, out_count);
}

int jw_db_list_indexed_games(const char *db_path, jw_game_entry **out,
                                size_t *out_count) {
    if (!out || !out_count) return -1;
    *out = NULL;
    *out_count = 0;
    if (!db_path) return -1;
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    jw_game_entry *games = NULL;
    size_t count = 0, capacity = 0;
    int rc = -1;
    const char *sql =
        "SELECT g.id,g.system,COALESCE(NULLIF(gs.value,''),NULLIF(ig.value,''),g.name),"
        "g.source_id,g.rom_relpath,g.rom_path,COALESCE(g.image_root_kind,''),"
        "COALESCE(g.image_relpath,''),COALESCE(g.image_path,''),"
        "EXISTS(SELECT 1 FROM favorites f WHERE f.kind='game' AND f.target_id=g.id),"
        "COALESCE(g.last_played,0),g.playtime_s FROM games g "
        "LEFT JOIN game_settings gs ON gs.game_id=g.id AND gs.key='display_name' "
        "LEFT JOIN game_settings ig ON ig.game_id=g.id AND ig.key='imported_display_name' "
        "ORDER BY g.id;";
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) goto done;
    int step;
    while ((step = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count == capacity) {
            size_t next_capacity = capacity ? capacity * 2 : 64;
            if (next_capacity < capacity ||
                next_capacity > (size_t)-1 / sizeof(*games)) goto done;
            jw_game_entry *next = realloc(games, next_capacity * sizeof(*games));
            if (!next) goto done;
            games = next;
            capacity = next_capacity;
        }
        jw__fill_game_entry(stmt, &games[count++]);
    }
    if (step != SQLITE_DONE) goto done;
    *out = games;
    *out_count = count;
    games = NULL;
    rc = 0;
done:
    free(games);
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

/* where_sql binds `rom_path` (or `game_id`) as ?1 and, when given, `text2` as ?2. */
static int jw__get_game(const char *db_path, const char *where_sql,
                        const char *rom_path, const char *text2, int game_id,
                        jw_game_entry *out) {
    if (!db_path || !where_sql || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    const char *select_sql =
        "SELECT g.id, g.system, COALESCE(NULLIF(gs.value, ''), NULLIF(ig.value, ''), g.name), "
        "g.source_id,g.rom_relpath,g.rom_path,COALESCE(g.image_root_kind,''),"
        "COALESCE(g.image_relpath,''),COALESCE(g.image_path,''), "
        "EXISTS(SELECT 1 FROM favorites f WHERE f.kind = 'game' AND f.target_id = g.id), "
        "COALESCE(g.last_played,0), g.playtime_s "
        "FROM games g "
        "LEFT JOIN game_settings gs ON gs.game_id = g.id AND gs.key = 'display_name' "
        "LEFT JOIN game_settings ig ON ig.game_id = g.id AND ig.key = 'imported_display_name' ";
    char sql[1024];
    if (snprintf(sql, sizeof(sql), "%s%s LIMIT 1;", select_sql, where_sql) >=
        (int)sizeof(sql)) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    if (rom_path) sqlite3_bind_text(stmt, 1, rom_path, -1, SQLITE_TRANSIENT);
    else sqlite3_bind_int(stmt, 1, game_id);
    if (text2) sqlite3_bind_text(stmt, 2, text2, -1, SQLITE_TRANSIENT);

    int rc = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        jw__fill_game_entry(stmt, out);
        rc = 0;
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_get_game_by_rom_path(const char *db_path, const char *rom_path,
                               jw_game_entry *out) {
    return (rom_path && rom_path[0])
        ? jw__get_game(db_path, "WHERE g.rom_path = ?", rom_path, NULL, 0, out)
        : -1;
}

int jw_db_get_game_by_source_relpath(const char *db_path, const char *source_id,
                                     const char *rom_relpath, jw_game_entry *out) {
    return (source_id && source_id[0] && rom_relpath && rom_relpath[0])
        ? jw__get_game(db_path, "WHERE g.source_id = ?1 AND g.rom_relpath = ?2",
                       source_id, rom_relpath, 0, out)
        : -1;
}

int jw_db_get_game_by_id(const char *db_path, int game_id, jw_game_entry *out) {
    return game_id > 0
        ? jw__get_game(db_path, "WHERE g.id = ?", NULL, NULL, game_id, out)
        : -1;
}

static int jw__get_scoped_setting(const char *db_path, const char *sql,
                                  const char *scope, int game_id,
                                  const char *key, char *out, size_t out_size) {
    if (!db_path || !sql || !key || !out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }
    if (scope) {
        sqlite3_bind_text(stmt, 1, scope, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_int(stmt, 1, game_id);
    }
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text) snprintf(out, out_size, "%s", text);
        rc = SQLITE_DONE;
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc == SQLITE_DONE ? 0 : -1;
}

static int jw__set_scoped_setting(const char *db_path, const char *sql,
                                  const char *scope, int game_id,
                                  const char *key, const char *value) {
    if (!db_path || !sql || !key || !value) {
        return -1;
    }

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }
    if (scope) {
        sqlite3_bind_text(stmt, 1, scope, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_int(stmt, 1, game_id);
    }
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, value, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

static int jw__delete_scoped_setting(const char *db_path, const char *sql,
                                     const char *scope, int game_id,
                                     const char *key) {
    if (!db_path || !sql || !key) {
        return -1;
    }

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }
    if (scope) {
        sqlite3_bind_text(stmt, 1, scope, -1, SQLITE_TRANSIENT);
    } else {
        sqlite3_bind_int(stmt, 1, game_id);
    }
    sqlite3_bind_text(stmt, 2, key, -1, SQLITE_TRANSIENT);

    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw_db_get_game_setting(const char *db_path, int game_id,
                           const char *key, char *out, size_t out_size) {
    if (game_id <= 0) {
        return -1;
    }
    return jw__get_scoped_setting(db_path,
        "SELECT value FROM game_settings WHERE game_id = ? AND key = ?;",
        NULL, game_id, key, out, out_size);
}

int jw_db_set_game_setting(const char *db_path, int game_id,
                           const char *key, const char *value) {
    if (game_id <= 0 || !value || !value[0]) {
        return -1;
    }
    return jw__set_scoped_setting(db_path,
        "INSERT INTO game_settings (game_id, key, value, updated_at) "
        "VALUES (?, ?, ?, strftime('%s','now')) "
        "ON CONFLICT(game_id, key) DO UPDATE SET "
        "value = excluded.value, updated_at = excluded.updated_at;",
        NULL, game_id, key, value);
}

int jw_db_delete_game_setting(const char *db_path, int game_id,
                              const char *key) {
    if (game_id <= 0) {
        return -1;
    }
    return jw__delete_scoped_setting(db_path,
        "DELETE FROM game_settings WHERE game_id = ? AND key = ?;",
        NULL, game_id, key);
}

int jw_db_get_system_setting(const char *db_path, const char *system,
                             const char *key, char *out, size_t out_size) {
    if (!system || !system[0]) {
        return -1;
    }
    return jw__get_scoped_setting(db_path,
        "SELECT value FROM system_settings WHERE system = ? AND key = ?;",
        system, 0, key, out, out_size);
}

int jw_db_set_system_setting(const char *db_path, const char *system,
                             const char *key, const char *value) {
    if (!system || !system[0] || !value || !value[0]) {
        return -1;
    }
    return jw__set_scoped_setting(db_path,
        "INSERT INTO system_settings (system, key, value, updated_at) "
        "VALUES (?, ?, ?, strftime('%s','now')) "
        "ON CONFLICT(system, key) DO UPDATE SET "
        "value = excluded.value, updated_at = excluded.updated_at;",
        system, 0, key, value);
}

int jw_db_delete_system_setting(const char *db_path, const char *system,
                                const char *key) {
    if (!system || !system[0]) {
        return -1;
    }
    return jw__delete_scoped_setting(db_path,
        "DELETE FROM system_settings WHERE system = ? AND key = ?;",
        system, 0, key);
}

int jw_db_list_favorite_games(const char *db_path, jw_game_entry *out,
                              int max_count, int *out_count) {
    if (!db_path || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    static const char *sql =
        "SELECT g.id, g.system, COALESCE(NULLIF(gs.value, ''), NULLIF(ig.value, ''), g.name), "
        "g.source_id,g.rom_relpath,g.rom_path,COALESCE(g.image_root_kind,''),"
        "COALESCE(g.image_relpath,''),COALESCE(g.image_path,''),1, "
        "COALESCE(g.last_played,0), g.playtime_s "
        "FROM games g JOIN favorites f ON f.kind = 'game' AND f.target_id = g.id "
        "LEFT JOIN game_settings gs ON gs.game_id = g.id AND gs.key = 'display_name' "
        "LEFT JOIN game_settings ig ON ig.game_id = g.id AND ig.key = 'imported_display_name' "
        "WHERE " JW_VISIBLE_GAME("g")
        "ORDER BY COALESCE(NULLIF(gs.value, ''), NULLIF(ig.value, ''), g.name) COLLATE NOCASE ASC, "
        "         f.added_at DESC LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_int(stmt, 1, max_count);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_count < max_count) {
        jw__fill_game_entry(stmt, &out[*out_count]);
        (*out_count)++;
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}

int jw_db_record_play_by_id(const char *db_path, int game_id, int duration_s) {
    if (!db_path || game_id <= 0) {
        return -1;
    }
    if (duration_s < 0) duration_s = 0;

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }

    sqlite3_stmt *stmt = NULL;
    /* Wrap both writes so playtime and recents update together: under
       SQLITE_BUSY past the busy timeout one write can fail while the other
       succeeds, silently desyncing stats. ROLLBACK on any failure and report
       it so the caller knows the session wasn't recorded. */
    if (sqlite3_exec(db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }
    int rc = 0;

    /* Cumulative playtime + last-played timestamp on the game row. */
    if (sqlite3_prepare_v2(db,
            "UPDATE games SET playtime_s = playtime_s + ?, last_played = strftime('%s','now') "
            "WHERE id = ?;", -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, duration_s);
        sqlite3_bind_int(stmt, 2, game_id);
        if (sqlite3_step(stmt) != SQLITE_DONE) rc = -1;
        sqlite3_finalize(stmt);
    } else {
        rc = -1;
    }

    /* Recents row: newest open wins; duration_s holds the last session length. */
    if (rc == 0 && sqlite3_prepare_v2(db,
            "INSERT INTO recents (kind, target_id, last_opened, duration_s) "
            "VALUES ('game', ?, strftime('%s','now'), ?) "
            "ON CONFLICT(kind, target_id) DO UPDATE SET "
            "last_opened = excluded.last_opened, duration_s = excluded.duration_s;",
            -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, game_id);
        sqlite3_bind_int(stmt, 2, duration_s);
        if (sqlite3_step(stmt) != SQLITE_DONE) rc = -1;
        sqlite3_finalize(stmt);
    } else if (rc == 0) {
        rc = -1;
    }

    sqlite3_exec(db, rc == 0 ? "COMMIT" : "ROLLBACK", NULL, NULL, NULL);

    jw_db_close(db);
    return rc;
}

int jw_db_record_play(const char *db_path, const char *rom_path, int duration_s) {
    jw_game_entry game;
    if (!rom_path || jw_db_get_game_by_rom_path(db_path, rom_path, &game) != 0) {
        return -1;
    }
    return jw_db_record_play_by_id(db_path, game.id, duration_s);
}

int jw_db_list_recent_games(const char *db_path, jw_game_entry *out,
                            int max_count, int *out_count) {
    if (!db_path || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    /* Most-recently-opened first; carry the favorite flag so recents rows show
       the star too. */
    static const char *sql =
        "SELECT g.id, g.system, COALESCE(NULLIF(gs.value, ''), NULLIF(ig.value, ''), g.name), "
        "g.source_id,g.rom_relpath,g.rom_path,COALESCE(g.image_root_kind,''),"
        "COALESCE(g.image_relpath,''),COALESCE(g.image_path,''), "
        "EXISTS(SELECT 1 FROM favorites f WHERE f.kind = 'game' AND f.target_id = g.id), "
        "COALESCE(g.last_played,0), g.playtime_s "
        "FROM games g JOIN recents r ON r.kind = 'game' AND r.target_id = g.id "
        "LEFT JOIN game_settings gs ON gs.game_id = g.id AND gs.key = 'display_name' "
        "LEFT JOIN game_settings ig ON ig.game_id = g.id AND ig.key = 'imported_display_name' "
        "WHERE " JW_VISIBLE_GAME("g")
        "ORDER BY r.last_opened DESC LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        jw_db_close(db);
        return -1;
    }

    sqlite3_bind_int(stmt, 1, max_count);

    while (sqlite3_step(stmt) == SQLITE_ROW && *out_count < max_count) {
        jw__fill_game_entry(stmt, &out[*out_count]);
        (*out_count)++;
    }

    sqlite3_finalize(stmt);
    jw_db_close(db);
    return 0;
}

static int jw__build_fts_query(const char *query, char *out, size_t out_size) {
    if (!query || !out || out_size == 0) {
        return -1;
    }

    out[0] = '\0';
    size_t used = 0;
    int token_count = 0;

    for (const unsigned char *p = (const unsigned char *)query; *p; ) {
        while (*p && !isalnum(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }

        char token[64];
        size_t token_len = 0;
        while (*p && isalnum(*p)) {
            if (token_len + 1 < sizeof(token)) {
                token[token_len++] = (char)tolower(*p);
            }
            p++;
        }
        token[token_len] = '\0';
        if (token_len == 0) {
            continue;
        }

        size_t needed = token_len + 1u + (token_count > 0 ? 1u : 0u);
        if (used + needed + 1u >= out_size) {
            break;
        }

        if (token_count > 0) {
            out[used++] = ' ';
        }
        memcpy(out + used, token, token_len);
        used += token_len;
        out[used++] = '*';
        out[used] = '\0';
        token_count++;
    }

    return token_count > 0 ? 0 : 1;
}

static int jw__build_like_query(const char *query, char *out, size_t out_size) {
    if (!query || !out || out_size < 2) {
        return -1;
    }

    out[0] = '\0';
    size_t used = 0;
    int token_count = 0;
    out[used++] = '%';
    out[used] = '\0';

    for (const unsigned char *p = (const unsigned char *)query; *p; ) {
        while (*p && !isalnum(*p)) {
            p++;
        }
        if (!*p) {
            break;
        }

        char token[64];
        size_t token_len = 0;
        while (*p && isalnum(*p)) {
            if (token_len + 1 < sizeof(token)) {
                token[token_len++] = (char)tolower(*p);
            }
            p++;
        }
        token[token_len] = '\0';
        if (token_len == 0) {
            continue;
        }

        if (used + token_len + 2u >= out_size) {
            break;
        }
        memcpy(out + used, token, token_len);
        used += token_len;
        out[used++] = '%';
        out[used] = '\0';
        token_count++;
    }

    return token_count > 0 ? 0 : 1;
}

static int jw__pinyin_search_enabled(sqlite3 *db, int *out_enabled) {
    if (!db || !out_enabled) return -1;
    *out_enabled = 0;

    static const char *sql =
        "SELECT value FROM settings WHERE key = 'language';";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        return -1;
    }

    int step_rc = sqlite3_step(stmt);
    if (step_rc == SQLITE_ROW) {
        const unsigned char *language = sqlite3_column_text(stmt, 0);
        *out_enabled = language && strcmp((const char *)language, "zh_CN") == 0;
    }
    int rc = (step_rc == SQLITE_ROW || step_rc == SQLITE_DONE) ? 0 : -1;
    sqlite3_finalize(stmt);
    return rc;
}

int jw_db_search_library(const char *db_path, const char *query,
                         jw_search_result *out, int max_count, int *out_count) {
    if (!db_path || !query || !out || max_count <= 0 || !out_count) {
        return -1;
    }

    *out_count = 0;
    memset(out, 0, sizeof(out[0]) * (size_t)max_count);

    jw_pinyin_query pinyin_query;
    jw_pinyin_prepare_query(query, &pinyin_query);
    if (!pinyin_query.has_search_chars) {
        return 0;
    }

    char fts_query[256];
    int query_rc = jw__build_fts_query(query, fts_query, sizeof(fts_query));
    char like_query[256];
    int like_rc = jw__build_like_query(query, like_query, sizeof(like_query));
    if (query_rc < 0 || like_rc < 0) {
        return -1;
    }

    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0) {
        return -1;
    }
    if (jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }

    int pinyin_enabled = 0;
    if (jw__pinyin_search_enabled(db, &pinyin_enabled) != 0) {
        jw_db_close(db);
        return -1;
    }
    if (!pinyin_enabled && query_rc > 0) {
        jw_db_close(db);
        return 0;
    }
    static const char *sql =
        "SELECT kind,id,name,system,source_id,rom_relpath,image_root_kind,"
        "image_relpath,rom_path,image_path,pak_dir,icon,favorite FROM ("
        "  SELECT 0 AS kind,games.id AS id,COALESCE(NULLIF(gs.value, ''), NULLIF(ig.value, ''), games.name) AS name, games.system AS system,"
        "         games.source_id,games.rom_relpath,"
        "         COALESCE(games.image_root_kind,'') AS image_root_kind,"
        "         COALESCE(games.image_relpath,'') AS image_relpath,"
        "         games.rom_path AS rom_path, COALESCE(games.image_path, '') AS image_path,"
        "         '' AS pak_dir, '' AS icon, EXISTS(SELECT 1 FROM favorites f WHERE f.kind = 'game' AND f.target_id = games.id) AS favorite, bm25(games_fts) AS score"
        "    FROM games_fts JOIN games ON games_fts.rowid = games.id"
        "    LEFT JOIN game_settings gs ON gs.game_id = games.id AND gs.key = 'display_name'"
        "    LEFT JOIN game_settings ig ON ig.game_id = games.id AND ig.key = 'imported_display_name'"
        "   WHERE games_fts MATCH ? AND " JW_VISIBLE_GAME("games")
        "  UNION ALL"
        "  SELECT 0 AS kind,games.id AS id,COALESCE(NULLIF(gs.value, ''), NULLIF(ig.value, ''), games.name) AS name, games.system AS system,"
        "         games.source_id,games.rom_relpath,COALESCE(games.image_root_kind,''),"
        "         COALESCE(games.image_relpath,''),"
        "         games.rom_path AS rom_path, COALESCE(games.image_path, '') AS image_path,"
        "         '' AS pak_dir, '' AS icon, EXISTS(SELECT 1 FROM favorites f WHERE f.kind = 'game' AND f.target_id = games.id) AS favorite, 1000000.0 AS score"
        "    FROM games"
        "    LEFT JOIN game_settings gs ON gs.game_id = games.id AND gs.key = 'display_name'"
        "    LEFT JOIN game_settings ig ON ig.game_id = games.id AND ig.key = 'imported_display_name'"
        "   WHERE lower(COALESCE(NULLIF(gs.value, ''), NULLIF(ig.value, ''), games.name)) LIKE ?"
        "     AND " JW_VISIBLE_GAME("games")
        "     AND games.id NOT IN (SELECT rowid FROM games_fts WHERE games_fts MATCH ?)"
        "  UNION ALL"
        "  SELECT 1 AS kind,apps.id AS id,apps.name AS name,'' AS system,"
        "         '' AS source_id,'' AS rom_relpath,'' AS image_root_kind,"
        "         '' AS image_relpath,"
        "         '' AS rom_path, '' AS image_path,"
        "         apps.pak_dir AS pak_dir, COALESCE(apps.icon, '') AS icon, 0 AS favorite,"
        "         bm25(apps_fts) AS score"
        "    FROM apps_fts JOIN apps ON apps_fts.rowid = apps.id"
        "   WHERE apps_fts MATCH ?"
        ") ORDER BY score ASC, kind ASC, name ASC LIMIT ?;";

    sqlite3_stmt *stmt = NULL;
    int step_rc = SQLITE_DONE;
    if (query_rc == 0) {
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            jw_db_close(db);
            return -1;
        }
        sqlite3_bind_text(stmt, 1, fts_query, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, like_query, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, fts_query, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, fts_query, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, max_count);
    }

    while (stmt && (step_rc = sqlite3_step(stmt)) == SQLITE_ROW && *out_count < max_count) {
        int i = *out_count;
        out[i].kind = sqlite3_column_int(stmt, 0) == 1 ? JW_SEARCH_APP : JW_SEARCH_GAME;
        out[i].id = sqlite3_column_int(stmt, 1);

        const unsigned char *name = sqlite3_column_text(stmt, 2);
        const unsigned char *system = sqlite3_column_text(stmt, 3);
        const unsigned char *source_id = sqlite3_column_text(stmt, 4);
        const unsigned char *rom_relpath = sqlite3_column_text(stmt, 5);
        const unsigned char *image_root_kind = sqlite3_column_text(stmt, 6);
        const unsigned char *image_relpath = sqlite3_column_text(stmt, 7);
        const unsigned char *rom_path = sqlite3_column_text(stmt, 8);
        const unsigned char *image_path = sqlite3_column_text(stmt, 9);
        const unsigned char *pak_dir = sqlite3_column_text(stmt, 10);
        const unsigned char *icon = sqlite3_column_text(stmt, 11);
        out[i].favorite = sqlite3_column_int(stmt, 12) != 0;

        if (name) snprintf(out[i].name, sizeof(out[i].name), "%s", name);
        if (system) snprintf(out[i].system, sizeof(out[i].system), "%s", system);
        if (source_id) snprintf(out[i].source_id, sizeof(out[i].source_id), "%s", source_id);
        if (rom_relpath) snprintf(out[i].rom_relpath, sizeof(out[i].rom_relpath), "%s", rom_relpath);
        if (image_root_kind) snprintf(out[i].image_root_kind, sizeof(out[i].image_root_kind), "%s", image_root_kind);
        if (image_relpath) snprintf(out[i].image_relpath, sizeof(out[i].image_relpath), "%s", image_relpath);
        if (rom_path) snprintf(out[i].rom_path, sizeof(out[i].rom_path), "%s", rom_path);
        if (image_path) snprintf(out[i].image_path, sizeof(out[i].image_path), "%s", image_path);
        if (pak_dir) snprintf(out[i].pak_dir, sizeof(out[i].pak_dir), "%s", pak_dir);
        if (icon) snprintf(out[i].icon, sizeof(out[i].icon), "%s", icon);
        (*out_count)++;
    }

    int rc = (step_rc == SQLITE_DONE || *out_count >= max_count) ? 0 : -1;
    if (stmt) sqlite3_finalize(stmt);

    /* Chinese names are not tokenized by the byte-oriented FTS builder. Scan
       when SQL had no tokens or usable pinyin initials can add results. */
    if (rc == 0 && pinyin_enabled && *out_count < max_count &&
        (query_rc > 0 || pinyin_query.wanted_count > 0)) {
        static const char *fallback_sql =
            "SELECT games.id, "
            "COALESCE(NULLIF(gs.value, ''), NULLIF(ig.value, ''), games.name) AS effective_name "
            "FROM games "
            "LEFT JOIN game_settings gs ON gs.game_id = games.id AND gs.key = 'display_name' "
            "LEFT JOIN game_settings ig ON ig.game_id = games.id AND ig.key = 'imported_display_name' "
            "WHERE " JW_VISIBLE_GAME("games")
            "ORDER BY effective_name COLLATE BINARY ASC, games.id ASC;";
        sqlite3_stmt *fallback = NULL;
        if (sqlite3_prepare_v2(db, fallback_sql, -1, &fallback, NULL) != SQLITE_OK) {
            jw_db_close(db);
            return -1;
        }
        static const char *detail_sql =
            "SELECT 0 AS kind, games.id, "
            "COALESCE(NULLIF(gs.value, ''), NULLIF(ig.value, ''), games.name), "
            "games.system, games.source_id, games.rom_relpath, "
            "COALESCE(games.image_root_kind,''), COALESCE(games.image_relpath,''), "
            "games.rom_path, COALESCE(games.image_path,''), '' AS pak_dir, '' AS icon, "
            "EXISTS(SELECT 1 FROM favorites f "
            "WHERE f.kind = 'game' AND f.target_id = games.id) "
            "FROM games "
            "LEFT JOIN game_settings gs ON gs.game_id = games.id AND gs.key = 'display_name' "
            "LEFT JOIN game_settings ig ON ig.game_id = games.id AND ig.key = 'imported_display_name' "
            "WHERE games.id = ?;";
        sqlite3_stmt *detail = NULL;
        if (sqlite3_prepare_v2(db, detail_sql, -1, &detail, NULL) != SQLITE_OK) {
            sqlite3_finalize(fallback);
            jw_db_close(db);
            return -1;
        }
        int fallback_step_rc = SQLITE_ROW;
        while ((fallback_step_rc = sqlite3_step(fallback)) == SQLITE_ROW &&
               *out_count < max_count) {
            const char *name = (const char *)sqlite3_column_text(fallback, 1);
            if (!jw_pinyin_match_prepared(name ? name : "", &pinyin_query)) continue;
            int id = sqlite3_column_int(fallback, 0);
            int duplicate = 0;
            for (int i = 0; i < *out_count; ++i) {
                if (out[i].kind == JW_SEARCH_GAME && out[i].id == id) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) continue;
            int i = *out_count;
            sqlite3_reset(detail);
            sqlite3_clear_bindings(detail);
            if (sqlite3_bind_int(detail, 1, id) != SQLITE_OK ||
                sqlite3_step(detail) != SQLITE_ROW) {
                sqlite3_finalize(detail);
                sqlite3_finalize(fallback);
                jw_db_close(db);
                return -1;
            }
            out[i].kind = JW_SEARCH_GAME;
            out[i].id = sqlite3_column_int(detail, 1);
            const unsigned char *detail_name = sqlite3_column_text(detail, 2);
            const unsigned char *system = sqlite3_column_text(detail, 3);
            const unsigned char *source_id = sqlite3_column_text(detail, 4);
            const unsigned char *rom_relpath = sqlite3_column_text(detail, 5);
            const unsigned char *image_root_kind = sqlite3_column_text(detail, 6);
            const unsigned char *image_relpath = sqlite3_column_text(detail, 7);
            const unsigned char *rom_path = sqlite3_column_text(detail, 8);
            const unsigned char *image_path = sqlite3_column_text(detail, 9);
            const unsigned char *pak_dir = sqlite3_column_text(detail, 10);
            const unsigned char *icon = sqlite3_column_text(detail, 11);
            out[i].favorite = sqlite3_column_int(detail, 12) != 0;
            if (detail_name) snprintf(out[i].name, sizeof(out[i].name), "%s", detail_name);
            if (system) snprintf(out[i].system, sizeof(out[i].system), "%s", system);
            if (source_id) snprintf(out[i].source_id, sizeof(out[i].source_id), "%s", source_id);
            if (rom_relpath) snprintf(out[i].rom_relpath, sizeof(out[i].rom_relpath), "%s", rom_relpath);
            if (image_root_kind) snprintf(out[i].image_root_kind, sizeof(out[i].image_root_kind), "%s", image_root_kind);
            if (image_relpath) snprintf(out[i].image_relpath, sizeof(out[i].image_relpath), "%s", image_relpath);
            if (rom_path) snprintf(out[i].rom_path, sizeof(out[i].rom_path), "%s", rom_path);
            if (image_path) snprintf(out[i].image_path, sizeof(out[i].image_path), "%s", image_path);
            if (pak_dir) snprintf(out[i].pak_dir, sizeof(out[i].pak_dir), "%s", pak_dir);
            if (icon) snprintf(out[i].icon, sizeof(out[i].icon), "%s", icon);
            (*out_count)++;
        }
        if (fallback_step_rc != SQLITE_DONE && *out_count < max_count) {
            rc = -1;
        }
        sqlite3_finalize(detail);
        sqlite3_finalize(fallback);
    }
    jw_db_close(db);
    return rc;
}
