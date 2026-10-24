#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/store/pakrat_txn.h"

#include "internal/store/pakrat_recovery.h"
#include "internal/store/pakrat_themes.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,       \
                    #condition);                                            \
            failures++;                                                     \
        }                                                                   \
    } while (0)

typedef struct {
    char root[PATH_MAX];
    char primary[PATH_MAX];
    char secondary[PATH_MAX];
    char absent[PATH_MAX];
    char state[PATH_MAX];
    char db[PATH_MAX];
    char runtime[PATH_MAX];
    char primary_userdata[PATH_MAX];
    char secondary_userdata[PATH_MAX];
    char primary_apps[PATH_MAX];
    char secondary_apps[PATH_MAX];
    jw_pakrat_context ctx;
} txn_fixture;

typedef struct {
    char target[PATH_MAX];
    char stage[PATH_MAX];
    char rollback[PATH_MAX];
    char origin[PATH_MAX];
    char trust[PATH_MAX];
    char history[PATH_MAX];
    char control[PATH_MAX];
} uninstall_fixture_paths;

static void join_path(char *out, size_t out_size, const char *a,
                      const char *b) {
    int n = snprintf(out, out_size, "%s/%s", a, b);
    if (n < 0 || (size_t)n >= out_size) {
        fprintf(stderr, "path overflow\n");
        exit(2);
    }
}

