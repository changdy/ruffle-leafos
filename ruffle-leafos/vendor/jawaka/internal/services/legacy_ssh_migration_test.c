#define _GNU_SOURCE
#include "internal/services/legacy_ssh_migration.h"

#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

typedef struct {
    char root[PATH_MAX];
    char db_path[PATH_MAX];
    char config_path[PATH_MAX];
    jw_svc_control_store *store;
} jw__fixture;

static void jw__fixture_open(jw__fixture *fixture) {
    memset(fixture, 0, sizeof(*fixture));
    snprintf(fixture->root, sizeof(fixture->root),
             "%s", "/tmp/jw-legacy-ssh-intent.XXXXXX");
    assert(mkdtemp(fixture->root));
    assert(snprintf(fixture->db_path, sizeof(fixture->db_path), "%s/control.db",
                    fixture->root) < (int)sizeof(fixture->db_path));
    assert(snprintf(fixture->config_path, sizeof(fixture->config_path),
                    "%s/config.ini", fixture->root) <
           (int)sizeof(fixture->config_path));
    assert(jw_svc_control_store_open(fixture->db_path, &fixture->store,
                                     NULL, 0));
}

static void jw__write_bytes(const char *path, const void *bytes, size_t length) {
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(bytes, 1, length, file) == length);
    assert(fclose(file) == 0);
}

static void jw__write_text(const char *path, const char *text) {
    jw__write_bytes(path, text, strlen(text));
}

static bool jw__desired(jw__fixture *fixture, bool *found) {
    jw_svc_control_state state;
    assert(jw_svc_control_store_get(fixture->store,
                                    JW_SVC_LEGACY_SSH_SERVICE_ID,
                                    &state, found, NULL, 0));
    return state.start_with_leaf;
}

static void jw__test_valid_plaintext_enables_once(void) {
    jw__fixture fixture;
    jw__fixture_open(&fixture);
    jw__write_text(
        fixture.config_path,
        "username=sshadmin\n"
        "password=legacy-test-only\n"
        "bind_address=0.0.0.0:2222\n"
        "start_dir=/mnt/sdcard\n"
        "last_applied_username=sshadmin\n");

    jw_svc_legacy_ssh_migration_report report;
    assert(jw_svc_migrate_legacy_ssh_intent(fixture.store,
                                             fixture.config_path, &report,
                                             NULL, 0));
    assert(report.applied && report.enabled && report.config_present &&
           report.config_valid);
    bool found = false;
    assert(jw__desired(&fixture, &found) && found);

    char unavailable[PATH_MAX + 64];
    memset(unavailable, 'x', sizeof(unavailable));
    unavailable[0] = '/';
    unavailable[sizeof(unavailable) - 1u] = '\0';
    assert(jw_svc_migrate_legacy_ssh_intent(fixture.store, unavailable,
                                             &report, NULL, 0));
    assert(!report.applied && !report.enabled && !report.config_present &&
           !report.config_valid);
    assert(jw__desired(&fixture, &found) && found);
    jw_svc_control_store_close(fixture.store);
    puts("PASS legacy-ssh-migration-test configured install enables once without rereading config");
}

static void jw__test_clean_install_stays_disabled_after_restore(void) {
    jw__fixture fixture;
    jw__fixture_open(&fixture);
    jw_svc_legacy_ssh_migration_report report;
    assert(jw_svc_migrate_legacy_ssh_intent(fixture.store,
                                             fixture.config_path, &report,
                                             NULL, 0));
    assert(report.applied && !report.enabled && !report.config_present &&
           !report.config_valid);
    bool found = false;
    assert(!jw__desired(&fixture, &found) && found);

    jw__write_text(
        fixture.config_path,
        "username=sshadmin\n"
        "password=restored-later\n"
        "port=2222\n"
        "start_dir=/mnt/sdcard\n");
    assert(jw_svc_migrate_legacy_ssh_intent(fixture.store,
                                             fixture.config_path, &report,
                                             NULL, 0));
    assert(!report.applied && !report.enabled && !report.config_present &&
           !report.config_valid);
    assert(!jw__desired(&fixture, &found) && found);
    jw_svc_control_store_close(fixture.store);
    puts("PASS legacy-ssh-migration-test config restore cannot re-enable");
}

