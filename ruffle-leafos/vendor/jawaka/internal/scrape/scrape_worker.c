#include "internal/scrape/scrape_worker.h"
#include "internal/scrape/scrape_identity.h"
#include "internal/scrape/scrape_systems.h"
#include "internal/scrape/ss_client.h"
#include "internal/core/log.h"
#include "internal/db/db.h"
#include "internal/discovery/art_path.h"
#include "internal/retroarch/catalog.h"
#include "internal/storage/health.h"
#include "internal/storage/sources.h"

#include <errno.h>
#include <sys/stat.h>
#include <libgen.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#define JW__SCRAPE_QUEUE_MAX   4096
#define JW__SCRAPE_LIST_MAX    4096   /* games per system when expanding a batch */
#define JW__SCRAPE_SYS_MAX     256    /* systems when expanding an "all" batch    */
#define JW__SCRAPE_PRIO_MAX    16
#define JW__SCRAPE_MAX_WORKERS 4      /* hard cap regardless of account allowance */
#define JW__SCRAPE_ETA_SAMPLES 16

typedef struct {
    unsigned row_id;
    char system[64];
    /* Canonical public image folder (basename of the catalog image_root, e.g.
       "NES" for system "FC"), resolved on the enqueue/main thread so the worker
       never touches the unsynchronized catalog cache. */
    char image_dir[64];
    int platform_ids[JW_SCRAPE_MAX_PLATFORM_IDS];
    size_t platform_id_count;
    jw_ra_scrape_name_source name_source;
    char lookup_extension[17];
    char effective_title[256];
    char rom_path[512];
    /* Requeued after a storage failure: a second storage failure while the
       card reports writable is a real error, not another pause. */
    bool storage_requeued;
    /* Artwork already promoted before the DB update failed; the retry
       reconciles the file instead of downloading it again. */
    bool art_saved;
} jw__scrape_item;

typedef struct {
    unsigned id;
    jw_scrape_row_state state;
    char display_name[256];
    char system[64];
    char rom_path[512];
    char output_path[512];
    char message[256];
    long long started_ms;
    long long finished_ms;
} jw__scrape_row;

typedef struct {
    bool            used;
    jw__scrape_item item;
    atomic_int      interrupt;   /* cancels this slot's HTTP work */
} jw__active_slot;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    bool            running;        /* pool alive */
    bool            shutdown;
    pthread_t       threads[JW__SCRAPE_MAX_WORKERS];
    int             thread_count;

    char db_path[PATH_MAX];
    char sdcard_root[PATH_MAX];

    jw__scrape_item *queue;         /* FIFO: [head, head+count) */
    int  head;
    int  count;
    jw__scrape_row *rows;           /* current-session rows; terminal rows live until clear/new batch */
    int  row_count;
    unsigned next_row_id;

    jw__active_slot active[JW__SCRAPE_MAX_WORKERS];
    int  active_count;
    int  permits;                   /* concurrent allowance: 1..MAX_WORKERS,
                                       follows the account's maxthreads */
    bool paused_quota;              /* daily quota exhausted; queue retained */
    bool paused_storage;            /* a required card or the DB is not writable */
    char storage_block_path[PATH_MAX]; /* dependency that paused the batch */
    bool db_readonly_logged;        /* one log line per read-only DB episode */

    int requests_today;
    int max_requests;
    int max_threads;
    int eta_samples[JW__SCRAPE_ETA_SAMPLES];
    int eta_sample_count;
    int eta_sample_pos;
    char message[256];
} jw__w = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
    .permits = 1,
};

/* ── Settings: credentials + priorities ──────────────────────────────── */

typedef struct {
    char user[256];
    char pass[256];
    int  max_threads;
    char *artwork[JW__SCRAPE_PRIO_MAX];
    int  artwork_count;
    char *regions[JW__SCRAPE_PRIO_MAX];
    int  region_count;
} jw__scrape_prefs;

/* Split a comma-separated settings value into strdup'd entries. */
static int jw__split_csv(const char *csv, char **out, int max) {
    int n = 0;
    const char *p = csv;
    while (p && *p && n < max) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        while (len > 0 && (*p == ' ' || *p == '\t')) { p++; len--; }
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
        if (len > 0) {
            char *entry = malloc(len + 1);
            if (!entry) break;
            memcpy(entry, p, len);
            entry[len] = '\0';
            out[n++] = entry;
        }
        p = comma ? comma + 1 : NULL;
    }
    return n;
}

static void jw__prefs_load(jw__scrape_prefs *prefs) {
    memset(prefs, 0, sizeof(*prefs));

    char artwork_csv[512] = "";
    char region_csv[256] = "";
    char max_threads_buf[32] = "";
    jw_db_setting_query queries[] = {
        { "screenscraper_user", prefs->user, sizeof(prefs->user), 0 },
        { "screenscraper_pass", prefs->pass, sizeof(prefs->pass), 0 },
        { "screenscraper_maxthreads", max_threads_buf, sizeof(max_threads_buf), 0 },
        { "scrape.artwork_priority", artwork_csv, sizeof(artwork_csv), 0 },
        { "scrape.region_priority", region_csv, sizeof(region_csv), 0 },
    };
    jw_db_get_settings(jw__w.db_path, queries, 5);
    prefs->max_threads = atoi(max_threads_buf);

    prefs->artwork_count = jw__split_csv(artwork_csv, prefs->artwork,
                                         JW__SCRAPE_PRIO_MAX);
    if (prefs->artwork_count == 0) {
        for (int i = 0; i < jw_ss_default_artwork_priority_count &&
                        i < JW__SCRAPE_PRIO_MAX; i++) {
            prefs->artwork[i] = strdup(jw_ss_default_artwork_priority[i]);
            prefs->artwork_count = i + 1;
        }
    }
    prefs->region_count = jw__split_csv(region_csv, prefs->regions,
                                        JW__SCRAPE_PRIO_MAX);
    if (prefs->region_count == 0) {
        for (int i = 0; i < jw_ss_default_region_priority_count &&
                        i < JW__SCRAPE_PRIO_MAX; i++) {
            prefs->regions[i] = strdup(jw_ss_default_region_priority[i]);
            prefs->region_count = i + 1;
        }
    }
}

static void jw__prefs_free(jw__scrape_prefs *prefs) {
    for (int i = 0; i < prefs->artwork_count; i++) free(prefs->artwork[i]);
    for (int i = 0; i < prefs->region_count; i++) free(prefs->regions[i]);
    prefs->artwork_count = prefs->region_count = 0;
}

/* ── Path resolution ─────────────────────────────────────────────────── */

/* Strip the extension from a rom filename to get the art basename
   (matches discovery's jw__title_from_filename). */
static void jw__art_base(const char *rom_name, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", rom_name);
    char *dot = strrchr(out, '.');
    if (dot) *dot = '\0';
}

static long long jw__now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000LL + (long long)ts.tv_nsec / 1000000LL;
}

static bool jw__row_state_is_terminal(jw_scrape_row_state state) {
    return state == JW_SCRAPE_ROW_DONE ||
           state == JW_SCRAPE_ROW_NOT_FOUND ||
           state == JW_SCRAPE_ROW_ERROR ||
           state == JW_SCRAPE_ROW_CANCELLED;
}

static bool jw__row_state_is_busy(jw_scrape_row_state state) {
    return state == JW_SCRAPE_ROW_HASH ||
           state == JW_SCRAPE_ROW_SEARCH ||
           state == JW_SCRAPE_ROW_DOWNLOAD ||
           state == JW_SCRAPE_ROW_SAVE;
}

static void jw__display_name_from_rom_path(const char *rom_path,
                                           char *out, size_t out_size) {
    char copy[512];
    snprintf(copy, sizeof(copy), "%s", rom_path ? rom_path : "");
    char base[256];
    jw__art_base(basename(copy), base, sizeof(base));
    snprintf(out, out_size, "%s", base[0] ? base : (rom_path ? rom_path : ""));
}

static jw__scrape_row *jw__row_find_locked(unsigned row_id) {
    for (int i = 0; i < jw__w.row_count; i++) {
        if (jw__w.rows[i].id == row_id) {
            return &jw__w.rows[i];
        }
    }
    return NULL;
}

