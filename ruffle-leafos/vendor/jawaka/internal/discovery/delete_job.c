#include "internal/discovery/delete_job.h"
#include "internal/discovery/delete.h"
#include "internal/db/db.h"
#include "internal/db/relocation.h"
#include "internal/storage/health.h"
#include "internal/scrape/scrape_worker.h"
#include "cJSON.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* What the preview and the commit both work out: the game's name, its files,
   or why it can't be deleted. */
typedef struct {
    char name[512];
    char error[512];
    char readonly_source[32];
    jw_delete_plan plan;
} jw_delete_view;

struct jw_delete_job {
    pthread_mutex_t mutex;
    pthread_t thread;
    bool started, changed;
    jw_ipc_delete_phase phase;
    char db_path[JW_STORAGE_PATH_MAX], sdcard_root[JW_STORAGE_PATH_MAX];
    char source_id[32], rom_relpath[512], digest[17];
    jw_delete_view view;
    jw_delete_result result;
};

static int jw__writable(jw_delete_view *view, const char *path, const char *source) {
#ifdef JW_ENABLE_FAULT_INJECTION
    const char *marker = getenv("JAWAKA_TEST_DELETE_READONLY_FILE");
    char blocked[32] = "";
    FILE *fp = marker ? fopen(marker, "r") : NULL;
    if (fp) { (void)fgets(blocked, sizeof(blocked), fp); fclose(fp); }
    blocked[strcspn(blocked, "\r\n")] = '\0';
    bool injected = !strcmp(blocked, source);
#else
    bool injected = false;
#endif
    if (!injected && jw_storage_path_writable(path, NULL, 0)) return 0;
    snprintf(view->readonly_source, sizeof(view->readonly_source), "%s",
             !strcmp(source, "primary") ? "launcher_sd" : source);
    snprintf(view->error, sizeof(view->error), "Your %s storage is unavailable or read-only.", source);
    return -1;
}

static int jw__build(const char *db_path, const char *sdcard_root, const char *source_id,
                     const char *rom_relpath, jw_delete_view *view) {
    jw_storage_source_list *sources = calloc(1, sizeof(*sources));
    jw_ra_catalog *catalog = NULL;
    jw_game_entry selected;
    int rc = -1;
    if (!sources) { snprintf(view->error, sizeof(view->error), "Could not allocate storage sources."); return -1; }
    if (jw__writable(view, db_path, "primary") != 0) goto done;
    if (jw_storage_sources_resolve(sdcard_root, sources) != 0) {
        snprintf(view->error, sizeof(view->error), "Could not resolve storage sources.");
        goto done;
    }
    const jw_storage_source *source = jw_storage_sources_find_by_id(sources, source_id);
    if (!source || !source->available) {
        snprintf(view->error, sizeof(view->error), "Your selected ROM card is not mounted.");
        goto done;
    }
    if (jw__writable(view, source->roms_path, source->id) != 0) goto done;
    if (jw_db_get_game_by_source_relpath(db_path, source_id, rom_relpath, &selected) != 0) {
        snprintf(view->error, sizeof(view->error), "Could not read your game's library identity.");
        goto done;
    }
    snprintf(view->name, sizeof(view->name), "%s", selected.name);
    catalog = jw_ra_catalog_load(sdcard_root, view->error, sizeof(view->error));
    if (!catalog) goto done;
    rc = jw_delete_plan_build(sources, jw_ra_catalog_find_system(catalog, selected.system),
                              source_id, rom_relpath, &view->plan, view->error, sizeof(view->error));
    for (size_t i = 0; rc == 0 && i < view->plan.file_count; ++i) {
        const jw_delete_file *file = &view->plan.files[i];
        if (!file->missing && jw__writable(view, file->path, file->source_id) != 0) rc = -1;
    }
done:
    free(sources);
    jw_ra_catalog_free(catalog);
    return rc;
}

static cJSON *jw__status(jw_ipc_delete_phase phase, const char *source_id,
                         const jw_delete_view *view, const jw_delete_result *result) {
    cJSON *reply = cJSON_CreateObject();
    if (!reply) return NULL;
    cJSON_AddStringToObject(reply, "type", "rom-delete-status");
    cJSON_AddNumberToObject(reply, "phase", phase);
    cJSON_AddStringToObject(reply, "source_id", source_id);
    if (phase == JW_IPC_DELETE_COMMITTING) return reply;
    const jw_delete_plan *plan = &view->plan;
    cJSON_AddStringToObject(reply, "name", view->name);
    cJSON_AddStringToObject(reply, "error", view->error);
    cJSON_AddStringToObject(reply, "readonly_source", view->readonly_source);
    if (phase == JW_IPC_DELETE_READY) cJSON_AddStringToObject(reply, "digest", plan->digest);
    cJSON_AddBoolToObject(reply, "writable_progress", plan->writable_image);
    cJSON_AddNumberToObject(reply, "bytes", result ? result->bytes : plan->bytes);
    cJSON_AddNumberToObject(reply, "removed_count", result ? result->removed_count : 0);
    cJSON_AddNumberToObject(reply, "absent_count", result ? result->absent_count : 0);
    cJSON *files = cJSON_AddArrayToObject(reply, "files");
    for (size_t i = 0; files && i < plan->file_count; ++i) {
        const jw_delete_file *file = &plan->files[i];
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "source_id", file->source_id);
        cJSON_AddStringToObject(item, "rom_relpath", file->rom_relpath);
        cJSON_AddNumberToObject(item, "size", file->size);
        cJSON_AddBoolToObject(item, "missing", file->missing);
        cJSON_AddBoolToObject(item, "removed",
            result && result->outcomes && result->outcomes[i] == JW_DELETE_REMOVED);
        cJSON_AddItemToArray(files, item);
    }
    return reply;
}

