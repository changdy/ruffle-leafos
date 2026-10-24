/* Failed cover decodes: the priority (selected cover) and pre-warm paths reach
   JW_COVER_FAILED, an unchanged failure is not decoded every frame, valid
   neighbouring covers still load, and replacement or a rescan recovers. The
   decoder and clock are fakes; files are real so identity checks run. */

#include "internal/launcher/cover_loader.h"

#include <assert.h>
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define DECODE_LOG_MAX 32

static atomic_uint g_now = 1000;
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    char path[PATH_MAX];
    int count;
} g_decodes[DECODE_LOG_MAX];
static int g_decode_paths;
static atomic_int g_live_surfaces;

static jw_cover_loader g_loader;
static char g_root[PATH_MAX];

/* Art is "valid" when the file starts with "OK". */
static void *fake_decode(const char *path, const char *thumb, int max_dim, void *ud) {
    (void)thumb;
    (void)max_dim;
    (void)ud;
    pthread_mutex_lock(&g_mu);
    int i = 0;
    while (i < g_decode_paths && strcmp(g_decodes[i].path, path) != 0) i++;
    if (i == g_decode_paths) {
        assert(g_decode_paths < DECODE_LOG_MAX);
        snprintf(g_decodes[g_decode_paths++].path, PATH_MAX, "%s", path);
    }
    g_decodes[i].count++;
    pthread_mutex_unlock(&g_mu);

    char head[3] = { 0 };
    FILE *f = fopen(path, "rb");
    if (f) {
        size_t n = fread(head, 1, 2, f);
        (void)n;
        fclose(f);
    }
    if (strcmp(head, "OK") != 0) {
        return NULL;
    }
    int *surface = malloc(sizeof(*surface));
    assert(surface);
    *surface = 1;
    atomic_fetch_add(&g_live_surfaces, 1);
    return surface;
}

static void fake_free(void *surface, void *ud) {
    (void)ud;
    free(surface);
    atomic_fetch_sub(&g_live_surfaces, 1);
}

static bool fake_persisted(const char *path, const char *thumb, void *ud) {
    (void)path;
    (void)thumb;
    (void)ud;
    return true;
}

static uint32_t fake_now(void *ud) {
    (void)ud;
    return atomic_load(&g_now);
}

static void advance(uint32_t ms) {
    atomic_fetch_add(&g_now, ms);
}

static int decodes(const char *path) {
    pthread_mutex_lock(&g_mu);
    int n = 0;
    for (int i = 0; i < g_decode_paths; i++) {
        if (strcmp(g_decodes[i].path, path) == 0) n = g_decodes[i].count;
    }
    pthread_mutex_unlock(&g_mu);
    return n;
}

static void art(const char *name, const char *content, char *out, size_t out_size) {
    snprintf(out, out_size, "%s/%s", g_root, name);
    FILE *f = fopen(out, "wb");
    assert(f);
    fputs(content, f);
    fclose(f);
}

static void wait_idle(void) {
    for (int i = 0; i < 3000; i++) {
        pthread_mutex_lock(&g_loader.lock);
        bool idle = !g_loader.has_req && g_loader.q_count == 0 &&
                    g_loader.inflight_path[0] == '\0';
        pthread_mutex_unlock(&g_loader.lock);
        if (idle) return;
        usleep(1000);
    }
    fprintf(stderr, "cover worker did not go idle\n");
    abort();
}

static jw_cover_status take_once(const char *path) {
    void *surface = NULL;
    jw_cover_status st = jw_cover_loader_take(&g_loader, path, NULL, 100, &surface);
    if (st == JW_COVER_READY) {
        assert(surface);
        fake_free(surface, NULL);
    }
    return st;
}

/* Ask for the selected cover once per frame until it settles. */
static jw_cover_status take_until_settled(const char *path) {
    for (int i = 0; i < 3000; i++) {
        jw_cover_status st = take_once(path);
        if (st != JW_COVER_PENDING) return st;
        usleep(1000);
    }
    return JW_COVER_PENDING;
}

/* Frames that keep asking for an already-failed cover. */
static void failed_frames(const char *path, int frames) {
    for (int i = 0; i < frames; i++) {
        assert(take_once(path) == JW_COVER_FAILED);
        usleep(200);
    }
    wait_idle();
}

static void test_selected_cover_failure_is_bounded(void) {
    char bad[PATH_MAX], good[PATH_MAX];
    art("selected-bad.jpg", "BAD", bad, sizeof(bad));
    art("neighbour-good.jpg", "OK", good, sizeof(good));

    assert(take_until_settled(bad) == JW_COVER_FAILED);
    assert(decodes(bad) == 1);
    failed_frames(bad, 300);
    assert(decodes(bad) == 1);

    /* Valid pre-warm work behind a failing selected cover still loads. */
    jw_cover_loader_enqueue(&g_loader, good, "thumb", 100);
    failed_frames(bad, 50);
    assert(decodes(good) == 1);

    /* Retries are spaced and capped. */
    for (int attempt = 2; attempt <= JW_COVER_FAIL_ATTEMPTS; attempt++) {
        advance(JW_COVER_FAIL_RETRY_MS);
        failed_frames(bad, 100);
        assert(decodes(bad) == attempt);
    }
    for (int i = 0; i < 5; i++) {
        advance(JW_COVER_FAIL_RETRY_MS * 4);
        failed_frames(bad, 50);
    }
    assert(decodes(bad) == JW_COVER_FAIL_ATTEMPTS);

    /* A different valid selected cover is unaffected. */
    char other[PATH_MAX];
    art("selected-good.jpg", "OK", other, sizeof(other));
    assert(take_until_settled(other) == JW_COVER_READY);

    /* Replacing the file (new inode, like a CS upload) recovers once the
       identity is rechecked. */
    char tmp[PATH_MAX];
    art("selected-bad.tmp", "OK valid now", tmp, sizeof(tmp));
    assert(rename(tmp, bad) == 0);
    advance(JW_COVER_FAIL_RECHECK_MS);
    assert(take_until_settled(bad) == JW_COVER_READY);
}

