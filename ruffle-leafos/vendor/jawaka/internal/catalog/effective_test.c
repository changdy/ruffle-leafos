/* CAT-1 effective-catalog tests.
 *
 * The expected digests below were produced by the contract's own reference
 * implementation, leaf-contracts/contracts/leaf-content/scripts/canonical.py,
 * not by this code. That is the point: a C emitter checked against itself
 * proves nothing, and if the producer and the readers disagree about the
 * canonical bytes by a single character they disagree about which
 * generation is current. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/catalog/effective.h"

#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* From canonical.py over the fixture built by seed_release() below. */
#define EXPECTED_GENERATION \
    "gen-e0bde2c9b2e01680101de54e63ea7ca424229a73811e77992dff61c1211fc826"
#define EMPTY_TREE_SHA256 \
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
#define INFO_TREE_SHA256 \
    "0a878cda53f66996c2db32c2a493e8091321dd796eddb1fd3c621c5004ed2719"
#define LEN_PREFIX_LEFT_SHA256 \
    "78e206aa51fdce3509ad2737023220267d08694bd3aff1fecd676131203cf203"
#define LEN_PREFIX_RIGHT_SHA256 \
    "e2a0fbdc701f11de05b2126c128d3888003cbdb5e106861f88733d9a9abe126f"

/* Deliberately well under PATH_MAX: these are joined into PATH_MAX buffers
 * all over the test, and sizing them at PATH_MAX makes every join a
 * potential truncation as far as -Wformat-truncation is concerned. */
#define SANDBOX_MAX 256
#define DERIVED_MAX 512

static int failures;
static char sandbox[SANDBOX_MAX];
static char state_dir[DERIVED_MAX];
static char defaults_dir[DERIVED_MAX];
static char catalog_dir[DERIVED_MAX + 64];

static void check(int condition, const char *what) {
    if (!condition) {
        failures++;
        fprintf(stderr, "effective-test: FAIL %s\n", what);
    }
}

static void check_str(const char *got, const char *want, const char *what) {
    if (!got || strcmp(got, want) != 0) {
        failures++;
        fprintf(stderr, "effective-test: FAIL %s: got %s, want %s\n",
                what, got ? got : "(null)", want);
    }
}

static void path_of(char *out, size_t size, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(out, size, fmt, args);
    va_end(args);
}

static int write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    size_t len = strlen(text);
    int ok = (len == 0 || fwrite(text, 1u, len, f) == len);
    return (fclose(f) == 0 && ok) ? 0 : -1;
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    static char buf[8192];
    size_t got = fread(buf, 1u, sizeof(buf) - 1u, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void rm_rf(const char *path) {
    char command[PATH_MAX + 16];
    snprintf(command, sizeof(command), "rm -rf '%s'", path);
    if (system(command) != 0) {
        fprintf(stderr, "effective-test: could not clean %s\n", path);
    }
}

static void mkdir_p(const char *path) {
    char command[PATH_MAX + 16];
    snprintf(command, sizeof(command), "mkdir -p '%s'", path);
    if (system(command) != 0) {
        fprintf(stderr, "effective-test: could not create %s\n", path);
    }
}

/* The exact inputs canonical.py was run against: systems.json "S",
 * cores.json "C", one info file x.info "I". */
static void seed_release(const char *systems, const char *cores) {
    char path[PATH_MAX];
    mkdir_p(defaults_dir);
    path_of(path, sizeof(path), "%s/systems.json", defaults_dir);
    write_text(path, systems);
    path_of(path, sizeof(path), "%s/cores.json", defaults_dir);
    write_text(path, cores);

    char info_dir[PATH_MAX];
    path_of(info_dir, sizeof(info_dir), "%s/info", sandbox);
    mkdir_p(info_dir);
    path_of(path, sizeof(path), "%s/x.info", info_dir);
    write_text(path, "I");
}

static void seed_release_id(const char *release_id) {
    char path[PATH_MAX];
    char json[512];
    mkdir_p(state_dir);
    path_of(path, sizeof(path), "%s/release.json", state_dir);
    snprintf(json, sizeof(json),
             "{\"schema\":1,\"version\":\"0.11.0\",\"release_id\":\"%s\"}",
             release_id);
    write_text(path, json);
}

static int count_generations(void) {
    DIR *dir = opendir(catalog_dir);
    if (!dir) {
        return 0;
    }
    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "gen-", 4u) == 0) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

/* ------------------------------------------------------------------ */

