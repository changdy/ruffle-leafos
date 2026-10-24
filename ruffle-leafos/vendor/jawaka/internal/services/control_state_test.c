#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/control_state.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void jw__test_mkdtemp_db_path(char *out, size_t out_size) {
    char dir[] = "/tmp/jw-control-state-test.XXXXXX";
    assert(mkdtemp(dir) != NULL);
    int n = snprintf(out, out_size, "%s/control.db", dir);
    assert(n > 0 && (size_t)n < out_size);
}

static void jw__test_sqlite_exec(const char *db_path, const char *sql) {
    sqlite3 *db = NULL;
    assert(sqlite3_open(db_path, &db) == SQLITE_OK);
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "test sqlite exec failed: %s\n",
                errmsg ? errmsg : "(no message)");
    }
    sqlite3_free(errmsg);
    assert(rc == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
}

static void jw__test_missing_row_is_not_found_not_error(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store = NULL;
    char reason[64] = {0};
    assert(jw_svc_control_store_open(db_path, &store, reason, sizeof(reason)));
    assert(store != NULL);

    jw_svc_control_state out;
    bool found = true;
    memset(reason, 0, sizeof(reason));
    assert(jw_svc_control_store_get(store, "org.umrk.nope", &out, &found, reason, sizeof(reason)));
    assert(!found);
    assert(!out.start_with_leaf && !out.session_run);
    assert(reason[0] == '\0');

    jw_svc_control_store_close(store);
    puts("PASS control-state-test a missing row reports not-found, not an error");
}

static void jw__test_round_trip_every_field(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store = NULL;
    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));

    jw_svc_control_state state;
    memset(&state, 0, sizeof(state));
    state.start_with_leaf = true;
    state.session_run = true;
    state.last_transition_at_us = 1234567890123LL;
    strcpy(state.last_transition_reason, "manual-run");
    state.has_last_exit = true;
    state.last_exit_code = 137;
    state.last_exit_at_us = 999888777LL;
    state.restart_count = 4;
    state.breaker_open = false;
    state.backoff_failure_count = 3;
    state.backoff_failure_times_us[0] = 100;
    state.backoff_failure_times_us[1] = 200;
    state.backoff_failure_times_us[2] = 300;
    strcpy(state.installed_package_id, "org.umrk.syncthing");
    strcpy(state.installed_package_version, "1.2.3");

    assert(jw_svc_control_store_put(store, "org.umrk.syncthing", &state, NULL, 0));

    jw_svc_control_state out;
    bool found = false;
    assert(jw_svc_control_store_get(store, "org.umrk.syncthing", &out, &found, NULL, 0));
    assert(found);
    assert(out.start_with_leaf == state.start_with_leaf);
    assert(out.session_run == state.session_run);
    assert(out.last_transition_at_us == state.last_transition_at_us);
    assert(strcmp(out.last_transition_reason, state.last_transition_reason) == 0);
    assert(out.has_last_exit == state.has_last_exit);
    assert(out.last_exit_code == state.last_exit_code);
    assert(out.last_exit_at_us == state.last_exit_at_us);
    assert(out.restart_count == state.restart_count);
    assert(out.breaker_open == state.breaker_open);
    assert(out.backoff_failure_count == state.backoff_failure_count);
    for (int i = 0; i < state.backoff_failure_count; i++) {
        assert(out.backoff_failure_times_us[i] == state.backoff_failure_times_us[i]);
    }
    assert(strcmp(out.installed_package_id, state.installed_package_id) == 0);
    assert(strcmp(out.installed_package_version, state.installed_package_version) == 0);

    jw_svc_control_store_close(store);
    puts("PASS control-state-test round trip preserves every field");
}

static void jw__test_put_overwrites_not_merges(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store = NULL;
    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));

    jw_svc_control_state first;
    memset(&first, 0, sizeof(first));
    first.start_with_leaf = true;
    first.restart_count = 9;
    assert(jw_svc_control_store_put(store, "org.umrk.a", &first, NULL, 0));

    jw_svc_control_state second;
    memset(&second, 0, sizeof(second));
    second.start_with_leaf = false;
    second.restart_count = 0;
    assert(jw_svc_control_store_put(store, "org.umrk.a", &second, NULL, 0));

    jw_svc_control_state out;
    bool found = false;
    assert(jw_svc_control_store_get(store, "org.umrk.a", &out, &found, NULL, 0));
    assert(found);
    assert(!out.start_with_leaf);
    assert(out.restart_count == 0);

    jw_svc_control_store_close(store);
    puts("PASS control-state-test a second put replaces the row instead of merging fields");
}

