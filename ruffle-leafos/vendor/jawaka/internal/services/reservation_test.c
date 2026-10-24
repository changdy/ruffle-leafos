#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/reservation.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JW_TEST_REASON_BUF 64
#define JW_TEST_MAX_SAFE_JSON_INTEGER 9007199254740991LL

static void jw__test_mkdtemp(const char *suffix, char *out, size_t out_size) {
    int n = snprintf(out, out_size, "/tmp/jw-reservation-test-%s.XXXXXX", suffix);
    assert(n > 0 && (size_t)n < out_size);
    assert(mkdtemp(out) != NULL);
}

static void jw__test_make_service_dir(const char *runtime_dir, const char *service_id,
                                      char *dir_out, size_t dir_out_size) {
    char services_dir[700];
    int n = snprintf(services_dir, sizeof(services_dir), "%s/services", runtime_dir);
    assert(n > 0 && (size_t)n < sizeof(services_dir));
    assert(mkdir(services_dir, 0700) == 0);

    n = snprintf(dir_out, dir_out_size, "%s/%s", services_dir, service_id);
    assert(n > 0 && (size_t)n < dir_out_size);
    assert(mkdir(dir_out, 0700) == 0);
}

static void jw__test_reservation_path(const char *service_dir,
                                      char *out, size_t out_size) {
    int n = snprintf(out, out_size, "%s/reservation", service_dir);
    assert(n > 0 && (size_t)n < out_size);
}

static void jw__test_write_record(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);

    size_t len = strlen(text);
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, text + written, len - written);
        assert(n > 0);
        written += (size_t)n;
    }
    assert(close(fd) == 0);
}

static void jw__test_assert_corrupt(const char *runtime_dir,
                                    const char *service_id) {
    jw_svc_reservation out;
    memset(&out, 0xa5, sizeof(out));
    char reason[JW_TEST_REASON_BUF] = {0};
    assert(!jw_svc_reservation_read(runtime_dir, service_id,
                                    &out, reason, sizeof(reason)));
    assert(strcmp(reason, "reservation-corrupt") == 0);

    const unsigned char *out_bytes = (const unsigned char *)&out;
    for (size_t i = 0; i < sizeof(out); i++) {
        assert(out_bytes[i] == 0);
    }
}

static void jw__test_assert_no_temp_files(const char *service_dir) {
    DIR *dir = opendir(service_dir);
    assert(dir != NULL);
    const char prefix[] = ".reservation.tmp.";
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        assert(strncmp(entry->d_name, prefix, sizeof(prefix) - 1U) != 0);
    }
    assert(closedir(dir) == 0);
}

