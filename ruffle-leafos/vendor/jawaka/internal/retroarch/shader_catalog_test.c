/* The manifest is release-validated, but it lives on a FAT card a user can
 * edit. These tests are about failing closed: a bad manifest yields an empty
 * catalog and a diagnostic, never a partial list, a traversal, or a crash. */

#include "internal/retroarch/shader_catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int failures;

static void check(int cond, const char *what) {
    if (!cond) {
        fprintf(stderr, "  FAIL: %s\n", what);
        failures++;
    }
}

static const char *write_manifest(const char *body) {
    static char path[] = "/tmp/jw-shader-manifest-XXXXXX";
    static char kept[sizeof(path)];
    int fd;
    FILE *fp;

    memcpy(path, "/tmp/jw-shader-manifest-XXXXXX", sizeof(path));
    fd = mkstemp(path);
    if (fd < 0) {
        return NULL;
    }
    fp = fdopen(fd, "w");
    if (!fp) {
        close(fd);
        return NULL;
    }
    fputs(body, fp);
    fclose(fp);
    memcpy(kept, path, sizeof(path));
    return kept;
}

#define ROW(name, path, systems, extra)                                        \
    "{\"group\":\"leaf-recommended\",\"qualification\":\"recommended\","        \
    "\"display_name\":\"" name "\",\"path\":\"" path "\","                      \
    "\"intended_systems\":" systems extra "}"

static const char *VALID =
    "{\"schema_version\":2,\"presets\":["
    ROW("Zed", "leaf-recommended/zed.glslp", "[\"FC\"]",
        ",\"description\":\"last alphabetically\"")
    ","
    ROW("Alpha", "leaf-recommended/alpha.glslp", "[\"FC\",\"SFC\"]",
        ",\"description\":\"first\",\"constraints\":[\"one\",\"two\"]")
    ","
    ROW("Other System", "leaf-recommended/other.glslp", "[\"GBA\"]", "")
    "]}";

static void test_valid(void) {
    jw_shader_catalog cat;
    const char *p = write_manifest(VALID);
    check(jw_shader_catalog_load(p, "FC", &cat), "valid manifest loads");
    check(cat.count == 2, "only rows declaring the active system are included");
    /* Sorted by display name, not manifest order: the manifest is ordered by
       path, which is meaningless to a reader. */
    check(cat.count == 2 && strcmp(cat.rows[0].display_name, "Alpha") == 0,
          "rows are sorted by display name");
    check(cat.count == 2 && cat.rows[0].constraint_count == 2,
          "constraints are carried through");
    check(cat.count == 2 && cat.rows[0].description &&
              strcmp(cat.rows[0].description, "first") == 0,
          "description is carried through");
    jw_shader_catalog_free(&cat);
    unlink(p);

    /* A system with nothing for it is a valid empty catalog, not an error. */
    p = write_manifest(VALID);
    check(jw_shader_catalog_load(p, "PS", &cat), "unknown system still parses");
    check(cat.count == 0, "no rows for a system with no recommendations");
    check(cat.diagnostic[0] != '\0', "empty catalog explains itself");
    jw_shader_catalog_free(&cat);
    unlink(p);
}

static void test_filtering(void) {
    jw_shader_catalog cat;
    const char *p = write_manifest(
        "{\"schema_version\":2,\"presets\":["
        /* not a recommendation */
        "{\"group\":\"leaf-bundled\",\"qualification\":\"recommended\","
        "\"display_name\":\"Bundled\",\"path\":\"leaf-bundled/x.glslp\","
        "\"intended_systems\":[\"FC\"]},"
        /* recommended group but only 'loads' */
        "{\"group\":\"leaf-recommended\",\"qualification\":\"loads\","
        "\"display_name\":\"Loads\",\"path\":\"leaf-recommended/l.glslp\","
        "\"intended_systems\":[\"FC\"]},"
        /* traversal */
        ROW("Escape", "leaf-recommended/../../etc/passwd.glslp", "[\"FC\"]", "")
        ","
        /* absolute */
        ROW("Absolute", "/etc/evil.glslp", "[\"FC\"]", "")
        ","
        /* wrong extension */
        ROW("NotPreset", "leaf-recommended/readme.txt", "[\"FC\"]", "")
        ","
        /* duplicate of a good row, listed before it */
        ROW("Dup", "leaf-recommended/good.glslp", "[\"FC\"]", "")
        ","
        ROW("Good", "leaf-recommended/good.glslp", "[\"FC\"]", "")
        "]}");
    check(jw_shader_catalog_load(p, "FC", &cat), "filtering manifest parses");
    check(cat.count == 1, "only the one legitimate row survives filtering");
    check(cat.count == 1 && strcmp(cat.rows[0].display_name, "Dup") == 0,
          "the first of a duplicate path wins");
    jw_shader_catalog_free(&cat);
    unlink(p);
}

static void test_rejections(void) {
    jw_shader_catalog cat;
    const char *p;

    check(!jw_shader_catalog_load("/tmp/definitely-not-here.json", "FC", &cat),
          "missing manifest is refused");
    check(cat.diagnostic[0] != '\0', "missing manifest explains itself");
    jw_shader_catalog_free(&cat);

    p = write_manifest("{ this is not json");
    check(!jw_shader_catalog_load(p, "FC", &cat), "malformed JSON is refused");
    jw_shader_catalog_free(&cat);
    unlink(p);

    /* A newer schema may redefine a field this code reads. */
    p = write_manifest("{\"schema_version\":3,\"presets\":[]}");
    check(!jw_shader_catalog_load(p, "FC", &cat), "future schema is refused");
    jw_shader_catalog_free(&cat);
    unlink(p);

    p = write_manifest("{\"schema_version\":2}");
    check(!jw_shader_catalog_load(p, "FC", &cat), "manifest without presets is refused");
    jw_shader_catalog_free(&cat);
    unlink(p);

    check(!jw_shader_catalog_load(NULL, "FC", &cat), "null path is refused");
    jw_shader_catalog_free(&cat);
}

