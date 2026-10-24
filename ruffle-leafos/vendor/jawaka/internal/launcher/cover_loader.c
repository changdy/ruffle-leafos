#include "internal/launcher/cover_loader.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef enum {
    JW__FAIL_NONE = 0,    /* no failure on record, or the file has changed */
    JW__FAIL_RETRY_DUE,   /* failed, and another attempt is allowed now */
    JW__FAIL_WAIT,        /* failed: do not decode */
} jw__fail_state;

static uint32_t jw__cover_now(const jw_cover_loader *L) {
    uint32_t now = L->ops.now_ms ? L->ops.now_ms(L->ops.userdata) : 0u;
    return now ? now : 1u;
}

static void jw__cover_identity(const char *path, jw_cover_identity *out) {
    memset(out, 0, sizeof(*out));
    struct stat st;
    if (stat(path, &st) != 0) {
        return;
    }
    out->exists = true;
    out->dev = st.st_dev;
    out->ino = st.st_ino;
    out->size = st.st_size;
    out->mtime = st.st_mtime;
}

static bool jw__cover_identity_equal(const jw_cover_identity *a,
                                     const jw_cover_identity *b) {
    return a->exists == b->exists && a->dev == b->dev && a->ino == b->ino &&
           a->size == b->size && a->mtime == b->mtime;
}

/* Caller holds the lock for every jw__failure_* helper. */
static jw_cover_failure *jw__failure_find(jw_cover_loader *L, const char *path) {
    for (int i = 0; i < JW_COVER_FAIL_MAX; ++i) {
        if (L->failures[i].used && strcmp(L->failures[i].path, path) == 0) {
            return &L->failures[i];
        }
    }
    return NULL;
}

/* A failure only blocks the file that failed. The identity check is a stat on
   the SD card, so it runs at most once per JW_COVER_FAIL_RECHECK_MS per entry
   rather than on every frame that draws the placeholder. */
static jw__fail_state jw__failure_state(jw_cover_loader *L, const char *path) {
    jw_cover_failure *f = jw__failure_find(L, path);
    if (!f) {
        return JW__FAIL_NONE;
    }
    uint32_t now = jw__cover_now(L);
    if (now - f->checked_ms >= JW_COVER_FAIL_RECHECK_MS) {
        jw_cover_identity current;
        jw__cover_identity(path, &current);
        f->checked_ms = now;
        if (!jw__cover_identity_equal(&current, &f->identity)) {
            f->used = false;                /* replaced: load it like new art */
            return JW__FAIL_NONE;
        }
    }
    if (f->attempts < JW_COVER_FAIL_ATTEMPTS &&
        now - f->failed_ms >= JW_COVER_FAIL_RETRY_MS) {
        return JW__FAIL_RETRY_DUE;
    }
    return JW__FAIL_WAIT;
}

static void jw__failure_record(jw_cover_loader *L, const char *path,
                               const jw_cover_identity *identity) {
    jw_cover_failure *f = jw__failure_find(L, path);
    if (f && !jw__cover_identity_equal(&f->identity, identity)) {
        f->attempts = 0;                    /* a different file failed */
    }
    for (int i = 0; !f && i < JW_COVER_FAIL_MAX; ++i) {
        if (!L->failures[i].used) {
            f = &L->failures[i];
        }
    }
    if (!f) {
        /* Full: evict in ring order. Forgetting a failure costs at most one
           more bounded round of attempts for that file. */
        f = &L->failures[L->fail_next];
        L->fail_next = (L->fail_next + 1) % JW_COVER_FAIL_MAX;
        f->used = false;
    }
    if (!f->used) {
        memset(f, 0, sizeof(*f));
        snprintf(f->path, sizeof(f->path), "%s", path);
        f->used = true;
    }
    uint32_t now = jw__cover_now(L);
    f->identity = *identity;
    f->attempts++;
    f->failed_ms = now;
    f->checked_ms = now;
}

static void jw__failure_clear(jw_cover_loader *L, const char *path) {
    jw_cover_failure *f = jw__failure_find(L, path);
    if (f) {
        f->used = false;
    }
}