static void write_file(const char *path, const char *content, mode_t mode) {
    if (jw__pakrat_mkdir_parent(path) != 0) {
        fprintf(stderr, "mkdir parent failed: %s\n", path);
        exit(2);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (fd < 0 || write(fd, content, strlen(content)) !=
                      (ssize_t)strlen(content) || close(fd) != 0) {
        fprintf(stderr, "write failed: %s\n", path);
        exit(2);
    }
}

static long long scalar_path(const char *path, const char *sql) {
    sqlite3 *db = NULL;
    sqlite3_stmt *stmt = NULL;
    long long result = -1;
    if (sqlite3_open(path, &db) == SQLITE_OK &&
        sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_step(stmt) == SQLITE_ROW) {
        result = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);
    if (db) sqlite3_close(db);
    return result;
}

static void fixture_env(txn_fixture *f) {
    char roots[PATH_MAX * 3];
    char userdata[PATH_MAX * 3];
    char apps[PATH_MAX * 3];
    snprintf(roots, sizeof(roots), "%s:%s:%s", f->primary, f->secondary,
             f->absent);
    snprintf(userdata, sizeof(userdata), "%s:%s:%s/.userdata/mac",
             f->primary_userdata, f->secondary_userdata, f->absent);
    snprintf(apps, sizeof(apps), "%s:%s:%s/Apps", f->primary_apps,
             f->secondary_apps, f->absent);
    setenv("PLATFORM", "mac", 1);
    setenv("SDCARD_PATH", f->primary, 1);
    setenv("SDCARD_PATHS", roots, 1);
    setenv("USERDATA_PATH", f->primary_userdata, 1);
    setenv("USERDATA_PATHS", userdata, 1);
    setenv("APPS_PATH", f->primary_apps, 1);
    setenv("APPS_PATHS", apps, 1);
}

/* Derive every fixture path from the root, without creating anything. A
   re-exec'd fault child rebuilds the fixture this way: the parent already
   seeded it on disk, and re-seeding would destroy the state under test. */
static void fixture_paths(txn_fixture *f, const char *root) {
    memset(f, 0, sizeof(*f));
    snprintf(f->root, sizeof(f->root), "%s", root);
    join_path(f->primary, sizeof(f->primary), f->root, "primary");
    join_path(f->secondary, sizeof(f->secondary), f->root, "secondary");
    join_path(f->absent, sizeof(f->absent), f->root, "absent");
    join_path(f->state, sizeof(f->state), f->primary, ".umrk/mac");
    join_path(f->db, sizeof(f->db), f->state, "library.db");
    join_path(f->runtime, sizeof(f->runtime), f->root, "runtime");
    join_path(f->primary_userdata, sizeof(f->primary_userdata), f->primary,
              ".userdata/mac");
    join_path(f->secondary_userdata, sizeof(f->secondary_userdata),
              f->secondary, ".userdata/mac");
    join_path(f->primary_apps, sizeof(f->primary_apps), f->primary, "Apps");
    join_path(f->secondary_apps, sizeof(f->secondary_apps), f->secondary,
              "Apps");
    fixture_env(f);
    snprintf(f->ctx.platform, sizeof(f->ctx.platform), "mac");
    snprintf(f->ctx.sdcard_root, sizeof(f->ctx.sdcard_root), "%s",
             f->primary);
    snprintf(f->ctx.state_dir, sizeof(f->ctx.state_dir), "%s", f->state);
    snprintf(f->ctx.db_path, sizeof(f->ctx.db_path), "%s", f->db);
    snprintf(f->ctx.runtime_dir, sizeof(f->ctx.runtime_dir), "%s",
             f->runtime);
}

static void fixture_setup(txn_fixture *f) {
    char root[PATH_MAX];
    snprintf(root, sizeof(root), "/tmp/jw-pakrat-txn.XXXXXX");
    if (!mkdtemp(root)) {
        fprintf(stderr, "mkdtemp: %s\n", strerror(errno));
        exit(2);
    }
    fixture_paths(f, root);
    if (jw__pakrat_mkdir_p(f->primary_userdata, 0700) != 0 ||
        jw__pakrat_mkdir_p(f->secondary_userdata, 0700) != 0 ||
        jw__pakrat_mkdir_p(f->primary_apps, 0700) != 0 ||
        jw__pakrat_mkdir_p(f->secondary_apps, 0700) != 0 ||
        jw__pakrat_mkdir_p(f->state, 0700) != 0 ||
        jw__pakrat_mkdir_p(f->runtime, 0700) != 0) {
        fprintf(stderr, "fixture mkdir failed\n");
        exit(2);
    }
}

static void fixture_teardown(txn_fixture *f) {
    (void)jw__pakrat_remove_tree(f->root);
}

static void write_real_pak(txn_fixture *f, const char *pak_root) {
    char run[PATH_MAX];
    char manifest[PATH_MAX];
    join_path(run, sizeof(run), pak_root, "bin/run.sh");
    join_path(manifest, sizeof(manifest), pak_root, "pak.json");
    write_file(run, "#!/bin/sh\nwhile :; do sleep 1; done\n", 0700);
    write_file(
        manifest,
        "{\"id\":\"org.umrk.test.txn\",\"name\":\"TXN Fixture\","
        "\"platform\":\"mac\",\"pak_version\":\"1.0.0\","
        "\"service\":{\"schema\":1,\"id\":\"org.umrk.test.txn\","
        "\"run\":{\"path\":\"bin/run.sh\",\"args\":[]},"
        "\"restart\":\"no\",\"default_enabled\":false,"
        "\"stop_grace_ms\":100},"
        "\"state\":{\"root\":\"Syncthing\","
        "\"revoke_on_uninstall\":[\"leaf/trusted.json\"],"
        "\"retained_roots\":[\"Syncthing\"]}}\n",
        0600);
}

static void write_floor_pak(const char *pak_root) {
    char launch[PATH_MAX];
    char manifest[PATH_MAX];
    join_path(launch, sizeof(launch), pak_root, "launch.sh");
    join_path(manifest, sizeof(manifest), pak_root, "pak.json");
    write_file(launch, "#!/bin/sh\nexit 0\n", 0700);
    write_file(manifest,
               "{\"id\":\"org.umrk.test.txn\",\"name\":\"TXN Floor\","
               "\"platform\":\"mac\",\"pak_version\":\"0.0.1\"}\n",
               0600);
}

static void write_content_pak(const char *pak_root) {
    char manifest[PATH_MAX];
    join_path(manifest, sizeof(manifest), pak_root, "pak.json");
    write_file(manifest,
               "{\"id\":\"org.umrk.test.content\",\"name\":\"Content\","
               "\"platform\":\"mac\",\"pak_version\":\"1.0.0\","
               "\"provides\":{\"systems\":[],\"cores\":[]}}\n",
               0600);
}

static void test_content_only_recovery(void) {
    static const char *sha =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static const char *token = "0123456789abcdef0123456789abcdef";
    for (int committed = 0; committed <= 1; committed++) {
        txn_fixture f;
        fixture_setup(&f);
        char target[PATH_MAX];
        join_path(target, sizeof(target), f.primary,
                  "Apps/mac/Content.pak");
        write_content_pak(target);
        if (committed) {
            jw_pakrat_commit_marker marker;
            memset(&marker, 0, sizeof(marker));
            snprintf(marker.store_id, sizeof(marker.store_id),
                     "org.umrk.test.content");
            snprintf(marker.version, sizeof(marker.version), "1.0.0");
            snprintf(marker.artifact_sha256, sizeof(marker.artifact_sha256),
                     "%s", sha);
            snprintf(marker.token, sizeof(marker.token), "%s", token);
            CHECK(jw__pakrat_write_commit_marker(target, &marker) == 0);
        }
        CHECK(jw_db_pakrat_upsert_install(
                  f.db, "org.umrk.test.content", "1.0.0", "mac",
                  "mac/Content.pak", sha, NULL,
                  committed ? token : NULL) == 0);
        jw_pakrat_install install;
        CHECK(jw_db_pakrat_get_install(
                  f.db, "org.umrk.test.content", &install) == 0);
        jw_pakrat_recovery_context recovery;
        memset(&recovery, 0, sizeof(recovery));
        snprintf(recovery.platform, sizeof(recovery.platform), "mac");
        snprintf(recovery.sdcard_root, sizeof(recovery.sdcard_root), "%s",
                 f.primary);
        snprintf(recovery.state_dir, sizeof(recovery.state_dir), "%s",
                 f.state);
        snprintf(recovery.db_path, sizeof(recovery.db_path), "%s", f.db);
        CHECK(jw__pakrat_reconcile_transition(
                  &recovery, install.store_id, install.install_path,
                  &install) == 0);
        fixture_teardown(&f);
    }
}

static void test_manifest_and_cache(txn_fixture *f,
                                    jw_pakrat_txn_metadata *real_out) {
    char real[PATH_MAX];
    char floor[PATH_MAX];
    join_path(real, sizeof(real), f->root, "real.pak");
    join_path(floor, sizeof(floor), f->root, "floor.pak");
    write_real_pak(f, real);
    write_floor_pak(floor);
    char reason[JW_SVC_REASON_BUF] = {0};
    CHECK(jw_pakrat_txn_inspect_manifest(
              real, "pak.json", f->primary_userdata,
              "org.umrk.test.txn", "mac/TXN.pak", real_out,
              reason, sizeof(reason)) == 0);
    CHECK(real_out->has_service);
    CHECK(strcmp(real_out->service_id, "org.umrk.test.txn") == 0);
    CHECK(real_out->revoke_count == 1 && real_out->retained_count == 1);
    CHECK(strcmp(real_out->state_root, "Syncthing") == 0);

    jw_pakrat_txn_metadata floor_metadata;
    CHECK(jw_pakrat_txn_inspect_manifest(
              floor, "pak.json", f->primary_userdata,
              "org.umrk.test.txn", "mac/TXN.pak", &floor_metadata,
              reason, sizeof(reason)) == 0);
    CHECK(!floor_metadata.has_service && floor_metadata.service_id[0] == '\0');
    jw_pakrat_txn_metadata_destroy(&floor_metadata);

    sqlite3 *db = NULL;
    CHECK(jw_db_open(f->db, &db) == 0);
    CHECK(jw_db_apply_schema(db) == 0);
    CHECK(jw_pakrat_txn_metadata_upsert_db(db, real_out) == 0);
    jw_db_close(db);
    jw_pakrat_txn_metadata loaded;
    CHECK(jw_pakrat_txn_metadata_get(
              f->db, "org.umrk.test.txn", &loaded) == 0);
    CHECK(loaded.has_service && loaded.revoke_count == 1 &&
          loaded.retained_count == 1);
    CHECK(strcmp(loaded.revoke_on_uninstall[0], "leaf/trusted.json") == 0);
    jw_pakrat_txn_metadata_destroy(&loaded);
}

static void test_lock_death(txn_fixture *f) {
    char unused_runtime[PATH_MAX];
    join_path(unused_runtime, sizeof(unused_runtime), f->root,
              "unused-runtime");
    CHECK(!jw_pakrat_mutation_lock_is_held(
        unused_runtime, "missing", "org.umrk.test.txn", "mac/TXN.pak"));
    CHECK(!jw__pakrat_path_exists(unused_runtime));
    CHECK(!jw_pakrat_mutation_lock_is_held(
        f->runtime, NULL, "org.umrk.test.txn", "mac/TXN.pak"));
    CHECK(!jw_pakrat_mutation_lock_is_held(
        f->runtime, "missing", NULL, "mac/TXN.pak"));
    CHECK(!jw_pakrat_mutation_lock_is_held(
        f->runtime, "missing", "org.umrk.test.txn", NULL));
    int ready[2];
    CHECK(pipe(ready) == 0);
    pid_t child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        close(ready[0]);
        jw_pakrat_mutation_lock lock;
        char reason[JW_SVC_REASON_BUF];
        int rc = jw_pakrat_mutation_lock_acquire(
            f->runtime, "pakrat-lock-test", "org.umrk.test.txn",
            "mac/TXN.pak", &lock, reason, sizeof(reason));
        char value = rc == 0 ? '1' : '0';
        (void)write(ready[1], &value, 1);
        pause();
        _exit(2);
    }
    close(ready[1]);
    char value = '0';
    CHECK(read(ready[0], &value, 1) == 1 && value == '1');
    close(ready[0]);
    CHECK(jw_pakrat_mutation_lock_is_held(
        f->runtime, "pakrat-lock-test", "org.umrk.test.txn",
        "mac/TXN.pak"));
    CHECK(!jw_pakrat_mutation_lock_is_held(
        f->runtime, "wrong-operation", "org.umrk.test.txn",
        "mac/TXN.pak"));
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    CHECK(!jw_pakrat_mutation_lock_is_held(
        f->runtime, "pakrat-lock-test", "org.umrk.test.txn",
        "mac/TXN.pak"));
}

