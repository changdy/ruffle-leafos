#define _POSIX_C_SOURCE 200809L

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

static int fail(const char *message) {
    fprintf(stderr, "catalog-folder-test: %s\n", message);
    return 1;
}

static int write_text(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t len = strlen(text);
    return fwrite(text, 1, len, fp) == len && fclose(fp) == 0 ? 0 : -1;
}

static int write_cores(const char *path, const char *rows) {
    char json[4096];
    int n = snprintf(json, sizeof(json),
                     "{\"platform\":\"mac\",\"cores\":[%s]}", rows);
    return n > 0 && n < (int)sizeof(json) ? write_text(path, json) : -1;
}

int main(void) {
    const char *safe[] = { "mGBA", "DOSBox-pure", "Game & Watch",
                           "MAME 2010", "PCSX-ReARMed", "FlyCast Fast UMRK" };
    for (size_t i = 0; i < sizeof(safe) / sizeof(safe[0]); i++) {
        if (!jw_ra_core_folder_is_safe(safe[i])) return fail("safe folder rejected");
    }
    const char *unsafe[] = { "", ".", "..", "../mGBA", "bad:name", "tail. ",
                             "CON", "con.txt", "COM1", "lpt9.states" };
    for (size_t i = 0; i < sizeof(unsafe) / sizeof(unsafe[0]); i++) {
        if (jw_ra_core_folder_is_safe(unsafe[i])) return fail("unsafe folder accepted");
    }
    char too_long[257];
    memset(too_long, 'a', sizeof(too_long) - 1u);
    too_long[sizeof(too_long) - 1u] = '\0';
    if (jw_ra_core_folder_is_safe(too_long)) return fail("overlong folder accepted");

    char root[] = "/tmp/jw-catalog-folder-XXXXXX";
    int fd = mkstemp(root);
    if (fd < 0) return fail("mkstemp failed");
    close(fd);
    if (unlink(root) != 0 || mkdir(root, 0700) != 0) return fail("root mkdir failed");
    char defaults[PATH_MAX];
    snprintf(defaults, sizeof(defaults), "%s/defaults", root);
    if (mkdir(defaults, 0755) != 0) return fail("defaults mkdir failed");
    setenv("UMRK_PLATFORM_PATH", root, 1);

    char cores[PATH_MAX], systems[PATH_MAX];
    snprintf(cores, sizeof(cores), "%s/cores.json", defaults);
    snprintf(systems, sizeof(systems), "%s/systems.json", defaults);
    if (write_text(systems,
                   "{\"platform\":\"mac\",\"systems\":["
                   "{\"id\":\"GBA\",\"default_core\":\"mgba\","
                   "\"legacy_flat_core\":\"mgba\"}]}") != 0) {
        return fail("systems fixture failed");
    }

    const char *safe_row =
        "{\"id\":\"mgba\",\"type\":\"retroarch\","
        "\"status\":\"packaged\",\"file_name\":\"mgba_libretro.so\","
        "\"config_folder\":\"Game & Watch\"}";
    if (write_cores(cores, safe_row) != 0) return fail("safe fixture failed");
    char error[256];
    jw_ra_catalog *catalog = jw_ra_catalog_load(root, error, sizeof(error));
    if (!catalog || !catalog->systems[0].legacy_flat_core ||
        strcmp(catalog->systems[0].legacy_flat_core, "mgba") != 0 ||
        catalog->systems[0].screenscraper_name_source !=
            JW_RA_SCRAPE_NAME_FILENAME ||
        catalog->systems[0].screenscraper_lookup_extension[0] != '\0') {
        return fail(error[0] ? error : "safe catalog rejected");
    }
    jw_ra_catalog_free(catalog);

    const char *art_fields[] = {
        "\"wordmark\":\"mark.png\",\"wordmark_provider\":\"mlp1/Art.pak\"",
        "\"wordmark\":\"mark.png\",\"wordmark_provider\":\"../Art.pak\"",
        "\"wordmark\":\"../mark.png\",\"wordmark_provider\":\"mlp1/Art.pak\"",
        "\"wordmark\":\"mark.png\"",
        "\"wordmark\":false,\"wordmark_provider\":\"mlp1/Art.pak\""
    };
    for (size_t i = 0; i < sizeof(art_fields)/sizeof(art_fields[0]); i++) {
        char json[2048];
        snprintf(json, sizeof(json), "{\"platform\":\"mac\",\"systems\":[{\"id\":\"GBA\",%s}]}", art_fields[i]);
        if (write_text(systems, json) || !(catalog = jw_ra_catalog_load(root, error, sizeof(error))))
            return fail("optional malformed art rejected a system");
        if ((i == 0) != (catalog->systems[0].wordmark != NULL) || catalog->systems[0].provider)
            return fail("art validation or system ownership changed");
        if (i == 0) {
            char apps[PATH_MAX], lane[PATH_MAX], pak[PATH_MAX], image[PATH_MAX], resolved[PATH_MAX];
            snprintf(apps, sizeof(apps), "%s/Apps", root); mkdir(apps, 0700);
            snprintf(lane, sizeof(lane), "%s/mlp1", apps); mkdir(lane, 0700);
            snprintf(pak, sizeof(pak), "%s/Art.pak", lane); mkdir(pak, 0700);
            snprintf(image, sizeof(image), "%s/mark.png", pak); write_text(image, "image");
            setenv("APPS_PATH", apps, 1);
            if (jw_ra_catalog_resolve_system_wordmark_path(catalog, &catalog->systems[0], resolved, sizeof(resolved)) || strcmp(image, resolved))
                return fail("wordmark didn't use live provider root");
            unlink(image);
            if (symlink(systems, image)) return fail("symlink fixture failed");
            if (!jw_ra_catalog_resolve_system_wordmark_path(catalog, &catalog->systems[0], resolved, sizeof(resolved)))
                return fail("wordmark followed escaping symlink");
            unlink(image);
            unsetenv("APPS_PATH");
        }
        jw_ra_catalog_free(catalog);
    }

    {
    /* wordmark_color is its own slot: same validation, same containment, and a
       failed resolve leaves no stale path behind. */
    const char *art_fields[] = {
        "\"wordmark_color\":\"mark.png\",\"wordmark_color_provider\":\"mlp1/Art.pak\"",
        "\"wordmark_color\":\"mark.png\",\"wordmark_color_provider\":\"../Art.pak\"",
        "\"wordmark_color\":\"../mark.png\",\"wordmark_color_provider\":\"mlp1/Art.pak\"",
        "\"wordmark_color\":\"mark.png\",\"wordmark_provider\":\"mlp1/Art.pak\"",
        "\"wordmark_color\":false,\"wordmark_color_provider\":\"mlp1/Art.pak\""
    };
    for (size_t i = 0; i < sizeof(art_fields)/sizeof(art_fields[0]); i++) {
        char json[2048];
        snprintf(json, sizeof(json), "{\"platform\":\"mac\",\"systems\":[{\"id\":\"GBA\",%s}]}", art_fields[i]);
        if (write_text(systems, json) || !(catalog = jw_ra_catalog_load(root, error, sizeof(error))))
            return fail("optional malformed art rejected a system");
        if ((i == 0) != (catalog->systems[0].wordmark_color != NULL) || catalog->systems[0].provider ||
            catalog->systems[0].wordmark)
            return fail("color wordmark validation or slot independence changed");
        if (i == 0) {
            char apps[PATH_MAX], lane[PATH_MAX], pak[PATH_MAX], image[PATH_MAX], resolved[PATH_MAX];
            snprintf(apps, sizeof(apps), "%s/Apps", root); mkdir(apps, 0700);
            snprintf(lane, sizeof(lane), "%s/mlp1", apps); mkdir(lane, 0700);
            snprintf(pak, sizeof(pak), "%s/Art.pak", lane); mkdir(pak, 0700);
            snprintf(image, sizeof(image), "%s/mark.png", pak); write_text(image, "image");
            setenv("APPS_PATH", apps, 1);
            if (jw_ra_catalog_resolve_system_wordmark_color_path(catalog, &catalog->systems[0], resolved, sizeof(resolved)) || strcmp(image, resolved))
                return fail("wordmark_color didn't use live provider root");
            unlink(image);
            if (symlink(systems, image)) return fail("symlink fixture failed");
            if (!jw_ra_catalog_resolve_system_wordmark_color_path(catalog, &catalog->systems[0], resolved, sizeof(resolved)) || resolved[0])
                return fail("wordmark_color followed escaping symlink or kept its path");
            unlink(image);
            if (!jw_ra_catalog_resolve_system_wordmark_color_path(catalog, &catalog->systems[0], resolved, sizeof(resolved)) || resolved[0])
                return fail("missing wordmark_color resolved or kept its path");
            unsetenv("APPS_PATH");
        }
        jw_ra_catalog_free(catalog);
    }
    }

    {
    const char *art_fields[] = {
        "\"grid_icon\":\"mark.png\",\"grid_icon_provider\":\"mlp1/Art.pak\"",
        "\"grid_icon\":\"mark.png\",\"grid_icon_provider\":\"../Art.pak\"",
        "\"grid_icon\":\"../mark.png\",\"grid_icon_provider\":\"mlp1/Art.pak\"",
        "\"grid_icon\":\"mark.png\"",
        "\"grid_icon\":false,\"grid_icon_provider\":\"mlp1/Art.pak\""
    };
    for (size_t i = 0; i < sizeof(art_fields)/sizeof(art_fields[0]); i++) {
        char json[2048];
        snprintf(json, sizeof(json), "{\"platform\":\"mac\",\"systems\":[{\"id\":\"GBA\",%s}]}", art_fields[i]);
        if (write_text(systems, json) || !(catalog = jw_ra_catalog_load(root, error, sizeof(error))))
            return fail("optional malformed art rejected a system");
        if ((i == 0) != (catalog->systems[0].grid_icon != NULL) || catalog->systems[0].provider)
            return fail("art validation or system ownership changed");
        if (i == 0) {
            char apps[PATH_MAX], lane[PATH_MAX], pak[PATH_MAX], image[PATH_MAX], resolved[PATH_MAX];
            snprintf(apps, sizeof(apps), "%s/Apps", root); mkdir(apps, 0700);
            snprintf(lane, sizeof(lane), "%s/mlp1", apps); mkdir(lane, 0700);
            snprintf(pak, sizeof(pak), "%s/Art.pak", lane); mkdir(pak, 0700);
            snprintf(image, sizeof(image), "%s/mark.png", pak); write_text(image, "image");
            setenv("APPS_PATH", apps, 1);
            if (jw_ra_catalog_resolve_system_grid_icon_path(catalog, &catalog->systems[0], resolved, sizeof(resolved)) || strcmp(image, resolved))
                return fail("grid_icon didn't use live provider root");
            unlink(image);
            if (symlink(systems, image)) return fail("symlink fixture failed");
            if (!jw_ra_catalog_resolve_system_grid_icon_path(catalog, &catalog->systems[0], resolved, sizeof(resolved)))
                return fail("grid_icon followed escaping symlink");
            unlink(image);
            unsetenv("APPS_PATH");
        }
        jw_ra_catalog_free(catalog);
    }
    }

    if (write_text(systems,
                   "{\"platform\":\"mac\",\"systems\":["
                   "{\"id\":\"SCUMMVM\",\"extensions\":[\"scummvm\",\"svm\"],"
                   "\"screenscraper_name_source\":\"descriptor\","
                   "\"screenscraper_lookup_extension\":\"scummvm\"}]}") != 0 ||
        (catalog = jw_ra_catalog_load(root, error, sizeof(error))) == NULL ||
        catalog->systems[0].screenscraper_name_source !=
            JW_RA_SCRAPE_NAME_DESCRIPTOR ||
        strcmp(catalog->systems[0].screenscraper_lookup_extension,
               "scummvm") != 0) {
        jw_ra_catalog_free(catalog);
        return fail(error[0] ? error : "descriptor scrape policy rejected");
    }
    jw_ra_catalog_free(catalog);

    if (write_text(systems,
                   "{\"platform\":\"mac\",\"systems\":["
                   "{\"id\":\"SCUMMVM\",\"extensions\":[\"scummvm\"],"
                   "\"screenscraper_name_source\":\"descriptor\"}]}") != 0 ||
        (catalog = jw_ra_catalog_load(root, error, sizeof(error))) != NULL) {
        jw_ra_catalog_free(catalog);
        return fail("partial scrape policy accepted");
    }

    if (write_text(systems,
                   "{\"platform\":\"mac\",\"systems\":["
                   "{\"id\":\"SCUMMVM\",\"extensions\":[\"scummvm\"],"
                   "\"screenscraper_name_source\":\"descriptor\","
                   "\"screenscraper_lookup_extension\":\"svm\"}]}") != 0 ||
        (catalog = jw_ra_catalog_load(root, error, sizeof(error))) != NULL) {
        jw_ra_catalog_free(catalog);
        return fail("undeclared scrape lookup extension accepted");
    }

    if (write_text(systems,
                   "{\"platform\":\"mac\",\"systems\":["
                   "{\"id\":\"GBA\",\"default_core\":\"mgba\","
                   "\"legacy_flat_core\":\"mgba\"}]}") != 0) {
        return fail("systems fixture restore failed");
    }

    const char *missing_row =
        "{\"id\":\"mgba\",\"type\":\"retroarch\","
        "\"status\":\"packaged\",\"file_name\":\"mgba_libretro.so\"}";
    if (write_cores(cores, missing_row) != 0 ||
        (catalog = jw_ra_catalog_load(root, error, sizeof(error))) != NULL) {
        jw_ra_catalog_free(catalog);
        return fail("missing folder catalog accepted");
    }

    const char *collision_rows =
        "{\"id\":\"a\",\"type\":\"retroarch\",\"status\":\"packaged\","
        "\"file_name\":\"a.so\",\"config_folder\":\"mGBA\"},"
        "{\"id\":\"b\",\"type\":\"retroarch\",\"status\":\"packaged\","
        "\"file_name\":\"b.so\",\"config_folder\":\"mgba\"}";
    if (write_cores(cores, collision_rows) != 0 ||
        (catalog = jw_ra_catalog_load(root, error, sizeof(error))) != NULL) {
        jw_ra_catalog_free(catalog);
        return fail("case-fold collision accepted");
    }

    /* The shipped Dreamcast trio has to coexist: the Fast core's folder is a
       different name from the standard "Flycast", and a case-insensitive
       comparison must not collapse the two into a collision. */
    const char *dreamcast_rows =
        "{\"id\":\"flycast\",\"type\":\"retroarch\",\"status\":\"packaged\","
        "\"file_name\":\"flycast_libretro.so\",\"config_folder\":\"Flycast\"},"
        "{\"id\":\"flycast_fast_umrk\",\"type\":\"retroarch\","
        "\"status\":\"packaged\","
        "\"file_name\":\"flycast_fast_umrk_libretro.so\","
        "\"config_folder\":\"FlyCast Fast UMRK\"}";
    if (write_cores(cores, dreamcast_rows) != 0 ||
        (catalog = jw_ra_catalog_load(root, error, sizeof(error))) == NULL) {
        jw_ra_catalog_free(catalog);
        return fail(error[0] ? error : "Dreamcast core folders collided");
    }
    jw_ra_catalog_free(catalog);

    printf("catalog-folder-test: ok\n");
    return 0;
}