static void jw__row_set_locked(unsigned row_id, jw_scrape_row_state state,
                               const char *message) {
    jw__scrape_row *row = jw__row_find_locked(row_id);
    if (!row) {
        return;
    }
    row->state = state;
    if (message) {
        snprintf(row->message, sizeof(row->message), "%s", message);
    }
    if (jw__row_state_is_busy(state) && row->started_ms <= 0) {
        row->started_ms = jw__now_ms();
    }
    if (jw__row_state_is_terminal(state) && row->finished_ms <= 0) {
        row->finished_ms = jw__now_ms();
    }
}

static void jw__row_set(unsigned row_id, jw_scrape_row_state state,
                        const char *message) {
    pthread_mutex_lock(&jw__w.mu);
    jw__row_set_locked(row_id, state, message);
    pthread_mutex_unlock(&jw__w.mu);
}

static void jw__row_set_output(unsigned row_id, const char *output_path) {
    pthread_mutex_lock(&jw__w.mu);
    jw__scrape_row *row = jw__row_find_locked(row_id);
    if (row && output_path) {
        size_t n = strlen(output_path);
        if (n >= sizeof(row->output_path)) {
            n = sizeof(row->output_path) - 1;
        }
        memcpy(row->output_path, output_path, n);
        row->output_path[n] = '\0';
    }
    pthread_mutex_unlock(&jw__w.mu);
}

static void jw__eta_record_done_locked(const jw__scrape_row *row) {
    if (!row || row->started_ms <= 0 || row->finished_ms <= row->started_ms) {
        return;
    }
    long long duration_ms = row->finished_ms - row->started_ms;
    int duration_s = (int)((duration_ms + 999) / 1000);
    if (duration_s < 1) {
        duration_s = 1;
    }
    jw__w.eta_samples[jw__w.eta_sample_pos] = duration_s;
    jw__w.eta_sample_pos =
        (jw__w.eta_sample_pos + 1) % JW__SCRAPE_ETA_SAMPLES;
    if (jw__w.eta_sample_count < JW__SCRAPE_ETA_SAMPLES) {
        jw__w.eta_sample_count++;
    }
}

static int jw__eta_seconds_locked(void) {
    if (jw__w.eta_sample_count < 2) {
        return -1;
    }
    int remaining = 0;
    for (int i = 0; i < jw__w.row_count; i++) {
        if (!jw__row_state_is_terminal(jw__w.rows[i].state)) {
            remaining++;
        }
    }
    if (remaining <= 0) {
        return 0;
    }
    int total = 0;
    for (int i = 0; i < jw__w.eta_sample_count; i++) {
        total += jw__w.eta_samples[i];
    }
    return (total / jw__w.eta_sample_count) * remaining;
}

static void jw__clear_finished_batch_locked(void) {
    jw__w.row_count = 0;
    jw__w.next_row_id = 1;
    jw__w.requests_today = 0;
    jw__w.max_requests = 0;
    jw__w.max_threads = 0;
    jw__w.eta_sample_count = 0;
    jw__w.eta_sample_pos = 0;
    memset(jw__w.eta_samples, 0, sizeof(jw__w.eta_samples));
    jw__w.message[0] = '\0';
}

static void jw__count_rows_locked(int *total, int *done, int *found,
                                  int *not_found, int *failed,
                                  int *cancelled, int *queued, int *active) {
    int t = 0, d = 0, f = 0, nf = 0, er = 0, c = 0, q = 0, a = 0;
    for (int i = 0; i < jw__w.row_count; i++) {
        jw_scrape_row_state s = jw__w.rows[i].state;
        t++;
        if (s == JW_SCRAPE_ROW_QUEUED) {
            q++;
        } else if (jw__row_state_is_busy(s)) {
            a++;
        } else if (s == JW_SCRAPE_ROW_DONE) {
            d++;
            f++;
        } else if (s == JW_SCRAPE_ROW_NOT_FOUND) {
            d++;
            nf++;
        } else if (s == JW_SCRAPE_ROW_ERROR) {
            d++;
            er++;
        } else if (s == JW_SCRAPE_ROW_CANCELLED) {
            d++;
            c++;
        }
    }
    if (total) *total = t;
    if (done) *done = d;
    if (found) *found = f;
    if (not_found) *not_found = nf;
    if (failed) *failed = er;
    if (cancelled) *cancelled = c;
    if (queued) *queued = q;
    if (active) *active = a;
}

/* Resolve everything needed to land art for one item. dest_abs is the target
   PNG; image_db is the games.image_path value (relative on the primary
   source, absolute elsewhere). */
/* Canonical public image folder for a system. Games store the canonical catalog
   id (e.g. "FC") whose image_root is the public folder ("Images/NES"); art must
   be written/checked there, not under the internal id, or it lands in a duplicate
   alias folder and reads back as missing. Falls back to the system id when the
   catalog or system is unavailable (compat mode). Call only on a thread that owns
   catalog access (enqueue/main thread) — jw_ra_catalog_get's cache is not
   synchronized; the worker reads the pre-resolved item->image_dir instead. */
static void jw__image_dir_for_system(const char *system, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", system ? system : "");
    if (!system || !system[0]) return;
    char error[256];
    const jw_ra_catalog *catalog = jw_ra_catalog_get(jw__w.sdcard_root, error, sizeof(error));
    if (!catalog) return;
    const jw_ra_system *sys = jw_ra_catalog_find_system(catalog, system);
    if (!sys || !sys->image_root || !sys->image_root[0]) return;
    const char *slash = strrchr(sys->image_root, '/');
    const char *dir = slash ? slash + 1 : sys->image_root;
    if (dir[0]) snprintf(out, out_size, "%s", dir);
}

static size_t jw__platform_ids_for_system(const char *system,
                                          int *out, size_t capacity) {
    char error[256];
    const jw_ra_catalog *catalog =
        jw_ra_catalog_get(jw__w.sdcard_root, error, sizeof(error));
    return jw_scrape_platform_ids_for_catalog(catalog, system, out, capacity);
}

static void jw__scrape_policy_for_system(
    const char *system,
    jw_ra_scrape_name_source *name_source,
    char *lookup_extension,
    size_t lookup_extension_size) {
    *name_source = JW_RA_SCRAPE_NAME_FILENAME;
    lookup_extension[0] = '\0';
    char error[256];
    const jw_ra_catalog *catalog =
        jw_ra_catalog_get(jw__w.sdcard_root, error, sizeof(error));
    const jw_ra_system *entry = catalog
                                    ? jw_ra_catalog_find_system(catalog, system)
                                    : NULL;
    if (!entry || entry->screenscraper_name_source !=
                      JW_RA_SCRAPE_NAME_DESCRIPTOR) {
        return;
    }
    *name_source = entry->screenscraper_name_source;
    snprintf(lookup_extension, lookup_extension_size, "%s",
             entry->screenscraper_lookup_extension);
}

static void jw__log_scrape_policy(const char *system) {
    jw_ra_scrape_name_source name_source;
    char extension[17];
    jw__scrape_policy_for_system(system, &name_source,
                                 extension, sizeof(extension));
    if (name_source == JW_RA_SCRAPE_NAME_DESCRIPTOR) {
        jw_log_info("scrape: system=%s name_source=descriptor lookup_extension=%s",
                    system, extension);
    } else {
        jw_log_info("scrape: system=%s name_source=filename", system);
    }
}

static int jw__resolve_paths(const jw__scrape_item *item,
                             char *rom_abs, size_t rom_abs_size,
                             char *rom_name, size_t rom_name_size,
                             char *dest_abs, size_t dest_abs_size,
                             char *image_db, size_t image_db_size) {
    jw_storage_source_list sources;
    if (jw_storage_sources_resolve(jw__w.sdcard_root, &sources) != 0 ||
        sources.count <= 0) {
        return -1;
    }
    if (jw_storage_resolve_path(&sources, item->rom_path,
                                rom_abs, rom_abs_size) != 0) {
        return -1;
    }

    char rom_copy[512];
    snprintf(rom_copy, sizeof(rom_copy), "%s", item->rom_path);
    snprintf(rom_name, rom_name_size, "%s", basename(rom_copy));

    char base[256];
    jw__art_base(rom_name, base, sizeof(base));

    const jw_storage_source *source =
        jw_storage_sources_find_for_path(&sources, rom_abs);
    if (!source) source = jw_storage_sources_primary(&sources);
    if (!source) return -1;

    const char *image_dir = item->image_dir[0] ? item->image_dir : item->system;
    int n = snprintf(dest_abs, dest_abs_size, "%s/%s/%s.png",
                     source->images_path, image_dir, base);
    if (n <= 0 || n >= (int)dest_abs_size) return -1;

    char image_rel[512];
    snprintf(image_rel, sizeof(image_rel), "Images/%s/%s.png",
             image_dir, base);
    return jw_storage_db_path_for_source(source, image_rel, dest_abs,
                                         image_db, image_db_size);
}