static void test_inventory_and_uninstall(
    txn_fixture *f, const jw_pakrat_txn_metadata *metadata) {
    char history[PATH_MAX];
    char trust[PATH_MAX];
    char secondary_root[PATH_MAX];
    join_path(history, sizeof(history), f->primary_userdata,
              "Syncthing/history/index.db");
    join_path(trust, sizeof(trust), f->primary_userdata,
              "Syncthing/leaf/trusted.json");
    join_path(secondary_root, sizeof(secondary_root), f->secondary_userdata,
              "Syncthing");
    write_file(history, "history", 0600);
    write_file(trust, "secret", 0600);
    CHECK(symlink("missing-target", secondary_root) == 0);

    jw_pakrat_uninstall_info info;
    CHECK(jw_pakrat_txn_inventory_retained(&f->ctx, metadata, &info) == 0);
    CHECK(info.item_count == 3);
    CHECK(info.items[0].source_present && info.items[0].size_known &&
          info.items[0].size_bytes > 0);
    CHECK(info.items[1].source_present && !info.items[1].size_known);
    CHECK(!info.items[2].source_present && !info.items[2].size_known);
    jw_pakrat_uninstall_info_destroy(&info);
    unlink(secondary_root);

    char target[PATH_MAX];
    join_path(target, sizeof(target), f->primary, "Apps/mac/TXN.pak");
    write_real_pak(f, target);
    CHECK(jw_db_pakrat_upsert_install(
              f->db, "org.umrk.test.txn", "1.0.0", "mac",
              "mac/TXN.pak",
              "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
              NULL, NULL) == 0);
    CHECK(jw_pakrat_txn_pending_persist(
              f->db, "primary", metadata) == 0);

    char control_path[PATH_MAX];
    join_path(control_path, sizeof(control_path), f->state,
              "services-control.db");
    sqlite3 *control = NULL;
    CHECK(sqlite3_open(control_path, &control) == SQLITE_OK);
    CHECK(sqlite3_exec(
              control,
              "CREATE TABLE service_control_state("
              "service_id TEXT PRIMARY KEY,start_with_leaf INTEGER,"
              "session_run INTEGER);"
              "INSERT INTO service_control_state VALUES("
              "'org.umrk.test.txn',1,1);",
              NULL, NULL, NULL) == SQLITE_OK);
    sqlite3_close(control);

    jw_pakrat_pending_uninstall pending;
    CHECK(jw_pakrat_txn_pending_get(
              f->db, "org.umrk.test.txn", &pending) == 0);
    CHECK(jw_pakrat_txn_complete_uninstall(&f->ctx, &pending) == 0);
    jw_pakrat_pending_uninstall_destroy(&pending);
    CHECK(!jw__pakrat_path_exists(target));
    CHECK(!jw__pakrat_path_exists(trust));
    CHECK(jw__pakrat_path_exists(history));
    CHECK(scalar_path(f->db,
                      "SELECT COUNT(*) FROM pakrat_installs;") == 0);
    CHECK(scalar_path(f->db,
                      "SELECT COUNT(*) FROM pakrat_pending_uninstalls;") == 0);
    CHECK(scalar_path(control_path,
                      "SELECT COUNT(*) FROM service_control_state;") == 0);
}

