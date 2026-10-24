/* Missing-art accounting against a real scanned library: art in any location
   the scan would use counts as existing, including art added before a rescan,
   so bulk scraping skips it, while an explicit single-game scrape still runs
   and writes PNG to its usual destination. ScreenScraper is stubbed below;
   nothing here touches the network. */

#include "internal/db/db.h"
#include "internal/discovery/discovery.h"
#include "internal/scrape/scrape_worker.h"
#include "internal/scrape/ss_client.h"

#include <assert.h>
#include <dirent.h>
#include <limits.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SEARCH_LOG_MAX 64

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_searched[SEARCH_LOG_MAX][256];
static int g_search_count;
static char g_downloaded[PATH_MAX];

bool jw_ss_available(void) {
    return true;
}

bool jw_ss_is_debug(void) {
    return false;
}

const char *jw_ss_last_error(void) {
    return "stub scraper";
}

int jw_ss_last_errno(void) {
    return 0;
}

jw_ss_search_status jw_ss_search_rom_platforms(
    const jw_ss_client *client,
    const char *rom_name,
    const char *rom_abs_path,
    const int *system_ids, size_t system_count,
    const char *const *artwork_types,
    int artwork_count,
    const char *const *region_prio,
    int region_count,
    jw_ss_result *result) {
    (void)client;
    (void)rom_abs_path;
    (void)system_ids;
    (void)system_count;
    (void)artwork_types;
    (void)artwork_count;
    (void)region_prio;
    (void)region_count;
    memset(result, 0, sizeof(*result));
    pthread_mutex_lock(&g_mu);
    if (g_search_count < SEARCH_LOG_MAX) {
        snprintf(g_searched[g_search_count++], sizeof(g_searched[0]), "%s", rom_name);
    }
    pthread_mutex_unlock(&g_mu);
    if (strcmp(rom_name, "Canon.gba") == 0) {
        snprintf(result->media_url, sizeof(result->media_url), "stub://canon");
        snprintf(result->media_format, sizeof(result->media_format), "png");
        return JW_SS_SEARCH_FOUND;
    }
    return JW_SS_SEARCH_NOT_FOUND;
}

int jw_ss_download_media(const jw_ss_client *client, const char *media_url,
                         const char *dest_path, int max_dim) {
    (void)client;
    (void)media_url;
    (void)max_dim;
    FILE *f = fopen(dest_path, "wb");
    if (!f) {
        return -1;
    }
    fputs("png", f);
    fclose(f);
    pthread_mutex_lock(&g_mu);
    snprintf(g_downloaded, sizeof(g_downloaded), "%s", dest_path);
    pthread_mutex_unlock(&g_mu);
    return 0;
}

static void make_dirs(const char *path) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static void copy_file(const char *from, const char *to) {
    FILE *in = fopen(from, "rb");
    assert(in);
    FILE *out = fopen(to, "wb");
    assert(out);
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        assert(fwrite(buf, 1, n, out) == n);
    }
    fclose(in);
    fclose(out);
}

static int is_file(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static void remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            struct stat st;
            if (lstat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                remove_tree(child);
            } else {
                unlink(child);
            }
        }
        closedir(d);
    }
    rmdir(path);
}

static void expect_missing(const char *system, int want_missing, int want_total) {
    int missing = -1, total = -1;
    const char *error = NULL;
    assert(jw_scrape_count_missing_system(system, &missing, &total, &error) == 0);
    if (missing != want_missing || total != want_total) {
        fprintf(stderr, "%s: want %d/%d missing, got %d/%d\n",
                system, want_missing, want_total, missing, total);
    }
    assert(missing == want_missing);
    assert(total == want_total);
}

static void expect_system_enqueue(const char *system, int want_enqueued,
                                  int want_skipped) {
    jw_scrape_enqueue_result r;
    const char *error = NULL;
    int rc = jw_scrape_enqueue_system_full(system, true, &r, &error);
    if (rc != want_enqueued || r.enqueued != want_enqueued ||
        r.skipped_existing != want_skipped) {
        fprintf(stderr, "%s: want enqueued=%d skipped=%d, got rc=%d enqueued=%d skipped=%d (%s)\n",
                system, want_enqueued, want_skipped, rc, r.enqueued,
                r.skipped_existing, error ? error : "");
    }
    assert(rc == want_enqueued);
    assert(r.enqueued == want_enqueued);
    assert(r.skipped_existing == want_skipped);
}

static void wait_idle(void) {
    for (int i = 0; i < 500; i++) {
        jw_scrape_status_info s;
        jw_scrape_status(&s);
        if (s.state == JW_SCRAPE_IDLE && s.queued == 0 && s.active == 0) {
            return;
        }
        usleep(10 * 1000);
    }
    fprintf(stderr, "scrape worker did not go idle\n");
    abort();
}

static int searched(const char *rom_name) {
    int hits = 0;
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < g_search_count; i++) {
        if (strcmp(g_searched[i], rom_name) == 0) hits++;
    }
    pthread_mutex_unlock(&g_mu);
    return hits;
}

static int search_count(void) {
    pthread_mutex_lock(&g_mu);
    int n = g_search_count;
    pthread_mutex_unlock(&g_mu);
    return n;
}

static void rom_path_for(const char *db_path, const char *system, const char *name,
                         char *out, size_t out_size) {
    jw_game_entry *games = calloc(64, sizeof(*games));
    assert(games);
    int count = 0;
    assert(jw_db_list_games_for_system(db_path, system, games, 64, &count) == 0);
    out[0] = '\0';
    for (int i = 0; i < count; i++) {
        if (strcmp(games[i].name, name) == 0) {
            snprintf(out, out_size, "%s", games[i].rom_path);
        }
    }
    free(games);
    assert(out[0]);
}