static void jw__test_bound_text_is_literal_and_rows_are_isolated(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    /* Separate handles exercise the same locking path that separate
     * jawakad/CLI processes would use. */
    jw_svc_control_store *store_a = NULL;
    jw_svc_control_store *store_b = NULL;
    assert(jw_svc_control_store_open(db_path, &store_a, NULL, 0));
    assert(jw_svc_control_store_open(db_path, &store_b, NULL, 0));

    const char *special_id = "org.umrk.quote'; DROP TABLE service_control_state;--";
    jw_svc_control_state special;
    memset(&special, 0, sizeof(special));
    special.start_with_leaf = true;
    strcpy(special.last_transition_reason,
           "manual'; DELETE FROM service_control_state;--");
    strcpy(special.installed_package_id,
           "pkg'; DROP TABLE service_control_state;--");
    strcpy(special.installed_package_version, "1.0';--");
    assert(jw_svc_control_store_put(store_a, special_id, &special, NULL, 0));

    jw_svc_control_state ordinary;
    memset(&ordinary, 0, sizeof(ordinary));
    ordinary.restart_count = 23;
    assert(jw_svc_control_store_put(store_b, "org.umrk.ordinary", &ordinary,
                                    NULL, 0));

    jw_svc_control_state out;
    bool found = false;
    assert(jw_svc_control_store_get(store_b, special_id, &out, &found, NULL, 0));
    assert(found);
    assert(out.start_with_leaf);
    assert(strcmp(out.last_transition_reason, special.last_transition_reason) == 0);
    assert(strcmp(out.installed_package_id, special.installed_package_id) == 0);
    assert(strcmp(out.installed_package_version,
                  special.installed_package_version) == 0);

    found = false;
    assert(jw_svc_control_store_get(store_a, "org.umrk.ordinary", &out, &found,
                                     NULL, 0));
    assert(found);
    assert(out.restart_count == 23);
    assert(out.last_transition_reason[0] == '\0');

    jw_svc_control_store_close(store_b);
    jw_svc_control_store_close(store_a);
    puts("PASS control-state-test bound SQL metacharacters stay literal and rows stay isolated");
}

static void jw__test_maximum_string_lengths_round_trip(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store = NULL;
    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));

    char service_id[JW_SVC_CONTROL_ID_MAX + 1];
    memset(service_id, 's', JW_SVC_CONTROL_ID_MAX);
    service_id[JW_SVC_CONTROL_ID_MAX] = '\0';

    jw_svc_control_state state;
    memset(&state, 0, sizeof(state));
    memset(state.last_transition_reason, 'r', JW_SVC_CONTROL_REASON_MAX);
    state.last_transition_reason[JW_SVC_CONTROL_REASON_MAX] = '\0';
    memset(state.installed_package_id, 'p', JW_SVC_CONTROL_PACKAGE_ID_MAX);
    state.installed_package_id[JW_SVC_CONTROL_PACKAGE_ID_MAX] = '\0';
    memset(state.installed_package_version, 'v',
           JW_SVC_CONTROL_PACKAGE_VERSION_MAX);
    state.installed_package_version[JW_SVC_CONTROL_PACKAGE_VERSION_MAX] = '\0';

    assert(jw_svc_control_store_put(store, service_id, &state, NULL, 0));

    jw_svc_control_state out;
    bool found = false;
    assert(jw_svc_control_store_get(store, service_id, &out, &found, NULL, 0));
    assert(found);
    assert(strcmp(out.last_transition_reason,
                  state.last_transition_reason) == 0);
    assert(strcmp(out.installed_package_id, state.installed_package_id) == 0);
    assert(strcmp(out.installed_package_version,
                  state.installed_package_version) == 0);

    jw_svc_control_store_close(store);
    puts("PASS control-state-test maximum supported string lengths round trip");
}