static void jw__cover_free(jw_cover_loader *L, void *surface) {
    if (surface && L->ops.free_surface) {
        L->ops.free_surface(surface, L->ops.userdata);
    }
}

/* Hand a surface back for later collection. Caller holds the lock.
   Replaces any pending entry for the same path, and when full drops the oldest:
   a result nobody has claimed is by definition the one least likely to be
   wanted, and dropping it only costs a re-decode, whereas keeping it would
   stall every page that follows. */
static void jw__cover_handback_put(jw_cover_loader *L, const char *path, void *surf) {
    for (int i = 0; i < L->hb_count; ++i) {
        int idx = (L->hb_head + i) % JW_COVER_HANDBACK_MAX;
        if (strcmp(L->hb_path[idx], path) == 0) {
            jw__cover_free(L, L->hb_surf[idx]);
            L->hb_surf[idx] = surf;
            return;
        }
    }
    if (L->hb_count == JW_COVER_HANDBACK_MAX) {
        jw__cover_free(L, L->hb_surf[L->hb_head]);
        L->hb_surf[L->hb_head] = NULL;
        L->hb_head = (L->hb_head + 1) % JW_COVER_HANDBACK_MAX;
        L->hb_count--;
    }
    int tail = (L->hb_head + L->hb_count) % JW_COVER_HANDBACK_MAX;
    snprintf(L->hb_path[tail], PATH_MAX, "%s", path);
    L->hb_surf[tail] = surf;
    L->hb_count++;
}

/* Free every pending handback. Caller holds the lock. */
static void jw__cover_handback_clear(jw_cover_loader *L) {
    for (int i = 0; i < L->hb_count; ++i) {
        int idx = (L->hb_head + i) % JW_COVER_HANDBACK_MAX;
        jw__cover_free(L, L->hb_surf[idx]);
        L->hb_surf[idx] = NULL;
    }
    L->hb_head = 0;
    L->hb_count = 0;
}

static void *jw__cover_worker(void *arg) {
    jw_cover_loader *L = (jw_cover_loader *)arg;
    pthread_mutex_lock(&L->lock);
    for (;;) {
        while (!L->stop && !L->has_req && L->q_count == 0) {
            pthread_cond_wait(&L->cond, &L->lock);
        }
        if (L->stop) break;

        char path[PATH_MAX], thumb[PATH_MAX];
        int  max_dim;
        bool was_req;
        if (L->has_req) {                          /* priority: cursor cover first */
            snprintf(path, sizeof(path), "%s", L->req_path);
            snprintf(thumb, sizeof(thumb), "%s", L->req_thumb);
            max_dim = L->req_max;
            L->has_req = false;
            was_req = true;
        } else {                                   /* else drain the pre-warm queue */
            snprintf(path, sizeof(path), "%s", L->q_path[L->q_head]);
            snprintf(thumb, sizeof(thumb), "%s", L->q_thumb[L->q_head]);
            max_dim = L->q_max[L->q_head];
            L->q_head = (L->q_head + 1) % JW_COVER_QUEUE_MAX;
            L->q_count--;
            was_req = false;
        }
        snprintf(L->inflight_path, sizeof(L->inflight_path), "%s", path);
        pthread_mutex_unlock(&L->lock);

        if (max_dim <= 0) max_dim = L->ops.default_max_dim;
        /* Identity before the decode: if the file is replaced mid-decode, the
           recorded failure no longer matches and the new file loads. */
        jw_cover_identity identity;
        jw__cover_identity(path, &identity);
        void *surf = L->ops.decode(path, thumb[0] ? thumb : NULL, max_dim,
                                   L->ops.userdata);

        /* The pre-warm contract is that a decoded cover is on disk afterwards, so
           the surface can be dropped and picked up inline on a later frame. When
           the write silently fails -- a full card, a thumbs path that is not a
           directory -- dropping it strands readable artwork forever. Persistence
           is best effort by design and IMG_SavePNG's result is deliberately
           ignored, so ask the disk rather than trust the contract. Two stats on
           the worker thread, outside the lock, only on a cold decode. */
        bool persisted = !surf || !thumb[0] ||
                         (L->ops.is_persisted &&
                          L->ops.is_persisted(path, thumb, L->ops.userdata));

        pthread_mutex_lock(&L->lock);
        L->inflight_path[0] = '\0';
        if (surf) {
            jw__failure_clear(L, path);
        } else {
            jw__failure_record(L, path, &identity);
        }
        if (was_req) {
            if (L->has_done && L->done_surf) {     /* drop a previous undelivered result */
                jw__cover_free(L, L->done_surf);
                L->done_surf = NULL;
                L->has_done = false;
            }
            if (surf) {
                snprintf(L->done_path, sizeof(L->done_path), "%s", path);
                L->done_surf = surf;
                L->has_done = true;
            }
        } else if (surf && !persisted) {
            /* Could not land on disk: hand it back so the page can draw it.
               Never through the priority slot -- a cursor cover the user is
               waiting on must not be displaced by a background pre-warm, and an
               unclaimed pre-warm result must not be able to block later ones. */
            jw__cover_handback_put(L, path, surf);
        } else if (surf) {
            jw__cover_free(L, surf);               /* pre-warm: thumbnail is on disk now */
        }
    }
    pthread_mutex_unlock(&L->lock);
    return NULL;
}