static void jw__test_round_trip(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("round-trip", runtime_dir, sizeof(runtime_dir));
    char service_dir[600];
    jw__test_make_service_dir(runtime_dir, "org.umrk.test", service_dir, sizeof(service_dir));

    jw_svc_reservation written = {
        .pgid = 4242,
        .launch_instant_us = jw_svc_reservation_now_us(),
        .game_policy = JW_SVC_RESERVATION_GAME_NOTIFY,
        .stop_on_storage_change = true,
        .stop_on_suspend = false,
    };

    char reason[JW_TEST_REASON_BUF] = {0};
    assert(jw_svc_reservation_write(runtime_dir, "org.umrk.test", &written,
                                    reason, sizeof(reason)));

    /* A successful rename must leave no uniquely named temporary behind,
     * and the promoted inode must retain its owner-only mode. */
    jw__test_assert_no_temp_files(service_dir);
    char reservation_path[700];
    jw__test_reservation_path(service_dir, reservation_path,
                              sizeof(reservation_path));
    struct stat st;
    assert(lstat(reservation_path, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert((st.st_mode & 0777) == 0600);

    jw_svc_reservation read_back;
    memset(reason, 0, sizeof(reason));
    assert(jw_svc_reservation_read(runtime_dir, "org.umrk.test", &read_back,
                                   reason, sizeof(reason)));
    assert(read_back.pgid == written.pgid);
    assert(read_back.launch_instant_us == written.launch_instant_us);
    assert(read_back.game_policy == written.game_policy);
    assert(read_back.stop_on_storage_change == written.stop_on_storage_change);
    assert(read_back.stop_on_suspend == written.stop_on_suspend);

    puts("PASS reservation-test round trip preserves every field");
}

static void jw__test_numeric_precision_boundary(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("precision", runtime_dir, sizeof(runtime_dir));
    char service_dir[600];
    jw__test_make_service_dir(runtime_dir, "org.umrk.test",
                              service_dir, sizeof(service_dir));

    jw_svc_reservation written = {
        .pgid = (pid_t)INT_MAX,
        .launch_instant_us = JW_TEST_MAX_SAFE_JSON_INTEGER,
        .game_policy = JW_SVC_RESERVATION_GAME_STOP,
        .stop_on_storage_change = true,
        .stop_on_suspend = true,
    };
    char reason[JW_TEST_REASON_BUF] = {0};
    assert(jw_svc_reservation_write(runtime_dir, "org.umrk.test", &written,
                                    reason, sizeof(reason)));

    jw_svc_reservation read_back;
    assert(jw_svc_reservation_read(runtime_dir, "org.umrk.test", &read_back,
                                   reason, sizeof(reason)));
    assert(read_back.pgid == written.pgid);
    assert(read_back.launch_instant_us == written.launch_instant_us);

    jw_svc_reservation too_large = written;
    too_large.launch_instant_us = JW_TEST_MAX_SAFE_JSON_INTEGER + 1LL;
    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_write(runtime_dir, "org.umrk.test", &too_large,
                                     reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    puts("PASS reservation-test JSON-number precision boundary is lossless and enforced");
}

static void jw__test_every_game_policy_round_trips(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("policies", runtime_dir, sizeof(runtime_dir));
    char service_dir[600];
    jw__test_make_service_dir(runtime_dir, "org.umrk.test", service_dir, sizeof(service_dir));

    jw_svc_reservation_game_policy policies[] = {
        JW_SVC_RESERVATION_GAME_IGNORE,
        JW_SVC_RESERVATION_GAME_STOP,
        JW_SVC_RESERVATION_GAME_NOTIFY,
    };
    for (size_t i = 0; i < sizeof(policies) / sizeof(policies[0]); i++) {
        jw_svc_reservation written = {
            .pgid = 100 + (pid_t)i,
            .launch_instant_us = jw_svc_reservation_now_us(),
            .game_policy = policies[i],
            .stop_on_storage_change = (i % 2) == 0,
            .stop_on_suspend = (i % 2) == 1,
        };
        char reason[JW_TEST_REASON_BUF] = {0};
        assert(jw_svc_reservation_write(runtime_dir, "org.umrk.test", &written,
                                        reason, sizeof(reason)));

        jw_svc_reservation read_back;
        assert(jw_svc_reservation_read(runtime_dir, "org.umrk.test", &read_back,
                                       reason, sizeof(reason)));
        assert(read_back.game_policy == written.game_policy);
        assert(read_back.pgid == written.pgid);
        assert(read_back.stop_on_storage_change == written.stop_on_storage_change);
        assert(read_back.stop_on_suspend == written.stop_on_suspend);
    }

    puts("PASS reservation-test every game policy round-trips, replacement overwrites cleanly");
}

static void jw__test_missing_vs_corrupt(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("missing-corrupt", runtime_dir, sizeof(runtime_dir));
    char service_dir[600];
    jw__test_make_service_dir(runtime_dir, "org.umrk.test", service_dir, sizeof(service_dir));

    jw_svc_reservation out;
    char reason[JW_TEST_REASON_BUF] = {0};
    assert(!jw_svc_reservation_read(runtime_dir, "org.umrk.test", &out, reason, sizeof(reason)));
    assert(strcmp(reason, "reservation-missing") == 0);
    /* *out must be zeroed on failure, not left with garbage a careless
     * caller might mistake for a real record. */
    assert(out.pgid == 0);

    char reservation_path[700];
    jw__test_reservation_path(service_dir, reservation_path,
                              sizeof(reservation_path));
    static const char *bad_records[] = {
        "",
        "{not json",
        "{\"pgid\":4242}",
        "{\"pgid\":4242,\"launch_instant_us\":1,\"game\":\"sleep\","
            "\"stop_on_storage_change\":false,\"stop_on_suspend\":false}",
        "{\"pgid\":4242,\"launch_instant_us\":1,"
            "\"game\":\"ignore\\u0000suffix\","
            "\"stop_on_storage_change\":false,\"stop_on_suspend\":false}",
        "{\"pgid\\u0000suffix\":4242,\"launch_instant_us\":1,"
            "\"game\":\"ignore\",\"stop_on_storage_change\":false,"
            "\"stop_on_suspend\":false}",
        "{\"pgid\":true,\"launch_instant_us\":1,\"game\":\"ignore\","
            "\"stop_on_storage_change\":false,\"stop_on_suspend\":false}",
        "{\"pgid\":1.5,\"launch_instant_us\":1,\"game\":\"ignore\","
            "\"stop_on_storage_change\":false,\"stop_on_suspend\":false}",
        "{\"pgid\":4242,\"launch_instant_us\":false,\"game\":\"ignore\","
            "\"stop_on_storage_change\":false,\"stop_on_suspend\":false}",
        "{\"pgid\":4242,\"launch_instant_us\":1.5,\"game\":\"ignore\","
            "\"stop_on_storage_change\":false,\"stop_on_suspend\":false}",
        "{\"pgid\":4242,\"launch_instant_us\":-1,\"game\":\"ignore\","
            "\"stop_on_storage_change\":false,\"stop_on_suspend\":false}",
        "{\"pgid\":4242,\"launch_instant_us\":9007199254740992,"
            "\"game\":\"ignore\",\"stop_on_storage_change\":false,"
            "\"stop_on_suspend\":false}",
        "{\"pgid\":4242,\"launch_instant_us\":1,\"game\":\"ignore\","
            "\"stop_on_storage_change\":0,\"stop_on_suspend\":false}",
        "{\"pgid\":4242,\"launch_instant_us\":1,\"game\":\"ignore\","
            "\"stop_on_storage_change\":false,\"stop_on_suspend\":\"false\"}",
        "{\"pgid\":4242,\"launch_instant_us\":1,\"game\":\"ignore\","
            "\"stop_on_storage_change\":false,\"stop_on_suspend\":false,"
            "\"extra\":true}",
        "{\"pgid\":4242,\"pgid\":4243,\"launch_instant_us\":1,"
            "\"game\":\"ignore\",\"stop_on_storage_change\":false,"
            "\"stop_on_suspend\":false}",
        "{\"pgid\":4242,\"launch_instant_us\":1,\"game\":\"ignore\","
            "\"stop_on_storage_change\":false,\"stop_on_suspend\":false}"
            " trailing-garbage",
        "[4242,1,\"ignore\",false,false]",
    };
    for (size_t i = 0; i < sizeof(bad_records) / sizeof(bad_records[0]); i++) {
        jw__test_write_record(reservation_path, bad_records[i]);
        jw__test_assert_corrupt(runtime_dir, "org.umrk.test");
    }

    char oversized[4098];
    memset(oversized, ' ', sizeof(oversized) - 1U);
    oversized[sizeof(oversized) - 1U] = '\0';
    jw__test_write_record(reservation_path, oversized);
    jw__test_assert_corrupt(runtime_dir, "org.umrk.test");

    /* Strict parsing still accepts legal trailing JSON whitespace. */
    static const char *valid_with_whitespace =
        "{\"pgid\":4242,\"launch_instant_us\":1,\"game\":\"ignore\","
        "\"stop_on_storage_change\":false,\"stop_on_suspend\":true}\n\t ";
    jw__test_write_record(reservation_path, valid_with_whitespace);
    memset(reason, 0, sizeof(reason));
    assert(jw_svc_reservation_read(runtime_dir, "org.umrk.test",
                                   &out, reason, sizeof(reason)));
    assert(out.pgid == 4242);
    assert(out.stop_on_suspend);

    puts("PASS reservation-test distinguishes missing from corrupt, "
         "rejects wrong types, values, shape, duplicates, and trailing garbage");
}

static void jw__test_write_without_service_dir(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("no-service-dir", runtime_dir, sizeof(runtime_dir));

    jw_svc_reservation reservation = {
        .pgid = 4242,
        .launch_instant_us = jw_svc_reservation_now_us(),
        .game_policy = JW_SVC_RESERVATION_GAME_IGNORE,
        .stop_on_storage_change = false,
        .stop_on_suspend = false,
    };
    char reason[JW_TEST_REASON_BUF] = {0};
    assert(!jw_svc_reservation_write(runtime_dir, "org.umrk.test", &reservation,
                                     reason, sizeof(reason)));
    assert(strcmp(reason, "service-dir-unavailable") == 0);

    char services_path[600];
    int n = snprintf(services_path, sizeof(services_path),
                     "%s/services", runtime_dir);
    assert(n > 0 && (size_t)n < sizeof(services_path));
    struct stat st;
    assert(lstat(services_path, &st) != 0);

    puts("PASS reservation-test refuses to write without an existing service directory");
}

static void jw__test_atomic_failure_cleans_temp(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("atomic-failure", runtime_dir, sizeof(runtime_dir));
    char service_dir[600];
    jw__test_make_service_dir(runtime_dir, "org.umrk.test",
                              service_dir, sizeof(service_dir));

    char reservation_path[700];
    jw__test_reservation_path(service_dir, reservation_path,
                              sizeof(reservation_path));
    /* renameat() cannot replace a directory with the completed temp file. */
    assert(mkdir(reservation_path, 0700) == 0);

    jw_svc_reservation reservation = {
        .pgid = 4242,
        .launch_instant_us = jw_svc_reservation_now_us(),
        .game_policy = JW_SVC_RESERVATION_GAME_IGNORE,
        .stop_on_storage_change = false,
        .stop_on_suspend = false,
    };
    char reason[JW_TEST_REASON_BUF] = {0};
    assert(!jw_svc_reservation_write(runtime_dir, "org.umrk.test",
                                     &reservation, reason, sizeof(reason)));
    assert(strcmp(reason, "write-failed") == 0);
    jw__test_assert_no_temp_files(service_dir);

    assert(rmdir(reservation_path) == 0);
    assert(jw_svc_reservation_write(runtime_dir, "org.umrk.test",
                                    &reservation, reason, sizeof(reason)));

    puts("PASS reservation-test cleans its unique temp after a pre-rename failure");
}

static void jw__test_non_regular_records_are_corrupt(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("non-regular", runtime_dir, sizeof(runtime_dir));
    char service_dir[600];
    jw__test_make_service_dir(runtime_dir, "org.umrk.test",
                              service_dir, sizeof(service_dir));

    char reservation_path[700];
    jw__test_reservation_path(service_dir, reservation_path,
                              sizeof(reservation_path));
    char external_path[700];
    int n = snprintf(external_path, sizeof(external_path),
                     "%s/external-record", runtime_dir);
    assert(n > 0 && (size_t)n < sizeof(external_path));
    static const char *valid =
        "{\"pgid\":4242,\"launch_instant_us\":1,\"game\":\"ignore\","
        "\"stop_on_storage_change\":false,\"stop_on_suspend\":false}";
    jw__test_write_record(external_path, valid);

    assert(symlink(external_path, reservation_path) == 0);
    jw__test_assert_corrupt(runtime_dir, "org.umrk.test");
    assert(unlink(reservation_path) == 0);

    assert(symlink("does-not-exist", reservation_path) == 0);
    jw__test_assert_corrupt(runtime_dir, "org.umrk.test");
    assert(unlink(reservation_path) == 0);

    assert(mkfifo(reservation_path, 0600) == 0);
    jw__test_assert_corrupt(runtime_dir, "org.umrk.test");
    assert(unlink(reservation_path) == 0);

    assert(mkdir(reservation_path, 0700) == 0);
    jw__test_assert_corrupt(runtime_dir, "org.umrk.test");

    puts("PASS reservation-test rejects symlink, dangling-symlink, FIFO, and directory records");
}

static void jw__test_intermediate_symlink_is_rejected(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("services-symlink", runtime_dir, sizeof(runtime_dir));
    char target_dir[512];
    jw__test_mkdtemp("services-target", target_dir, sizeof(target_dir));

    char target_service_dir[600];
    int n = snprintf(target_service_dir, sizeof(target_service_dir),
                     "%s/org.umrk.test", target_dir);
    assert(n > 0 && (size_t)n < sizeof(target_service_dir));
    assert(mkdir(target_service_dir, 0700) == 0);

    char services_link[600];
    n = snprintf(services_link, sizeof(services_link), "%s/services", runtime_dir);
    assert(n > 0 && (size_t)n < sizeof(services_link));
    assert(symlink(target_dir, services_link) == 0);

    jw_svc_reservation reservation = {
        .pgid = 4242,
        .launch_instant_us = jw_svc_reservation_now_us(),
        .game_policy = JW_SVC_RESERVATION_GAME_IGNORE,
        .stop_on_storage_change = false,
        .stop_on_suspend = false,
    };
    char reason[JW_TEST_REASON_BUF] = {0};
    assert(!jw_svc_reservation_write(runtime_dir, "org.umrk.test",
                                     &reservation, reason, sizeof(reason)));
    assert(strcmp(reason, "service-dir-unavailable") == 0);
    jw__test_assert_corrupt(runtime_dir, "org.umrk.test");

    puts("PASS reservation-test refuses an intermediate services-directory symlink");
}

static void jw__test_path_too_long(void) {
    char too_long[PATH_MAX + 1U];
    memset(too_long, 'x', sizeof(too_long) - 1U);
    too_long[sizeof(too_long) - 1U] = '\0';

    jw_svc_reservation reservation = {
        .pgid = 4242,
        .launch_instant_us = 1,
        .game_policy = JW_SVC_RESERVATION_GAME_IGNORE,
        .stop_on_storage_change = false,
        .stop_on_suspend = false,
    };
    char reason[JW_TEST_REASON_BUF] = {0};
    assert(!jw_svc_reservation_write(too_long, "org.umrk.test",
                                     &reservation, reason, sizeof(reason)));
    assert(strcmp(reason, "path-too-long") == 0);

    jw_svc_reservation out;
    memset(&out, 0xa5, sizeof(out));
    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_read(too_long, "org.umrk.test",
                                    &out, reason, sizeof(reason)));
    assert(strcmp(reason, "path-too-long") == 0);
    assert(out.pgid == 0);

    puts("PASS reservation-test reports PATH_MAX overflow without touching the filesystem");
}

static void jw__test_invalid_arguments(void) {
    char runtime_dir[512];
    jw__test_mkdtemp("invalid-args", runtime_dir, sizeof(runtime_dir));
    char service_dir[600];
    jw__test_make_service_dir(runtime_dir, "org.umrk.test", service_dir, sizeof(service_dir));

    jw_svc_reservation reservation = {
        .pgid = 4242,
        .launch_instant_us = jw_svc_reservation_now_us(),
        .game_policy = JW_SVC_RESERVATION_GAME_IGNORE,
        .stop_on_storage_change = false,
        .stop_on_suspend = false,
    };
    char reason[JW_TEST_REASON_BUF] = {0};

    assert(!jw_svc_reservation_write(runtime_dir, "a/b", &reservation, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_write(runtime_dir, "..", &reservation, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    jw_svc_reservation zero_pgid = reservation;
    zero_pgid.pgid = 0;
    assert(!jw_svc_reservation_write(runtime_dir, "org.umrk.test", &zero_pgid,
                                     reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    jw_svc_reservation negative_instant = reservation;
    negative_instant.launch_instant_us = -1;
    assert(!jw_svc_reservation_write(runtime_dir, "org.umrk.test",
                                     &negative_instant, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    jw_svc_reservation unknown_policy = reservation;
    unknown_policy.game_policy = (jw_svc_reservation_game_policy)99;
    assert(!jw_svc_reservation_write(runtime_dir, "org.umrk.test",
                                     &unknown_policy, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_write(runtime_dir, "org.umrk.test", NULL,
                                     reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_write(NULL, "org.umrk.test", &reservation,
                                     reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    jw_svc_reservation out;
    memset(&out, 0xa5, sizeof(out));
    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_read(runtime_dir, "a/b", &out, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);
    assert(out.pgid == 0);

    memset(reason, 0, sizeof(reason));
    assert(!jw_svc_reservation_read(runtime_dir, "org.umrk.test",
                                    NULL, reason, sizeof(reason)));
    assert(strcmp(reason, "invalid-arguments") == 0);

    assert(jw_svc_reservation_now_us() >= 0);

    puts("PASS reservation-test rejects invalid arguments on both write and read");
}

int main(void) {
    jw__test_round_trip();
    jw__test_numeric_precision_boundary();
    jw__test_every_game_policy_round_trips();
    jw__test_missing_vs_corrupt();
    jw__test_write_without_service_dir();
    jw__test_atomic_failure_cleans_temp();
    jw__test_non_regular_records_are_corrupt();
    jw__test_intermediate_symlink_is_rejected();
    jw__test_path_too_long();
    jw__test_invalid_arguments();
    puts("PASS reservation-test");
    return 0;
}