cJSON *jw_delete_preview(const char *db_path, const char *sdcard_root,
                         const char *source_id, const char *rom_relpath) {
    jw_delete_view view = {0};
    int rc = jw__build(db_path, sdcard_root, source_id, rom_relpath, &view);
    if (rc != 0) jw_delete_plan_free(&view.plan);
    cJSON *reply = jw__status(rc == 0 ? JW_IPC_DELETE_READY : JW_IPC_DELETE_ERROR,
                              source_id, &view, NULL);
    jw_delete_plan_free(&view.plan);
    return reply;
}

static int jw__guard(void *context, const jw_delete_file *file, char *error, size_t size) {
    jw_delete_job *job = context;
#ifdef JW_ENABLE_FAULT_INJECTION
    const char *marker = getenv("JAWAKA_TEST_DELETE_FAIL_AFTER_FILE");
    FILE *fp = marker ? fopen(marker, "r") : NULL;
    unsigned limit = 0;
    bool fail = fp && fscanf(fp, "%u", &limit) == 1 && job->result.removed_count >= limit;
    if (fp) fclose(fp);
    if (fail) {
        snprintf(error, size, "Injected removal failure before %.160s", file->rom_relpath);
        return -1;
    }
#endif
    if (jw__writable(&job->view, job->db_path, "primary") != 0 ||
        jw__writable(&job->view, file->path, file->source_id) != 0) {
        if (error != job->view.error) snprintf(error, size, "%s", job->view.error);
        return -1;
    }
    return 0;
}

static int jw__cancel_scrapes(jw_delete_job *job) {
    jw_game_entry *games = NULL;
    size_t count = 0;
    if (jw_db_list_indexed_games(job->db_path, &games, &count) != 0) {
        snprintf(job->view.error, sizeof(job->view.error), "Could not check artwork work before deletion.");
        return -1;
    }
    int rc = 0;
    for (size_t g = 0; g < count && rc == 0; ++g) {
        for (size_t f = 0; f < job->view.plan.file_count; ++f) {
            const jw_delete_file *file = &job->view.plan.files[f];
            if (strcmp(file->source_id, games[g].source_id) ||
                strcmp(file->rom_relpath, games[g].rom_relpath)) continue;
            jw_scrape_cancel_game(games[g].system, games[g].rom_path);
            for (int attempt = 0; attempt < 200 && jw_scrape_is_pending_game(games[g].system, games[g].rom_path); ++attempt)
                usleep(10000);
            if (jw_scrape_is_pending_game(games[g].system, games[g].rom_path)) {
                snprintf(job->view.error, sizeof(job->view.error),
                         "Artwork scraping is stopping. Try again when it finishes.");
                rc = -1;
            }
            break;
        }
    }
    free(games);
    return rc;
}

