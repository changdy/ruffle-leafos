#ifndef JW_LAUNCHER_COVER_LOADER_H
#define JW_LAUNCHER_COVER_LOADER_H

#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

/* ---- Background cover decoder -----------------------------------------------
   Generating a thumbnail means decoding the full ~1MB source image, which blocks
   the UI thread ~200ms — so the *first* pass through a list (before any thumbnails
   exist) stutters, and the cover under the cursor shows blank until it is built.
   Move that decode to a worker thread with two inputs:
     - a high-priority slot = the cover under the cursor (latest wins), decoded
       first and handed back to the UI as a texture; and
     - a low-priority FIFO queue = the rest of the visible list (pre-warm), decoded
       only when the priority slot is idle. Pre-warm decodes just write the
       thumbnail to disk; the surface is discarded, so when the cursor later lands
       on that cover the loader hits the fast on-disk thumbnail path.
   Net effect: scroll freely while covers fill in a beat ahead of the cursor, in
   Favorites/Recents/search/systems alike. Existing thumbnails usually decode
   inline, but coverflow motion can route them through the worker to avoid
   mid-animation hitches.

   A decode that fails (unreadable or invalid art) is a result too: the file is
   remembered with its identity, callers get JW_COVER_FAILED and draw their
   placeholder, and the same unchanged file is decoded at most
   JW_COVER_FAIL_ATTEMPTS times. Replacing the file, or a library rescan
   (jw_cover_loader_forget_failures), lets it load again.

   Surfaces are opaque here; the launcher supplies decode/free callbacks, which
   keeps the scheduling testable without SDL. */

#define JW_COVER_QUEUE_MAX 48          /* pre-warm backlog cap (ring buffer) */
/* Handbacks are only produced when the thumbnail cache cannot be written, and a
   page drains them every frame, so this need only cover one frame's worth of
   arrivals. Small on purpose: each entry holds a decoded surface. */
#define JW_COVER_HANDBACK_MAX 4

#define JW_COVER_FAIL_MAX        64     /* failed files remembered (ring) */
#define JW_COVER_FAIL_ATTEMPTS   3      /* decodes of an unchanged file, then give up */
#define JW_COVER_FAIL_RETRY_MS   2000u  /* spacing between those attempts */
#define JW_COVER_FAIL_RECHECK_MS 1000u  /* how often a failed file is stat'ed for replacement */

typedef enum {
    JW_COVER_PENDING = 0,   /* not decoded yet (or no worker): try again next frame */
    JW_COVER_READY,         /* *out holds a surface the caller now owns */
    JW_COVER_FAILED,        /* the art cannot be decoded: draw the placeholder */
} jw_cover_status;

typedef struct {
    /* Decode `path` (or its fresh thumbnail at `thumb`, when non-NULL) to at most
       max_dim on the longest edge. Returns a surface or NULL on failure. Runs on
       the worker thread. */
    void *(*decode)(const char *path, const char *thumb, int max_dim, void *userdata);
    void (*free_surface)(void *surface, void *userdata);
    /* True once `thumb` holds a fresh thumbnail for `path`. */
    bool (*is_persisted)(const char *path, const char *thumb, void *userdata);
    uint32_t (*now_ms)(void *userdata);
    void *userdata;
    int default_max_dim;
} jw_cover_loader_ops;

typedef struct {
    bool   exists;
    dev_t  dev;
    ino_t  ino;
    off_t  size;
    time_t mtime;
} jw_cover_identity;

typedef struct {
    bool              used;
    char              path[PATH_MAX];
    jw_cover_identity identity;      /* of the file whose decode failed */
    int               attempts;      /* failed decodes of that identity */
    uint32_t          failed_ms;     /* when the last one finished */
    uint32_t          checked_ms;    /* last identity re-check */
} jw_cover_failure;

typedef struct {
    pthread_t       thread;
    pthread_mutex_t lock;
    pthread_cond_t  cond;
    bool            started;
    bool            stop;
    jw_cover_loader_ops ops;

    /* High-priority request: the cover under the cursor (latest wins). */
    char            req_path[PATH_MAX];
    char            req_thumb[PATH_MAX];
    int             req_max;          /* longest edge wanted, px */
    bool            has_req;

    /* Low-priority pre-warm queue (FIFO ring of covers to build ahead of time). */
    char            q_path[JW_COVER_QUEUE_MAX][PATH_MAX];
    char            q_thumb[JW_COVER_QUEUE_MAX][PATH_MAX];
    int             q_max[JW_COVER_QUEUE_MAX];
    int             q_head;
    int             q_count;

    /* The decode the worker is running now, so a caller asking for it again
       every frame does not queue a second decode behind it. */
    char            inflight_path[PATH_MAX];

    /* Result: worker -> main (only the priority request is delivered this way). */
    char            done_path[PATH_MAX];
    void           *done_surf;
    bool            has_done;

    /* Pre-warm surfaces the thumbnail write could not persist, handed back so a
       page can still draw them. Kept apart from the priority slot and bounded:
       a page asks for every tile every frame and can leave before a decode
       lands, so delivery must retire what the page no longer wants instead of
       waiting on it. Sharing one slot let a single unclaimed result from a page
       the user had left block every later handback. */
    char            hb_path[JW_COVER_HANDBACK_MAX][PATH_MAX];
    void           *hb_surf[JW_COVER_HANDBACK_MAX];
    int             hb_head;                 /* oldest entry */
    int             hb_count;

    jw_cover_failure failures[JW_COVER_FAIL_MAX];
    int              fail_next;              /* ring eviction cursor */
} jw_cover_loader;

/* Reset the loader and install its callbacks. The worker starts lazily. */
void jw_cover_loader_init(jw_cover_loader *L, const jw_cover_loader_ops *ops);

/* Start the worker if needed (main thread only). False when it cannot start;
   callers then decode synchronously. */
bool jw_cover_loader_ensure(jw_cover_loader *L);

/* Join the worker and free every undelivered surface. */
void jw_cover_loader_shutdown(jw_cover_loader *L);

/* Collect the decoded priority result for `path`, or make `path` the priority
   request (newest wins). */
jw_cover_status jw_cover_loader_take(jw_cover_loader *L, const char *path,
                                     const char *thumb, int max_dim, void **out);

/* Collect a handed-back pre-warm surface for `path` without claiming the
   priority slot. */
jw_cover_status jw_cover_loader_poll(jw_cover_loader *L, const char *path, void **out);

/* Queue `path` for pre-warm, skipping duplicates, the in-flight decode, and
   failed art that is not due a retry. No-op when the queue is full. */
void jw_cover_loader_enqueue(jw_cover_loader *L, const char *path,
                             const char *thumb, int max_dim);

/* For the synchronous fallback: whether `path` failed and should not be decoded
   now, and recording a failed synchronous decode. */
bool jw_cover_loader_is_failed(jw_cover_loader *L, const char *path);
void jw_cover_loader_record_failure(jw_cover_loader *L, const char *path);

/* A library rescan: every failed file may be tried again. */
void jw_cover_loader_forget_failures(jw_cover_loader *L);

#endif