static void seed_uninstall_fixture(txn_fixture *f,
                                   jw_pakrat_txn_metadata *metadata,
                                   uninstall_fixture_paths *paths,
                                   bool persist_intent) {
    memset(metadata, 0, sizeof(*metadata));
    memset(paths, 0, sizeof(*paths));
    join_path(paths->target, sizeof(paths->target), f->primary,
              "Apps/mac/TXN.pak");
    write_real_pak(f, paths->target);
    char reason[JW_SVC_REASON_BUF] = {0};
    if (jw_pakrat_txn_inspect_manifest(
            paths->target, "pak.json", f->primary_userdata,
            "org.umrk.test.txn", "mac/TXN.pak", metadata,
            reason, sizeof(reason)) != 0) {
        fprintf(stderr, "fixture manifest invalid: %s\n", reason);
        exit(2);
    }
    if (jw__pakrat_target_sibling_path(
            paths->target, metadata->store_id, "stage", paths->stage,
            sizeof(paths->stage)) != 0 ||
        jw__pakrat_target_sibling_path(
            paths->target, metadata->store_id, "rollback", paths->rollback,
            sizeof(paths->rollback)) != 0 ||
        jw__pakrat_target_sibling_path(
            paths->target, metadata->store_id, "origin", paths->origin,
            sizeof(paths->origin)) != 0) {
        fprintf(stderr, "fixture sibling path failed\n");
        exit(2);
    }
    write_floor_pak(paths->stage);
    write_floor_pak(paths->rollback);
    if (jw__pakrat_write_origin_marker(
            paths->target, metadata->store_id, metadata->install_path) != 0) {
        fprintf(stderr, "fixture origin marker failed\n");
        exit(2);
    }
    join_path(paths->history, sizeof(paths->history), f->primary_userdata,
              "Syncthing/history/index.db");
    join_path(paths->trust, sizeof(paths->trust), f->primary_userdata,
              "Syncthing/leaf/trusted.json");
    write_file(paths->history, "history", 0600);
    write_file(paths->trust, "secret", 0600);

    sqlite3 *db = NULL;
    if (jw_db_open(f->db, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        jw_pakrat_txn_metadata_upsert_db(db, metadata) != 0) {
        fprintf(stderr, "fixture metadata database failed\n");
        jw_db_close(db);
        exit(2);
    }
    jw_db_close(db);
    if (jw_db_pakrat_upsert_install(
            f->db, metadata->store_id, "1.0.0", "mac",
            metadata->install_path,
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
            NULL, NULL) != 0 ||
        (persist_intent &&
         jw_pakrat_txn_pending_persist(f->db, "primary", metadata) != 0)) {
        fprintf(stderr, "fixture install database failed\n");
        exit(2);
    }

    join_path(paths->control, sizeof(paths->control), f->state,
              "services-control.db");
    sqlite3 *control = NULL;
    if (sqlite3_open(paths->control, &control) != SQLITE_OK ||
        sqlite3_exec(
            control,
            "CREATE TABLE service_control_state("
            "service_id TEXT PRIMARY KEY,start_with_leaf INTEGER,"
            "session_run INTEGER);"
            "INSERT INTO service_control_state VALUES("
            "'org.umrk.test.txn',1,1);",
            NULL, NULL, NULL) != SQLITE_OK) {
        fprintf(stderr, "fixture control database failed\n");
        sqlite3_close(control);
        exit(2);
    }
    sqlite3_close(control);
}

/* ---- Themes ---------------------------------------------------------------- */

#define THEME_SHA "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define THEME_TOKEN "fedcba9876543210fedcba9876543210"

static void write_theme(const char *dir, const char *id, const char *version) {
    char manifest[PATH_MAX];
    char json[512];
    join_path(manifest, sizeof(manifest), dir, "theme.json");
    snprintf(json, sizeof(json),
             "{\"schema\":1,\"id\":\"%s\",\"name\":\"Neon Nights\","
             "\"author\":\"Leaf\",\"version\":\"%s\","
             "\"min_leaf_version\":\"0.12.0\",\"license\":\"CC0-1.0\"}\n",
             id, version);
    write_file(manifest, json, 0644);
}

static void write_theme_marker(const char *dir, const char *id,
                               const char *version, const char *token) {
    jw_pakrat_commit_marker marker;
    memset(&marker, 0, sizeof(marker));
    snprintf(marker.store_id, sizeof(marker.store_id), "%s", id);
    snprintf(marker.version, sizeof(marker.version), "%s", version);
    snprintf(marker.artifact_sha256, sizeof(marker.artifact_sha256), "%s", THEME_SHA);
    snprintf(marker.token, sizeof(marker.token), "%s", token);
    CHECK(jw__pakrat_write_commit_marker(dir, &marker) == 0);
}

static void recovery_context_for(const txn_fixture *f, jw_pakrat_recovery_context *out) {
    memset(out, 0, sizeof(*out));
    snprintf(out->platform, sizeof(out->platform), "mac");
    snprintf(out->sdcard_root, sizeof(out->sdcard_root), "%s", f->primary);
    snprintf(out->state_dir, sizeof(out->state_dir), "%s", f->state);
    snprintf(out->db_path, sizeof(out->db_path), "%s", f->db);
}

/* The install root is chosen by kind, and the kind travels in install_path. */
static void test_theme_install_roots(void) {
    char out[PATH_MAX];
    CHECK(jw__pakrat_target_path("/sd", "mlp1/App.pak", out, sizeof(out)) == 0 &&
          strcmp(out, "/sd/Apps/mlp1/App.pak") == 0);
    CHECK(jw__pakrat_target_path("/sd", "Apps/mlp1/App.pak", out, sizeof(out)) == 0 &&
          strcmp(out, "/sd/Apps/mlp1/App.pak") == 0);
    CHECK(jw__pakrat_target_path("/sd", "Themes/neon-nights", out, sizeof(out)) == 0 &&
          strcmp(out, "/sd/Themes/neon-nights") == 0);
    CHECK(jw__pakrat_target_path("/sd", "Apps/Themes/x", out, sizeof(out)) == 0 &&
          strcmp(out, "/sd/Apps/Themes/x") == 0);
    CHECK(jw__pakrat_target_path("/sd", "Themes/..", out, sizeof(out)) != 0);
    CHECK(jw__pakrat_target_path("/sd", "Themes/a/b", out, sizeof(out)) != 0);
    CHECK(jw__pakrat_target_path("/sd", "Themes/", out, sizeof(out)) != 0);
    CHECK(jw_pakrat_install_path_kind("Themes/neon-nights") == JW_PAKRAT_KIND_THEME);
    CHECK(jw_pakrat_install_path_kind("mlp1/Themes.pak") == JW_PAKRAT_KIND_APP);
    CHECK(jw_pakrat_install_path_kind("Apps/Themes/x") == JW_PAKRAT_KIND_APP);
    CHECK(strcmp(jw_pakrat_install_path_display_prefix("Themes/neon-nights"), "") == 0);
    CHECK(strcmp(jw_pakrat_install_path_display_prefix("mlp1/App.pak"), "Apps/") == 0);
    CHECK(jw_pakrat_txn_target_path_valid("Themes/neon-nights"));
    CHECK(!jw_pakrat_txn_target_path_valid("Themes/a/b"));
    CHECK(jw__pakrat_target_sibling_path("/sd/Themes/neon-nights", "neon-nights",
                                         "stage", out, sizeof(out)) == 0 &&
          strcmp(out, "/sd/Themes/.pakrat-stage-neon-nights") == 0);

    jw_pakrat_txn_metadata metadata;
    CHECK(jw_pakrat_txn_theme_metadata("neon-nights", "Themes/neon-nights",
                                       "Neon Nights", &metadata) == 0);
    CHECK(!metadata.has_service && metadata.revoke_count == 0 &&
          metadata.retained_count == 0 &&
          strcmp(metadata.display_name, "Neon Nights") == 0);
    jw_pakrat_txn_metadata_destroy(&metadata);
    CHECK(jw_pakrat_txn_theme_metadata("neon-nights", "mac/Neon.pak", "Neon",
                                       &metadata) == -1);
}

/* Uninstalling the selected theme sets the selection to None in the same
   commit; uninstalling another theme leaves the selection alone. */
static void test_theme_uninstall_selection(void) {
    for (int selected = 0; selected <= 1; selected++) {
        txn_fixture f;
        fixture_setup(&f);
        char target[PATH_MAX];
        join_path(target, sizeof(target), f.primary, "Themes/neon-nights");
        write_theme(target, "neon-nights", "1.0.0");

        jw_pakrat_txn_metadata metadata;
        CHECK(jw_pakrat_txn_theme_metadata("neon-nights", "Themes/neon-nights",
                                           "Neon Nights", &metadata) == 0);
        sqlite3 *db = NULL;
        CHECK(jw_db_open(f.db, &db) == 0 && jw_db_apply_schema(db) == 0 &&
              jw_pakrat_txn_metadata_upsert_db(db, &metadata) == 0);
        jw_db_close(db);
        CHECK(jw_db_pakrat_upsert_install(f.db, "neon-nights", "1.0.0",
                                          JW_PAKRAT_THEME_PLATFORM,
                                          "Themes/neon-nights", THEME_SHA, NULL,
                                          NULL) == 0);
        jw_pakrat_install row;
        CHECK(jw_db_pakrat_get_install(f.db, "neon-nights", &row) == 0 &&
              strcmp(row.kind, "theme") == 0);
        CHECK(jw_db_set_setting(f.db, "user_theme",
                                selected ? "neon-nights" : "aurora") == 0);

        CHECK(jw_pakrat_txn_pending_persist(f.db, "primary", &metadata) == 0);
        jw_pakrat_pending_uninstall pending;
        jw_pakrat_outcome outcome;
        memset(&outcome, 0, sizeof(outcome));
        f.ctx.outcome = &outcome;
        CHECK(jw_pakrat_txn_pending_get(f.db, "neon-nights", &pending) == 0);
        CHECK(jw_pakrat_txn_complete_uninstall(&f.ctx, &pending) == 0);
        jw_pakrat_pending_uninstall_destroy(&pending);

        char value[128];
        CHECK(jw_db_get_setting(f.db, "user_theme", value, sizeof(value)) == 0);
        CHECK(strcmp(value, selected ? "" : "aurora") == 0);
        CHECK(outcome.theme_selection_cleared == selected);
        CHECK(!jw__pakrat_path_exists(target));
        CHECK(scalar_path(f.db, "SELECT COUNT(*) FROM pakrat_installs;") == 0);
        jw_pakrat_txn_metadata_destroy(&metadata);
        fixture_teardown(&f);
    }
}

/* Crash recovery reads a theme's identity from theme.json, and the orphan
   sweep reaches Themes/ -- without ever touching a user's own folder. */
static void test_theme_recovery(void) {
    txn_fixture f;
    fixture_setup(&f);
    jw_pakrat_recovery_context recovery;
    recovery_context_for(&f, &recovery);

    /* Committed: the record, marker and theme.json agree. */
    char target[PATH_MAX];
    join_path(target, sizeof(target), f.primary, "Themes/neon-nights");
    write_theme(target, "neon-nights", "1.1.0");
    write_theme_marker(target, "neon-nights", "1.1.0", THEME_TOKEN);
    CHECK(jw_db_pakrat_upsert_install(f.db, "neon-nights", "1.1.0",
                                      JW_PAKRAT_THEME_PLATFORM, "Themes/neon-nights",
                                      THEME_SHA, NULL, THEME_TOKEN) == 0);
    jw_pakrat_install row;
    CHECK(jw_db_pakrat_get_install(f.db, "neon-nights", &row) == 0);
    CHECK(jw__pakrat_reconcile_transition(&recovery, row.store_id,
                                          row.install_path, &row) == 0);
    CHECK(jw__pakrat_path_exists(target));

    /* An update promoted but never recorded: the running version comes back. */
    char rollback[PATH_MAX];
    CHECK(jw__pakrat_target_sibling_path(target, "neon-nights", "rollback", rollback,
                                         sizeof(rollback)) == 0);
    CHECK(rename(target, rollback) == 0);
    write_theme(target, "neon-nights", "1.2.0");
    write_theme_marker(target, "neon-nights", "1.2.0",
                       "00000000000000000000000000000000");
    CHECK(jw__pakrat_reconcile_transition(&recovery, row.store_id,
                                          row.install_path, &row) == 0);
    char id[128], version[64];
    CHECK(jw__pakrat_read_theme_identity(target, id, sizeof(id), version,
                                         sizeof(version)) == 0 &&
          strcmp(version, "1.1.0") == 0);
    CHECK(!jw__pakrat_path_exists(rollback));

    /* An interrupted first install has a marker and no row: swept. A user's
       copy of an installed theme carries the same marker under another name:
       kept. So is every hand-made folder. */
    char fresh[PATH_MAX], copy[PATH_MAX], hand[PATH_MAX], stage[PATH_MAX];
    join_path(fresh, sizeof(fresh), f.primary, "Themes/fresh-theme");
    join_path(copy, sizeof(copy), f.primary, "Themes/my-copy");
    join_path(hand, sizeof(hand), f.primary, "Themes/Hand Made");
    join_path(stage, sizeof(stage), f.primary, "Themes/.pakrat-stage-gone");
    write_theme(fresh, "fresh-theme", "1.0.0");
    write_theme_marker(fresh, "fresh-theme", "1.0.0", THEME_TOKEN);
    write_theme(copy, "fresh-theme", "1.0.0");
    write_theme_marker(copy, "fresh-theme", "1.0.0", THEME_TOKEN);
    write_theme(hand, "hand-made", "0.1.0");
    write_theme(stage, "gone", "1.0.0");
    CHECK(jw_pakrat_recover_installs(&recovery) == 0);
    CHECK(!jw__pakrat_path_exists(fresh));
    CHECK(jw__pakrat_path_exists(copy));
    CHECK(jw__pakrat_path_exists(hand));
    CHECK(!jw__pakrat_path_exists(stage));
    CHECK(jw__pakrat_path_exists(target));
    fixture_teardown(&f);
}

/* Bundled theme names are refused, case-insensitively, from THEME-1's list and
   from the running release's bundled-themes.txt. Themes/ holds at most
   JW_USER_THEME_MAX folders the launcher will list. */
static void test_theme_policy(void) {
    txn_fixture f;
    fixture_setup(&f);
    char platform_root[PATH_MAX];
    join_path(platform_root, sizeof(platform_root), f.primary,
              ".system/leaf/platforms/mac");
    CHECK(jw__pakrat_mkdir_p(platform_root, 0755) == 0);

    char list[PATH_MAX];
    CHECK(jw_pakrat_bundled_themes_path(platform_root, f.state, list,
                                        sizeof(list)) == 1);
    CHECK(jw_pakrat_theme_name_reserved(platform_root, f.state, "sample") == 1);
    CHECK(jw_pakrat_theme_name_reserved(platform_root, f.state, "SAMPLE") == 1);
    CHECK(jw_pakrat_theme_name_reserved(platform_root, f.state, "aurora") == 0);

    char release[PATH_MAX], bundled[PATH_MAX], expected[PATH_MAX];
    join_path(release, sizeof(release), f.state, "release.json");
    write_file(release,
               "{\"schema\":1,\"version\":\"0.12.0\",\"release_id\":\"leaf-test\"}\n",
               0644);
    join_path(expected, sizeof(expected), f.primary,
              ".system/leaf/releases/leaf-test/bundled-themes.txt");
    CHECK(jw_pakrat_bundled_themes_path(platform_root, f.state, list,
                                        sizeof(list)) == 0 &&
          strcmp(list, expected) == 0);
    /* The release's own list is optional: absent means THEME-1's names only. */
    CHECK(jw_pakrat_theme_name_reserved(platform_root, f.state, "aurora") == 0);
    snprintf(bundled, sizeof(bundled), "%s", expected);
    write_file(bundled, "# shipped with Leaf\nSample\r\nAurora\n\n", 0644);
    CHECK(jw_pakrat_theme_name_reserved(platform_root, f.state, "aurora") == 1);
    CHECK(jw_pakrat_theme_name_reserved(platform_root, f.state, "AURORA") == 1);
    CHECK(jw_pakrat_theme_name_reserved(platform_root, f.state, "neon-nights") == 0);
    CHECK(jw_pakrat_theme_name_listed(bundled, "# shipped with Leaf") == 0);

    CHECK(jw_pakrat_theme_folder_count(f.primary) == 0);
    char path[PATH_MAX];
    for (int i = 0; i < 31; i++) {
        char name[32];
        snprintf(name, sizeof(name), "Themes/theme-%02d", i);
        join_path(path, sizeof(path), f.primary, name);
        CHECK(jw__pakrat_mkdir_p(path, 0755) == 0);
    }
    join_path(path, sizeof(path), f.primary, "Themes/.pakrat-stage-neon-nights");
    CHECK(jw__pakrat_mkdir_p(path, 0755) == 0);
    join_path(path, sizeof(path), f.primary, "Themes/notes.txt");
    write_file(path, "not a theme\n", 0644);
    CHECK(jw_pakrat_theme_folder_count(f.primary) == 31);
    CHECK(!jw_pakrat_theme_slots_full(31, false));
    join_path(path, sizeof(path), f.primary, "Themes/theme-31");
    CHECK(jw__pakrat_mkdir_p(path, 0755) == 0);
    CHECK(jw_pakrat_theme_folder_count(f.primary) == 32);
    CHECK(jw_pakrat_theme_slots_full(32, false));
    CHECK(!jw_pakrat_theme_slots_full(32, true));
    fixture_teardown(&f);
}

/* Absolute path to this test binary, captured before anything can chdir. */
static char g_self[PATH_MAX];

/* Fault children re-exec this binary instead of continuing in the forked
   image.
 *
 * POSIX only permits async-signal-safe calls between fork() and exec() in a
 * process that may be multithreaded, and this one is: the C runtime, and
 * macOS's libsystem_trace in particular, start threads of their own. The
 * child used to call straight into jw_db_open(), and on macOS
 * sqlite3_open() reaches os_signpost_id_make_with_pointer() ->
 * _os_log_preferences_refresh(), which is not fork-safe. When a preferences
 * refresh happened to be due, the child died with SIGSEGV instead of the
 * fault point's _exit(42) -- roughly one run in twenty, and always at a
 * fault point AFTER the first database open, never before it.
 *
 * Re-execing makes the child a fresh process, so the fault points test what
 * they are meant to test -- crash consistency of an irreversible uninstall --
 * rather than the host's fork safety. The environment is set in the PARENT so
 * the child does nothing but execv. */
static pid_t spawn_fault_child(const txn_fixture *f, const char *mode,
                               const char *point) {
    setenv("JW_TXN_TEST_CHILD", mode, 1);
    setenv("JW_TXN_TEST_ROOT", f->root, 1);
    setenv("JW_PAKRAT_FAULT_AT", point, 1);

    pid_t child = fork();
    if (child == 0) {
        char *const argv[] = {g_self, NULL};
        execv(g_self, argv);
        _exit(127);
    }

    unsetenv("JW_TXN_TEST_CHILD");
    unsetenv("JW_TXN_TEST_ROOT");
    unsetenv("JW_PAKRAT_FAULT_AT");
    return child;
}

/* The re-exec'd half of spawn_fault_child. Rebuilds the fixture the parent
   already seeded -- read-only, via the same manifest inspection production
   uses -- then runs the one operation under test. */
static int run_fault_child(const char *mode) {
    const char *root = getenv("JW_TXN_TEST_ROOT");
    if (!root || !root[0]) {
        return 127;
    }
    txn_fixture f;
    fixture_paths(&f, root);

    char target[PATH_MAX];
    join_path(target, sizeof(target), f.primary, "Apps/mac/TXN.pak");
    jw_pakrat_txn_metadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    char reason[JW_SVC_REASON_BUF] = {0};
    if (jw_pakrat_txn_inspect_manifest(target, "pak.json", f.primary_userdata,
                                       "org.umrk.test.txn", "mac/TXN.pak",
                                       &metadata, reason,
                                       sizeof(reason)) != 0) {
        fprintf(stderr, "fault child: manifest invalid: %s\n", reason);
        return 127;
    }

    if (strcmp(mode, "persist") == 0) {
        int rc = jw_pakrat_txn_pending_persist(f.db, "primary", &metadata);
        jw_pakrat_txn_metadata_destroy(&metadata);
        return rc == 0 ? 90 : 91;
    }
    if (strcmp(mode, "complete") == 0) {
        jw_pakrat_pending_uninstall pending;
        int get_rc = jw_pakrat_txn_pending_get(f.db, metadata.store_id,
                                               &pending);
        int complete_rc =
            get_rc == 0 ? jw_pakrat_txn_complete_uninstall(&f.ctx, &pending)
                        : -1;
        if (get_rc == 0) {
            jw_pakrat_pending_uninstall_destroy(&pending);
        }
        jw_pakrat_txn_metadata_destroy(&metadata);
        return complete_rc == 0 ? 90 : 91;
    }
    jw_pakrat_txn_metadata_destroy(&metadata);
    fprintf(stderr, "fault child: unknown mode %s\n", mode);
    return 127;
}

static void expect_fault_exit(pid_t child, const char *point) {
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42) {
        fprintf(stderr, "FAIL fault point %s exited status=%d\n", point,
                status);
        failures++;
    }
}