int main(void) {
    const char *defaults = getenv("JW_TEST_DEFAULTS_DIR");
    assert(defaults && defaults[0]);
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";

    char root[PATH_MAX];
    snprintf(root, sizeof(root), "%s/jawaka-scrape-art.XXXXXX", tmp);
    assert(mkdtemp(root));

    char sd[PATH_MAX], secondary[PATH_MAX], db_path[PATH_MAX], platform[PATH_MAX];
    char path[PATH_MAX], from[PATH_MAX];
    snprintf(sd, sizeof(sd), "%s/sd", root);
    snprintf(secondary, sizeof(secondary), "%s/secondary-sd", root);
    snprintf(db_path, sizeof(db_path), "%s/library.db", root);
    snprintf(platform, sizeof(platform), "%s/.system/leaf/platforms/mlp1", sd);

    snprintf(path, sizeof(path), "%s/defaults", platform);
    make_dirs(path);
    static const char *const catalog_files[] = { "systems.json", "cores.json", "arcade_names.txt" };
    for (size_t i = 0; i < 3; i++) {
        snprintf(from, sizeof(from), "%s/%s", defaults, catalog_files[i]);
        snprintf(path, sizeof(path), "%s/defaults/%s", platform, catalog_files[i]);
        copy_file(from, path);
    }
    setenv("UMRK_PLATFORM_PATH", platform, 1);
    snprintf(path, sizeof(path), "%s:%s", sd, secondary);
    setenv("SDCARD_PATHS", path, 1);

    static const char *const dirs[] = {
        "Roms/GBA/Imgs", "Roms/FC", "Roms/MD", "Images/GBA", "Images/FC", "Images/MD",
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", sd, dirs[i]);
        make_dirs(path);
    }
    snprintf(path, sizeof(path), "%s/Roms/GBA/Imgs", secondary);
    make_dirs(path);

    static const char *const roms[] = {
        "Roms/GBA/Canon.gba", "Roms/GBA/Fallback.gba", "Roms/GBA/Missing.gba",
        "Roms/FC/AliasOnly.nes", "Roms/MD/Sonic.md.zip",
    };
    for (size_t i = 0; i < sizeof(roms) / sizeof(roms[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", sd, roms[i]);
        write_file(path, "rom");
    }
    snprintf(path, sizeof(path), "%s/Roms/GBA/Second.gba", secondary);
    write_file(path, "rom");

    sqlite3 *db = NULL;
    jw_scan_result scan;
    assert(jw_db_open(db_path, &db) == 0 && db);
    assert(jw_db_apply_schema(db) == 0);
    assert(jw_scan_library(db, sd, &scan) == 0);
    sqlite3_close(db);

    assert(jw_scrape_worker_start(db_path, sd) == 0);

    /* Nothing has art yet. */
    expect_missing("GBA", 4, 4);
    expect_missing("FC", 1, 1);
    expect_missing("MD", 1, 1);

    /* Hand-dropped art after the scan, in every location the scan uses:
       canonical Images/GBA, physical-folder Images/FC (FC's canonical root
       is Images/NES) and Images/MD (canonical Images/GENESIS, stem from
       "Sonic.md.zip"), Roms/<folder>/Imgs, and a secondary source. */
    static const char *const art[] = {
        "Images/GBA/Canon.jpeg", "Roms/GBA/Imgs/Fallback.JPEG",
        "Images/FC/AliasOnly.jpg", "Images/MD/Sonic.jpg",
    };
    for (size_t i = 0; i < sizeof(art) / sizeof(art[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", sd, art[i]);
        write_file(path, "jpeg");
    }
    snprintf(path, sizeof(path), "%s/Roms/GBA/Imgs/Second.jpg", secondary);
    write_file(path, "jpeg");

    expect_missing("GBA", 1, 4);
    expect_missing("FC", 0, 1);
    expect_missing("MD", 0, 1);

    /* An SD repair truncates damaged art to 0 bytes: that game needs art again. */
    snprintf(path, sizeof(path), "%s/Images/GBA/Canon.jpeg", sd);
    write_file(path, "");
    expect_missing("GBA", 2, 4);
    write_file(path, "jpeg");
    expect_missing("GBA", 1, 4);

    jw_scrape_missing_row rows[16];
    int row_count = 0, total_missing = -1;
    assert(jw_scrape_missing_counts(rows, 16, &row_count, &total_missing) == 0);
    assert(row_count == 3);
    assert(total_missing == 1);

    /* Scrape-all (missing only) enqueues just the artless game. */
    expect_system_enqueue("GBA", 1, 3);
    expect_system_enqueue("FC", 0, 1);
    expect_system_enqueue("MD", 0, 1);
    wait_idle();
    assert(search_count() == 1);
    assert(searched("Missing.gba") == 1);

    /* An explicit single-game scrape still runs for a game with JPEG art and
       writes PNG to the scraper's destination, leaving the JPEG in place. */
    char canon_rom[512];
    rom_path_for(db_path, "GBA", "Canon", canon_rom, sizeof(canon_rom));
    const char *error = NULL;
    assert(jw_scrape_enqueue_game("GBA", canon_rom, &error) == 1);
    wait_idle();
    assert(searched("Canon.gba") == 1);
    snprintf(path, sizeof(path), "%s/Images/GBA/Canon.png", sd);
    pthread_mutex_lock(&g_mu);
    assert(strcmp(g_downloaded, path) == 0);
    pthread_mutex_unlock(&g_mu);
    assert(is_file(path));
    snprintf(path, sizeof(path), "%s/Images/GBA/Canon.jpeg", sd);
    assert(is_file(path));
    expect_missing("GBA", 1, 4);

    jw_scrape_worker_stop();
    remove_tree(root);
    printf("scrape-art-test: ok\n");
    return 0;
}