/* ── Queue helpers (mutex held) ──────────────────────────────────────── */

static bool jw__pending_contains_locked(const char *system, const char *rom_path) {
    for (int i = 0; i < jw__w.row_count; i++) {
        jw__scrape_row *row = &jw__w.rows[i];
        if (jw__row_state_is_terminal(row->state)) {
            continue;
        }
        if ((!system || strcmp(row->system, system) == 0) &&
            (!rom_path || strcmp(row->rom_path, rom_path) == 0)) {
            return true;
        }
    }
    return false;
}

static int jw__queue_push(const char *system, const char *rom_path) {
    if (jw__w.count == 0 && jw__w.active_count == 0 && !jw__w.paused_quota) {
        /* New batch from idle: terminal rows remain readable until this point. */
        jw__clear_finished_batch_locked();
    }
    if (jw__pending_contains_locked(system, rom_path)) {
        return 0;
    }
    if (jw__w.count >= JW__SCRAPE_QUEUE_MAX ||
        jw__w.row_count >= JW__SCRAPE_QUEUE_MAX) {
        return -1;
    }
    /* A user-initiated start is the retry gesture that clears a quota pause
       (e.g. the day rolled over). */
    jw__w.paused_quota = false;

    jw__scrape_row *row = &jw__w.rows[jw__w.row_count++];
    memset(row, 0, sizeof(*row));
    row->id = jw__w.next_row_id++;
    if (jw__w.next_row_id == 0) {
        jw__w.next_row_id = 1;
    }
    row->state = JW_SCRAPE_ROW_QUEUED;
    snprintf(row->system, sizeof(row->system), "%s", system);
    snprintf(row->rom_path, sizeof(row->rom_path), "%s", rom_path);
    jw_game_entry game;
    if (!jw__w.db_path[0] ||
        jw_db_get_game_by_rom_path(jw__w.db_path, rom_path, &game) != 0) {
        jw__display_name_from_rom_path(rom_path, row->display_name,
                                       sizeof(row->display_name));
    } else {
        snprintf(row->display_name, sizeof(row->display_name), "%s", game.name);
    }

    jw__scrape_item *slot =
        &jw__w.queue[(jw__w.head + jw__w.count) % JW__SCRAPE_QUEUE_MAX];
    slot->row_id = row->id;
    snprintf(slot->system, sizeof(slot->system), "%s", system);
    jw__image_dir_for_system(system, slot->image_dir, sizeof(slot->image_dir));
    slot->platform_id_count = jw__platform_ids_for_system(
        system, slot->platform_ids, JW_SCRAPE_MAX_PLATFORM_IDS);
    jw__scrape_policy_for_system(system, &slot->name_source,
                                 slot->lookup_extension,
                                 sizeof(slot->lookup_extension));
    snprintf(slot->effective_title, sizeof(slot->effective_title), "%s",
             row->display_name);
    snprintf(slot->rom_path, sizeof(slot->rom_path), "%s", rom_path);
    jw__w.count++;
    return 1;
}

/* Remove queued items matching system (NULL = all) and rom_path (NULL = any
   in system). Returns the number removed. */
static int jw__queue_remove(const char *system, const char *rom_path) {
    int removed = 0;
    int kept = 0;
    for (int i = 0; i < jw__w.count; i++) {
        jw__scrape_item *it =
            &jw__w.queue[(jw__w.head + i) % JW__SCRAPE_QUEUE_MAX];
        bool match = (!system || strcmp(it->system, system) == 0) &&
                     (!rom_path || strcmp(it->rom_path, rom_path) == 0);
        if (match) {
            jw__row_set_locked(it->row_id, JW_SCRAPE_ROW_CANCELLED,
                               "Cancelled before it started");
            removed++;
            continue;
        }
        jw__scrape_item *dst =
            &jw__w.queue[(jw__w.head + kept) % JW__SCRAPE_QUEUE_MAX];
        if (dst != it) *dst = *it;
        kept++;
    }
    jw__w.count = kept;
    return removed;
}

/* ── Worker pool ─────────────────────────────────────────────────────── */

static bool jw__error_is_quota(const char *msg) {
    return msg && (strstr(msg, "Daily quota exceeded") != NULL);
}

static bool jw__error_is_thread_limit(const char *msg) {
    return msg && (strstr(msg, "Thread limit reached") != NULL);
}

typedef enum {
    JW__SCRAPE_ITEM_DONE = 0,
    JW__SCRAPE_ITEM_PAUSED_QUOTA,
    JW__SCRAPE_ITEM_PAUSED_STORAGE,
} jw__scrape_item_result;

static const char *jw__storage_pause_message(const char *reason, bool db) {
    if (reason && strcmp(reason, "storage-repair-hold") == 0) {
        return db ? "Paused: the library card is waiting for repair"
                  : "Paused: the artwork card is waiting for repair";
    }
    if (reason && strcmp(reason, "storage-missing") == 0) {
        return db ? "Paused: the library card is not available"
                  : "Paused: the artwork card is not available";
    }
    return db ? "Paused: the library card is read-only"
              : "Paused: the artwork card is read-only";
}

/* Enter (or stay in) a whole-batch storage pause. Active downloads are
   interrupted so they return to the queue instead of failing one by one.
   Caller holds jw__w.mu. */
static void jw__enter_storage_pause_locked(const char *blocked_path,
                                           const char *message) {
    if (!jw__w.paused_storage) {
        jw_log_warn("scrape: pausing batch; %s (%s)", message,
                    blocked_path ? blocked_path : "?");
    }
    jw__w.paused_storage = true;
    snprintf(jw__w.storage_block_path, sizeof(jw__w.storage_block_path), "%s",
             blocked_path ? blocked_path : "");
    snprintf(jw__w.message, sizeof(jw__w.message), "%s", message);
    for (int i = 0; i < JW__SCRAPE_MAX_WORKERS; i++) {
        if (jw__w.active[i].used) {
            atomic_store(&jw__w.active[i].interrupt, 1);
        }
    }
}

/* Both the artwork destination and the library DB must be writable: a
   writable second card is no use when the DB on the launcher card is not. */
static bool jw__storage_ready_for_item(const char *dest_abs) {
    char reason[JW_STORAGE_REASON_MAX];
    if (!jw_storage_path_writable(jw__w.db_path, reason, sizeof(reason))) {
        pthread_mutex_lock(&jw__w.mu);
        jw__enter_storage_pause_locked(jw__w.db_path,
                                       jw__storage_pause_message(reason, true));
        pthread_mutex_unlock(&jw__w.mu);
        return false;
    }
    if (dest_abs && !jw_storage_path_writable(dest_abs, reason, sizeof(reason))) {
        pthread_mutex_lock(&jw__w.mu);
        jw__enter_storage_pause_locked(dest_abs,
                                       jw__storage_pause_message(reason, false));
        pthread_mutex_unlock(&jw__w.mu);
        return false;
    }
    return true;
}

static void jw__apply_thread_allowance(int max_threads) {
    if (max_threads <= 0)
        return;
    int permits = max_threads;
    if (permits > JW__SCRAPE_MAX_WORKERS) permits = JW__SCRAPE_MAX_WORKERS;
    if (permits < 1) permits = 1;

    pthread_mutex_lock(&jw__w.mu);
    if (permits != jw__w.permits) {
        jw__w.permits = permits;
        pthread_cond_broadcast(&jw__w.cv);
    }
    pthread_mutex_unlock(&jw__w.mu);
}