static void test_prewarm_failure_is_bounded(void) {
    char bad[PATH_MAX];
    art("prewarm-bad.jpg", "BAD", bad, sizeof(bad));

    for (int i = 0; i < 300; i++) {
        jw_cover_loader_enqueue(&g_loader, bad, "thumb", 100);
        usleep(200);
    }
    wait_idle();
    assert(decodes(bad) == 1);
    void *surface = NULL;
    assert(jw_cover_loader_poll(&g_loader, bad, &surface) == JW_COVER_FAILED);
    assert(jw_cover_loader_is_failed(&g_loader, bad));

    for (int attempt = 2; attempt <= JW_COVER_FAIL_ATTEMPTS; attempt++) {
        advance(JW_COVER_FAIL_RETRY_MS);
        for (int i = 0; i < 100; i++) {
            jw_cover_loader_enqueue(&g_loader, bad, "thumb", 100);
            usleep(200);
        }
        wait_idle();
        assert(decodes(bad) == attempt);
    }
    advance(JW_COVER_FAIL_RETRY_MS * 10);
    for (int i = 0; i < 100; i++) {
        jw_cover_loader_enqueue(&g_loader, bad, "thumb", 100);
    }
    wait_idle();
    assert(decodes(bad) == JW_COVER_FAIL_ATTEMPTS);

    /* Fix the bytes in place with the same size and mtime: the identity does
       not change, so only a rescan lets it load again. */
    struct stat before;
    assert(stat(bad, &before) == 0);
    FILE *f = fopen(bad, "r+b");
    assert(f);
    fputs("OKX", f);
    fclose(f);
    struct timeval times[2] = { { before.st_atime, 0 }, { before.st_mtime, 0 } };
    assert(utimes(bad, times) == 0);
    advance(JW_COVER_FAIL_RECHECK_MS);
    assert(jw_cover_loader_poll(&g_loader, bad, &surface) == JW_COVER_FAILED);
    assert(take_once(bad) == JW_COVER_FAILED);
    wait_idle();
    assert(decodes(bad) == JW_COVER_FAIL_ATTEMPTS);

    jw_cover_loader_forget_failures(&g_loader);
    assert(take_until_settled(bad) == JW_COVER_READY);
    assert(decodes(bad) == JW_COVER_FAIL_ATTEMPTS + 1);
}

static void test_missing_file_fails(void) {
    char missing[PATH_MAX];
    snprintf(missing, sizeof(missing), "%s/never-there.jpg", g_root);
    assert(take_until_settled(missing) == JW_COVER_FAILED);
    failed_frames(missing, 100);
    assert(decodes(missing) == 1);

    /* The art appears at that path later. */
    char created[PATH_MAX];
    art("never-there.jpg", "OK", created, sizeof(created));
    advance(JW_COVER_FAIL_RECHECK_MS);
    assert(take_until_settled(missing) == JW_COVER_READY);
}

static void test_failure_cache_eviction_resets_entry(void) {
    wait_idle();
    jw_cover_loader_forget_failures(&g_loader);
    char path[PATH_MAX], oldest[PATH_MAX];
    snprintf(oldest, sizeof(oldest), "%s/capacity-0.jpg", g_root);
    for (int i = 0; i < JW_COVER_FAIL_MAX; i++) {
        snprintf(path, sizeof(path), "%s/capacity-%d.jpg", g_root, i);
        for (int attempt = 0; attempt < JW_COVER_FAIL_ATTEMPTS; attempt++) {
            jw_cover_loader_record_failure(&g_loader, path);
        }
    }

    snprintf(path, sizeof(path), "%s/capacity-new.jpg", g_root);
    jw_cover_loader_record_failure(&g_loader, path);
    assert(jw_cover_loader_is_failed(&g_loader, path));
    assert(!jw_cover_loader_is_failed(&g_loader, oldest));
    /* The new path gets its own retry budget, not the evicted path's count. */
    advance(JW_COVER_FAIL_RETRY_MS);
    assert(!jw_cover_loader_is_failed(&g_loader, path));
}

static void remove_tree(const char *path) {
    DIR *d = opendir(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
            char child[PATH_MAX];
            snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
            unlink(child);
        }
        closedir(d);
    }
    rmdir(path);
}

int main(void) {
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !tmp[0]) tmp = "/tmp";
    snprintf(g_root, sizeof(g_root), "%s/jawaka-cover-failure.XXXXXX", tmp);
    assert(mkdtemp(g_root));

    jw_cover_loader_ops ops = {
        .decode = fake_decode,
        .free_surface = fake_free,
        .is_persisted = fake_persisted,
        .now_ms = fake_now,
        .default_max_dim = 100,
    };
    jw_cover_loader_init(&g_loader, &ops);
    assert(jw_cover_loader_ensure(&g_loader));

    test_selected_cover_failure_is_bounded();
    test_prewarm_failure_is_bounded();
    test_missing_file_fails();
    test_failure_cache_eviction_resets_entry();

    jw_cover_loader_shutdown(&g_loader);
    assert(atomic_load(&g_live_surfaces) == 0);
    remove_tree(g_root);
    printf("cover-failure-test: ok\n");
    return 0;
}
