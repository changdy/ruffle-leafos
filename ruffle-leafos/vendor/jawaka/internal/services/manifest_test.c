/* mkdtemp() is hidden by glibc under a bare -std=c11. Without this it was
 * implicitly declared, so gcc assumed it returned `int` and TRUNCATED the
 * returned pointer to 32 bits -- an immediate SEGV inside the first
 * snprintf("%s", dir) on Linux. The failure was invisible until manifest.c
 * itself started compiling there. Must precede every #include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/manifest.h"

#include "cJSON.h"

#include <assert.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Two fast, dependency-free sanity checks that don't need the sibling
 * umrk-workspace checkout, so this test still proves the library works
 * even when that checkout is absent. The real coverage is the fixture walk
 * below. */

static char *jw__test_read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)size, fp);
    buf[n] = '\0';
    fclose(fp);
    return buf;
}

static void jw__test_write_file(const char *path, const char *content) {
    FILE *fp = fopen(path, "wb");
    assert(fp);
    fwrite(content, 1, strlen(content), fp);
    fclose(fp);
}

static void jw__test_expect_rejected(const char *json,
                                     const char *pak_root,
                                     const char *userdata_root,
                                     const char *expected_reason) {
    jw_service_manifest m;
    char reason[JW_SVC_REASON_BUF] = {0};
    bool ok = jw_service_manifest_validate(json, pak_root, userdata_root,
                                           &m, reason, sizeof(reason));
    if (ok || strcmp(reason, expected_reason) != 0) {
        fprintf(stderr, "FAIL inline rejection: expected %s, got ok=%d reason=%s\n",
                expected_reason, ok, reason);
        if (ok) {
            jw_service_manifest_destroy(&m);
        }
        assert(false);
    }
}

