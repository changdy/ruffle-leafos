#include "internal/db/db.h"
#include "internal/retroarch/catalog.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define JW_CORE_SETTING_ID "core_id"
#define JW_MAX_CHOICES 8

static void fail(const char *message) {
    fprintf(stderr, "core-override-smoke: %s\n", message);
    exit(1);
}

static void expect_string(const char *label, const char *got, const char *want) {
    if (!got || strcmp(got, want) != 0) {
        fprintf(stderr, "core-override-smoke: %s got=%s want=%s\n",
                label, got ? got : "(null)", want);
        exit(1);
    }
}

static void expect_resolved(const jw_ra_catalog *catalog,
                            const char *system_id,
                            const char *preferred_core_id,
                            const char *cores_dir,
                            const char *want_core_id,
                            const char *want_core_file) {
    char core_file[PATH_MAX];
    char core_id[64];
    char diagnostic[256];
    if (jw_ra_catalog_resolve_core_file_for_choice(
            catalog, system_id, preferred_core_id, cores_dir,
            core_file, sizeof(core_file), core_id, sizeof(core_id),
            diagnostic, sizeof(diagnostic)) != 0) {
        fprintf(stderr,
                "core-override-smoke: resolve failed system=%s preferred=%s diagnostic=%s\n",
                system_id, preferred_core_id ? preferred_core_id : "(none)",
                diagnostic);
        exit(1);
    }
    expect_string("resolved core id", core_id, want_core_id);
    expect_string("resolved core file", core_file, want_core_file);

    char full_path[PATH_MAX];
    if (snprintf(full_path, sizeof(full_path), "%s/%s", cores_dir, core_file) >=
        (int)sizeof(full_path)) {
        fail("resolved core path is too long");
    }
    if (access(full_path, F_OK) != 0) {
        fail("resolved core path does not exist");
    }
    printf("resolved\t%s\t%s\n", core_id, full_path);
}

static int prepare_game(const char *db_path, const char *system_id,
                        const char *rom_path) {
    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        jw_db_scan_begin(db) != 0 ||
        jw_db_insert_game(db, system_id, "gpSP smoke game", rom_path, NULL) != 0) {
        if (db) {
            jw_db_close(db);
        }
        fail("could not prepare fixture database");
    }
    jw_db_close(db);

    jw_game_entry game;
    if (jw_db_get_game_by_rom_path(db_path, rom_path, &game) != 0 || game.id <= 0) {
        fail("fixture game lookup failed");
    }
    return game.id;
}

int main(int argc, char **argv) {
    if (argc != 8) {
        fprintf(stderr,
                "usage: %s <sdcard-root> <system-id> <cores-dir> <platform-dir> "
                "<db-path> <rom-path> <alternate-core-id>\n",
                argv[0]);
        return 2;
    }

    const char *sdcard_root = argv[1];
    const char *system_id = argv[2];
    const char *cores_dir = argv[3];
    const char *platform_dir = argv[4];
    const char *db_path = argv[5];
    const char *rom_path = argv[6];
    const char *alternate_core_id = argv[7];

    char error[256];
    jw_ra_catalog *catalog = jw_ra_catalog_load(sdcard_root, error, sizeof(error));
    if (!catalog) {
        fprintf(stderr, "core-override-smoke: catalog load failed: %s\n",
                error[0] ? error : "unknown");
        return 1;
    }

    jw_ra_core_choice choices[JW_MAX_CHOICES];
    size_t count = 0;
    if (jw_ra_catalog_list_system_cores(catalog, system_id, cores_dir,
                                        platform_dir, choices, JW_MAX_CHOICES,
                                        &count) != 0) {
        fail("core choice listing failed");
    }
    if (count != 2) {
        fail("expected exactly two GBA core choices");
    }
    expect_string("default choice id", choices[0].id, "mgba");
    expect_string("alternate choice id", choices[1].id, alternate_core_id);
    if (!choices[0].is_default || choices[1].is_default) {
        fail("GBA default/alternate ordering is incorrect");
    }
    printf("choices\t%s\t%s\n", choices[0].id, choices[1].id);

    expect_resolved(catalog, system_id, NULL, cores_dir,
                    "mgba", "mgba_libretro.so");

    int game_id = prepare_game(db_path, system_id, rom_path);
    char persisted[64];

    if (jw_db_set_system_setting(db_path, system_id, JW_CORE_SETTING_ID,
                                 alternate_core_id) != 0 ||
        jw_db_get_system_setting(db_path, system_id, JW_CORE_SETTING_ID,
                                 persisted, sizeof(persisted)) != 0) {
        fail("system core override round-trip failed");
    }
    expect_string("system override", persisted, alternate_core_id);
    expect_resolved(catalog, system_id, persisted, cores_dir,
                    alternate_core_id, "gpsp_libretro.so");
    printf("persisted\tsystem\t%s\n", persisted);

    if (jw_db_set_game_setting(db_path, game_id, JW_CORE_SETTING_ID,
                               alternate_core_id) != 0 ||
        jw_db_get_game_setting(db_path, game_id, JW_CORE_SETTING_ID,
                               persisted, sizeof(persisted)) != 0) {
        fail("game core override round-trip failed");
    }
    expect_string("game override", persisted, alternate_core_id);
    expect_resolved(catalog, system_id, persisted, cores_dir,
                    alternate_core_id, "gpsp_libretro.so");
    printf("persisted\tgame\t%s\n", persisted);

    char alternate_path[PATH_MAX];
    if (snprintf(alternate_path, sizeof(alternate_path), "%s/gpsp_libretro.so",
                 cores_dir) >= (int)sizeof(alternate_path) ||
        unlink(alternate_path) != 0) {
        fail("could not remove alternate fixture core");
    }

    count = 0;
    if (jw_ra_catalog_list_system_cores(catalog, system_id, cores_dir,
                                        platform_dir, choices, JW_MAX_CHOICES,
                                        &count) != 0 || count != 1) {
        fail("missing gpSP fixture did not collapse the choice list");
    }
    expect_string("fallback choice id", choices[0].id, "mgba");
    expect_resolved(catalog, system_id, persisted, cores_dir,
                    "mgba", "mgba_libretro.so");
    printf("fallback\t%s\n", choices[0].id);

    if (jw_db_get_game_setting(db_path, game_id, JW_CORE_SETTING_ID,
                               persisted, sizeof(persisted)) != 0) {
        fail("stale game override was not retained");
    }
    expect_string("stale game override", persisted, alternate_core_id);

    jw_ra_catalog_free(catalog);
    puts("PASS core-override-smoke");
    return 0;
}