static void assert_uninstall_records(const txn_fixture *f,
                                     const uninstall_fixture_paths *paths,
                                     long long expected) {
    CHECK(scalar_path(f->db,
                      "SELECT COUNT(*) FROM pakrat_installs;") == expected);
    CHECK(scalar_path(
              f->db,
              "SELECT COUNT(*) FROM pakrat_pending_uninstalls;") == expected);
    CHECK(scalar_path(paths->control,
                      "SELECT COUNT(*) FROM service_control_state;") ==
          expected);
}

static void finish_pending_uninstall(txn_fixture *f) {
    jw_pakrat_pending_uninstall pending;
    CHECK(jw_pakrat_txn_pending_get(
              f->db, "org.umrk.test.txn", &pending) == 0);
    CHECK(jw_pakrat_txn_complete_uninstall(&f->ctx, &pending) == 0);
    jw_pakrat_pending_uninstall_destroy(&pending);
}

static void test_pending_intent_faults(void) {
    static const struct {
        const char *point;
        bool committed;
    } cases[] = {
        {"uninstall-before-intent", false},
        {"uninstall-during-intent", false},
        {"uninstall-after-intent", true},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        txn_fixture f;
        jw_pakrat_txn_metadata metadata;
        uninstall_fixture_paths paths;
        fixture_setup(&f);
        seed_uninstall_fixture(&f, &metadata, &paths, false);
        pid_t child = spawn_fault_child(&f, "persist", cases[i].point);
        CHECK(child >= 0);
        expect_fault_exit(child, cases[i].point);
        CHECK(jw__pakrat_path_exists(paths.target));
        CHECK(jw__pakrat_path_exists(paths.trust));
        CHECK(jw__pakrat_path_exists(paths.history));
        CHECK(scalar_path(
                  f.db,
                  "SELECT COUNT(*) FROM pakrat_pending_uninstalls;") ==
              (cases[i].committed ? 1 : 0));
        if (cases[i].committed) {
            finish_pending_uninstall(&f);
            CHECK(!jw__pakrat_path_exists(paths.target));
            CHECK(!jw__pakrat_path_exists(paths.trust));
            CHECK(jw__pakrat_path_exists(paths.history));
            assert_uninstall_records(&f, &paths, 0);
        } else {
            CHECK(scalar_path(
                      f.db,
                      "SELECT COUNT(*) FROM pakrat_installs;") == 1);
            CHECK(scalar_path(
                      paths.control,
                      "SELECT COUNT(*) FROM service_control_state;") == 1);
        }
        jw_pakrat_txn_metadata_destroy(&metadata);
        fixture_teardown(&f);
    }
}

