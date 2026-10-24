#define _POSIX_C_SOURCE 200809L

/* LEAF-RA-1 frontend: what the catalog loader keeps of a core's "frontend"
   object, and what jw_ra_catalog_resolve_frontend_path() makes of it.

   The contract validator (internal/catalog/manifest.c, covered by the
   leaf-contracts fixtures) decides which pak manifests are valid. This test
   covers the second line: the loader reads only well-formed leaf-ra-1
   frontends and only from pak rows, and the resolver turns one into an
   absolute path to an executable regular file under the pak's live Apps root.

   Every fixture lives under /tmp, the machine's own filesystem. Bind mounts
   such as a host volume mounted noowners answer access(X_OK) with "yes" for
   every file, which would make the not-executable cases vacuous. */

#include "internal/retroarch/catalog.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int failures;

static void check(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "catalog-frontend-test: FAIL: %s\n", what);
        failures++;
    }
}

static int write_file(const char *path, const char *text, mode_t mode) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t len = strlen(text);
    int ok = fwrite(text, 1, len, fp) == len && fclose(fp) == 0;
    return ok && chmod(path, mode) == 0 ? 0 : -1;
}

static int mkdir_p(const char *path) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(buf, 0755) != 0 && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    return mkdir(buf, 0755) == 0 || errno == EEXIST ? 0 : -1;
}

/* An executable (0755) or plain (0644) file at <base>/<pak>/<rel>. */
static int make_file(const char *base, const char *pak, const char *rel,
                     mode_t mode) {
    char path[PATH_MAX], dir[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s/%s", base, pak, rel);
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) return -1;
    *slash = '\0';
    if (mkdir_p(dir) != 0) return -1;
    return write_file(path, "#!/bin/sh\nexit 0\n", mode);
}

/* <base>/<rel> into out (PATH_MAX). */
static void path_join(char *out, const char *base, const char *rel) {
    snprintf(out, PATH_MAX, "%s/%s", base, rel);
}

#define FE_OK "{\"path\":\"scripts/retroarch.sh\",\"protocol\":\"leaf-ra-1\"}"

/* One cores.json row. provider may be NULL (a release row); frontend_json is
   the raw JSON value of the "frontend" member, or NULL for no member. */
static void append_row(char *json, size_t size, const char *id,
                       const char *provider, const char *frontend_json) {
    char provider_part[160] = "";
    char frontend_part[256] = "";
    if (provider) {
        snprintf(provider_part, sizeof(provider_part), ",\"provider\":\"%s\"", provider);
    }
    if (frontend_json) {
        snprintf(frontend_part, sizeof(frontend_part), ",\"frontend\":%s", frontend_json);
    }
    size_t used = strlen(json);
    snprintf(json + used, size - used,
             "%s{\"id\":\"%s\",\"type\":\"retroarch\",\"status\":\"packaged\","
             "\"libretro_name\":\"%s\",\"file_name\":\"cores/%s_libretro.so\","
             "\"config_folder\":\"%s\",\"info_name\":\"info/%s_libretro.info\"%s%s}",
             json[used - 1] == '[' ? "" : ",", id, id, id, id, id,
             provider_part, frontend_part);
}

static const jw_ra_core *core_of(const jw_ra_catalog *catalog, const char *id) {
    const jw_ra_core *core = jw_ra_catalog_find_core(catalog, id);
    if (!core) fprintf(stderr, "catalog-frontend-test: core %s not loaded\n", id);
    return core;
}