static void test_selector_grammar(void) {
    char out[JW_CAT_GEN_NAME_MAX];
    const char *hex64 = "0123456789abcdef0123456789abcdef"
                        "0123456789abcdef0123456789abcdef";
    char valid[128];
    snprintf(valid, sizeof(valid), "gen-%s\n", hex64);

    check(jw_catalog_parse_selector(valid, out, sizeof(out)), "valid selector parses");
    check_str(out, valid[0] ? "gen-0123456789abcdef0123456789abcdef"
                              "0123456789abcdef0123456789abcdef" : "",
              "parsed selector value");

    char no_newline[128];
    snprintf(no_newline, sizeof(no_newline), "gen-%s", hex64);
    check(!jw_catalog_parse_selector(no_newline, out, sizeof(out)),
          "missing trailing newline rejected");

    char upper[128];
    snprintf(upper, sizeof(upper),
             "gen-0123456789ABCDEF0123456789abcdef"
             "0123456789abcdef0123456789abcdef\n");
    check(!jw_catalog_parse_selector(upper, out, sizeof(out)),
          "uppercase hex rejected");

    char two_lines[160];
    snprintf(two_lines, sizeof(two_lines), "gen-%s\n\n", hex64);
    check(!jw_catalog_parse_selector(two_lines, out, sizeof(out)),
          "trailing junk rejected");

    /* The reason a strict grammar exists at all: this value names a
       directory a reader is about to open. */
    check(!jw_catalog_parse_selector("../../elsewhere\n", out, sizeof(out)),
          "traversal rejected");
    check(!jw_catalog_parse_selector("gen-abc\n", out, sizeof(out)),
          "short digest rejected");
    check(!jw_catalog_parse_selector("", out, sizeof(out)), "empty rejected");
}

static void test_tree_hash(void) {
    char hex[65];
    char dir[PATH_MAX];
    char path[PATH_MAX];

    path_of(dir, sizeof(dir), "%s/tree-missing", sandbox);
    check(jw_catalog_tree_sha256(dir, hex) == 0, "missing dir hashes");
    check_str(hex, EMPTY_TREE_SHA256, "missing dir hashes as empty");

    path_of(dir, sizeof(dir), "%s/tree-empty", sandbox);
    mkdir_p(dir);
    check(jw_catalog_tree_sha256(dir, hex) == 0, "empty dir hashes");
    check_str(hex, EMPTY_TREE_SHA256, "empty dir hashes as empty");

    path_of(dir, sizeof(dir), "%s/tree-info", sandbox);
    mkdir_p(dir);
    path_of(path, sizeof(path), "%s/x.info", dir);
    write_text(path, "I");
    check(jw_catalog_tree_sha256(dir, hex) == 0, "info dir hashes");
    check_str(hex, INFO_TREE_SHA256, "info tree matches canonical.py");

    /* Without the uint64 length prefix these two hash identically, and an
       info directory could be rewritten across a file boundary while
       keeping a valid stamp. */
    path_of(dir, sizeof(dir), "%s/tree-left", sandbox);
    mkdir_p(dir);
    path_of(path, sizeof(path), "%s/a", dir);
    write_text(path, "x");
    path_of(path, sizeof(path), "%s/b", dir);
    write_text(path, "yz");
    check(jw_catalog_tree_sha256(dir, hex) == 0, "left tree hashes");
    check_str(hex, LEN_PREFIX_LEFT_SHA256, "left tree matches canonical.py");

    path_of(dir, sizeof(dir), "%s/tree-right", sandbox);
    mkdir_p(dir);
    path_of(path, sizeof(path), "%s/a", dir);
    write_text(path, "xy");
    path_of(path, sizeof(path), "%s/b", dir);
    write_text(path, "z");
    check(jw_catalog_tree_sha256(dir, hex) == 0, "right tree hashes");
    check_str(hex, LEN_PREFIX_RIGHT_SHA256, "right tree matches canonical.py");
    check(strcmp(LEN_PREFIX_LEFT_SHA256, LEN_PREFIX_RIGHT_SHA256) != 0,
          "length prefix separates the two trees");
}

static void test_publish_and_reuse(void) {
    char generation[JW_CAT_GEN_NAME_MAX];
    char reason[JW_CAT_REASON_MAX];
    char selector_path[PATH_MAX];
    path_of(selector_path, sizeof(selector_path), "%s/current", catalog_dir);

    int rc = jw_catalog_refresh(sandbox, defaults_dir, generation,
                                sizeof(generation), reason, sizeof(reason));
    check(rc == 0, "first refresh publishes");
    check_str(generation, EXPECTED_GENERATION,
              "generation name matches canonical.py");
    check_str(reason, "published", "first refresh reports published");
    check(count_generations() == 1, "exactly one generation on disk");

    char expected_line[128];
    snprintf(expected_line, sizeof(expected_line), "%s\n", EXPECTED_GENERATION);
    check_str(read_text(selector_path), expected_line, "selector contents");

    /* Content addressing: an identical recompile lands on the same name,
       verifies, and reuses. No counter, no garbage, no second copy. */
    rc = jw_catalog_refresh(sandbox, defaults_dir, generation,
                            sizeof(generation), reason, sizeof(reason));
    check(rc == 0, "second refresh succeeds");
    check_str(reason, "reused", "identical recompile reuses the generation");
    check(count_generations() == 1, "no second directory for identical input");

    char path[PATH_MAX];
    path_of(path, sizeof(path), "%s/%s/systems.json", catalog_dir, EXPECTED_GENERATION);
    check_str(read_text(path), "S", "systems.json materialized");
    path_of(path, sizeof(path), "%s/%s/cores.json", catalog_dir, EXPECTED_GENERATION);
    check_str(read_text(path), "C", "cores.json materialized");
    path_of(path, sizeof(path), "%s/%s/info/x.info", catalog_dir, EXPECTED_GENERATION);
    check_str(read_text(path), "I", "info/ materialized");

    /* arcade_names.txt, retroarch.cfg and friends stay release-owned and are
       never copied in. Repointing the defaults dir wholesale would have
       silently dropped arcade titles. */
    path_of(path, sizeof(path), "%s/%s/arcade_names.txt", catalog_dir, EXPECTED_GENERATION);
    check(!exists(path), "release-owned ancillary data is not copied");
}