void jw_cover_loader_init(jw_cover_loader *L, const jw_cover_loader_ops *ops) {
    memset(L, 0, sizeof(*L));
    pthread_mutex_init(&L->lock, NULL);
    pthread_cond_init(&L->cond, NULL);
    if (ops) {
        L->ops = *ops;
    }
}

bool jw_cover_loader_ensure(jw_cover_loader *L) {
    if (L->started) return true;       /* called on the main thread only */
    L->started = true;
    if (pthread_create(&L->thread, NULL, jw__cover_worker, L) != 0) {
        L->started = false;            /* no worker -> callers decode synchronously */
    }
    return L->started;
}

void jw_cover_loader_shutdown(jw_cover_loader *L) {
    if (!L->started) return;

    pthread_mutex_lock(&L->lock);
    L->stop = true;
    pthread_cond_signal(&L->cond);
    pthread_mutex_unlock(&L->lock);

    pthread_join(L->thread, NULL);

    pthread_mutex_lock(&L->lock);
    if (L->done_surf) {
        jw__cover_free(L, L->done_surf);
        L->done_surf = NULL;
    }
    jw__cover_handback_clear(L);
    L->has_done = false;
    L->has_req = false;
    L->q_head = 0;
    L->q_count = 0;
    L->inflight_path[0] = '\0';
    L->stop = false;
    L->started = false;
    pthread_mutex_unlock(&L->lock);
}

jw_cover_status jw_cover_loader_take(jw_cover_loader *L, const char *path,
                                     const char *thumb, int max_dim, void **out) {
    if (!jw_cover_loader_ensure(L)) return JW_COVER_PENDING;
    jw_cover_status status = JW_COVER_PENDING;
    pthread_mutex_lock(&L->lock);
    if (L->has_done && strcmp(L->done_path, path) == 0) {
        *out = L->done_surf;
        L->done_surf = NULL;
        L->has_done = false;
        status = JW_COVER_READY;
    } else {
        jw__fail_state failed = jw__failure_state(L, path);
        bool queued = (L->has_req && strcmp(L->req_path, path) == 0) ||
                      strcmp(L->inflight_path, path) == 0;
        /* Keep an unmatched result for the rest of this draw pass. Coverflow draws
           side cards before the centre card, so dropping it here can discard the
           cover that is about to be requested later in the same frame. The worker
           still replaces stale results when a newer decode completes. */
        if (failed != JW__FAIL_WAIT && !queued) {
            L->req_max = max_dim;
            snprintf(L->req_path, sizeof(L->req_path), "%s", path);
            snprintf(L->req_thumb, sizeof(L->req_thumb), "%s", thumb ? thumb : "");
            L->has_req = true;
            pthread_cond_signal(&L->cond);
        }
        /* A retry keeps the placeholder up rather than blanking the panel. */
        if (failed != JW__FAIL_NONE) {
            status = JW_COVER_FAILED;
        }
    }
    pthread_mutex_unlock(&L->lock);
    return status;
}