static void jw__test_hash_and_key_only_configs_are_valid(void) {
    jw__fixture hashed;
    jw__fixture_open(&hashed);
    jw__write_text(
        hashed.config_path,
        "username=sshadmin\n"
        "password_hash=$6$0123456789abcdef$0123456789abcdef\n"
        "password_configured=true\n"
        "password_auth_enabled=true\n"
        "bind_address=0.0.0.0:2222\n"
        "start_dir=/mnt/sdcard\n");
    jw_svc_legacy_ssh_migration_report report;
    assert(jw_svc_migrate_legacy_ssh_intent(hashed.store,
                                             hashed.config_path, &report,
                                             NULL, 0));
    assert(report.applied && report.enabled && report.config_valid);
    jw_svc_control_store_close(hashed.store);

    jw__fixture key_only;
    jw__fixture_open(&key_only);
    jw__write_text(
        key_only.config_path,
        "username=sshadmin\n"
        "password_hash=\n"
        "password_configured=false\n"
        "password_auth_enabled=false\n"
        "bind_address=0.0.0.0:2222\n"
        "start_dir=/mnt/sdcard\n");
    assert(jw_svc_migrate_legacy_ssh_intent(key_only.store,
                                             key_only.config_path, &report,
                                             NULL, 0));
    assert(report.applied && report.enabled && report.config_valid);
    jw_svc_control_store_close(key_only.store);
    puts("PASS legacy-ssh-migration-test hash and key-only configs enable");
}

static void jw__test_invalid_files_complete_disabled(void) {
    static const char *const invalid[] = {
        "",
        "username=sshadmin\npassword=x\nbind_address=bad\nstart_dir=/mnt/sdcard\n",
        "username=sshadmin\npassword=x\nbind_address=0.0.0.0:2222\nstart_dir=relative\n",
        "username=sshadmin\nbind_address=0.0.0.0:2222\nstart_dir=/mnt/sdcard\n",
        "username=bad user\npassword=x\nport=2222\nstart_dir=/mnt/sdcard\n",
        "username=sshadmin\npassword=x\nport=2222\nstart_dir=/mnt/sdcard\npassword_auth_enabled=maybe\n",
    };
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        jw__fixture fixture;
        jw__fixture_open(&fixture);
        jw__write_text(fixture.config_path, invalid[i]);
        jw_svc_legacy_ssh_migration_report report;
        assert(jw_svc_migrate_legacy_ssh_intent(fixture.store,
                                                 fixture.config_path, &report,
                                                 NULL, 0));
        assert(report.applied && !report.enabled && report.config_present &&
               !report.config_valid);
        bool found = false;
        assert(!jw__desired(&fixture, &found) && found);
        jw_svc_control_store_close(fixture.store);
    }

    jw__fixture binary;
    jw__fixture_open(&binary);
    static const char bytes[] =
        "username=sshadmin\npassword=x\0port=2222\nstart_dir=/mnt/sdcard\n";
    jw__write_bytes(binary.config_path, bytes, sizeof(bytes) - 1u);
    jw_svc_legacy_ssh_migration_report report;
    assert(jw_svc_migrate_legacy_ssh_intent(binary.store,
                                             binary.config_path, &report,
                                             NULL, 0));
    assert(report.applied && !report.enabled && !report.config_valid);
    jw_svc_control_store_close(binary.store);
    puts("PASS legacy-ssh-migration-test invalid files complete disabled");
}

static void jw__test_read_error_does_not_mark_complete(void) {
    jw__fixture fixture;
    jw__fixture_open(&fixture);
    char too_long[PATH_MAX + 64];
    memset(too_long, 'x', sizeof(too_long));
    too_long[0] = '/';
    too_long[sizeof(too_long) - 1u] = '\0';
    jw_svc_legacy_ssh_migration_report report;
    char reason[32] = {0};
    assert(!jw_svc_migrate_legacy_ssh_intent(fixture.store, too_long,
                                              &report, reason,
                                              sizeof(reason)));
    assert(strcmp(reason, "config-read-failed") == 0);

    assert(jw_svc_migrate_legacy_ssh_intent(fixture.store,
                                             fixture.config_path, &report,
                                             NULL, 0));
    assert(report.applied && !report.enabled);
    jw_svc_control_store_close(fixture.store);
    puts("PASS legacy-ssh-migration-test read errors retry without a marker");
}

int main(void) {
    jw__test_valid_plaintext_enables_once();
    jw__test_clean_install_stays_disabled_after_restore();
    jw__test_hash_and_key_only_configs_are_valid();
    jw__test_invalid_files_complete_disabled();
    jw__test_read_error_does_not_mark_complete();
    puts("PASS legacy-ssh-migration-test");
    return 0;
}
