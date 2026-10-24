#ifndef JW_SCRAPE_WORKER_H
#define JW_SCRAPE_WORKER_H

#include <stdbool.h>

/* Daemon-side scrape queue + worker. One global queue (the daemon owns one
   scrape pipeline); items are deduped by (system, rom_path). Art lands at
   the rom's own storage source under Images/<SYS>/<rom-base>.png, the game
   row's image_path is updated, and library.generation is bumped per file so
   the launcher refreshes live. */

#define JW_SCRAPE_MAX_DIM 1000   /* longest-side cap for saved art */
#define JW_SCRAPE_QUEUE_SNAPSHOT_MAX 256

typedef enum {
    JW_SCRAPE_IDLE = 0,
    JW_SCRAPE_RUNNING,
    JW_SCRAPE_PAUSED_QUOTA,   /* daily quota exhausted; queue retained */
    JW_SCRAPE_PAUSED_STORAGE, /* artwork card or library DB not writable; queue retained */
} jw_scrape_state;

typedef struct {
    jw_scrape_state state;
    int  total;               /* items accepted since the queue last drained */
    int  done;                /* terminal rows (found + not_found + failed + cancelled) */
    int  found;
    int  not_found;
    int  failed;
    int  cancelled;
    int  queued;              /* still waiting (excludes in-flight) */
    int  active;              /* in-flight worker slots */
    char current_name[256];   /* rom filename being processed ("" when idle) */
    char current_system[64];
    char message[256];        /* last error / quota message ("" when none) */
} jw_scrape_status_info;

typedef enum {
    JW_SCRAPE_ROW_QUEUED = 0,
    JW_SCRAPE_ROW_HASH,
    JW_SCRAPE_ROW_SEARCH,
    JW_SCRAPE_ROW_DOWNLOAD,
    JW_SCRAPE_ROW_SAVE,
    JW_SCRAPE_ROW_DONE,
    JW_SCRAPE_ROW_NOT_FOUND,
    JW_SCRAPE_ROW_ERROR,
    JW_SCRAPE_ROW_CANCELLED,
} jw_scrape_row_state;

typedef struct {
    unsigned id;
    jw_scrape_row_state state;
    char display_name[256];
    char system[64];
    char rom_path[512];
    char output_path[512];
    char message[256];
} jw_scrape_queue_row;

typedef struct {
    jw_scrape_state state;
    int total;
    int done;
    int found;
    int not_found;
    int failed;
    int cancelled;
    int queued;
    int active;
    int row_count;
    int requests_today;
    int max_requests;
    int max_threads;
    int permits;
    int eta_seconds;          /* -1 when unavailable */
    char message[256];
    jw_scrape_queue_row rows[JW_SCRAPE_QUEUE_SNAPSHOT_MAX];
} jw_scrape_queue_info;

typedef struct {
    int requested;        /* games selected for enqueue after mode filters */
    int enqueued;         /* new queue rows accepted */
    int already_queued;   /* selected games already queued or active */
    int skipped_existing; /* missing-only games that already have art */
    bool queue_full;      /* queue filled before all selected games fit */
} jw_scrape_enqueue_result;

/* Start/stop the worker thread. Both are idempotent; stop joins the thread
   and drops any queued items. */
int  jw_scrape_worker_start(const char *db_path, const char *sdcard_root);
void jw_scrape_worker_stop(void);

/* Enqueue one game (always re-fetches and replaces existing art). Returns the
   number of items enqueued (0 when deduped), or -1 with *error set to a static
   message. Queue full intentionally stays on the error path for this legacy
   single-game API; batch callers use jw_scrape_enqueue_result.queue_full. */
int jw_scrape_enqueue_game(const char *system, const char *rom_path,
                           const char **error);

/* Enqueue every game of a system; missing_only skips games whose art file
   already exists. Same return convention as enqueue_game. */
int jw_scrape_enqueue_system(const char *system, bool missing_only,
                             const char **error);
int jw_scrape_enqueue_system_full(const char *system, bool missing_only,
                                  jw_scrape_enqueue_result *out,
                                  const char **error);

/* Enqueue every mapped system at once (used by scope "all"). Returns the total
   enqueued; per-system errors are tolerated. -1 only on a fatal setup error. */
int jw_scrape_enqueue_all(bool missing_only, const char **error);
int jw_scrape_enqueue_all_full(bool missing_only,
                               jw_scrape_enqueue_result *out,
                               const char **error);

/* Count a system's games still needing art (out_missing) and its total
   (out_total). Returns 0 on success; unmapped systems report zero. */
int jw_scrape_count_missing_system(const char *system, int *out_missing,
                                   int *out_total, const char **error);

typedef struct {
    char system[64];   /* system code (e.g. "GB") */
    int  missing;      /* games still needing art */
    int  total;        /* games in this system */
} jw_scrape_missing_row;

/* Per-system missing/total counts for every mapped system that has games.
   Fills up to `max` rows, sets *out_count and *out_total_missing. Returns 0 on
   success, -1 on a fatal setup error. */
int jw_scrape_missing_counts(jw_scrape_missing_row *out, int max,
                             int *out_count, int *out_total_missing);

void jw_scrape_status(jw_scrape_status_info *out);

/* Re-evaluate a storage pause. The daemon calls this when storage health
   changes; the batch resumes only when every dependency it stopped on is
   writable again and no repair hold remains. The queue lives in memory only:
   a reboot, including a repair reboot, starts with an empty queue. */
void jw_scrape_storage_recheck(void);
void jw_scrape_queue_snapshot(jw_scrape_queue_info *out, int offset, int limit);

/* Clear terminal rows (done/not-found/error/cancelled), preserving queued and
   active work. Returns the number of rows removed. */
int jw_scrape_clear_done(void);

/* Stop queued and active work. Active HTTP/backoff is interrupted where the
   ScreenScraper client can observe it. Returns terminal rows created. */
int jw_scrape_stop_all(void);

/* Cancel queued and matching in-flight work (returns affected items). */
int jw_scrape_cancel_all(void);
int jw_scrape_cancel_system(const char *system);
int jw_scrape_cancel_game(const char *system, const char *rom_path);

/* True when the game/system has queued or in-flight work (drives the
   Actions-menu "Cancel Scraping" swap). */
bool jw_scrape_is_pending_game(const char *system, const char *rom_path);
bool jw_scrape_is_pending_system(const char *system);

#endif /* JW_SCRAPE_WORKER_H */