static void jw__test_clear_all_sessions_only_touches_session_run(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store = NULL;
    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));

    jw_svc_control_state a;
    memset(&a, 0, sizeof(a));
    a.start_with_leaf = true;
    a.session_run = true;
    a.restart_count = 5;
    assert(jw_svc_control_store_put(store, "org.umrk.a", &a, NULL, 0));

    jw_svc_control_state b;
    memset(&b, 0, sizeof(b));
    b.start_with_leaf = false;
    b.session_run = true;
    assert(jw_svc_control_store_put(store, "org.umrk.b", &b, NULL, 0));

    assert(jw_svc_control_store_clear_all_sessions(store, NULL, 0));

    jw_svc_control_state out_a, out_b;
    bool found_a = false, found_b = false;
    assert(jw_svc_control_store_get(store, "org.umrk.a", &out_a, &found_a, NULL, 0));
    assert(jw_svc_control_store_get(store, "org.umrk.b", &out_b, &found_b, NULL, 0));
    assert(found_a && found_b);
    assert(!out_a.session_run && out_a.start_with_leaf && out_a.restart_count == 5);
    assert(!out_b.session_run && !out_b.start_with_leaf);

    jw_svc_control_store_close(store);
    puts("PASS control-state-test clearing sessions resets session_run without touching persistent fields");
}

static void jw__test_reopen_survives_close(void) {
    /* Simulates a daemon restart: close the handle, reopen the same
     * file, and confirm persistent data (but not session data, once the
     * caller clears it) is still there. */
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store1 = NULL;
    assert(jw_svc_control_store_open(db_path, &store1, NULL, 0));

    jw_svc_control_state state;
    memset(&state, 0, sizeof(state));
    state.start_with_leaf = true;
    state.session_run = true;
    state.restart_count = 2;
    strcpy(state.installed_package_id, "org.umrk.persisted");
    assert(jw_svc_control_store_put(store1, "org.umrk.persisted", &state, NULL, 0));
    jw_svc_control_store_close(store1);

    jw_svc_control_store *store2 = NULL;
    assert(jw_svc_control_store_open(db_path, &store2, NULL, 0));
    assert(jw_svc_control_store_clear_all_sessions(store2, NULL, 0));

    jw_svc_control_state out;
    bool found = false;
    assert(jw_svc_control_store_get(store2, "org.umrk.persisted", &out, &found, NULL, 0));
    assert(found);
    assert(out.start_with_leaf);
    assert(!out.session_run);
    assert(out.restart_count == 2);
    assert(strcmp(out.installed_package_id, "org.umrk.persisted") == 0);

    jw_svc_control_store_close(store2);
    puts("PASS control-state-test data survives a close/reopen cycle, session_run only clears when asked");
}