static void test_reader_resolution(void) {
    char dir[PATH_MAX];
    char reason[JW_CAT_REASON_MAX];
    char selector_path[PATH_MAX];
    path_of(selector_path, sizeof(selector_path), "%s/current", catalog_dir);

    jw_catalog_resolution res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir),
                                                         reason, sizeof(reason));
    check(res == JW_CAT_EFFECTIVE, "reader resolves the published generation");
    char expected_dir[PATH_MAX];
    path_of(expected_dir, sizeof(expected_dir), "%s/%s", catalog_dir, EXPECTED_GENERATION);
    check_str(dir, expected_dir, "resolved generation directory");

    write_text(selector_path, "gen-not-a-digest\n");
    res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir), reason, sizeof(reason));
    check(res == JW_CAT_RELEASE_DEFAULTS, "malformed selector falls back");
    check_str(reason, "selector-malformed", "malformed selector reason");

    unlink(selector_path);
    res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir), reason, sizeof(reason));
    check(res == JW_CAT_RELEASE_DEFAULTS, "absent selector falls back");
    check_str(reason, "selector-missing", "absent selector reason");

    char missing[128];
    snprintf(missing, sizeof(missing),
             "gen-1111111111111111111111111111111111111111111111111111111111111111\n");
    write_text(selector_path, missing);
    res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir), reason, sizeof(reason));
    check(res == JW_CAT_RELEASE_DEFAULTS, "missing generation falls back");
    check_str(reason, "generation-missing", "missing generation reason");

    /* A partially written generation must never be served. */
    char cores_path[PATH_MAX];
    char stash[PATH_MAX];
    path_of(cores_path, sizeof(cores_path), "%s/%s/cores.json", catalog_dir,
            EXPECTED_GENERATION);
    path_of(stash, sizeof(stash), "%s/cores.stash", sandbox);
    check(rename(cores_path, stash) == 0, "stash cores.json");
    snprintf(missing, sizeof(missing), "%s\n", EXPECTED_GENERATION);
    write_text(selector_path, missing);
    res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir), reason, sizeof(reason));
    check(res == JW_CAT_RELEASE_DEFAULTS, "incomplete generation falls back");
    check_str(reason, "generation-missing", "incomplete generation reason");
    check(rename(stash, cores_path) == 0, "restore cores.json");

    /* An OTA moves release_id. The post-OTA system list must match the new
       release exactly, so a generation stamped for the old one is refused
       rather than served. */
    seed_release_id("2026-09-01-gdeadbee");
    res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir), reason, sizeof(reason));
    check(res == JW_CAT_RELEASE_DEFAULTS, "post-OTA generation falls back");
    check_str(reason, "stamp-release-mismatch", "post-OTA reason");
    seed_release_id("2026-08-28-gtest001");

    res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir), reason, sizeof(reason));
    check(res == JW_CAT_EFFECTIVE, "restored release id resolves again");
}

static void test_corrupt_stamp(void) {
    char dir[PATH_MAX];
    char reason[JW_CAT_REASON_MAX];
    char stamp_path[PATH_MAX];
    char stash[PATH_MAX];
    path_of(stamp_path, sizeof(stamp_path), "%s/%s/stamp.json", catalog_dir,
            EXPECTED_GENERATION);
    path_of(stash, sizeof(stash), "%s/stamp.stash", sandbox);
    check(rename(stamp_path, stash) == 0, "stash stamp.json");

    jw_catalog_resolution res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir),
                                                         reason, sizeof(reason));
    check(res == JW_CAT_RELEASE_DEFAULTS, "absent stamp falls back");
    check_str(reason, "stamp-missing", "absent stamp reason");

    write_text(stamp_path, "{ this is not json");
    res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir), reason, sizeof(reason));
    check(res == JW_CAT_RELEASE_DEFAULTS, "unparseable stamp falls back");
    check_str(reason, "stamp-missing", "unparseable stamp reason");

    write_text(stamp_path,
               "{\"schema\":2,\"platform\":\"mac\","
               "\"release_id\":\"2026-08-28-gtest001\"}");
    res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir), reason, sizeof(reason));
    check(res == JW_CAT_RELEASE_DEFAULTS, "future stamp schema falls back");
    check_str(reason, "stamp-schema-unsupported", "future schema reason");

    write_text(stamp_path,
               "{\"schema\":1,\"platform\":\"tg5040\","
               "\"release_id\":\"2026-08-28-gtest001\"}");
    res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir), reason, sizeof(reason));
    check(res == JW_CAT_RELEASE_DEFAULTS, "foreign platform falls back");
    check_str(reason, "stamp-platform-mismatch", "foreign platform reason");

    /* The same content-addressed name holding different bytes is corruption.
       Fail closed, diagnose it, and never overwrite a directory a reader may
       be inside. */
    char generation[JW_CAT_GEN_NAME_MAX];
    int rc = jw_catalog_refresh(sandbox, defaults_dir, generation,
                                sizeof(generation), reason, sizeof(reason));
    check(rc == 1, "digest conflict fails closed");
    check_str(reason, "generation-digest-conflict", "digest conflict reason");
    check_str(read_text(stamp_path),
              "{\"schema\":1,\"platform\":\"tg5040\","
              "\"release_id\":\"2026-08-28-gtest001\"}",
              "conflicting generation is not overwritten");

    char diagnostics[PATH_MAX];
    path_of(diagnostics, sizeof(diagnostics), "%s/diagnostics.json", catalog_dir);
    check(exists(diagnostics), "conflict leaves readable diagnostics");
    check(strstr(read_text(diagnostics), "generation-digest-conflict") != NULL,
          "diagnostics name the reason");

    char selector_path[PATH_MAX];
    path_of(selector_path, sizeof(selector_path), "%s/current", catalog_dir);
    check(!exists(selector_path), "conflict invalidates the selector");

    check(rename(stash, stamp_path) == 0, "restore stamp.json");
}