/* Consume a ready surface for `path` if the worker left one, without ever
   claiming the priority slot. A page asks for every tile on every frame, so
   the take path's newest-wins request would have the twelve of them
   overwrite each other and starve the cursor cover. */
jw_cover_status jw_cover_loader_poll(jw_cover_loader *L, const char *path, void **out) {
    if (!L->started) return JW_COVER_PENDING;
    jw_cover_status status = JW_COVER_PENDING;
    pthread_mutex_lock(&L->lock);
    for (int i = 0; i < L->hb_count; ++i) {
        int idx = (L->hb_head + i) % JW_COVER_HANDBACK_MAX;
        if (strcmp(L->hb_path[idx], path) != 0) continue;
        *out = L->hb_surf[idx];
        L->hb_surf[idx] = NULL;
        /* Close the gap by sliding the older entries forward, so the ring stays
           oldest-first and eviction keeps meaning what it says. */
        for (int j = i; j > 0; --j) {
            int dst = (L->hb_head + j) % JW_COVER_HANDBACK_MAX;
            int src = (L->hb_head + j - 1) % JW_COVER_HANDBACK_MAX;
            memcpy(L->hb_path[dst], L->hb_path[src], PATH_MAX);
            L->hb_surf[dst] = L->hb_surf[src];
        }
        L->hb_surf[L->hb_head] = NULL;
        L->hb_head = (L->hb_head + 1) % JW_COVER_HANDBACK_MAX;
        L->hb_count--;
        status = JW_COVER_READY;
        break;
    }
    if (status == JW_COVER_PENDING && jw__failure_state(L, path) != JW__FAIL_NONE) {
        status = JW_COVER_FAILED;
    }
    pthread_mutex_unlock(&L->lock);
    return status;
}

/* Append a cover to the low-priority pre-warm queue, skipping duplicates and the
   in-flight priority request. Caller has already confirmed the thumbnail is
   missing. No-op when the queue is full (we just pre-warm fewer covers). */
void jw_cover_loader_enqueue(jw_cover_loader *L, const char *path,
                             const char *thumb, int max_dim) {
    if (!jw_cover_loader_ensure(L)) return;
    pthread_mutex_lock(&L->lock);
    bool dup = jw__failure_state(L, path) == JW__FAIL_WAIT ||
               (L->has_req && strcmp(L->req_path, path) == 0) ||
               strcmp(L->inflight_path, path) == 0;
    for (int i = 0; !dup && i < L->q_count; ++i) {
        int idx = (L->q_head + i) % JW_COVER_QUEUE_MAX;
        if (strcmp(L->q_path[idx], path) == 0) dup = true;
    }
    if (!dup && L->q_count < JW_COVER_QUEUE_MAX) {
        int tail = (L->q_head + L->q_count) % JW_COVER_QUEUE_MAX;
        snprintf(L->q_path[tail], PATH_MAX, "%s", path);
        snprintf(L->q_thumb[tail], PATH_MAX, "%s", thumb ? thumb : "");
        L->q_max[tail] = max_dim;
        L->q_count++;
        pthread_cond_signal(&L->cond);
    }
    pthread_mutex_unlock(&L->lock);
}

bool jw_cover_loader_is_failed(jw_cover_loader *L, const char *path) {
    pthread_mutex_lock(&L->lock);
    bool failed = jw__failure_state(L, path) == JW__FAIL_WAIT;
    pthread_mutex_unlock(&L->lock);
    return failed;
}

void jw_cover_loader_record_failure(jw_cover_loader *L, const char *path) {
    jw_cover_identity identity;
    jw__cover_identity(path, &identity);
    pthread_mutex_lock(&L->lock);
    jw__failure_record(L, path, &identity);
    pthread_mutex_unlock(&L->lock);
}

void jw_cover_loader_forget_failures(jw_cover_loader *L) {
    pthread_mutex_lock(&L->lock);
    memset(L->failures, 0, sizeof(L->failures));
    L->fail_next = 0;
    pthread_mutex_unlock(&L->lock);
}