int main(void) {
    char root[] = "/tmp/jw-catalog-frontend-XXXXXX";
    if (!mkdtemp(root)) {
        fprintf(stderr, "catalog-frontend-test: mkdtemp failed\n");
        return 1;
    }
    char platform[PATH_MAX], defaults[PATH_MAX], sd[PATH_MAX], alt_apps[PATH_MAX];
    path_join(platform, root, "platform");
    path_join(defaults, platform, "defaults");
    path_join(sd, root, "sd");
    path_join(alt_apps, root, "other-apps");
    if (mkdir_p(defaults) != 0 || mkdir_p(sd) != 0 || mkdir_p(alt_apps) != 0) {
        fprintf(stderr, "catalog-frontend-test: fixture mkdir failed\n");
        return 1;
    }
    setenv("UMRK_PLATFORM_PATH", platform, 1);
    unsetenv("APPS_PATH");

    char cores_path[PATH_MAX], systems_path[PATH_MAX];
    path_join(cores_path, defaults, "cores.json");
    path_join(systems_path, defaults, "systems.json");
    if (write_file(systems_path,
                   "{\"platform\":\"mac\",\"systems\":[{\"id\":\"SAT\","
                   "\"default_core\":\"pak_ok\"}]}", 0644) != 0) {
        fprintf(stderr, "catalog-frontend-test: systems fixture failed\n");
        return 1;
    }

    static char json[8192];
    snprintf(json, sizeof(json), "{\"platform\":\"mac\",\"cores\":[");
    /* A pak row with a valid frontend: the only row that keeps one. */
    append_row(json, sizeof(json), "pak_ok", "mac/Frontend.pak", FE_OK);
    /* A release row (no provider) cannot carry one, whatever it declares. */
    append_row(json, sizeof(json), "release_fe", NULL, FE_OK);
    append_row(json, sizeof(json), "pak_release_empty_provider", "", FE_OK);
    /* Anything but exactly "leaf-ra-1" is ignored. */
    append_row(json, sizeof(json), "pak_proto2", "mac/Frontend.pak",
               "{\"path\":\"scripts/retroarch.sh\",\"protocol\":\"leaf-ra-2\"}");
    append_row(json, sizeof(json), "pak_proto_case", "mac/Frontend.pak",
               "{\"path\":\"scripts/retroarch.sh\",\"protocol\":\"LEAF-RA-1\"}");
    append_row(json, sizeof(json), "pak_proto_empty", "mac/Frontend.pak",
               "{\"path\":\"scripts/retroarch.sh\",\"protocol\":\"\"}");
    append_row(json, sizeof(json), "pak_proto_number", "mac/Frontend.pak",
               "{\"path\":\"scripts/retroarch.sh\",\"protocol\":1}");
    append_row(json, sizeof(json), "pak_no_proto", "mac/Frontend.pak",
               "{\"path\":\"scripts/retroarch.sh\"}");
    /* Malformed shapes load as a plain pak core with no frontend. */
    append_row(json, sizeof(json), "pak_no_path", "mac/Frontend.pak",
               "{\"protocol\":\"leaf-ra-1\"}");
    append_row(json, sizeof(json), "pak_empty_path", "mac/Frontend.pak",
               "{\"path\":\"\",\"protocol\":\"leaf-ra-1\"}");
    append_row(json, sizeof(json), "pak_path_number", "mac/Frontend.pak",
               "{\"path\":7,\"protocol\":\"leaf-ra-1\"}");
    append_row(json, sizeof(json), "pak_frontend_string", "mac/Frontend.pak",
               "\"scripts/retroarch.sh\"");
    append_row(json, sizeof(json), "pak_frontend_null", "mac/Frontend.pak", "null");
    append_row(json, sizeof(json), "pak_frontend_array", "mac/Frontend.pak",
               "[\"scripts/retroarch.sh\",\"leaf-ra-1\"]");
    /* No frontend at all: a pak core that runs in Leaf's RetroArch. */
    append_row(json, sizeof(json), "pak_plain", "mac/Frontend.pak", NULL);
    /* Valid and loaded, but resolution differs per file on disk. */
    append_row(json, sizeof(json), "pak_missing", "mac/Frontend.pak",
               "{\"path\":\"scripts/missing.sh\",\"protocol\":\"leaf-ra-1\"}");
    append_row(json, sizeof(json), "pak_noexec", "mac/Frontend.pak",
               "{\"path\":\"scripts/plain.sh\",\"protocol\":\"leaf-ra-1\"}");
    append_row(json, sizeof(json), "pak_dir", "mac/Frontend.pak",
               "{\"path\":\"scripts/a-directory\",\"protocol\":\"leaf-ra-1\"}");
    append_row(json, sizeof(json), "pak_other_pak", "mac/Other.pak",
               "{\"path\":\"scripts/retroarch.sh\",\"protocol\":\"leaf-ra-1\"}");
    append_row(json, sizeof(json), "pak_nested", "mac/Frontend.pak",
               "{\"path\":\"runtime/bin/retroarch\",\"protocol\":\"leaf-ra-1\"}");
    strncat(json, "]}", sizeof(json) - strlen(json) - 1);
    if (write_file(cores_path, json, 0644) != 0) {
        fprintf(stderr, "catalog-frontend-test: cores fixture failed\n");
        return 1;
    }

    char error[256] = "";
    jw_ra_catalog *catalog = jw_ra_catalog_load(sd, error, sizeof(error));
    if (!catalog) {
        fprintf(stderr, "catalog-frontend-test: catalog load failed: %s\n", error);
        return 1;
    }

    /* --- Loader ----------------------------------------------------------- */
    const jw_ra_core *pak_ok = core_of(catalog, "pak_ok");
    check(pak_ok && pak_ok->frontend_path &&
              strcmp(pak_ok->frontend_path, "scripts/retroarch.sh") == 0,
          "a pak row with a valid leaf-ra-1 frontend keeps frontend_path");
    check(pak_ok && pak_ok->provider &&
              strcmp(pak_ok->provider, "mac/Frontend.pak") == 0,
          "the pak row keeps its provider");

    static const char *const ignored[] = {
        "release_fe", "pak_release_empty_provider", "pak_proto2",
        "pak_proto_case", "pak_proto_empty", "pak_proto_number",
        "pak_no_proto", "pak_no_path", "pak_empty_path", "pak_path_number",
        "pak_frontend_string", "pak_frontend_null", "pak_frontend_array",
        "pak_plain",
    };
    for (size_t i = 0; i < sizeof(ignored) / sizeof(ignored[0]); i++) {
        const jw_ra_core *core = core_of(catalog, ignored[i]);
        char what[128];
        snprintf(what, sizeof(what), "%s: no frontend_path", ignored[i]);
        check(core && core->frontend_path == NULL, what);
    }
    {
        const jw_ra_core *release = core_of(catalog, "release_fe");
        check(release && release->provider == NULL,
              "release_fe really is a release row (no provider)");
    }
    {
        /* A frontend never disturbs the other fields of the same row. */
        const jw_ra_core *core = core_of(catalog, "pak_proto2");
        check(core && core->provider && jw_ra_core_is_packaged_retroarch(core),
              "an ignored frontend still leaves a loadable pak RetroArch core");
    }
    {
        /* The other pak rows with a declared frontend, for the resolver. */
        static const char *const loaded[] = {
            "pak_missing", "pak_noexec", "pak_dir", "pak_other_pak", "pak_nested",
        };
        for (size_t i = 0; i < sizeof(loaded) / sizeof(loaded[0]); i++) {
            const jw_ra_core *core = core_of(catalog, loaded[i]);
            char what[128];
            snprintf(what, sizeof(what), "%s: frontend_path loaded", loaded[i]);
            check(core && core->frontend_path && core->frontend_path[0], what);
        }
    }

    /* --- Resolver: APPS_PATH branch --------------------------------------- */
    char want[PATH_MAX], got[PATH_MAX];
    check(make_file(alt_apps, "mac/Frontend.pak", "scripts/retroarch.sh", 0755) == 0 &&
              make_file(alt_apps, "mac/Frontend.pak", "scripts/plain.sh", 0644) == 0 &&
              make_file(alt_apps, "mac/Frontend.pak", "runtime/bin/retroarch", 0755) == 0 &&
              make_file(alt_apps, "mac/Other.pak", "scripts/retroarch.sh", 0755) == 0,
          "APPS_PATH fixture files");
    {
        char dir[PATH_MAX];
        path_join(dir, alt_apps, "mac/Frontend.pak/scripts/a-directory");
        check(mkdir_p(dir) == 0 && chmod(dir, 0755) == 0, "APPS_PATH directory fixture");
    }
    setenv("APPS_PATH", alt_apps, 1);

    path_join(want, alt_apps, "mac/Frontend.pak/scripts/retroarch.sh");
    got[0] = 'x';
    check(jw_ra_catalog_resolve_frontend_path(catalog, pak_ok, got, sizeof(got)) == 0 &&
              strcmp(got, want) == 0,
          "APPS_PATH: executable frontend resolves to the absolute path");
    check(got[0] == '/', "APPS_PATH: resolved path is absolute");

    {
        const jw_ra_core *core = core_of(catalog, "pak_nested");
        path_join(want, alt_apps, "mac/Frontend.pak/runtime/bin/retroarch");
        check(core && jw_ra_catalog_resolve_frontend_path(catalog, core, got, sizeof(got)) == 0 &&
                  strcmp(got, want) == 0,
              "APPS_PATH: the frontend need not sit beside the core (nested pak-relative path)");
    }
    {
        const jw_ra_core *core = core_of(catalog, "pak_other_pak");
        path_join(want, alt_apps, "mac/Other.pak/scripts/retroarch.sh");
        check(core && jw_ra_catalog_resolve_frontend_path(catalog, core, got, sizeof(got)) == 0 &&
                  strcmp(got, want) == 0,
              "APPS_PATH: the provider selects the pak directory");
    }

    {
        const jw_ra_core *missing = core_of(catalog, "pak_missing");
        got[0] = 'x';
        check(missing &&
                  jw_ra_catalog_resolve_frontend_path(catalog, missing, got, sizeof(got)) == -1,
              "APPS_PATH: a missing file is -1");
        check(got[0] == '\0', "APPS_PATH: a failed resolve leaves out empty");

        const jw_ra_core *noexec = core_of(catalog, "pak_noexec");
        got[0] = 'x';
        check(noexec &&
                  jw_ra_catalog_resolve_frontend_path(catalog, noexec, got, sizeof(got)) == -1,
              "APPS_PATH: a non-executable file is -1");
        check(got[0] == '\0', "APPS_PATH: a non-executable file leaves out empty");

        const jw_ra_core *dir = core_of(catalog, "pak_dir");
        check(dir && jw_ra_catalog_resolve_frontend_path(catalog, dir, got, sizeof(got)) == -1,
              "APPS_PATH: a directory (searchable, mode 0755) is -1");

        const jw_ra_core *plain = core_of(catalog, "pak_plain");
        got[0] = 'x';
        check(plain && jw_ra_catalog_resolve_frontend_path(catalog, plain, got, sizeof(got)) == -1,
              "APPS_PATH: a core without a frontend is -1");
        check(got[0] == '\0', "APPS_PATH: a core without a frontend leaves out empty");

        const jw_ra_core *release = core_of(catalog, "release_fe");
        check(release &&
                  jw_ra_catalog_resolve_frontend_path(catalog, release, got, sizeof(got)) == -1,
              "APPS_PATH: a release row is -1 even if a file sits at its would-be path");
    }

    /* The file the SD-card branch would find is not consulted when APPS_PATH
       is set: the resolver reports the APPS_PATH answer only. */
    check(make_file(sd, "Apps/mac/Frontend.pak", "scripts/plain.sh", 0755) == 0,
          "SD fixture: executable plain.sh");
    {
        const jw_ra_core *noexec = core_of(catalog, "pak_noexec");
        check(noexec &&
                  jw_ra_catalog_resolve_frontend_path(catalog, noexec, got, sizeof(got)) == -1,
              "APPS_PATH wins: an executable copy under <sdcard>/Apps is not used");
    }

    /* --- Resolver: arguments, buffers ------------------------------------- */
    {
        char small[8];
        small[0] = 'x';
        check(jw_ra_catalog_resolve_frontend_path(catalog, pak_ok, small, sizeof(small)) == -1,
              "a buffer too small for the path is -1");
        check(small[0] == '\0', "a too-small buffer is left empty");
        check(jw_ra_catalog_resolve_frontend_path(catalog, pak_ok, got, 0) == -1,
              "out_size 0 is -1");
        check(jw_ra_catalog_resolve_frontend_path(catalog, pak_ok, NULL, sizeof(got)) == -1,
              "NULL out is -1");
        check(jw_ra_catalog_resolve_frontend_path(NULL, pak_ok, got, sizeof(got)) == -1,
              "NULL catalog is -1");
        check(jw_ra_catalog_resolve_frontend_path(catalog, NULL, got, sizeof(got)) == -1,
              "NULL core is -1");
        /* The exact fit: strlen + 1 works, one byte less does not. */
        path_join(want, alt_apps, "mac/Frontend.pak/scripts/retroarch.sh");
        char exact[PATH_MAX];
        check(jw_ra_catalog_resolve_frontend_path(catalog, pak_ok, exact, strlen(want) + 1) == 0 &&
                  strcmp(exact, want) == 0,
              "a buffer of exactly strlen+1 fits");
        check(jw_ra_catalog_resolve_frontend_path(catalog, pak_ok, exact, strlen(want)) == -1,
              "a buffer one byte short is -1");
    }

    /* --- Resolver: <sdcard_root>/Apps branch ------------------------------ */
    unsetenv("APPS_PATH");
    check(make_file(sd, "Apps/mac/Frontend.pak", "scripts/retroarch.sh", 0755) == 0 &&
              make_file(sd, "Apps/mac/Frontend.pak", "scripts/plain.sh", 0644) == 0 &&
              make_file(sd, "Apps/mac/Frontend.pak", "runtime/bin/retroarch", 0755) == 0,
          "SD fixture files");
    {
        char dir[PATH_MAX];
        path_join(dir, sd, "Apps/mac/Frontend.pak/scripts/a-directory");
        check(mkdir_p(dir) == 0, "SD directory fixture");
    }
    path_join(want, sd, "Apps/mac/Frontend.pak/scripts/retroarch.sh");
    got[0] = 'x';
    check(jw_ra_catalog_resolve_frontend_path(catalog, pak_ok, got, sizeof(got)) == 0 &&
              strcmp(got, want) == 0,
          "sdcard_root: executable frontend resolves under <sdcard>/Apps/<provider>");
    {
        const jw_ra_core *core = core_of(catalog, "pak_nested");
        path_join(want, sd, "Apps/mac/Frontend.pak/runtime/bin/retroarch");
        check(core && jw_ra_catalog_resolve_frontend_path(catalog, core, got, sizeof(got)) == 0 &&
                  strcmp(got, want) == 0,
              "sdcard_root: nested pak-relative path");
    }
    {
        /* Same core, same pak, but the file only exists under APPS_PATH now. */
        const jw_ra_core *other = core_of(catalog, "pak_other_pak");
        got[0] = 'x';
        check(other &&
                  jw_ra_catalog_resolve_frontend_path(catalog, other, got, sizeof(got)) == -1,
              "sdcard_root: a pak that exists only under another Apps root is -1");
        check(got[0] == '\0', "sdcard_root: a miss leaves out empty");

        const jw_ra_core *missing = core_of(catalog, "pak_missing");
        check(missing &&
                  jw_ra_catalog_resolve_frontend_path(catalog, missing, got, sizeof(got)) == -1,
              "sdcard_root: a missing file is -1");
        const jw_ra_core *noexec = core_of(catalog, "pak_noexec");
        check(noexec &&
                  jw_ra_catalog_resolve_frontend_path(catalog, noexec, got, sizeof(got)) == -1,
              "sdcard_root: a non-executable file is -1");
        const jw_ra_core *dir = core_of(catalog, "pak_dir");
        check(dir && jw_ra_catalog_resolve_frontend_path(catalog, dir, got, sizeof(got)) == -1,
              "sdcard_root: a directory is -1");
        const jw_ra_core *plain = core_of(catalog, "pak_plain");
        check(plain && jw_ra_catalog_resolve_frontend_path(catalog, plain, got, sizeof(got)) == -1,
              "sdcard_root: a core without a frontend is -1");
        const jw_ra_core *release = core_of(catalog, "release_fe");
        check(release &&
                  jw_ra_catalog_resolve_frontend_path(catalog, release, got, sizeof(got)) == -1,
              "sdcard_root: a release row is -1");
    }

    /* An empty APPS_PATH is no APPS_PATH. */
    setenv("APPS_PATH", "", 1);
    path_join(want, sd, "Apps/mac/Frontend.pak/scripts/retroarch.sh");
    check(jw_ra_catalog_resolve_frontend_path(catalog, pak_ok, got, sizeof(got)) == 0 &&
              strcmp(got, want) == 0,
          "an empty APPS_PATH falls back to <sdcard>/Apps");
    unsetenv("APPS_PATH");

    /* Both roots, the file executable in both: APPS_PATH is the answer. */
    setenv("APPS_PATH", alt_apps, 1);
    path_join(want, alt_apps, "mac/Frontend.pak/scripts/retroarch.sh");
    check(jw_ra_catalog_resolve_frontend_path(catalog, pak_ok, got, sizeof(got)) == 0 &&
              strcmp(got, want) == 0,
          "both roots populated: APPS_PATH is preferred");
    unsetenv("APPS_PATH");

    /* Neither root known: a hand-built catalog with no sdcard_root. The
       resolver must refuse rather than resolve against the working directory. */
    {
        jw_ra_catalog bare;
        memset(&bare, 0, sizeof(bare));
        jw_ra_core core;
        memset(&core, 0, sizeof(core));
        core.provider = (char *)"mac/Frontend.pak";
        core.frontend_path = (char *)"scripts/retroarch.sh";
        got[0] = 'x';
        check(jw_ra_catalog_resolve_frontend_path(&bare, &core, got, sizeof(got)) == -1,
              "no APPS_PATH and no sdcard_root is -1");
        check(got[0] == '\0', "no root leaves out empty");
        bare.sdcard_root = (char *)"";
        check(jw_ra_catalog_resolve_frontend_path(&bare, &core, got, sizeof(got)) == -1,
              "no APPS_PATH and an empty sdcard_root is -1");
        core.provider = (char *)"";
        setenv("APPS_PATH", alt_apps, 1);
        check(jw_ra_catalog_resolve_frontend_path(&bare, &core, got, sizeof(got)) == -1,
              "an empty provider is -1");
        core.provider = NULL;
        check(jw_ra_catalog_resolve_frontend_path(&bare, &core, got, sizeof(got)) == -1,
              "a NULL provider is -1");
        unsetenv("APPS_PATH");
    }

    /* The frontend does not change how the core file itself resolves. */
    {
        char core_file[PATH_MAX];
        check(make_file(sd, "Apps/mac/Frontend.pak", "cores/pak_ok_libretro.so", 0644) == 0,
              "core file fixture");
        path_join(want, sd, "Apps/mac/Frontend.pak/cores/pak_ok_libretro.so");
        check(jw_ra_catalog_resolve_core_path(catalog, pak_ok, NULL, NULL, false,
                                              core_file, sizeof(core_file)) == 0 &&
                  strcmp(core_file, want) == 0,
              "resolve_core_path is unchanged for a core with a frontend");
    }

    jw_ra_catalog_free(catalog);

    if (failures) {
        fprintf(stderr, "catalog-frontend-test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("PASS catalog-frontend-test");
    return 0;
}