static void jw__test_invalid_arguments(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store = NULL;
    char reason[64] = {0};

    assert(!jw_svc_control_store_open(NULL, &store, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);
    assert(store == NULL);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_open("", &store, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));

    jw_svc_control_state state;
    memset(&state, 0, sizeof(state));

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_get(store, NULL, &state, &(bool){false}, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_get(store, "", &state, &(bool){false}, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    jw_svc_control_id *ids = NULL;
    size_t id_count = 0;
    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_list_ids(NULL, &ids, &id_count,
                                          reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);
    assert(ids == NULL && id_count == 0);
    assert(!jw_svc_control_store_list_ids(store, NULL, &id_count,
                                          reason, sizeof(reason)));
    assert(!jw_svc_control_store_list_ids(store, &ids, NULL,
                                          reason, sizeof(reason)));
    jw_svc_control_store_free_ids(NULL);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_put(store, "org.umrk.a", NULL, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    state.backoff_failure_count = JW_SVC_CONTROL_BACKOFF_TRACKED + 1;
    assert(!jw_svc_control_store_put(store, "org.umrk.a", &state, reason,
                                     sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);
    state.backoff_failure_count = -1;
    assert(!jw_svc_control_store_put(store, "org.umrk.a", &state, reason,
                                     sizeof(reason)));
    state.backoff_failure_count = 0;

    char oversized_id[JW_SVC_CONTROL_ID_MAX + 2];
    memset(oversized_id, 'a', sizeof(oversized_id) - 1);
    oversized_id[sizeof(oversized_id) - 1] = '\0';
    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_put(store, oversized_id, &state, reason,
                                     sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    char unterminated_id[JW_SVC_CONTROL_ID_MAX + 1];
    memset(unterminated_id, 'a', sizeof(unterminated_id));
    assert(!jw_svc_control_store_put(store, unterminated_id, &state, reason,
                                     sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    /* Every string field bound with a -1 byte count must first prove it
     * has a terminator within its fixed array. */
    memset(state.last_transition_reason, 'x', sizeof(state.last_transition_reason));
    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_control_store_put(store, "org.umrk.a", &state, reason,
                                     sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(&state, 0, sizeof(state));
    memset(state.installed_package_id, 'x', sizeof(state.installed_package_id));
    assert(!jw_svc_control_store_put(store, "org.umrk.a", &state, reason,
                                     sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(&state, 0, sizeof(state));
    memset(state.installed_package_version, 'x',
           sizeof(state.installed_package_version));
    assert(!jw_svc_control_store_put(store, "org.umrk.a", &state, reason,
                                     sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    jw_svc_control_store_close(store);
    puts("PASS control-state-test rejects invalid arguments across open/get/list/put");
}

static void jw__test_corrupt_rows_fail_closed(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    /* A deliberately lax lookalike schema simulates a file modified
     * outside this API. It has the expected columns but permits values
     * the owned schema's NOT NULL/CHECK constraints reject. */
    static const char *const kCorruptSql =
        "CREATE TABLE service_control_state ("
        "service_id TEXT PRIMARY KEY,"
        "start_with_leaf INTEGER, session_run INTEGER,"
        "last_transition_at_us INTEGER, last_transition_reason TEXT,"
        "has_last_exit INTEGER, last_exit_code INTEGER,"
        "last_exit_at_us INTEGER, restart_count INTEGER,"
        "breaker_open INTEGER, backoff_failure_count INTEGER,"
        "backoff_failure_time_1_us INTEGER,"
        "backoff_failure_time_2_us INTEGER,"
        "backoff_failure_time_3_us INTEGER,"
        "backoff_failure_time_4_us INTEGER,"
        "backoff_failure_time_5_us INTEGER,"
        "installed_package_id TEXT, installed_package_version TEXT);"
        "INSERT INTO service_control_state VALUES "
        "('org.umrk.null',0,0,0,NULL,0,0,0,0,0,0,0,0,0,0,0,'',''),"
        "('org.umrk.embedded-nul',0,0,0,CAST(X'61620063' AS TEXT),"
        "0,0,0,0,0,0,0,0,0,0,0,'',''),"
        "('org.umrk.long-reason',0,0,0,"
        "'rrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrrr',"
        "0,0,0,0,0,0,0,0,0,0,0,'',''),"
        "('org.umrk.long-package',0,0,0,'',0,0,0,0,0,0,0,0,0,0,0,"
        "'pppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp"
        "pppppppppppppppppppppppppppppppppppppppppppppppppppppppppppp"
        "pppppppp',''),"
        "('org.umrk.long-version',0,0,0,'',0,0,0,0,0,0,0,0,0,0,0,'',"
        "'vvvvvvvvvvvvvvvvvvvvvvvvvvvvvvvv'),"
        "('org.umrk.bad-count',0,0,0,'',0,0,0,0,0,6,0,0,0,0,0,'',''),"
        "('org.umrk.big-int',0,0,0,'',0,0,0,2147483648,0,0,0,0,0,0,0,'','');";
    jw__test_sqlite_exec(db_path, kCorruptSql);

    jw_svc_control_store *store = NULL;
    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));

    static const char *const kIds[] = {
        "org.umrk.null",
        "org.umrk.embedded-nul",
        "org.umrk.long-reason",
        "org.umrk.long-package",
        "org.umrk.long-version",
        "org.umrk.bad-count",
        "org.umrk.big-int",
    };
    for (size_t i = 0; i < sizeof(kIds) / sizeof(kIds[0]); i++) {
        jw_svc_control_state out;
        memset(&out, 0x5a, sizeof(out));
        jw_svc_control_state zero;
        memset(&zero, 0, sizeof(zero));
        bool found = true;
        char reason[32] = {0};
        assert(!jw_svc_control_store_get(store, kIds[i], &out, &found, reason,
                                          sizeof(reason)));
        assert(!found);
        assert(strcmp(reason, "read-failed") == 0);
        assert(memcmp(&out, &zero, sizeof(out)) == 0);
    }

    jw_svc_control_store_close(store);
    puts("PASS control-state-test corrupt NULL, oversized, and out-of-range rows fail closed");
}

static void jw__test_future_schema_is_not_downgraded(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));
    jw__test_sqlite_exec(db_path, "PRAGMA user_version = 3;");

    jw_svc_control_store *store = NULL;
    char reason[32] = {0};
    assert(!jw_svc_control_store_open(db_path, &store, reason, sizeof(reason)));
    assert(store == NULL);
    assert(strcmp(reason, "open-failed") == 0);

    sqlite3 *db = NULL;
    assert(sqlite3_open(db_path, &db) == SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL) ==
           SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 3);
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
    puts("PASS control-state-test a future schema version is rejected without downgrade");
}

static void jw__test_v1_schema_upgrades_without_losing_state(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));

    jw_svc_control_store *store = NULL;
    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));
    jw_svc_control_state state;
    memset(&state, 0, sizeof(state));
    state.start_with_leaf = true;
    state.restart_count = 7;
    assert(jw_svc_control_store_put(store, "org.umrk.keep", &state, NULL, 0));
    jw_svc_control_store_close(store);

    jw__test_sqlite_exec(db_path,
                         "DROP TABLE service_control_migrations;"
                         "PRAGMA user_version = 1;");
    store = NULL;
    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));
    bool found = false;
    jw_svc_control_state out;
    assert(jw_svc_control_store_get(store, "org.umrk.keep", &out, &found,
                                    NULL, 0));
    assert(found && out.start_with_leaf && out.restart_count == 7);
    jw_svc_control_store_close(store);

    sqlite3 *db = NULL;
    assert(sqlite3_open(db_path, &db) == SQLITE_OK);
    sqlite3_stmt *stmt = NULL;
    assert(sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, NULL) ==
           SQLITE_OK);
    assert(sqlite3_step(stmt) == SQLITE_ROW);
    assert(sqlite3_column_int(stmt, 0) == 2);
    assert(sqlite3_finalize(stmt) == SQLITE_OK);
    assert(sqlite3_close(db) == SQLITE_OK);
    puts("PASS control-state-test v1 upgrades to v2 without losing service state");
}