static void test_oversized(void) {
    jw_shader_catalog cat;
    char *body = malloc(JW_SHADER_CATALOG_MAX_BYTES + 4096u);
    const char *p;
    size_t n;

    if (!body) {
        return;
    }
    n = (size_t)snprintf(body, 128, "{\"schema_version\":2,\"presets\":[");
    memset(body + n, ' ', JW_SHADER_CATALOG_MAX_BYTES + 1024u);
    strcpy(body + n + JW_SHADER_CATALOG_MAX_BYTES + 1024u, "]}");
    p = write_manifest(body);
    free(body);
    check(!jw_shader_catalog_load(p, "FC", &cat), "oversized manifest is refused");
    check(strstr(cat.diagnostic, "large") != NULL,
          "oversized manifest says so");
    jw_shader_catalog_free(&cat);
    unlink(p);
}

static void test_row_cap(void) {
    jw_shader_catalog cat;
    char *body = malloc(256u * 1024u);
    const char *p;
    size_t n;

    if (!body) {
        return;
    }
    n = (size_t)snprintf(body, 64, "{\"schema_version\":2,\"presets\":[");
    for (unsigned i = 0; i < JW_SHADER_CATALOG_MAX_ROWS + 20u; i++) {
        n += (size_t)snprintf(body + n, 512,
                              "%s{\"group\":\"leaf-recommended\","
                              "\"qualification\":\"recommended\","
                              "\"display_name\":\"S%u\","
                              "\"path\":\"leaf-recommended/s%u.glslp\","
                              "\"intended_systems\":[\"FC\"]}",
                              i ? "," : "", i, i);
    }
    strcpy(body + n, "]}");
    p = write_manifest(body);
    free(body);
    check(jw_shader_catalog_load(p, "FC", &cat), "row-capped manifest parses");
    check(cat.count == JW_SHADER_CATALOG_MAX_ROWS, "row count is capped");
    jw_shader_catalog_free(&cat);
    unlink(p);
}

static void test_path_rules(void) {
    check(jw_shader_catalog_path_ok("leaf-recommended/a.glslp"), "plain path ok");
    check(!jw_shader_catalog_path_ok("leaf-recommended/../a.glslp"), "traversal rejected");
    check(!jw_shader_catalog_path_ok("leaf-recommended/x/../../a.glslp"), "deep traversal rejected");
    check(jw_shader_catalog_path_ok("leaf-recommended/a..b.glslp"), "dots inside a name are fine");
    check(!jw_shader_catalog_path_ok("/leaf-recommended/a.glslp"), "absolute rejected");
    check(!jw_shader_catalog_path_ok("leaf-bundled/a.glslp"), "other namespace rejected");
    check(!jw_shader_catalog_path_ok("leaf-recommended/a.txt"), "wrong extension rejected");
    check(!jw_shader_catalog_path_ok("leaf-recommended/.glslp"), "bare extension rejected");
    check(!jw_shader_catalog_path_ok("leaf-recommended\\a.glslp"), "backslash rejected");
    check(!jw_shader_catalog_path_ok(""), "empty rejected");
    check(!jw_shader_catalog_path_ok(NULL), "null rejected");
}

static void test_automatic_preset_reference(void) {
    char dir[] = "/tmp/jw-shader-reference-XXXXXX";
    char target[512], wrapper[512], resolved[512], expected[512];
    check(mkdtemp(dir) != NULL, "reference fixture directory created");
    snprintf(target, sizeof(target), "%s/recommended.glslp", dir);
    snprintf(wrapper, sizeof(wrapper), "%s/game.glslp", dir);
    FILE *fp = fopen(target, "w");
    check(fp != NULL, "reference target created");
    if (fp) {
        fputs("shaders = 0\n", fp);
        fclose(fp);
    }
    fp = fopen(wrapper, "w");
    check(fp != NULL, "automatic preset wrapper created");
    if (fp) {
        fputs("#reference \"recommended.glslp\"\n", fp);
        fclose(fp);
    }
    check(realpath(target, expected) != NULL, "reference target resolves");
    check(jw_shader_catalog_reference_target(wrapper, resolved, sizeof(resolved)),
          "automatic preset reference resolves");
    check(strcmp(resolved, expected) == 0,
          "automatic preset maps to its referenced recommendation");

    fp = fopen(wrapper, "w");
    if (fp) {
        fputs("#reference recommended.glslp\n", fp);
        fclose(fp);
    }
    check(jw_shader_catalog_reference_target(wrapper, resolved, sizeof(resolved)),
          "unquoted automatic preset reference resolves");
    check(strcmp(resolved, expected) == 0,
          "unquoted preset maps to its referenced recommendation");

    fp = fopen(wrapper, "w");
    if (fp) {
        fputs("#reference \"missing.glslp\"\n", fp);
        fclose(fp);
    }
    check(!jw_shader_catalog_reference_target(wrapper, resolved, sizeof(resolved)),
          "missing reference target is not mapped");
    unlink(wrapper);
    unlink(target);
    rmdir(dir);
}

int main(void) {
    test_valid();
    test_filtering();
    test_rejections();
    test_oversized();
    test_row_cap();
    test_path_rules();
    test_automatic_preset_reference();

    if (failures) {
        fprintf(stderr, "shader-catalog-test: %d failure(s)\n", failures);
        return 1;
    }
    puts("PASS shader-catalog-test");
    return 0;
}