static void jw__test_inline_sanity(void) {
    char tmpl[] = "/tmp/jw-svc-manifest-test.XXXXXX";
    char *dir = mkdtemp(tmpl);
    assert(dir);

    char bin_path[512];
    snprintf(bin_path, sizeof(bin_path), "%s/svc", dir);
    jw__test_write_file(bin_path, "#!/bin/sh\nexit 0\n");
    chmod(bin_path, 0755);

    char pak_json_path[512];
    snprintf(pak_json_path, sizeof(pak_json_path), "%s/pak.json", dir);
    jw__test_write_file(pak_json_path,
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"svc\"},\"restart\":\"no\",\"default_enabled\":false}}");

    char *json = jw__test_read_file(pak_json_path);
    assert(json);

    jw_service_manifest m;
    char reason[JW_SVC_REASON_BUF];
    bool ok = jw_service_manifest_validate(json, dir, NULL, &m, reason, sizeof(reason));
    assert(ok);
    assert(strcmp(m.id, "org.umrk.test") == 0);
    assert(m.stop_grace_ms == 5000); /* absent -> default */
    jw_service_manifest_destroy(&m);
    free(json);

    /* Same manifest, but run.path is now absolute -- must be rejected with
     * that specific reason, and the containing directory needs no changes
     * on disk for this since the string check runs before any filesystem
     * access. */
    char *bad_json = strdup(
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"/etc/passwd\"},\"restart\":\"no\",\"default_enabled\":false}}");
    ok = jw_service_manifest_validate(bad_json, dir, NULL, &m, reason, sizeof(reason));
    assert(!ok);
    assert(strcmp(reason, "absolute-run-path") == 0);
    free(bad_json);

    /* A leaf symlink whose first target is confined but whose second hop
     * escapes must not inherit stat()/access() success from outside the pak. */
    char outside_tmpl[] = "/tmp/jw-svc-manifest-outside.XXXXXX";
    char *outside_dir = mkdtemp(outside_tmpl);
    assert(outside_dir);
    char outside_bin[512], hop_a[512], hop_b[512];
    snprintf(outside_bin, sizeof(outside_bin), "%s/outside-svc", outside_dir);
    snprintf(hop_a, sizeof(hop_a), "%s/hop-a", dir);
    snprintf(hop_b, sizeof(hop_b), "%s/hop-b", dir);
    jw__test_write_file(outside_bin, "#!/bin/sh\nexit 0\n");
    chmod(outside_bin, 0755);
    assert(symlink("hop-b", hop_a) == 0);
    assert(symlink(outside_bin, hop_b) == 0);
    jw__test_expect_rejected(
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"hop-a\"},\"restart\":\"no\",\"default_enabled\":false}}",
        dir, dir, "escaping-symlink");

    jw__test_expect_rejected(
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"svc\"},\"restart\":\"no\",\"default_enabled\":false},"
        "\"state\":{\"root\":\"../../escape\",\"revoke_on_uninstall\":[\"token\"]}}",
        dir, dir, "invalid-state-root");

    jw__test_expect_rejected(
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"svc\",\"args\":[42]},\"restart\":\"no\","
        "\"default_enabled\":false}}",
        dir, dir, "invalid-json-shape");

    jw__test_expect_rejected(
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"svc\"},\"restart\":\"no\",\"default_enabled\":false,"
        "\"lifecycle\":{\"stop_on_suspend\":\"yes\"}}}",
        dir, dir, "invalid-json-shape");

    jw__test_expect_rejected(
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"svc\"},\"restart\":\"no\",\"default_enabled\":false},"
        "\"state\":{\"root\":\"State\",\"retained_roots\":[false]}}",
        dir, dir, "invalid-json-shape");

    jw__test_expect_rejected(
        "{\"ID\":\"org.umrk.test\",\"SERVICE\":{\"SCHEMA\":1,\"ID\":\"org.umrk.test\","
        "\"RUN\":{\"PATH\":\"svc\"},\"RESTART\":\"no\",\"DEFAULT_ENABLED\":false}}",
        dir, dir, "missing-service");

    jw__test_expect_rejected(
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"svc\"},\"restart\":\"no\",\"default_enabled\":false,"
        "\"unexpected\":true}}",
        dir, dir, "invalid-json-shape");

    jw__test_expect_rejected(
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"svc\"},\"restart\":\"no\",\"default_enabled\":false}}garbage",
        dir, dir, "invalid-json");

    jw__test_expect_rejected(
        "{\"id\":\"org.umrk.test\\u0000suffix\",\"service\":{\"schema\":1,"
        "\"id\":\"org.umrk.test\\u0000suffix\",\"run\":{\"path\":\"svc\"},"
        "\"restart\":\"no\",\"default_enabled\":false}}",
        dir, dir, "invalid-json");

    jw__test_expect_rejected(
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"svc\"},\"restart\":\"no\",\"default_enabled\":false},"
        "\"state\":{\"root\":\"State\",\"revoke_on_uninstall\":[\"token\"]}}",
        dir, NULL, "revoke-on-uninstall-root-unavailable");

    const char *huge_grace =
        "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,\"id\":\"org.umrk.test\","
        "\"run\":{\"path\":\"svc\"},\"restart\":\"no\",\"default_enabled\":false,"
        "\"stop_grace_ms\":1e100}}";
    memset(reason, 0, sizeof(reason));
    ok = jw_service_manifest_validate(huge_grace, dir, dir, &m,
                                      reason, sizeof(reason));
    assert(ok);
    assert(m.stop_grace_ms == 15000);
    jw_service_manifest_destroy(&m);

    /* JSON Schema permits exactly 128 bytes for ids; storage needs the NUL. */
    char max_id[JW_SVC_ID_BUF];
    max_id[0] = 'a';
    max_id[1] = '.';
    memset(max_id + 2, 'b', JW_SVC_ID_MAX - 2);
    max_id[JW_SVC_ID_MAX] = '\0';
    char max_id_json[1024];
    snprintf(max_id_json, sizeof(max_id_json),
             "{\"id\":\"%s\",\"service\":{\"schema\":1,\"id\":\"%s\","
             "\"run\":{\"path\":\"svc\"},\"restart\":\"no\","
             "\"default_enabled\":false}}", max_id, max_id);
    ok = jw_service_manifest_validate(max_id_json, dir, dir, &m,
                                      reason, sizeof(reason));
    assert(ok);
    assert(strlen(m.id) == JW_SVC_ID_MAX);
    jw_service_manifest_destroy(&m);

    /* State paths have no SVC-1 length ceiling and must not be truncated. */
    char long_state_path[701];
    memset(long_state_path, 'p', sizeof(long_state_path) - 1);
    long_state_path[sizeof(long_state_path) - 1] = '\0';
    char long_state_json[2048];
    snprintf(long_state_json, sizeof(long_state_json),
             "{\"id\":\"org.umrk.test\",\"service\":{\"schema\":1,"
             "\"id\":\"org.umrk.test\",\"run\":{\"path\":\"svc\"},"
             "\"restart\":\"no\",\"default_enabled\":false},"
             "\"state\":{\"root\":\"State\",\"retained_roots\":[\"%s\"]}}",
             long_state_path);
    ok = jw_service_manifest_validate(long_state_json, dir, dir, &m,
                                      reason, sizeof(reason));
    assert(ok);
    assert(strlen(m.state_retained_roots[0]) == strlen(long_state_path));
    assert(strcmp(m.state_retained_roots[0], long_state_path) == 0);
    jw_service_manifest_destroy(&m);

    puts("PASS manifest-test inline sanity");
}