static void test_recompile_after_input_change(void) {
    char generation[JW_CAT_GEN_NAME_MAX];
    char reason[JW_CAT_REASON_MAX];

    int rc = jw_catalog_refresh(sandbox, defaults_dir, generation,
                                sizeof(generation), reason, sizeof(reason));
    check(rc == 0, "republish after restoring the stamp");
    check_str(generation, EXPECTED_GENERATION, "back to the original generation");

    /* The drag-the-ZIP case: the payload changed but release_id did not.
       base.info_sha256 and the file hashes are in the stamp precisely so
       this is still caught. */
    seed_release("S-CHANGED", "C");
    rc = jw_catalog_refresh(sandbox, defaults_dir, generation,
                            sizeof(generation), reason, sizeof(reason));
    check(rc == 0, "changed input recompiles");
    check(strcmp(generation, EXPECTED_GENERATION) != 0,
          "changed input produces a different generation");
    check(count_generations() == 2, "the previous generation is retained");

    char dir[PATH_MAX];
    jw_catalog_resolution res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir),
                                                         reason, sizeof(reason));
    check(res == JW_CAT_EFFECTIVE, "reader resolves after the swap");
    char expected_dir[PATH_MAX];
    path_of(expected_dir, sizeof(expected_dir), "%s/%s", catalog_dir, generation);
    check_str(dir, expected_dir, "reader follows the swap to the new generation");

    seed_release("S", "C");
    rc = jw_catalog_refresh(sandbox, defaults_dir, generation,
                            sizeof(generation), reason, sizeof(reason));
    check(rc == 0, "reverting recompiles");
    check_str(generation, EXPECTED_GENERATION, "revert lands on the original name");
    check_str(reason, "reused", "revert REUSES the retained generation");
    check(count_generations() == 2, "revert adds no third directory");
}

static void test_cleanup_temps(void) {
    char tmp_a[PATH_MAX];
    char tmp_b[PATH_MAX];
    char inner[PATH_MAX];
    path_of(tmp_a, sizeof(tmp_a), "%s/tmp-abandoned-1", catalog_dir);
    path_of(tmp_b, sizeof(tmp_b), "%s/tmp-abandoned-2", catalog_dir);
    mkdir_p(tmp_a);
    mkdir_p(tmp_b);
    path_of(inner, sizeof(inner), "%s/info/nested", tmp_a);
    mkdir_p(inner);
    path_of(inner, sizeof(inner), "%s/info/nested/deep.info", tmp_a);
    write_text(inner, "deep");

    int before = count_generations();
    size_t removed = 0;
    check(jw_catalog_cleanup_temps(sandbox, &removed) == 0, "cleanup runs");
    check(removed == 2, "both abandoned temp dirs removed");
    check(!exists(tmp_a) && !exists(tmp_b), "temp dirs are gone");

    /* v1 has no finalized-generation GC: a CentralScrutinizer process can
       outlive a jawakad crash, so even daemon startup is not a reader-free
       window. */
    check(count_generations() == before, "no finalized generation was pruned");

    char selector_path[PATH_MAX];
    path_of(selector_path, sizeof(selector_path), "%s/current", catalog_dir);
    check(exists(selector_path), "cleanup leaves the selector alone");
}

static void test_missing_release_identity(void) {
    char generation[JW_CAT_GEN_NAME_MAX];
    char reason[JW_CAT_REASON_MAX];
    char release_path[PATH_MAX];
    char stash[PATH_MAX];
    char selector_path[PATH_MAX];
    path_of(release_path, sizeof(release_path), "%s/release.json", state_dir);
    path_of(stash, sizeof(stash), "%s/release.stash", sandbox);
    path_of(selector_path, sizeof(selector_path), "%s/current", catalog_dir);
    check(rename(release_path, stash) == 0, "stash release.json");

    /* The compiler never guesses an identity. */
    int rc = jw_catalog_refresh(sandbox, defaults_dir, generation,
                                sizeof(generation), reason, sizeof(reason));
    check(rc == 1, "missing release identity refuses to publish");
    check_str(reason, "release-identity-unavailable", "missing identity reason");
    check(!exists(selector_path), "missing identity invalidates the selector");

    char dir[PATH_MAX];
    jw_catalog_resolution res = jw_catalog_effective_dir(sandbox, dir, sizeof(dir),
                                                         reason, sizeof(reason));
    check(res == JW_CAT_RELEASE_DEFAULTS, "reader falls back without an identity");
    check_str(reason, "release-identity-unavailable", "reader identity reason");

    check(rename(stash, release_path) == 0, "restore release.json");
}