static int jw__queue_prepend_locked(const jw__scrape_item *item,
                                    const char *message) {
    if (jw__w.count >= JW__SCRAPE_QUEUE_MAX)
        return -1;
    jw__w.head = (jw__w.head + JW__SCRAPE_QUEUE_MAX - 1) % JW__SCRAPE_QUEUE_MAX;
    jw__w.queue[jw__w.head] = *item;
    jw__w.count++;
    jw__row_set_locked(item->row_id, JW_SCRAPE_ROW_QUEUED, message);
    return 0;
}

typedef struct {
    unsigned row_id;
} jw__scrape_progress_ctx;

static void jw__scrape_progress(void *userdata, jw_ss_phase phase) {
    jw__scrape_progress_ctx *ctx = (jw__scrape_progress_ctx *)userdata;
    if (!ctx) {
        return;
    }
    switch (phase) {
        case JW_SS_PHASE_HASHING:
            jw__row_set(ctx->row_id, JW_SCRAPE_ROW_HASH, "Hashing ROM");
            break;
        case JW_SS_PHASE_SEARCHING:
            jw__row_set(ctx->row_id, JW_SCRAPE_ROW_SEARCH,
                        "Searching ScreenScraper");
            break;
        case JW_SS_PHASE_DOWNLOADING:
            jw__row_set(ctx->row_id, JW_SCRAPE_ROW_DOWNLOAD,
                        "Downloading artwork");
            break;
        case JW_SS_PHASE_SAVING:
            jw__row_set(ctx->row_id, JW_SCRAPE_ROW_SAVE, "Saving artwork");
            break;
    }
}

/* Record promoted artwork in the DB. Returns DONE, or PAUSED_STORAGE when
   the DB turned out to be read-only (the artwork stays and is reconciled on
   the retry). meta may be NULL on a reconcile pass. */
static jw__scrape_item_result jw__record_saved_art(jw__scrape_item *item,
                                                   const char *image_db,
                                                   const jw_ss_result *result) {
    int db_rc = jw_db_set_game_image(jw__w.db_path, item->rom_path, image_db);
    /* SQLITE_BUSY: the DB is briefly locked by another writer; retry a few
       times before giving up. */
    for (int attempt = 0; db_rc == JW_DB_RC_BUSY && attempt < 3; attempt++) {
        usleep(75 * 1000);
        db_rc = jw_db_set_game_image(jw__w.db_path, item->rom_path, image_db);
    }
    if (db_rc == JW_DB_RC_READONLY) {
        jw_storage_report_write_error(jw__w.db_path, EROFS);
        pthread_mutex_lock(&jw__w.mu);
        if (!jw__w.db_readonly_logged) {
            jw_log_warn("scrape: image saved but the library database is read-only; "
                        "keeping %s queued", item->rom_path);
            jw__w.db_readonly_logged = true;
        }
        item->art_saved = true;
        jw__enter_storage_pause_locked(jw__w.db_path,
                                       jw__storage_pause_message("storage-read-only", true));
        pthread_mutex_unlock(&jw__w.mu);
        return JW__SCRAPE_ITEM_PAUSED_STORAGE;
    }
    if (db_rc == 0) {
        pthread_mutex_lock(&jw__w.mu);
        jw__w.db_readonly_logged = false;
        pthread_mutex_unlock(&jw__w.mu);
    } else if (db_rc == JW_DB_RC_NO_ROW) {
        jw_log_info("scrape: image saved but no game row for %s", item->rom_path);
    } else if (db_rc == JW_DB_RC_IOERR) {
        jw_storage_report_write_error(jw__w.db_path, EIO);
        jw_log_warn("scrape: image saved but the library database reported an I/O error "
                    "for %s", item->rom_path);
    } else {
        jw_log_warn("scrape: image saved but the library update failed for %s (rc=%d)",
                    item->rom_path, db_rc);
    }
    if (result) {
        /* The facts came back in the same response as the artwork, so store them
           while we have them. Best effort: a game with art and no metadata is a
           normal state, and the view renders only what exists. */
        jw_game_meta meta;
        memset(&meta, 0, sizeof(meta));
        snprintf(meta.genre,     sizeof(meta.genre),     "%s", result->genre);
        snprintf(meta.developer, sizeof(meta.developer), "%s", result->developer);
        snprintf(meta.publisher, sizeof(meta.publisher), "%s", result->publisher);
        snprintf(meta.players,   sizeof(meta.players),   "%s", result->players);
        snprintf(meta.rating,    sizeof(meta.rating),    "%s", result->rating);
        snprintf(meta.year,      sizeof(meta.year),      "%s", result->year);
        snprintf(meta.synopsis,  sizeof(meta.synopsis),  "%s", result->synopsis);
        jw_db_set_game_meta(jw__w.db_path, item->rom_path, &meta);
    }
    jw_db_increment_setting(jw__w.db_path, "library.generation");
    return JW__SCRAPE_ITEM_DONE;
}