static void test_pending_intent_idempotence(void) {
    txn_fixture f;
    jw_pakrat_txn_metadata metadata;
    uninstall_fixture_paths paths;
    fixture_setup(&f);
    seed_uninstall_fixture(&f, &metadata, &paths, true);
    CHECK(jw_pakrat_txn_pending_persist(f.db, "primary", &metadata) == 0);
    char original = metadata.display_name[0];
    metadata.display_name[0] = original == 'X' ? 'Y' : 'X';
    CHECK(jw_pakrat_txn_pending_persist(f.db, "primary", &metadata) == -1);
    metadata.display_name[0] = original;
    CHECK(scalar_path(
              f.db,
              "SELECT COUNT(*) FROM pakrat_pending_uninstalls;") == 1);
    finish_pending_uninstall(&f);
    assert_uninstall_records(&f, &paths, 0);
    jw_pakrat_txn_metadata_destroy(&metadata);
    fixture_teardown(&f);
}

static void test_uninstall_completion_faults(void) {
    static const struct {
        const char *point;
        bool trust_present;
        bool package_present;
        bool records_present;
    } cases[] = {
        {"uninstall-before-revoke", true, true, true},
        {"uninstall-after-revoke", false, true, true},
        {"uninstall-before-package-remove", false, true, true},
        {"uninstall-after-package-remove", false, false, true},
        {"uninstall-before-syncfs", false, false, true},
        {"uninstall-after-syncfs", false, false, true},
        {"uninstall-before-final-db", false, false, true},
        {"uninstall-during-final-db", false, false, true},
        {"uninstall-after-final-db", false, false, false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        txn_fixture f;
        jw_pakrat_txn_metadata metadata;
        uninstall_fixture_paths paths;
        fixture_setup(&f);
        seed_uninstall_fixture(&f, &metadata, &paths, true);
        pid_t child = spawn_fault_child(&f, "complete", cases[i].point);
        CHECK(child >= 0);
        expect_fault_exit(child, cases[i].point);
        CHECK(jw__pakrat_path_exists(paths.trust) == cases[i].trust_present);
        CHECK(jw__pakrat_path_exists(paths.target) ==
              cases[i].package_present);
        CHECK(jw__pakrat_path_exists(paths.stage) ==
              cases[i].package_present);
        CHECK(jw__pakrat_path_exists(paths.rollback) ==
              cases[i].package_present);
        CHECK(jw__pakrat_path_exists(paths.origin) ==
              cases[i].package_present);
        CHECK(jw__pakrat_path_exists(paths.history));
        assert_uninstall_records(&f, &paths,
                                 cases[i].records_present ? 1 : 0);
        if (cases[i].records_present) {
            finish_pending_uninstall(&f);
        } else {
            jw_pakrat_pending_uninstall pending;
            CHECK(jw_pakrat_txn_pending_get(
                      f.db, metadata.store_id, &pending) == 1);
        }
        CHECK(!jw__pakrat_path_exists(paths.trust));
        CHECK(!jw__pakrat_path_exists(paths.target));
        CHECK(!jw__pakrat_path_exists(paths.stage));
        CHECK(!jw__pakrat_path_exists(paths.rollback));
        CHECK(!jw__pakrat_path_exists(paths.origin));
        CHECK(jw__pakrat_path_exists(paths.history));
        assert_uninstall_records(&f, &paths, 0);
        jw_pakrat_txn_metadata_destroy(&metadata);
        fixture_teardown(&f);
    }
}

int main(int argc, char **argv) {
    if (argc > 0 && argv[0] && !realpath(argv[0], g_self)) {
        snprintf(g_self, sizeof(g_self), "%s", argv[0]);
    }
    /* Re-exec'd fault child: run one operation and exit, never the suite. */
    const char *child_mode = getenv("JW_TXN_TEST_CHILD");
    if (child_mode && child_mode[0]) {
        return run_fault_child(child_mode);
    }

    txn_fixture fixture;
    fixture_setup(&fixture);
    jw_pakrat_txn_metadata metadata;
    memset(&metadata, 0, sizeof(metadata));
    test_manifest_and_cache(&fixture, &metadata);
    test_lock_death(&fixture);
    test_inventory_and_uninstall(&fixture, &metadata);
    jw_pakrat_txn_metadata_destroy(&metadata);
    fixture_teardown(&fixture);
    test_pending_intent_faults();
    test_pending_intent_idempotence();
    test_uninstall_completion_faults();
    test_content_only_recovery();
    test_theme_install_roots();
    test_theme_uninstall_selection();
    test_theme_recovery();
    test_theme_policy();
    if (failures == 0) {
        puts("PASS pakrat-txn-test");
        return 0;
    }
    fprintf(stderr, "FAIL pakrat-txn-test (%d failures)\n", failures);
    return 1;
}