static void test_compile_failure_diagnostics(void) {
    char generation[JW_CAT_GEN_NAME_MAX];
    char reason[JW_CAT_REASON_MAX];
    char too_long[PATH_MAX + 1];
    memset(too_long, 'x', sizeof(too_long) - 1);
    too_long[sizeof(too_long) - 1] = '\0';

    int rc = jw_catalog_refresh(sandbox, too_long, generation,
                                sizeof(generation), reason, sizeof(reason));
    check(rc == -1, "overlong release path fails compilation");
    check_str(reason, "compile-failed", "overlong release path reason");

    char diagnostics[PATH_MAX];
    char selector[PATH_MAX];
    path_of(diagnostics, sizeof(diagnostics), "%s/diagnostics.json", catalog_dir);
    path_of(selector, sizeof(selector), "%s/current", catalog_dir);
    check(exists(diagnostics), "failed compile leaves readable diagnostics");
    check(strstr(read_text(diagnostics), "compile-failed") != NULL,
          "failed compile diagnostics name the reason");
    check(!exists(selector), "failed compile invalidates the selector");
}

static void test_content_scrape_policy(void) {
    rm_rf(sandbox);
    mkdir_p(sandbox);
    mkdir_p(defaults_dir);
    seed_release("{\"platform\":\"mac\",\"systems\":[]}",
                 "{\"platform\":\"mac\",\"cores\":[]}");
    seed_release_id("2026-08-28-gtest001");

    char pak_dir[PATH_MAX];
    char path[PATH_MAX];
    path_of(pak_dir, sizeof(pak_dir), "%s/ScummVM.pak", sandbox);
    mkdir_p(pak_dir);
    path_of(path, sizeof(path), "%s/cores", pak_dir);
    mkdir_p(path);
    path_of(path, sizeof(path), "%s/info", pak_dir);
    mkdir_p(path);
    path_of(path, sizeof(path), "%s/cores/scummvm_libretro.so", pak_dir);
    write_text(path, "core");
    path_of(path, sizeof(path), "%s/info/scummvm_libretro.info", pak_dir);
    write_text(path, "info");
    path_of(path, sizeof(path), "%s/pak.json", pak_dir);
    write_text(path, "manifest-v1");

    char contributor_json[8192];
    snprintf(contributor_json, sizeof(contributor_json),
             "[{\"provider\":\"mlp1/ScummVM.pak\","
             "\"source_id\":\"primary\",\"pak_version\":\"1.0.0\","
             "\"pak_dir\":\"%s\","
             "\"provides\":{\"schema\":1,\"systems\":[{"
             "\"id\":\"SCUMMVM\",\"name\":\"ScummVM\","
             "\"patterns\":[\"SCUMMVM\"],"
             "\"extensions\":[\"scummvm\",\"svm\"],"
             "\"rom_root\":\"Roms/SCUMMVM\","
             "\"image_root\":\"Images/SCUMMVM\","
             "\"default_core\":\"scummvm\",\"alternate_cores\":[]}],"
             "\"cores\":[{\"id\":\"scummvm\","
             "\"display_name\":\"ScummVM\",\"type\":\"retroarch\","
             "\"libretro_name\":\"scummvm\","
             "\"file_name\":\"cores/scummvm_libretro.so\","
             "\"info_name\":\"info/scummvm_libretro.info\","
             "\"config_folder\":\"ScummVM\"}],"
             "\"system_extensions\":[]},"
             "\"content_scrape\":{\"schema\":1,\"systems\":[{"
             "\"id\":\"SCUMMVM\",\"name_source\":\"descriptor\","
             "\"lookup_extension\":\"scummvm\"}]}}]",
             pak_dir);
    cJSON *contributors = cJSON_Parse(contributor_json);
    cJSON *diagnostics = cJSON_CreateArray();
    check(contributors != NULL && diagnostics != NULL,
          "content scrape contributor fixture parses");

    char generation[JW_CAT_GEN_NAME_MAX];
    char reason[JW_CAT_REASON_MAX];
    int rc = jw_catalog_refresh_with_contributors(
        sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0, "content scrape contributor publishes");

    path_of(path, sizeof(path), "%s/%s/systems.json", catalog_dir, generation);
    const char *systems = read_text(path);
    check(systems && strstr(systems,
          "\"screenscraper_lookup_extension\":\"scummvm\"") != NULL,
          "effective system contains lookup extension");
    check(systems && strstr(systems,
          "\"screenscraper_name_source\":\"descriptor\"") != NULL,
          "effective system contains descriptor source");
    check(systems && strstr(systems, "\"content_scrape\"") == NULL,
          "effective system omits raw companion block");

    path_of(path, sizeof(path), "%s/%s/stamp.json", catalog_dir, generation);
    const char *stamp = read_text(path);
    check(stamp && strstr(stamp, "\"rel\":\"pak.json\"") != NULL,
          "active companion fingerprints pak.json");

    char first_generation[JW_CAT_GEN_NAME_MAX];
    snprintf(first_generation, sizeof(first_generation), "%s", generation);
    path_of(path, sizeof(path), "%s/pak.json", pak_dir);
    write_text(path, "manifest-v2");
    rc = jw_catalog_refresh_with_contributors(
        sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0, "changed companion manifest republishes");
    check(strcmp(first_generation, generation) != 0,
          "pak.json change moves the generation");

    cJSON *contributor = cJSON_GetArrayItem(contributors, 0);
    cJSON_AddItemToObject(contributor, "content_art", cJSON_Parse(
        "{\"schema\":1,\"systems\":[{\"id\":\"SCUMMVM\",\"wordmark\":\"mark.png\"}]}"));
    path_of(path, sizeof(path), "%s/mark.png", pak_dir);
    write_text(path, "\x89PNG\r\n\x1a\none");
    rc = jw_catalog_refresh_with_contributors(sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0, "wordmark publishes");
    path_of(path, sizeof(path), "%s/%s/systems.json", catalog_dir, generation);
    systems = read_text(path);
    check(systems && strstr(systems, "\"wordmark\":\"mark.png\"") &&
          strstr(systems, "\"wordmark_provider\":\"mlp1/ScummVM.pak\""),
          "wordmark path and separate provenance serialized");
    check(systems && !strstr(systems, sandbox), "no live mount path in systems output");
    path_of(path, sizeof(path), "%s/%s/stamp.json", catalog_dir, generation);
    stamp = read_text(path);
    check(stamp && strstr(stamp, "\"rel\":\"mark.png\""), "PNG fingerprinted");
    snprintf(first_generation, sizeof(first_generation), "%s", generation);
    path_of(path, sizeof(path), "%s/mark.png", pak_dir);
    write_text(path, "\x89PNG\r\n\x1a\ntwo");
    rc = jw_catalog_refresh_with_contributors(sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0 && strcmp(first_generation, generation), "same-path PNG bytes republish");
    snprintf(first_generation, sizeof(first_generation), "%s", generation);
    cJSON_ReplaceItemInObjectCaseSensitive(contributor, "content_art", cJSON_Parse(
        "{\"schema\":2,\"systems\":[{\"id\":\"SCUMMVM\",\"wordmark\":\"mark.png\",\"grid_icon\":\"grid.png\",\"wordmark_color\":\"color.png\"}]}"));
    path_of(path, sizeof(path), "%s/grid.png", pak_dir);
    write_text(path, "grid-one");
    path_of(path, sizeof(path), "%s/color.png", pak_dir);
    write_text(path, "color-one");
    rc = jw_catalog_refresh_with_contributors(sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0, "grid icon publishes from accepted contributor");
    path_of(path, sizeof(path), "%s/%s/systems.json", catalog_dir, generation);
    systems = read_text(path);
    check(systems && strstr(systems, "\"grid_icon\":\"grid.png\"") &&
          strstr(systems, "\"grid_icon_provider\":\"mlp1/ScummVM.pak\""), "grid path and independent provider serialized");
    check(systems && strstr(systems, "\"wordmark_color\":\"color.png\"") &&
          strstr(systems, "\"wordmark_color_provider\":\"mlp1/ScummVM.pak\""), "color wordmark and provider serialized");
    path_of(path, sizeof(path), "%s/%s/stamp.json", catalog_dir, generation);
    stamp = read_text(path);
    check(stamp && strstr(stamp, "\"rel\":\"grid.png\"") && strstr(stamp, "\"rel\":\"color.png\""),
          "grid and color PNGs fingerprinted");
    snprintf(first_generation, sizeof(first_generation), "%s", generation);
    path_of(path, sizeof(path), "%s/color.png", pak_dir); write_text(path, "color-two");
    rc = jw_catalog_refresh_with_contributors(sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0 && strcmp(first_generation, generation), "same-path color bytes republish");
    snprintf(first_generation, sizeof(first_generation), "%s", generation);
    path_of(path, sizeof(path), "%s/grid.png", pak_dir); write_text(path, "grid-two");
    rc = jw_catalog_refresh_with_contributors(sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0 && strcmp(first_generation, generation), "same-path grid bytes republish");
    snprintf(first_generation, sizeof(first_generation), "%s", generation);
    char moved[PATH_MAX]; path_of(moved, sizeof(moved), "%s/relocated.pak", sandbox);
    check(rename(pak_dir, moved) == 0, "move provider install root");
    cJSON_ReplaceItemInObjectCaseSensitive(contributor, "pak_dir", cJSON_CreateString(moved));
    rc = jw_catalog_refresh_with_contributors(sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0 && !strcmp(first_generation, generation), "root relocation preserves generation");
    cJSON_DeleteItemFromArray(contributors, 0);
    rc = jw_catalog_refresh_with_contributors(sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0 && strcmp(first_generation, generation), "provider removal republishes");
    path_of(path, sizeof(path), "%s/%s/systems.json", catalog_dir, generation);
    systems = read_text(path);
    check(systems && !strstr(systems, "wordmark") && !strstr(systems, "grid_icon"), "removed provider leaves no art fields");

    cJSON_Delete(diagnostics);
    cJSON_Delete(contributors);
}