static void *jw__commit(void *context) {
    jw_delete_job *job = context;
#ifdef JW_ENABLE_FAULT_INJECTION
    const char *delay = getenv("JAWAKA_TEST_DELETE_COMMIT_DELAY_MS");
    int milliseconds = delay ? atoi(delay) : 0;
    if (milliseconds > 3000) milliseconds = 3000;
    for (int elapsed = 0; elapsed < milliseconds; elapsed += 10) usleep(10000);
#endif
    jw_delete_view *view = &job->view;
    sqlite3 *db = NULL;
    int rc = -1;
    bool changed = false, transaction = false;
    jw_db_rom_key *removed = NULL;
    if (jw__build(job->db_path, job->sdcard_root, job->source_id, job->rom_relpath, view) != 0) goto done;
    if (strcmp(view->plan.digest, job->digest)) {
        snprintf(view->error, sizeof(view->error), "Your game's files changed. Open Delete Game again.");
        goto done;
    }
    if (jw__cancel_scrapes(job) != 0) goto done;
    if (jw_db_open(job->db_path, &db) != 0 || jw_db_apply_schema(db) != 0 ||
        sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        snprintf(view->error, sizeof(view->error), "Your library is busy or read-only. Try again.");
        if (!jw_storage_path_writable(job->db_path, NULL, 0))
            snprintf(view->readonly_source, sizeof(view->readonly_source), "launcher_sd");
        goto done;
    }
    transaction = true;
    for (size_t i = 0; i < view->plan.file_count; ++i) {
        if (jw_db_relocation_key_reserved(db, view->plan.files[i].source_id, view->plan.files[i].rom_relpath)) {
            snprintf(view->error, sizeof(view->error), "A file is being moved. Try again when it finishes.");
            goto done;
        }
    }
    removed = calloc(view->plan.file_count, sizeof(*removed));
    if (!removed) { snprintf(view->error, sizeof(view->error), "Could not allocate deletion results."); goto done; }
    rc = jw_delete_execute(&view->plan, jw__guard, job, &job->result, view->error, sizeof(view->error));
    size_t count = 0;
    for (size_t i = 0; i < view->plan.file_count; ++i) {
        if (!job->result.outcomes || job->result.outcomes[i] == JW_DELETE_PENDING) continue;
        snprintf(removed[count].source_id, sizeof(removed[count].source_id), "%s", view->plan.files[i].source_id);
        snprintf(removed[count++].rom_relpath, sizeof(removed[0].rom_relpath), "%s", view->plan.files[i].rom_relpath);
    }
    changed = count > 0;
    if (jw_db_reconcile_removed_roms(db, removed, count) != 0 ||
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        size_t used = strlen(view->error);
        snprintf(view->error + used, sizeof(view->error) - used,
                 "%sLibrary cleanup failed after %zu files were removed. Refresh the library.",
                 used ? " " : "", job->result.removed_count);
        rc = -1;
    } else transaction = false;
done:
    if (transaction) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    jw_db_close(db);
    free(removed);
    pthread_mutex_lock(&job->mutex);
    job->changed = changed;
    job->phase = rc == 0 ? JW_IPC_DELETE_DONE : JW_IPC_DELETE_ERROR;
    pthread_mutex_unlock(&job->mutex);
    return NULL;
}

jw_delete_job *jw_delete_job_start(const char *db_path, const char *sdcard_root,
                                  const char *source_id, const char *rom_relpath,
                                  const char *digest) {
    jw_delete_job *job = calloc(1, sizeof(*job));
    if (!job) return NULL;
    pthread_mutex_init(&job->mutex, NULL);
    job->phase = JW_IPC_DELETE_COMMITTING;
    snprintf(job->db_path, sizeof(job->db_path), "%s", db_path);
    snprintf(job->sdcard_root, sizeof(job->sdcard_root), "%s", sdcard_root);
    snprintf(job->source_id, sizeof(job->source_id), "%s", source_id);
    snprintf(job->rom_relpath, sizeof(job->rom_relpath), "%s", rom_relpath);
    snprintf(job->digest, sizeof(job->digest), "%s", digest);
    pthread_attr_t attributes;
    int rc = pthread_attr_init(&attributes);
    /* Descriptor resolution recurses; macOS's default worker stack is 512 KB. */
    if (rc == 0) {
        rc = pthread_attr_setstacksize(&attributes, 2 * 1024 * 1024);
        if (rc == 0) rc = pthread_create(&job->thread, &attributes, jw__commit, job);
        pthread_attr_destroy(&attributes);
    }
    if (rc != 0) { jw_delete_job_destroy(job); return NULL; }
    job->started = true;
    return job;
}

bool jw_delete_job_busy(jw_delete_job *job) {
    if (!job) return false;
    pthread_mutex_lock(&job->mutex);
    bool busy = job->phase == JW_IPC_DELETE_COMMITTING;
    pthread_mutex_unlock(&job->mutex);
    return busy;
}

bool jw_delete_job_changed(jw_delete_job *job) {
    if (!job) return false;
    pthread_mutex_lock(&job->mutex);
    bool changed = job->changed;
    job->changed = false;
    pthread_mutex_unlock(&job->mutex);
    return changed;
}

cJSON *jw_delete_job_status(jw_delete_job *job) {
    if (!job) return NULL;
    pthread_mutex_lock(&job->mutex);
    jw_ipc_delete_phase phase = job->phase;
    pthread_mutex_unlock(&job->mutex);
    /* The worker owns the view until it publishes a final phase. */
    return jw__status(phase, job->source_id, &job->view,
                      phase == JW_IPC_DELETE_COMMITTING ? NULL : &job->result);
}

void jw_delete_job_destroy(jw_delete_job *job) {
    if (!job) return;
    if (job->started) pthread_join(job->thread, NULL);
    jw_delete_plan_free(&job->view.plan);
    jw_delete_result_free(&job->result);
    pthread_mutex_destroy(&job->mutex);
    free(job);
}
