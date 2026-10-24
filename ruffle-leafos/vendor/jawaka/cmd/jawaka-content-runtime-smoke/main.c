#define _POSIX_C_SOURCE 200809L

#include "internal/catalog/effective.h"
#include "internal/retroarch/catalog.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int select_generation(const char *sdcard_root, const char *generation) {
    char dir[PATH_MAX], current[PATH_MAX], temp[PATH_MAX], bytes[96];
    if (jw_catalog_dir(sdcard_root, dir, sizeof(dir)) != 0 ||
        snprintf(current, sizeof(current), "%s/current", dir) >= (int)sizeof(current) ||
        snprintf(temp, sizeof(temp), "%s/current.runtime-smoke", dir) >= (int)sizeof(temp) ||
        snprintf(bytes, sizeof(bytes), "%s\n", generation) >= (int)sizeof(bytes)) {
        return -1;
    }
    FILE *file = fopen(temp, "wb");
    if (!file) return -1;
    int ok = fputs(bytes, file) >= 0 && fflush(file) == 0 &&
             fsync(fileno(file)) == 0 && fclose(file) == 0 &&
             rename(temp, current) == 0;
    if (!ok) unlink(temp);
    return ok ? 0 : -1;
}

static int inspect(const char *sdcard_root, const char *core_dir,
                   const char *platform_dir, const char *system,
                   const char *label) {
    char error[256], core_file[PATH_MAX], core_id[64], diagnostic[256];
    const jw_ra_catalog *catalog = jw_ra_catalog_get(sdcard_root, error, sizeof(error));
    if (!catalog ||
        jw_ra_catalog_resolve_core_file(catalog, system, core_dir,
                                        core_file, sizeof(core_file),
                                        core_id, sizeof(core_id),
                                        diagnostic, sizeof(diagnostic)) != 0) {
        fprintf(stderr, "runtime-smoke: core resolve failed: %s%s\n",
                error, diagnostic);
        return -1;
    }
    char info[PATH_MAX], standalone[PATH_MAX], flat[PATH_MAX], photo[PATH_MAX];
    const jw_ra_core *retro_core = jw_ra_catalog_find_core(catalog, "contenttest");
    const jw_ra_core *path_core = jw_ra_catalog_find_core(catalog, "contentpath");
    const jw_ra_system *runtime_system =
        jw_ra_catalog_find_system(catalog, "CONTENTTEST");
    if (!retro_core || !retro_core->provider ||
        strcmp(retro_core->provider, "mac/ContentTest.pak") != 0 ||
        !retro_core->source_id || strcmp(retro_core->source_id, "primary") != 0 ||
        !retro_core->core_root_rel || strcmp(retro_core->core_root_rel, "cores") != 0 ||
        !path_core || !path_core->core_root_rel || !runtime_system ||
        !runtime_system->provider ||
        strcmp(runtime_system->provider, "mac/ContentTest.pak") != 0 ||
        !runtime_system->source_id ||
        strcmp(runtime_system->source_id, "primary") != 0 ||
        strcmp(path_core->core_root_rel, "emulators") != 0 ||
        jw_ra_catalog_info_dir(catalog, info, sizeof(info)) != 0 ||
        jw_ra_catalog_resolve_system_icon_path(catalog, runtime_system, false,
                                               flat, sizeof(flat)) != 0 ||
        jw_ra_catalog_resolve_system_icon_path(catalog, runtime_system, true,
                                               photo, sizeof(photo)) != 0 ||
        jw_ra_catalog_resolve_core_path(catalog, path_core, core_dir,
                                        platform_dir, true, standalone,
                                        sizeof(standalone)) != 0) {
        fprintf(stderr, "runtime-smoke: info or standalone resolve failed\n");
        return -1;
    }
    printf("%s\t%s\t%s\t%s\t%s\t%s\n", label, core_file, standalone,
           info, flat, photo);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: %s sdcard core-dir platform-dir system old-gen new-gen\n",
                argv[0]);
        return 2;
    }
    unsetenv("APPS_PATH");
    if (select_generation(argv[1], argv[5]) != 0 ||
        inspect(argv[1], argv[2], argv[3], argv[4], "old") != 0 ||
        select_generation(argv[1], argv[6]) != 0 ||
        inspect(argv[1], argv[2], argv[3], argv[4], "new") != 0) {
        return 1;
    }
    return 0;
}