/* LEAF-RA-1: the frontend a core declares is a declared file. Replacing it
 * (same path, new bytes) moves the generation exactly as replacing the core
 * does, and dropping the declaration stops fingerprinting it. */
static void test_frontend_declared_file(void) {
    rm_rf(sandbox);
    mkdir_p(sandbox);
    mkdir_p(defaults_dir);
    seed_release("{\"platform\":\"mac\",\"systems\":[]}",
                 "{\"platform\":\"mac\",\"cores\":[]}");
    seed_release_id("2026-08-28-gtest001");

    char pak_dir[PATH_MAX];
    char path[PATH_MAX];
    path_of(pak_dir, sizeof(pak_dir), "%s/Frontend.pak", sandbox);
    path_of(path, sizeof(path), "%s/cores", pak_dir);
    mkdir_p(path);
    path_of(path, sizeof(path), "%s/info", pak_dir);
    mkdir_p(path);
    path_of(path, sizeof(path), "%s/scripts", pak_dir);
    mkdir_p(path);
    path_of(path, sizeof(path), "%s/cores/yaba_libretro.so", pak_dir);
    write_text(path, "core");
    path_of(path, sizeof(path), "%s/info/yaba_libretro.info", pak_dir);
    write_text(path, "info");
    char script[PATH_MAX];
    path_of(script, sizeof(script), "%s/scripts/retroarch.sh", pak_dir);
    write_text(script, "frontend-v1");

    char contributor_json[8192];
    snprintf(contributor_json, sizeof(contributor_json),
             "[{\"provider\":\"mlp1/Frontend.pak\","
             "\"source_id\":\"primary\",\"pak_version\":\"1.0.0\","
             "\"pak_dir\":\"%s\","
             "\"provides\":{\"schema\":1,\"systems\":[{"
             "\"id\":\"SAT\",\"name\":\"Saturn\","
             "\"patterns\":[\"SAT\"],\"extensions\":[\"cue\"],"
             "\"rom_root\":\"Roms/SAT\",\"image_root\":\"Images/SAT\","
             "\"default_core\":\"yaba\",\"alternate_cores\":[]}],"
             "\"cores\":[{\"id\":\"yaba\","
             "\"display_name\":\"Yaba\",\"type\":\"retroarch\","
             "\"libretro_name\":\"yaba\","
             "\"file_name\":\"cores/yaba_libretro.so\","
             "\"info_name\":\"info/yaba_libretro.info\","
             "\"config_folder\":\"Yaba\","
             "\"frontend\":{\"path\":\"scripts/retroarch.sh\","
             "\"protocol\":\"leaf-ra-1\"}}],"
             "\"system_extensions\":[]}}]",
             pak_dir);
    cJSON *contributors = cJSON_Parse(contributor_json);
    cJSON *diagnostics = cJSON_CreateArray();
    check(contributors != NULL && diagnostics != NULL,
          "frontend contributor fixture parses");

    char generation[JW_CAT_GEN_NAME_MAX];
    char first_generation[JW_CAT_GEN_NAME_MAX];
    char reason[JW_CAT_REASON_MAX];
    int rc = jw_catalog_refresh_with_contributors(
        sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0, "pak core with a frontend publishes");

    /* The effective core row keeps the declaration, with provenance, so the
       daemon can launch it. */
    path_of(path, sizeof(path), "%s/%s/cores.json", catalog_dir, generation);
    const char *cores = read_text(path);
    check(cores && strstr(cores, "\"frontend\":{") != NULL &&
              strstr(cores, "\"path\":\"scripts/retroarch.sh\"") != NULL &&
              strstr(cores, "\"protocol\":\"leaf-ra-1\"") != NULL &&
              strstr(cores, "\"provider\":\"mlp1/Frontend.pak\"") != NULL,
          "effective cores.json carries the frontend and the provider");
    check(cores && !strstr(cores, sandbox), "no live mount path in cores output");

    path_of(path, sizeof(path), "%s/%s/stamp.json", catalog_dir, generation);
    const char *stamp = read_text(path);
    check(stamp && strstr(stamp, "\"rel\":\"scripts/retroarch.sh\"") != NULL,
          "the frontend script is fingerprinted");
    check(stamp && strstr(stamp, "\"rel\":\"cores/yaba_libretro.so\"") != NULL,
          "the core is still fingerprinted beside it");

    /* Unchanged inputs reuse the generation. */
    snprintf(first_generation, sizeof(first_generation), "%s", generation);
    rc = jw_catalog_refresh_with_contributors(
        sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0 && strcmp(first_generation, generation) == 0,
          "unchanged frontend keeps the generation");
    check_str(reason, "reused", "unchanged frontend reuses the generation");

    /* Same path, new bytes: not the same catalog. */
    write_text(script, "frontend-v2");
    rc = jw_catalog_refresh_with_contributors(
        sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0, "replaced frontend republishes");
    check(strcmp(first_generation, generation) != 0,
          "same-path frontend bytes move the generation");
    check_str(reason, "published", "replaced frontend publishes a new generation");

    /* The old generation is also what a reader would refuse: the stamp no
       longer matches the live pak. Reverting the bytes goes back to it. */
    char second_generation[JW_CAT_GEN_NAME_MAX];
    snprintf(second_generation, sizeof(second_generation), "%s", generation);
    write_text(script, "frontend-v1");
    rc = jw_catalog_refresh_with_contributors(
        sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0 && strcmp(generation, first_generation) == 0,
          "reverting the frontend bytes returns to the first generation");
    check(strcmp(generation, second_generation) != 0,
          "the two frontend contents are two generations");

    /* A different frontend path moves it too (the canonical provides differ),
       and the old script stops being fingerprinted. */
    char moved_script[PATH_MAX];
    path_of(moved_script, sizeof(moved_script), "%s/scripts/other.sh", pak_dir);
    write_text(moved_script, "frontend-v1");
    cJSON *contributor = cJSON_GetArrayItem(contributors, 0);
    cJSON *provides = cJSON_GetObjectItemCaseSensitive(contributor, "provides");
    cJSON *core = cJSON_GetArrayItem(
        cJSON_GetObjectItemCaseSensitive(provides, "cores"), 0);
    cJSON *frontend = cJSON_GetObjectItemCaseSensitive(core, "frontend");
    cJSON_ReplaceItemInObjectCaseSensitive(frontend, "path",
                                           cJSON_CreateString("scripts/other.sh"));
    rc = jw_catalog_refresh_with_contributors(
        sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0 && strcmp(generation, first_generation) != 0,
          "a different frontend path moves the generation");
    path_of(path, sizeof(path), "%s/%s/stamp.json", catalog_dir, generation);
    stamp = read_text(path);
    check(stamp && strstr(stamp, "\"rel\":\"scripts/other.sh\"") != NULL &&
              strstr(stamp, "\"rel\":\"scripts/retroarch.sh\"") == NULL,
          "only the declared frontend path is fingerprinted");

    /* Dropping the declaration leaves no frontend in the catalog and no
       fingerprint of the script, which stays on disk. */
    snprintf(second_generation, sizeof(second_generation), "%s", generation);
    cJSON_DeleteItemFromObjectCaseSensitive(core, "frontend");
    rc = jw_catalog_refresh_with_contributors(
        sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0 && strcmp(generation, second_generation) != 0,
          "dropping the frontend republishes");
    path_of(path, sizeof(path), "%s/%s/cores.json", catalog_dir, generation);
    cores = read_text(path);
    check(cores && strstr(cores, "frontend") == NULL,
          "a core without a frontend has none in the effective catalog");
    path_of(path, sizeof(path), "%s/%s/stamp.json", catalog_dir, generation);
    stamp = read_text(path);
    check(stamp && strstr(stamp, "scripts/") == NULL,
          "a core without a frontend fingerprints no script");
    check(stamp && strstr(stamp, "\"rel\":\"cores/yaba_libretro.so\"") != NULL,
          "the core file is still fingerprinted");
    write_text(script, "frontend-v3");
    snprintf(second_generation, sizeof(second_generation), "%s", generation);
    rc = jw_catalog_refresh_with_contributors(
        sandbox, defaults_dir, contributors, diagnostics,
        generation, sizeof(generation), reason, sizeof(reason));
    check(rc == 0 && strcmp(generation, second_generation) == 0,
          "an undeclared script's bytes do not move the generation");

    cJSON_Delete(diagnostics);
    cJSON_Delete(contributors);
}