static jw__scrape_item_result jw__process_item(jw__scrape_item *item,
                                               atomic_int *interrupt) {
    char rom_abs[PATH_MAX], rom_name[256];
    char dest_abs[PATH_MAX], image_db[512];
    jw__row_set(item->row_id, JW_SCRAPE_ROW_HASH, "Resolving ROM path");
    if (jw__resolve_paths(item, rom_abs, sizeof(rom_abs),
                          rom_name, sizeof(rom_name),
                          dest_abs, sizeof(dest_abs),
                          image_db, sizeof(image_db)) != 0) {
        pthread_mutex_lock(&jw__w.mu);
        snprintf(jw__w.message, sizeof(jw__w.message),
                 "Could not resolve paths for %.200s", item->rom_path);
        jw__row_set_locked(item->row_id, JW_SCRAPE_ROW_ERROR,
                           jw__w.message);
        pthread_mutex_unlock(&jw__w.mu);
        return JW__SCRAPE_ITEM_DONE;
    }
    jw__row_set_output(item->row_id, dest_abs);

    if (!jw__storage_ready_for_item(dest_abs)) {
        return JW__SCRAPE_ITEM_PAUSED_STORAGE;
    }
    if (item->art_saved && access(dest_abs, F_OK) == 0) {
        jw__scrape_item_result reconciled = jw__record_saved_art(item, image_db, NULL);
        if (reconciled != JW__SCRAPE_ITEM_DONE) {
            return reconciled;
        }
        pthread_mutex_lock(&jw__w.mu);
        jw__row_set_locked(item->row_id, JW_SCRAPE_ROW_DONE, "Artwork saved");
        pthread_mutex_unlock(&jw__w.mu);
        return JW__SCRAPE_ITEM_DONE;
    }
    item->art_saved = false;

    const int *system_ids = item->platform_ids;
    size_t system_count = item->platform_id_count;
    if (system_count == 0) {
        /* enqueue validates this; only reachable through races */
        pthread_mutex_lock(&jw__w.mu);
        jw__row_set_locked(item->row_id, JW_SCRAPE_ROW_ERROR,
                           "No ScreenScraper mapping for this system");
        pthread_mutex_unlock(&jw__w.mu);
        return JW__SCRAPE_ITEM_DONE;
    }
    if (system_count > JW_SCRAPE_MAX_PLATFORM_IDS) {
        pthread_mutex_lock(&jw__w.mu);
        jw__row_set_locked(item->row_id, JW_SCRAPE_ROW_ERROR,
                           "Too many ScreenScraper mappings for this system");
        pthread_mutex_unlock(&jw__w.mu);
        return JW__SCRAPE_ITEM_DONE;
    }

    jw__scrape_prefs prefs;
    jw__prefs_load(&prefs);
    jw__apply_thread_allowance(prefs.max_threads);

    jw_ss_client client = {0};
    snprintf(client.username, sizeof(client.username), "%s", prefs.user);
    snprintf(client.password, sizeof(client.password), "%s", prefs.pass);
    client.interrupt = interrupt;
    jw__scrape_progress_ctx progress_ctx = { item->row_id };
    client.progress = jw__scrape_progress;
    client.progress_userdata = &progress_ctx;

    jw_ss_result result;
    jw_ss_search_status search_status;
    if (item->name_source == JW_RA_SCRAPE_NAME_DESCRIPTOR) {
        jw_scrape_identity_candidates candidates;
        jw_scrape_identity_build(rom_abs, rom_name, item->effective_title,
                                 item->lookup_extension, &candidates);
        search_status = JW_SS_SEARCH_NOT_FOUND;
        for (size_t i = 0; i < candidates.count; i++) {
            search_status = jw_ss_search_rom_platforms(
                &client, candidates.names[i], NULL, system_ids, system_count,
                (const char *const *)prefs.artwork, prefs.artwork_count,
                (const char *const *)prefs.regions, prefs.region_count, &result);
            if (search_status != JW_SS_SEARCH_NOT_FOUND) {
                break;
            }
        }
    } else {
        search_status = jw_ss_search_rom_platforms(
            &client, rom_name, rom_abs, system_ids, system_count,
            (const char *const *)prefs.artwork, prefs.artwork_count,
            (const char *const *)prefs.regions, prefs.region_count, &result);
    }
    int rc = search_status;
    if (search_status == JW_SS_SEARCH_FOUND) {
        rc = jw_ss_download_media(&client, result.media_url, dest_abs,
                                  JW_SCRAPE_MAX_DIM);
    }
    jw__prefs_free(&prefs);

    int write_errno = rc != 0 ? jw_ss_last_errno() : 0;
    if (rc == 0) {
        jw__scrape_item_result recorded = jw__record_saved_art(item, image_db, &result);
        if (recorded != JW__SCRAPE_ITEM_DONE) {
            return recorded;
        }

        pthread_mutex_lock(&jw__w.mu);
        jw__row_set_locked(item->row_id, JW_SCRAPE_ROW_DONE, "Artwork saved");
        jw__scrape_row *row = jw__row_find_locked(item->row_id);
        jw__eta_record_done_locked(row);
        /* Follow the account's thread allowance, capped by the pool. */
        if (result.max_threads > 0) {
            int permits = result.max_threads;
            if (permits > JW__SCRAPE_MAX_WORKERS) permits = JW__SCRAPE_MAX_WORKERS;
            if (permits != jw__w.permits) {
                jw__w.permits = permits;
                pthread_cond_broadcast(&jw__w.cv);
            }
            jw__w.max_threads = result.max_threads;
        }
        if (result.max_requests > 0) {
            jw__w.requests_today = result.requests_today;
            jw__w.max_requests = result.max_requests;
        }
        /* Out of daily requests: pause before burning failed lookups. */
        if (result.max_requests > 0 &&
            result.requests_today >= result.max_requests) {
            jw__w.paused_quota = true;
            snprintf(jw__w.message, sizeof(jw__w.message),
                     "Daily quota exhausted (%d/%d)",
                     result.requests_today, result.max_requests);
        }
        pthread_mutex_unlock(&jw__w.mu);
        jw_log_info("scrape: %s/%s -> %s", item->system, rom_name, dest_abs);
        return JW__SCRAPE_ITEM_DONE;
    }

    if (write_errno != 0) {
        jw_storage_report_write_error(dest_abs, write_errno);
    }
    /* A storage failure (or an interrupt because another worker saw one)
       returns the item to the queue without a terminal error row. */
    pthread_mutex_lock(&jw__w.mu);
    /* On a FAT card that has just flipped, opening an existing file answers
       EIO while creating one answers EROFS; EIO counts once the gate agrees. */
    bool storage_failure = jw_storage_errno_is_storage_failure(write_errno) ||
                           (write_errno == EIO &&
                            !jw_storage_path_writable(dest_abs, NULL, 0));
    if ((storage_failure || (rc == JW_SS_SEARCH_CANCELLED && jw__w.paused_storage)) &&
        !(storage_failure && item->storage_requeued && !jw__w.paused_storage)) {
        if (storage_failure) {
            jw__enter_storage_pause_locked(dest_abs,
                                           jw__storage_pause_message("storage-read-only", false));
        }
        pthread_mutex_unlock(&jw__w.mu);
        return JW__SCRAPE_ITEM_PAUSED_STORAGE;
    }
    if (search_status == JW_SS_SEARCH_NOT_FOUND) {
        jw__row_set_locked(item->row_id, JW_SCRAPE_ROW_NOT_FOUND,
                           "No matching artwork found");
    } else if (search_status == JW_SS_SEARCH_NO_MEDIA) {
        jw__row_set_locked(item->row_id, JW_SCRAPE_ROW_NOT_FOUND,
                           "Game found, but no preferred artwork is available");
    } else if (rc == JW_SS_SEARCH_CANCELLED) {
        jw__row_set_locked(item->row_id, JW_SCRAPE_ROW_CANCELLED,
                           "Cancelled");
    } else {
        const char *msg = jw_ss_last_error();
        char errno_msg[256];
        if (write_errno != 0 && msg) {
            /* Keep the real cause visible: a full card and a permission
               problem are not a read-only card. */
            snprintf(errno_msg, sizeof(errno_msg), "%.180s: %.60s", msg,
                     strerror(write_errno));
            msg = errno_msg;
        }
        if (msg) {
            snprintf(jw__w.message, sizeof(jw__w.message), "%s", msg);
            if (jw__error_is_quota(msg)) {
                jw__w.paused_quota = true;
                pthread_mutex_unlock(&jw__w.mu);
                return JW__SCRAPE_ITEM_PAUSED_QUOTA;
            } else if (jw__error_is_thread_limit(msg) && jw__w.permits > 1) {
                /* The account allows fewer concurrent requests than we run;
                   back off a permit. */
                jw__w.permits--;
            }
        }
        jw__row_set_locked(item->row_id, JW_SCRAPE_ROW_ERROR,
                           msg ? msg : "ScreenScraper request failed");
    }
    pthread_mutex_unlock(&jw__w.mu);
    return JW__SCRAPE_ITEM_DONE;
}

static void *jw__worker_main(void *arg) {
    (void)arg;
    pthread_mutex_lock(&jw__w.mu);
    for (;;) {
        while (!jw__w.shutdown &&
               (jw__w.count == 0 || jw__w.paused_quota || jw__w.paused_storage ||
                jw__w.active_count >= jw__w.permits)) {
            pthread_cond_wait(&jw__w.cv, &jw__w.mu);
        }
        if (jw__w.shutdown) break;

        jw__active_slot *slot = NULL;
        for (int i = 0; i < JW__SCRAPE_MAX_WORKERS; i++) {
            if (!jw__w.active[i].used) {
                slot = &jw__w.active[i];
                break;
            }
        }
        if (!slot) {
            /* active_count < permits <= MAX_WORKERS guarantees a free slot;
               defensive only. */
            pthread_cond_wait(&jw__w.cv, &jw__w.mu);
            continue;
        }

        slot->item = jw__w.queue[jw__w.head];
        slot->used = true;
        atomic_store(&slot->interrupt, 0);
        jw__w.head = (jw__w.head + 1) % JW__SCRAPE_QUEUE_MAX;
        jw__w.count--;
        jw__w.active_count++;
        jw__row_set_locked(slot->item.row_id, JW_SCRAPE_ROW_HASH,
                           "Starting scrape");
        pthread_mutex_unlock(&jw__w.mu);

        jw__scrape_item_result result =
            jw__process_item(&slot->item, &slot->interrupt);

        pthread_mutex_lock(&jw__w.mu);
        if (result == JW__SCRAPE_ITEM_PAUSED_QUOTA &&
            jw__queue_prepend_locked(&slot->item,
                                     "Paused by ScreenScraper quota") != 0) {
            jw__row_set_locked(slot->item.row_id, JW_SCRAPE_ROW_ERROR,
                               "Could not keep quota-paused item queued");
            result = JW__SCRAPE_ITEM_DONE;
        }
        if (result == JW__SCRAPE_ITEM_PAUSED_STORAGE) {
            slot->item.storage_requeued = true;
            if (jw__queue_prepend_locked(&slot->item,
                                         "Paused: SD card is not writable") != 0) {
                jw__row_set_locked(slot->item.row_id, JW_SCRAPE_ROW_ERROR,
                                   "Could not keep storage-paused item queued");
            }
        }
        slot->used = false;
        jw__w.active_count--;
        /* Wake siblings: a permit freed up, or the drain just completed. */
        pthread_cond_broadcast(&jw__w.cv);
    }
    pthread_mutex_unlock(&jw__w.mu);
    return NULL;
}

