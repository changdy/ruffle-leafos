#include "internal/launcher/core_selection.h"
#include "internal/launcher/standalone_policy.h"
#include "internal/retroarch/catalog.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#define JW_CATALOG_SMOKE_MAX_CHOICES 32

typedef struct {
    const jw_ra_catalog *catalog;
    const char *core_dir;
    const char *platform_dir;
    const char *rom_path;
} jw_catalog_smoke_launch;

/* The catalog half of jawakad's per-candidate availability: the exact core's
   file or executable launcher, plus release content restrictions for path
   cores. The daemon additionally runs its PICO-8 preflight. */
static bool jw_catalog_smoke_available(void *userdata, const char *core_id) {
    const jw_catalog_smoke_launch *launch = userdata;
    const jw_ra_core *core = jw_ra_catalog_find_core(launch->catalog, core_id);
    if (!jw_ra_catalog_core_available(launch->catalog, core, launch->core_dir,
                                      launch->platform_dir)) {
        return false;
    }
    if (jw_ra_core_is_packaged_retroarch(core)) {
        return true;
    }
    char path[PATH_MAX];
    if (jw_ra_catalog_resolve_core_path(launch->catalog, core, NULL,
                                        launch->platform_dir, true, path,
                                        sizeof(path)) != 0) {
        return false;
    }
    jw_standalone_policy policy =
        jw_standalone_policy_resolve(core->id, path, core->provider);
    return jw_standalone_policy_supports_content(&policy, launch->rom_path);
}

/* --launch: which exact core a library launch resolves to. Empty choice
   arguments mean no saved choice at that scope. */
static int jw_catalog_smoke_launch_main(int argc, char **argv) {
    if (argc != 9) {
        fprintf(stderr,
                "usage: %s --launch <sdcard-root> <system-id> <cores-dir> "
                "<platform-dir> <rom-path> <game-choice> <system-choice>\n",
                argv[0]);
        return 2;
    }

    char error[256];
    jw_ra_catalog *catalog = jw_ra_catalog_load(argv[2], error, sizeof(error));
    if (!catalog) {
        fprintf(stderr, "catalog load failed: %s\n",
                error[0] ? error : "unknown");
        return 1;
    }
    const jw_ra_system *system = jw_ra_catalog_find_system(catalog, argv[3]);
    if (!system) {
        system = jw_ra_catalog_match_system_folder(catalog, argv[3]);
    }
    if (!system) {
        fprintf(stderr, "system missing from metadata: %s\n", argv[3]);
        jw_ra_catalog_free(catalog);
        return 1;
    }

    jw_catalog_smoke_launch launch = {
        .catalog = catalog,
        .core_dir = argv[4],
        .platform_dir = argv[5],
        .rom_path = argv[6],
    };
    jw_core_selection selection = jw_core_select(
        jw_core_selection_saved_choice(argv[7], argv[8]), system->default_core,
        (const char *const *)system->alternate_cores.items,
        system->alternate_cores.count, jw_catalog_smoke_available, &launch);
    const jw_ra_core *core =
        selection.core_id ? jw_ra_catalog_find_core(catalog, selection.core_id)
                          : NULL;
    if (core) {
        printf("launch\t%s\t%s\t%s\n", core->id, core->type,
               jw_core_selection_origin_name(selection.origin));
    } else {
        printf("launch\tnone\n");
    }

    jw_ra_catalog_free(catalog);
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--launch") == 0) {
        return jw_catalog_smoke_launch_main(argc, argv);
    }
    if (argc != 5 && argc != 6) {
        fprintf(stderr,
                "usage: %s <sdcard-root> <system-id> <cores-dir> <platform-dir> "
                "[preferred-core-id]\n",
                argv[0]);
        return 2;
    }

    /* Optional: what a persisted per-system or per-game core choice resolves
       to. A path core is launched by its own launcher rather than through the
       RetroArch core-file resolver, so this reports the launcher the choice
       selects. */
    const char *preferred_core_id = (argc == 6) ? argv[5] : NULL;

    char error[256];
    jw_ra_catalog *catalog = jw_ra_catalog_load(argv[1], error, sizeof(error));
    if (!catalog) {
        fprintf(stderr, "catalog load failed: %s\n",
                error[0] ? error : "unknown");
        return 1;
    }

    jw_ra_core_choice choices[JW_CATALOG_SMOKE_MAX_CHOICES];
    size_t count = 0;
    if (jw_ra_catalog_list_system_cores(catalog, argv[2], argv[3], argv[4],
                                        choices, JW_CATALOG_SMOKE_MAX_CHOICES,
                                        &count) != 0) {
        fprintf(stderr, "core choice listing failed for %s\n", argv[2]);
        jw_ra_catalog_free(catalog);
        return 1;
    }

    printf("count\t%zu\n", count);
    for (size_t i = 0; i < count; i++) {
        printf("choice\t%zu\t%s\t%s\t%s\t%s\t%s\t%s\n",
               i,
               choices[i].id,
               choices[i].type,
               choices[i].is_default ? "default" : "alternate",
               choices[i].display_name,
               choices[i].type[0] && choices[i].type[0] == 'p'
                   ? choices[i].path
                   : choices[i].file_name,
               choices[i].requires_direct_drm ? "direct-drm" : "shared-drm");
    }

    if (preferred_core_id) {
        int matched = 0;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(choices[i].id, preferred_core_id) != 0) {
                continue;
            }
            printf("preferred\t%s\t%s\t%s\t%s\n",
                   choices[i].id,
                   choices[i].type,
                   choices[i].is_default ? "default" : "alternate",
                   choices[i].type[0] == 'p' ? choices[i].path
                                             : choices[i].file_name);
            matched = 1;
            break;
        }
        if (!matched) {
            printf("preferred\t%s\tunavailable\n", preferred_core_id);
        }
    }

    jw_ra_catalog_free(catalog);
    return 0;
}