static void jw__test_intent_migration_is_atomic_and_one_time(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));
    jw_svc_control_store *store = NULL;
    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));

    jw_svc_control_state state;
    memset(&state, 0, sizeof(state));
    state.session_run = true;
    state.restart_count = 9;
    snprintf(state.last_transition_reason,
             sizeof(state.last_transition_reason), "%s", "existing");
    assert(jw_svc_control_store_put(store, "org.umrk.sshserver", &state,
                                    NULL, 0));

    bool applied = false;
    assert(jw_svc_control_store_apply_intent_migration(
        store, "release-a-ssh-intent-v1", "org.umrk.sshserver", true,
        &applied, NULL, 0));
    assert(applied);
    bool marker_found = false;
    assert(jw_svc_control_store_has_migration(
        store, "release-a-ssh-intent-v1", &marker_found, NULL, 0));
    assert(marker_found);

    bool found = false;
    jw_svc_control_state out;
    assert(jw_svc_control_store_get(store, "org.umrk.sshserver", &out,
                                    &found, NULL, 0));
    assert(found && out.start_with_leaf && out.session_run);
    assert(out.restart_count == 9);
    assert(strcmp(out.last_transition_reason, "existing") == 0);

    /* Restoring or changing the legacy config after the marker exists cannot
     * reverse the first decision. */
    applied = true;
    assert(jw_svc_control_store_apply_intent_migration(
        store, "release-a-ssh-intent-v1", "org.umrk.sshserver", false,
        &applied, NULL, 0));
    assert(!applied);
    assert(jw_svc_control_store_get(store, "org.umrk.sshserver", &out,
                                    &found, NULL, 0));
    assert(found && out.start_with_leaf);

    /* The marker outlives row deletion, so reinstall/config restore does not
     * resurrect persistent enablement. */
    assert(jw_svc_control_store_delete(store, "org.umrk.sshserver", NULL, 0));
    applied = true;
    assert(jw_svc_control_store_apply_intent_migration(
        store, "release-a-ssh-intent-v1", "org.umrk.sshserver", true,
        &applied, NULL, 0));
    assert(!applied);
    assert(jw_svc_control_store_get(store, "org.umrk.sshserver", &out,
                                    &found, NULL, 0));
    assert(!found);

    jw_svc_control_store_close(store);
    puts("PASS control-state-test intent decision and marker are one-time");
}