/* ------------------------------------------------------------------------
 * Fixture-tree walk against the frozen A0 contract fixtures.
 *
 * leaf-contracts/contracts/leaf-services/README.md documents exactly two ways a consuming
 * repo references that directory: a local sibling checkout (the default
 * below), or a pinned commit SHA in CI. Missing fixtures are a failure:
 * otherwise this target can report PASS without exercising the contract it
 * claims to test. CI may override JW_TEST_FIXTURES_ROOT at compile time.
 * ------------------------------------------------------------------------ */

#ifndef JW_TEST_FIXTURES_ROOT
#define JW_TEST_FIXTURES_ROOT "../leaf-contracts/contracts/leaf-services/manifests"
#endif

static bool jw__test_fixture_json_string(cJSON *obj, const char *key, char *out, size_t out_size) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    snprintf(out, out_size, "%s", item->valuestring);
    return true;
}

static int jw__test_walk_valid(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) {
        return 0;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char fixture_dir[1024], pak_path[1100], expect_path[1100];
        snprintf(fixture_dir, sizeof(fixture_dir), "%s/%s", dir_path, entry->d_name);
        snprintf(pak_path, sizeof(pak_path), "%s/pak.json", fixture_dir);
        snprintf(expect_path, sizeof(expect_path), "%s/expect.json", fixture_dir);

        struct stat st;
        if (stat(pak_path, &st) != 0) {
            fprintf(stderr, "FAIL manifests/valid/%s: missing pak.json\n",
                    entry->d_name);
            assert(false);
        }

        char *pak_text = jw__test_read_file(pak_path);
        char *expect_text = jw__test_read_file(expect_path);
        assert(pak_text && expect_text);

        cJSON *expect = cJSON_Parse(expect_text);
        assert(expect);

        jw_service_manifest m;
        char reason[JW_SVC_REASON_BUF] = {0};
        bool ok = jw_service_manifest_validate(pak_text, fixture_dir, fixture_dir, &m, reason, sizeof(reason));
        if (!ok) {
            fprintf(stderr, "FAIL manifests/valid/%s: expected valid, got reason=%s\n",
                    entry->d_name, reason);
            assert(false);
        }

        cJSON *resolved_item =
            cJSON_GetObjectItemCaseSensitive(expect, "resolved_stop_grace_ms");
        if (cJSON_IsNumber(resolved_item)) {
            int want = (int)resolved_item->valuedouble;
            if (m.stop_grace_ms != want) {
                fprintf(stderr, "FAIL manifests/valid/%s: resolved stop_grace_ms %d != expected %d\n",
                        entry->d_name, m.stop_grace_ms, want);
                assert(false);
            }
        }

        printf("ok:   manifests/valid/%s\n", entry->d_name);
        count++;

        jw_service_manifest_destroy(&m);
        cJSON_Delete(expect);
        free(pak_text);
        free(expect_text);
    }
    closedir(d);
    return count;
}