/* ── Public API ──────────────────────────────────────────────────────── */

int jw_scrape_worker_start(const char *db_path, const char *sdcard_root) {
    pthread_mutex_lock(&jw__w.mu);
    if (jw__w.running) {
        pthread_mutex_unlock(&jw__w.mu);
        return 0;
    }
    if (!db_path || !db_path[0] || !sdcard_root || !sdcard_root[0]) {
        pthread_mutex_unlock(&jw__w.mu);
        return -1;
    }
    snprintf(jw__w.db_path, sizeof(jw__w.db_path), "%s", db_path);
    snprintf(jw__w.sdcard_root, sizeof(jw__w.sdcard_root), "%s", sdcard_root);
    if (!jw__w.queue) {
        jw__w.queue = calloc(JW__SCRAPE_QUEUE_MAX, sizeof(*jw__w.queue));
        if (!jw__w.queue) {
            pthread_mutex_unlock(&jw__w.mu);
            return -1;
        }
    }
    if (!jw__w.rows) {
        jw__w.rows = calloc(JW__SCRAPE_QUEUE_MAX, sizeof(*jw__w.rows));
        if (!jw__w.rows) {
            pthread_mutex_unlock(&jw__w.mu);
            return -1;
        }
    }
    jw__w.head = 0;
    jw__w.count = 0;
    jw__w.active_count = 0;
    jw__w.paused_quota = false;
    jw__w.paused_storage = false;
    jw__w.storage_block_path[0] = '\0';
    jw__clear_finished_batch_locked();
    jw__w.shutdown = false;
    jw__w.permits = 1;
    jw__w.thread_count = 0;
    for (int i = 0; i < JW__SCRAPE_MAX_WORKERS; i++) {
        if (pthread_create(&jw__w.threads[i], NULL, jw__worker_main, NULL) != 0) {
            break;
        }
        jw__w.thread_count++;
    }
    jw__w.running = jw__w.thread_count > 0;
    pthread_mutex_unlock(&jw__w.mu);
    return jw__w.running ? 0 : -1;
}

void jw_scrape_worker_stop(void) {
    pthread_mutex_lock(&jw__w.mu);
    if (!jw__w.running) {
        pthread_mutex_unlock(&jw__w.mu);
        return;
    }
    jw__w.shutdown = true;
    jw__w.count = 0;
    for (int i = 0; i < JW__SCRAPE_MAX_WORKERS; i++) {
        if (jw__w.active[i].used) {
            atomic_store(&jw__w.active[i].interrupt, 1);
        }
    }
    pthread_cond_broadcast(&jw__w.cv);
    int thread_count = jw__w.thread_count;
    pthread_mutex_unlock(&jw__w.mu);

    for (int i = 0; i < thread_count; i++) {
        pthread_join(jw__w.threads[i], NULL);
    }

    pthread_mutex_lock(&jw__w.mu);
    jw__w.running = false;
    jw__w.thread_count = 0;
    pthread_mutex_unlock(&jw__w.mu);
}

int jw_scrape_enqueue_game(const char *system, const char *rom_path,
                           const char **error) {
    if (error) *error = NULL;
    if (!system || !system[0] || !rom_path || !rom_path[0]) {
        if (error) *error = "missing system or rom_path";
        return -1;
    }
    if (!jw_ss_available()) {
        if (error) *error = "scraping unavailable in this build";
        return -1;
    }
    if (jw__platform_ids_for_system(system, NULL, 0) == 0) {
        if (error) *error = "no ScreenScraper mapping for this system";
        return -1;
    }

    pthread_mutex_lock(&jw__w.mu);
    if (!jw__w.running) {
        pthread_mutex_unlock(&jw__w.mu);
        if (error) *error = "scrape worker not running";
        return -1;
    }
    int rc = jw__queue_push(system, rom_path);
    if (rc > 0) pthread_cond_broadcast(&jw__w.cv);
    pthread_mutex_unlock(&jw__w.mu);
    if (rc < 0) {
        if (error) *error = "scrape queue is full";
        return -1;
    }
    if (rc > 0) jw__log_scrape_policy(system);
    return rc;
}

/* The Roms/ folder a ROM physically sits in ("FC" for Roms/FC/Mario.nes, also
   for nested Roms/FC/Hacks/Mario.nes), or "" when it is outside roms_path. */
static void jw__rom_physical_folder(const jw_storage_source *source,
                                    const char *rom_abs, const char *rom_path,
                                    char *out, size_t out_size) {
    out[0] = '\0';
    const char *rest = NULL;
    size_t roms_len = strlen(source->roms_path);
    if (roms_len > 0 && strncmp(rom_abs, source->roms_path, roms_len) == 0 &&
        rom_abs[roms_len] == '/') {
        rest = rom_abs + roms_len + 1;
    } else if (strncmp(rom_path, "Roms/", 5) == 0) {
        rest = rom_path + 5;
    }
    const char *slash = rest ? strchr(rest, '/') : NULL;
    if (!slash || slash == rest || (size_t)(slash - rest) >= out_size) {
        return;
    }
    memcpy(out, rest, (size_t)(slash - rest));
    out[slash - rest] = '\0';
}

/* True if the game already has art on disk, wherever the library scan would
   find it (canonical image root, Images/<folder>, Roms/<folder>/Imgs, in any
   accepted format) or where a scrape would write it. Checked on disk so art
   that landed without a rescan still counts. Shared by the enqueue and the
   missing-count paths; runs on the calling thread, like the image-dir lookup. */
/* An SD repair can truncate damaged artwork to 0 bytes. The file is still
   there, but it is missing art as far as "scrape missing" is concerned. */
static bool jw__art_file_has_content(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && st.st_size > 0;
}

static bool jw__game_art_exists(const jw_storage_source_list *sources,
                                jw_art_index *art_index,
                                const char *system, const char *rom_path) {
    char rom_abs[PATH_MAX];
    if (jw_storage_resolve_path(sources, rom_path, rom_abs, sizeof(rom_abs)) != 0)
        return false;
    const jw_storage_source *source =
        jw_storage_sources_find_for_path(sources, rom_abs);
    if (!source) source = jw_storage_sources_primary(sources);
    if (!source) return false;

    char rom_copy[512];
    snprintf(rom_copy, sizeof(rom_copy), "%s", rom_path);
    const char *rom_name = basename(rom_copy);

    char error[256];
    const jw_ra_catalog *catalog =
        jw_ra_catalog_get(jw__w.sdcard_root, error, sizeof(error));
    const jw_ra_system *sys =
        catalog ? jw_ra_catalog_find_system(catalog, system) : NULL;

    char folder[256], stem[256], art_abs[PATH_MAX], art_rel[PATH_MAX];
    jw__rom_physical_folder(source, rom_abs, rom_path, folder, sizeof(folder));
    jw_art_stem_for_rom(sys, rom_name, stem, sizeof(stem));
    if (jw_art_find_for_rom(art_index, source, sys ? sys->image_root : NULL, folder, stem,
                            art_abs, sizeof(art_abs),
                            art_rel, sizeof(art_rel)) == 0 &&
        jw__art_file_has_content(art_abs)) {
        return true;
    }

    /* The scrape destination keeps only the last extension off the stem. */
    char base[256], image_dir[64], dir[PATH_MAX], name[PATH_MAX], found[PATH_MAX];
    jw__art_base(rom_name, base, sizeof(base));
    jw__image_dir_for_system(system, image_dir, sizeof(image_dir));
    if (snprintf(dir, sizeof(dir), "%s/%s",
                 source->images_path, image_dir) >= (int)sizeof(dir) ||
        jw_art_find(art_index, dir, base, name, sizeof(name)) != 0) {
        return false;
    }
    if (name[0] == '/') {
        return jw__art_file_has_content(name);
    }
    return snprintf(found, sizeof(found), "%s/%s", dir, name) < (int)sizeof(found) &&
           jw__art_file_has_content(found);
}

/* Count a system's games that still need art (and its total). Returns 0 on
   success; unmapped systems report zero rather than erroring. */