static void jw__test_intent_migration_rolls_back_both_writes(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));
    jw_svc_control_store *store = NULL;
    assert(jw_svc_control_store_open(db_path, &store, NULL, 0));

    jw__test_sqlite_exec(
        db_path,
        "CREATE TRIGGER fail_migration_marker "
        "BEFORE INSERT ON service_control_migrations "
        "BEGIN SELECT RAISE(ABORT, 'forced marker failure'); END;");
    bool applied = true;
    char reason[32] = {0};
    assert(!jw_svc_control_store_apply_intent_migration(
        store, "release-a-ssh-intent-v1", "org.umrk.sshserver", true,
        &applied, reason, sizeof(reason)));
    assert(!applied);
    assert(strcmp(reason, "migration-failed") == 0);

    bool found = true;
    jw_svc_control_state out;
    assert(jw_svc_control_store_get(store, "org.umrk.sshserver", &out,
                                    &found, NULL, 0));
    assert(!found);

    jw__test_sqlite_exec(db_path, "DROP TRIGGER fail_migration_marker;");
    assert(jw_svc_control_store_apply_intent_migration(
        store, "release-a-ssh-intent-v1", "org.umrk.sshserver", true,
        &applied, NULL, 0));
    assert(applied);
    jw_svc_control_store_close(store);
    puts("PASS control-state-test failed marker rolls back enablement");
}

static void jw__test_list_ids_is_sorted_and_retained(void) {
    char db_path[256];
    jw__test_mkdtemp_db_path(db_path, sizeof(db_path));
    jw_svc_control_store *store = NULL;
    char reason[32] = {0};
    assert(jw_svc_control_store_open(db_path, &store, reason,
                                     sizeof(reason)));

    jw_svc_control_state state;
    memset(&state, 0, sizeof(state));
    state.start_with_leaf = true;
    assert(jw_svc_control_store_put(store, "org.umrk.zeta", &state,
                                    reason, sizeof(reason)));
    state.start_with_leaf = false;
    assert(jw_svc_control_store_put(store, "org.umrk.alpha", &state,
                                    reason, sizeof(reason)));

    jw_svc_control_id *ids = NULL;
    size_t count = 99;
    assert(jw_svc_control_store_list_ids(store, &ids, &count,
                                         reason, sizeof(reason)));
    assert(count == 2);
    assert(ids != NULL);
    assert(strcmp(ids[0].service_id, "org.umrk.alpha") == 0);
    assert(strcmp(ids[1].service_id, "org.umrk.zeta") == 0);
    jw_svc_control_store_free_ids(ids);

    jw_svc_control_store_close(store);
    puts("PASS control-state-test retained ids enumerate in stable order");
}

static void jw__test_close_null_is_a_no_op(void) {
    jw_svc_control_store_close(NULL);
    puts("PASS control-state-test closing a NULL store is a no-op");
}

int main(void) {
    jw__test_missing_row_is_not_found_not_error();
    jw__test_round_trip_every_field();
    jw__test_put_overwrites_not_merges();
    jw__test_bound_text_is_literal_and_rows_are_isolated();
    jw__test_maximum_string_lengths_round_trip();
    jw__test_clear_all_sessions_only_touches_session_run();
    jw__test_reopen_survives_close();
    jw__test_invalid_arguments();
    jw__test_corrupt_rows_fail_closed();
    jw__test_v1_schema_upgrades_without_losing_state();
    jw__test_future_schema_is_not_downgraded();
    jw__test_intent_migration_is_atomic_and_one_time();
    jw__test_intent_migration_rolls_back_both_writes();
    jw__test_list_ids_is_sorted_and_retained();
    jw__test_close_null_is_a_no_op();
    puts("PASS control-state-test");
    return 0;
}