static int jw__test_walk_invalid(const char *dir_path) {
    DIR *d = opendir(dir_path);
    if (!d) {
        return 0;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char fixture_dir[1024], expect_path[1100];
        snprintf(fixture_dir, sizeof(fixture_dir), "%s/%s", dir_path, entry->d_name);
        snprintf(expect_path, sizeof(expect_path), "%s/expect.json", fixture_dir);

        struct stat st;
        if (stat(expect_path, &st) != 0) {
            fprintf(stderr, "FAIL manifests/invalid/%s: missing expect.json\n",
                    entry->d_name);
            assert(false);
        }
        char *expect_text = jw__test_read_file(expect_path);
        assert(expect_text);
        cJSON *expect = cJSON_Parse(expect_text);
        assert(expect);

        if (strcmp(entry->d_name, "duplicate-id") == 0) {
            /* Pairwise case: this module deliberately does not implement
             * cross-manifest duplicate-service-id detection (that's a
             * discovery-level concern once every candidate has been
             * individually validated) -- assert the precondition instead:
             * both manifests validate individually and declare the same
             * service id, which is what makes them a genuine duplicate
             * rather than two unrelated rejects. */
            char pak_a_path[1100], pak_b_path[1100];
            snprintf(pak_a_path, sizeof(pak_a_path), "%s/pak-a.json", fixture_dir);
            snprintf(pak_b_path, sizeof(pak_b_path), "%s/pak-b.json", fixture_dir);
            char *pak_a_text = jw__test_read_file(pak_a_path);
            char *pak_b_text = jw__test_read_file(pak_b_path);
            assert(pak_a_text && pak_b_text);

            jw_service_manifest ma, mb;
            char reason_a[JW_SVC_REASON_BUF] = {0}, reason_b[JW_SVC_REASON_BUF] = {0};
            bool ok_a = jw_service_manifest_validate(pak_a_text, fixture_dir, fixture_dir, &ma,
                                                      reason_a, sizeof(reason_a));
            bool ok_b = jw_service_manifest_validate(pak_b_text, fixture_dir, fixture_dir, &mb,
                                                      reason_b, sizeof(reason_b));
            if (!ok_a || !ok_b || strcmp(ma.id, mb.id) != 0) {
                fprintf(stderr, "FAIL manifests/invalid/duplicate-id: expected both individually "
                                "valid with matching id, got ok_a=%d(%s) ok_b=%d(%s)\n",
                        ok_a, reason_a, ok_b, reason_b);
                assert(false);
            }
            printf("ok:   manifests/invalid/duplicate-id (duplicate-service-id, both unavailable)\n");
            count++;
            jw_service_manifest_destroy(&ma);
            jw_service_manifest_destroy(&mb);
            cJSON_Delete(expect);
            free(expect_text);
            free(pak_a_text);
            free(pak_b_text);
            continue;
        }

        char pak_path[1100];
        snprintf(pak_path, sizeof(pak_path), "%s/pak.json", fixture_dir);
        char *pak_text = jw__test_read_file(pak_path);
        assert(pak_text);

        char expected_reason[JW_SVC_REASON_BUF];
        assert(jw__test_fixture_json_string(expect, "reason", expected_reason, sizeof(expected_reason)));

        jw_service_manifest m;
        char reason[JW_SVC_REASON_BUF] = {0};
        bool ok = jw_service_manifest_validate(pak_text, fixture_dir, fixture_dir, &m, reason, sizeof(reason));
        if (ok) {
            fprintf(stderr, "FAIL manifests/invalid/%s: expected reason %s, got valid\n",
                    entry->d_name, expected_reason);
            assert(false);
        }
        if (strcmp(reason, expected_reason) != 0) {
            fprintf(stderr, "FAIL manifests/invalid/%s: expected reason %s, got %s\n",
                    entry->d_name, expected_reason, reason);
            assert(false);
        }
        printf("ok:   manifests/invalid/%s -> %s\n", entry->d_name, reason);
        count++;

        cJSON_Delete(expect);
        free(pak_text);
        free(expect_text);
    }
    closedir(d);
    return count;
}

int main(void) {
    jw__test_inline_sanity();

    char valid_dir[1024], invalid_dir[1024];
    snprintf(valid_dir, sizeof(valid_dir), "%s/valid", JW_TEST_FIXTURES_ROOT);
    snprintf(invalid_dir, sizeof(invalid_dir), "%s/invalid", JW_TEST_FIXTURES_ROOT);

    struct stat st;
    if (stat(valid_dir, &st) != 0) {
        fprintf(stderr, "FAIL manifest-test fixture walk: %s not found; "
                        "provide the sibling checkout or compile with "
                        "-DJW_TEST_FIXTURES_ROOT=\\\"/pinned/path\\\"\n",
                JW_TEST_FIXTURES_ROOT);
        return 1;
    }

    int valid_count = jw__test_walk_valid(valid_dir);
    int invalid_count = jw__test_walk_invalid(invalid_dir);
    printf("manifest-test: %d valid + %d invalid fixtures checked\n", valid_count, invalid_count);
    assert(valid_count >= 10);
    assert(invalid_count >= 27);

    puts("PASS manifest-test");
    return 0;
}