int jw_scrape_count_missing_system(const char *system, int *out_missing,
                                   int *out_total, const char **error) {
    if (error) *error = NULL;
    if (out_missing) *out_missing = 0;
    if (out_total) *out_total = 0;
    if (!system || !system[0]) {
        if (error) *error = "missing system";
        return -1;
    }
    if (jw__platform_ids_for_system(system, NULL, 0) == 0)
        return 0;   /* no ScreenScraper mapping -> nothing scrapeable */

    jw_game_entry *games = calloc(JW__SCRAPE_LIST_MAX, sizeof(*games));
    if (!games) {
        if (error) *error = "out of memory";
        return -1;
    }
    int game_count = 0;
    if (jw_db_list_games_for_system(jw__w.db_path, system, games,
                                    JW__SCRAPE_LIST_MAX, &game_count) != 0) {
        free(games);
        if (error) *error = "could not list games for this system";
        return -1;
    }

    jw_storage_source_list sources;
    bool have_sources =
        jw_storage_sources_resolve(jw__w.sdcard_root, &sources) == 0 &&
        sources.count > 0;

    int missing = 0;
    jw_art_index *art_index = have_sources ? jw_art_index_new() : NULL;
    for (int i = 0; i < game_count; i++) {
        if (have_sources &&
            jw__game_art_exists(&sources, art_index, system, games[i].rom_path))
            continue;
        missing++;   /* with no resolvable sources we can't verify -> treat as missing */
    }
    jw_art_index_free(art_index);
    if (out_total)   *out_total   = game_count;
    if (out_missing) *out_missing = missing;
    free(games);
    return 0;
}

/* Per-system missing/total counts for every mapped system that has games. */
int jw_scrape_missing_counts(jw_scrape_missing_row *out, int max,
                             int *out_count, int *out_total_missing) {
    if (out_count) *out_count = 0;
    if (out_total_missing) *out_total_missing = 0;
    if (!out || max <= 0) return -1;

    jw_system_entry *systems = calloc(JW__SCRAPE_SYS_MAX, sizeof(*systems));
    if (!systems) return -1;
    int sys_count = 0;
    if (jw_db_list_systems(jw__w.db_path, systems, JW__SCRAPE_SYS_MAX,
                           &sys_count) != 0) {
        free(systems);
        return -1;
    }

    int n = 0, total_missing = 0;
    for (int i = 0; i < sys_count && n < max; i++) {
        if (jw__platform_ids_for_system(systems[i].name, NULL, 0) == 0) continue;
        int missing = 0, total = 0;
        if (jw_scrape_count_missing_system(systems[i].name, &missing, &total,
                                           NULL) != 0)
            continue;
        if (total <= 0) continue;   /* no games -> nothing to scrape */
        snprintf(out[n].system, sizeof(out[n].system), "%s", systems[i].name);
        out[n].missing = missing;
        out[n].total   = total;
        total_missing += missing;
        n++;
    }
    if (out_count) *out_count = n;
    if (out_total_missing) *out_total_missing = total_missing;
    free(systems);
    return 0;
}

static void jw__enqueue_result_add(jw_scrape_enqueue_result *dst,
                                   const jw_scrape_enqueue_result *src) {
    if (!dst || !src) return;
    dst->requested += src->requested;
    dst->enqueued += src->enqueued;
    dst->already_queued += src->already_queued;
    dst->skipped_existing += src->skipped_existing;
    dst->queue_full = dst->queue_full || src->queue_full;
}

/* Enqueue every mapped system in one shot (used by scope "all"). Per-system
   errors are tolerated so one bad system can't abort the batch. Returns the
   total enqueued, or -1 only on a fatal setup error. */
int jw_scrape_enqueue_all_full(bool missing_only,
                               jw_scrape_enqueue_result *out,
                               const char **error) {
    jw_scrape_enqueue_result fallback;
    if (!out) out = &fallback;
    memset(out, 0, sizeof(*out));
    if (error) *error = NULL;
    if (!jw_ss_available()) {
        if (error) *error = "scraping unavailable in this build";
        return -1;
    }
    jw_system_entry *systems = calloc(JW__SCRAPE_SYS_MAX, sizeof(*systems));
    if (!systems) {
        if (error) *error = "out of memory";
        return -1;
    }
    int sys_count = 0;
    if (jw_db_list_systems(jw__w.db_path, systems, JW__SCRAPE_SYS_MAX,
                           &sys_count) != 0) {
        free(systems);
        if (error) *error = "could not list systems";
        return -1;
    }
    for (int i = 0; i < sys_count; i++) {
        if (jw__platform_ids_for_system(systems[i].name, NULL, 0) == 0) continue;
        const char *err = NULL;
        jw_scrape_enqueue_result one;
        (void)jw_scrape_enqueue_system_full(systems[i].name, missing_only,
                                            &one, &err);
        jw__enqueue_result_add(out, &one);
    }
    free(systems);
    return out->enqueued;
}

int jw_scrape_enqueue_all(bool missing_only, const char **error) {
    jw_scrape_enqueue_result result;
    int rc = jw_scrape_enqueue_all_full(missing_only, &result, error);
    return rc < 0 ? rc : result.enqueued;
}

int jw_scrape_enqueue_system_full(const char *system, bool missing_only,
                                  jw_scrape_enqueue_result *out,
                                  const char **error) {
    jw_scrape_enqueue_result fallback;
    if (!out) out = &fallback;
    memset(out, 0, sizeof(*out));
    if (error) *error = NULL;
    if (!system || !system[0]) {
        if (error) *error = "missing system";
        return -1;
    }
    if (!jw_ss_available()) {
        if (error) *error = "scraping unavailable in this build";
        return -1;
    }
    if (jw__platform_ids_for_system(system, NULL, 0) == 0) {
        if (error) *error = "no ScreenScraper mapping for this system";
        return -1;
    }

    jw_game_entry *games = calloc(JW__SCRAPE_LIST_MAX, sizeof(*games));
    if (!games) {
        if (error) *error = "out of memory";
        return -1;
    }
    int game_count = 0;
    if (jw_db_list_games_for_system(jw__w.db_path, system, games,
                                    JW__SCRAPE_LIST_MAX, &game_count) != 0) {
        free(games);
        if (error) *error = "could not list games for this system";
        return -1;
    }

    /* Check the library DB and this card's artwork root before enqueueing.
       Work is still queued, but held as a storage pause from the start rather
       than failing item by item. The per-item check covers the second card. */
    char images_root[PATH_MAX];
    char storage_reason[JW_STORAGE_REASON_MAX] = "";
    const char *storage_blocked = NULL;
    bool storage_db = false;
    if (snprintf(images_root, sizeof(images_root), "%s/Images", jw__w.sdcard_root) >=
        (int)sizeof(images_root)) {
        images_root[0] = '\0';
    }
    if (!jw_storage_path_writable(jw__w.db_path, storage_reason, sizeof(storage_reason))) {
        storage_blocked = jw__w.db_path;
        storage_db = true;
    } else if (!jw_storage_path_writable(images_root, storage_reason,
                                         sizeof(storage_reason))) {
        storage_blocked = images_root;
    }

    jw_storage_source_list sources;
    bool have_sources =
        missing_only &&
        jw_storage_sources_resolve(jw__w.sdcard_root, &sources) == 0 &&
        sources.count > 0;

    /* One pass over the system: read each art folder once, before taking
       the queue lock. */
    jw_art_index *art_index = have_sources ? jw_art_index_new() : NULL;
    bool queue_full = false;
    pthread_mutex_lock(&jw__w.mu);
    if (!jw__w.running) {
        pthread_mutex_unlock(&jw__w.mu);
        jw_art_index_free(art_index);
        free(games);
        if (error) *error = "scrape worker not running";
        return -1;
    }
    for (int i = 0; i < game_count; i++) {
        if (missing_only && have_sources &&
            jw__game_art_exists(&sources, art_index, system, games[i].rom_path)) {
            out->skipped_existing++;
            continue;   /* art already on disk */
        }
        out->requested++;
        if (queue_full) {
            continue;   /* keep counting remaining selected games */
        }
        int rc = jw__queue_push(system, games[i].rom_path);
        if (rc < 0) {
            queue_full = true;   /* queue full: enqueue what fit */
            out->queue_full = true;
            continue;
        }
        if (rc > 0) {
            out->enqueued++;
        } else {
            out->already_queued++;
        }
    }
    if (storage_blocked && out->enqueued > 0) {
        jw__enter_storage_pause_locked(storage_blocked,
                                       jw__storage_pause_message(storage_reason, storage_db));
    }
    if (out->enqueued > 0) pthread_cond_broadcast(&jw__w.cv);
    pthread_mutex_unlock(&jw__w.mu);
    jw_art_index_free(art_index);
    if (out->enqueued > 0) jw__log_scrape_policy(system);
    free(games);
    return out->enqueued;
}