int main(void) {
    const char *base = getenv("TMPDIR");
    snprintf(sandbox, sizeof(sandbox), "%s/jw-effective-test-%ld",
             (base && base[0]) ? base : "/tmp", (long)getpid());
    rm_rf(sandbox);
    mkdir_p(sandbox);

    snprintf(state_dir, sizeof(state_dir), "%s/state", sandbox);
    snprintf(defaults_dir, sizeof(defaults_dir), "%s/defaults", sandbox);
    snprintf(catalog_dir, sizeof(catalog_dir), "%s/catalog", state_dir);
    setenv("UMRK_INTERNAL_DATA_PATH", state_dir, 1);

    seed_release("S", "C");
    seed_release_id("2026-08-28-gtest001");

    test_selector_grammar();
    test_tree_hash();
    test_publish_and_reuse();
    test_reader_resolution();
    test_corrupt_stamp();
    test_recompile_after_input_change();
    test_cleanup_temps();
    test_missing_release_identity();
    test_compile_failure_diagnostics();
    test_content_scrape_policy();
    test_frontend_declared_file();

    rm_rf(sandbox);
    if (failures) {
        fprintf(stderr, "effective-test: %d failure(s)\n", failures);
        return 1;
    }
    printf("effective-test: ok\n");
    return 0;
}
