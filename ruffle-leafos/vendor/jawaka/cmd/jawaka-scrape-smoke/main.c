/* jawaka-scrape-smoke — developer CLI for the ScreenScraper client.

   Looks up one ROM (name + optional md5) and downloads its artwork using
   the built-in priority defaults. Exercises internal/scrape end-to-end on
   the Mac lane without the daemon or any UI.

       jawaka-scrape-smoke [-u user -p pass] [-o out.png] [-m max_dim] \
                           <SYSTEM_TAG> <rom_path>
       jawaka-scrape-smoke --validate -u user -p pass

   Exit codes: 0 found/valid, 1 not found/rejected, 2 error. */

#include "internal/scrape/scrape_systems.h"
#include "internal/scrape/ss_client.h"

#include <libgen.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0) {
    fprintf(stderr,
            "usage: %s [-u user -p pass] [-o out.png] [-m max_dim] "
            "<SYSTEM_TAG> <rom_path>\n"
            "       %s --validate -u user -p pass\n",
            argv0, argv0);
}

int main(int argc, char **argv) {
    const char *user = "";
    const char *pass = "";
    const char *out_path = NULL;
    int max_dim = 1000;
    int validate = 0;
    const char *system_tag = NULL;
    const char *rom_path = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            user = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            pass = argv[++i];
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            out_path = argv[++i];
        } else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            max_dim = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--validate") == 0) {
            validate = 1;
        } else if (!system_tag) {
            system_tag = argv[i];
        } else if (!rom_path) {
            rom_path = argv[i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (!jw_ss_available()) {
        fprintf(stderr, "scraping unavailable: no dev credentials in this build "
                        "(create .env.local, see .env.example)\n");
        return 2;
    }
    if (jw_ss_is_debug())
        fprintf(stderr, "[debug] ScreenScraper debug mode enabled\n");

    jw_ss_client client = {0};
    snprintf(client.username, sizeof(client.username), "%s", user);
    snprintf(client.password, sizeof(client.password), "%s", pass);

    if (validate) {
        jw_ss_user info;
        int rc = jw_ss_validate_user(&client, &info);
        if (rc == 0) {
            printf("valid: level=%d maxthreads=%d quota=%d/%d\n",
                   info.user_level, info.max_threads,
                   info.requests_today, info.max_requests);
            return 0;
        }
        fprintf(stderr, "%s\n", jw_ss_last_error() ? jw_ss_last_error()
                                                   : "validation failed");
        return rc == 1 ? 1 : 2;
    }

    if (!system_tag || !rom_path) {
        usage(argv[0]);
        return 2;
    }

    int system_id = jw_scrape_platform_id(system_tag);
    if (system_id < 0) {
        fprintf(stderr, "no ScreenScraper mapping for system tag '%s'\n",
                system_tag);
        return 2;
    }

    char rom_copy[PATH_MAX];
    snprintf(rom_copy, sizeof(rom_copy), "%s", rom_path);
    const char *rom_name = basename(rom_copy);

    jw_ss_result result;
    jw_ss_search_status search_status = jw_ss_search_rom(
        &client, rom_name, rom_path, system_id,
        jw_ss_default_artwork_priority,
        jw_ss_default_artwork_priority_count,
        jw_ss_default_region_priority,
        jw_ss_default_region_priority_count,
        &result);
    if (search_status == JW_SS_SEARCH_NOT_FOUND) {
        printf("not found: %s (system %s/%d)\n", rom_name, system_tag, system_id);
        return 1;
    }
    if (search_status == JW_SS_SEARCH_NO_MEDIA) {
        printf("no media: %s (system %s/%d)\n", rom_name, system_tag, system_id);
        return 1;
    }
    if (search_status != JW_SS_SEARCH_FOUND) {
        fprintf(stderr, "lookup failed: %s\n",
                jw_ss_last_error() ? jw_ss_last_error() : "unknown error");
        return 2;
    }

    printf("found: %s\n", result.game_name);
    printf("media format: %s\n", result.media_format);
    if (result.max_requests > 0)
        printf("quota: %d/%d, maxthreads=%d\n", result.requests_today,
               result.max_requests, result.max_threads);

    char default_out[PATH_MAX];
    if (!out_path) {
        snprintf(default_out, sizeof(default_out), "%s.scrape.png", rom_name);
        out_path = default_out;
    }

    int rc = jw_ss_download_media(&client, result.media_url, out_path, max_dim);
    if (rc != 0) {
        fprintf(stderr, "download failed: %s\n",
                jw_ss_last_error() ? jw_ss_last_error() : "unknown error");
        return 2;
    }

    printf("saved: %s\n", out_path);
    return 0;
}