int jw_scrape_enqueue_system(const char *system, bool missing_only,
                             const char **error) {
    jw_scrape_enqueue_result result;
    int rc = jw_scrape_enqueue_system_full(system, missing_only, &result, error);
    return rc < 0 ? rc : result.enqueued;
}

void jw_scrape_storage_recheck(void) {
    char blocked[PATH_MAX];
    char db_path[PATH_MAX];
    pthread_mutex_lock(&jw__w.mu);
    if (!jw__w.paused_storage) {
        pthread_mutex_unlock(&jw__w.mu);
        return;
    }
    snprintf(blocked, sizeof(blocked), "%s", jw__w.storage_block_path);
    snprintf(db_path, sizeof(db_path), "%s", jw__w.db_path);
    pthread_mutex_unlock(&jw__w.mu);

    /* Checked without the lock: each check reads the mount table. */
    if (!jw_storage_path_writable(db_path, NULL, 0) ||
        (blocked[0] && !jw_storage_path_writable(blocked, NULL, 0))) {
        return;
    }
    pthread_mutex_lock(&jw__w.mu);
    if (jw__w.paused_storage && strcmp(blocked, jw__w.storage_block_path) == 0) {
        jw__w.paused_storage = false;
        jw__w.storage_block_path[0] = '\0';
        jw__w.message[0] = '\0';
        jw_log_info("scrape: storage writable again; resuming batch");
        pthread_cond_broadcast(&jw__w.cv);
    }
    pthread_mutex_unlock(&jw__w.mu);
}

void jw_scrape_status(jw_scrape_status_info *out) {
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&jw__w.mu);
    if (jw__w.paused_storage) {
        out->state = JW_SCRAPE_PAUSED_STORAGE;
    } else if (jw__w.paused_quota) {
        out->state = JW_SCRAPE_PAUSED_QUOTA;
    } else if (jw__w.count > 0 || jw__w.active_count > 0) {
        out->state = JW_SCRAPE_RUNNING;
    } else {
        out->state = JW_SCRAPE_IDLE;
    }
    jw__count_rows_locked(&out->total, &out->done, &out->found,
                          &out->not_found, &out->failed,
                          &out->cancelled, &out->queued, &out->active);
    for (int i = 0; i < JW__SCRAPE_MAX_WORKERS; i++) {
        if (jw__w.active[i].used) {
            char rom_copy[512];
            snprintf(rom_copy, sizeof(rom_copy), "%s",
                     jw__w.active[i].item.rom_path);
            snprintf(out->current_name, sizeof(out->current_name), "%s",
                     basename(rom_copy));
            snprintf(out->current_system, sizeof(out->current_system), "%s",
                     jw__w.active[i].item.system);
            break;
        }
    }
    snprintf(out->message, sizeof(out->message), "%s", jw__w.message);
    pthread_mutex_unlock(&jw__w.mu);
}

void jw_scrape_queue_snapshot(jw_scrape_queue_info *out, int offset, int limit) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->eta_seconds = -1;
    if (offset < 0) {
        offset = 0;
    }
    if (limit <= 0 || limit > JW_SCRAPE_QUEUE_SNAPSHOT_MAX) {
        limit = JW_SCRAPE_QUEUE_SNAPSHOT_MAX;
    }

    pthread_mutex_lock(&jw__w.mu);
    if (jw__w.paused_storage) {
        out->state = JW_SCRAPE_PAUSED_STORAGE;
    } else if (jw__w.paused_quota) {
        out->state = JW_SCRAPE_PAUSED_QUOTA;
    } else if (jw__w.count > 0 || jw__w.active_count > 0) {
        out->state = JW_SCRAPE_RUNNING;
    } else {
        out->state = JW_SCRAPE_IDLE;
    }
    jw__count_rows_locked(&out->total, &out->done, &out->found,
                          &out->not_found, &out->failed,
                          &out->cancelled, &out->queued, &out->active);
    out->requests_today = jw__w.requests_today;
    out->max_requests = jw__w.max_requests;
    out->max_threads = jw__w.max_threads;
    out->permits = jw__w.permits;
    out->eta_seconds = jw__eta_seconds_locked();
    snprintf(out->message, sizeof(out->message), "%s", jw__w.message);

    int available = jw__w.row_count - offset;
    if (available < 0) {
        available = 0;
    }
    if (available > limit) {
        available = limit;
    }
    out->row_count = available;
    for (int i = 0; i < available; i++) {
        const jw__scrape_row *src = &jw__w.rows[offset + i];
        jw_scrape_queue_row *dst = &out->rows[i];
        dst->id = src->id;
        dst->state = src->state;
        snprintf(dst->display_name, sizeof(dst->display_name), "%s",
                 src->display_name);
        snprintf(dst->system, sizeof(dst->system), "%s", src->system);
        snprintf(dst->rom_path, sizeof(dst->rom_path), "%s", src->rom_path);
        snprintf(dst->output_path, sizeof(dst->output_path), "%s",
                 src->output_path);
        snprintf(dst->message, sizeof(dst->message), "%s", src->message);
    }
    pthread_mutex_unlock(&jw__w.mu);
}

int jw_scrape_clear_done(void) {
    pthread_mutex_lock(&jw__w.mu);
    int removed = 0;
    int kept = 0;
    for (int i = 0; i < jw__w.row_count; i++) {
        if (jw__row_state_is_terminal(jw__w.rows[i].state)) {
            removed++;
            continue;
        }
        if (kept != i) {
            jw__w.rows[kept] = jw__w.rows[i];
        }
        kept++;
    }
    jw__w.row_count = kept;
    if (kept == 0 && jw__w.count == 0 && jw__w.active_count == 0) {
        jw__clear_finished_batch_locked();
    }
    pthread_mutex_unlock(&jw__w.mu);
    return removed;
}

static int jw__cancel(const char *system, const char *rom_path,
                      bool interrupt_active) {
    pthread_mutex_lock(&jw__w.mu);
    int removed = jw__queue_remove(system, rom_path);
    if (interrupt_active) {
        for (int i = 0; i < JW__SCRAPE_MAX_WORKERS; i++) {
            jw__active_slot *slot = &jw__w.active[i];
            if (!slot->used) {
                continue;
            }
            bool match = (!system || strcmp(slot->item.system, system) == 0) &&
                         (!rom_path || strcmp(slot->item.rom_path, rom_path) == 0);
            if (match) {
                atomic_store(&slot->interrupt, 1);
                removed++;
            }
        }
    }
    if (!system && !rom_path) {
        /* Cancel-all is also the "stop showing the pause banner" gesture. */
        jw__w.paused_quota = false;
        jw__w.paused_storage = false;
        jw__w.storage_block_path[0] = '\0';
    }
    pthread_cond_broadcast(&jw__w.cv);
    pthread_mutex_unlock(&jw__w.mu);
    return removed;
}

int jw_scrape_stop_all(void) {
    return jw__cancel(NULL, NULL, true);
}

int jw_scrape_cancel_all(void) {
    return jw_scrape_stop_all();
}

int jw_scrape_cancel_system(const char *system) {
    return jw__cancel(system, NULL, true);
}

int jw_scrape_cancel_game(const char *system, const char *rom_path) {
    return jw__cancel(system, rom_path, true);
}

bool jw_scrape_is_pending_game(const char *system, const char *rom_path) {
    pthread_mutex_lock(&jw__w.mu);
    bool pending = jw__pending_contains_locked(system, rom_path);
    pthread_mutex_unlock(&jw__w.mu);
    return pending;
}

bool jw_scrape_is_pending_system(const char *system) {
    pthread_mutex_lock(&jw__w.mu);
    bool pending = jw__pending_contains_locked(system, NULL);
    pthread_mutex_unlock(&jw__w.mu);
    return pending;
}
