#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/launcher/active_game.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#define JW_ACTIVE_GAME_FILE_MAX (64u * 1024u)

static atomic_uint_fast64_t jw__active_game_counter = 1;

static void jw__reason(char *out, size_t out_size, const char *value) {
    if (out && out_size > 0) {
        snprintf(out, out_size, "%s", value ? value : "unknown");
    }
}

static bool jw__path(char *out, size_t out_size, const char *runtime_dir,
                     const char *leaf) {
    if (!out || out_size == 0 || !runtime_dir || !runtime_dir[0] ||
        !leaf || !leaf[0]) {
        return false;
    }
    int n = snprintf(out, out_size, "%s/%s", runtime_dir, leaf);
    return n >= 0 && (size_t)n < out_size;
}

static bool jw__write_all(int fd, const char *data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n > 0) {
            written += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

static bool jw__fsync_dir(const char *runtime_dir) {
    int fd = open(runtime_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    int rc;
    do {
        rc = fsync(fd);
    } while (rc != 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    errno = saved;
    return rc == 0;
}

static int jw__object_size(const cJSON *object) {
    int count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, object) {
        if (count == INT_MAX) {
            return -1;
        }
        count++;
    }
    return count;
}

static bool jw__copy_string(const cJSON *object, const char *name,
                            char *out, size_t out_size, bool absolute) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || !item->valuestring ||
        !item->valuestring[0] ||
        (absolute && item->valuestring[0] != '/')) {
        return false;
    }
    size_t len = strlen(item->valuestring);
    if (len >= out_size) {
        return false;
    }
    memcpy(out, item->valuestring, len + 1u);
    return true;
}

static bool jw__record_valid(const jw_active_game *record) {
    return record && record->active && !record->uncertain &&
           record->launch_id[0] && record->source_id[0] &&
           record->saves_path[0] == '/' && record->states_path[0] == '/' &&
           strlen(record->launch_id) <= JW_ACTIVE_GAME_LAUNCH_ID_MAX &&
           strlen(record->source_id) <= JW_ACTIVE_GAME_SOURCE_ID_MAX &&
           strlen(record->saves_path) < sizeof(record->saves_path) &&
           strlen(record->states_path) < sizeof(record->states_path);
}

bool jw_active_game_persist(const char *runtime_dir,
                            const jw_active_game *record,
                            char *reason, size_t reason_size) {
    jw__reason(reason, reason_size, "invalid-arguments");
    if (!runtime_dir || !runtime_dir[0] || !jw__record_valid(record)) {
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root ||
        !cJSON_AddStringToObject(root, "launch_id", record->launch_id) ||
        !cJSON_AddStringToObject(root, "source_id", record->source_id) ||
        !cJSON_AddStringToObject(root, "saves_path", record->saves_path) ||
        !cJSON_AddStringToObject(root, "states_path", record->states_path)) {
        cJSON_Delete(root);
        jw__reason(reason, reason_size, "encode-failed");
        return false;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        jw__reason(reason, reason_size, "encode-failed");
        return false;
    }

    char final_path[JW_ACTIVE_GAME_PATH_MAX];
    char temp_leaf[160];
    uint64_t serial = atomic_fetch_add(&jw__active_game_counter, 1);
    int leaf_len = snprintf(temp_leaf, sizeof(temp_leaf),
                            ".%s.tmp.%ld.%llu", JW_ACTIVE_GAME_FILENAME,
                            (long)getpid(), (unsigned long long)serial);
    char temp_path[JW_ACTIVE_GAME_PATH_MAX];
    if (leaf_len < 0 || (size_t)leaf_len >= sizeof(temp_leaf) ||
        !jw__path(final_path, sizeof(final_path), runtime_dir,
                  JW_ACTIVE_GAME_FILENAME) ||
        !jw__path(temp_path, sizeof(temp_path), runtime_dir, temp_leaf)) {
        cJSON_free(json);
        jw__reason(reason, reason_size, "path-too-long");
        return false;
    }

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        cJSON_free(json);
        jw__reason(reason, reason_size, "temp-open-failed");
        return false;
    }
    size_t len = strlen(json);
    bool ok = jw__write_all(fd, json, len);
    if (ok) {
        int rc;
        do {
            rc = fsync(fd);
        } while (rc != 0 && errno == EINTR);
        ok = rc == 0;
    }
    int saved = errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        saved = errno;
    }
    cJSON_free(json);
    if (!ok) {
        unlink(temp_path);
        errno = saved;
        jw__reason(reason, reason_size, "write-failed");
        return false;
    }
    if (rename(temp_path, final_path) != 0) {
        saved = errno;
        unlink(temp_path);
        errno = saved;
        jw__reason(reason, reason_size, "rename-failed");
        return false;
    }
    if (!jw__fsync_dir(runtime_dir)) {
        jw__reason(reason, reason_size, "directory-fsync-failed");
        return false;
    }
    jw__reason(reason, reason_size, "ok");
    return true;
}

jw_active_game_load_result jw_active_game_load(
    const char *runtime_dir, jw_active_game *out,
    char *reason, size_t reason_size) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    jw__reason(reason, reason_size, "invalid-arguments");
    if (!runtime_dir || !runtime_dir[0] || !out) {
        return JW_ACTIVE_GAME_LOAD_ERROR;
    }
    char path[JW_ACTIVE_GAME_PATH_MAX];
    if (!jw__path(path, sizeof(path), runtime_dir, JW_ACTIVE_GAME_FILENAME)) {
        jw__reason(reason, reason_size, "path-too-long");
        return JW_ACTIVE_GAME_LOAD_ERROR;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ENOENT) {
            jw__reason(reason, reason_size, "absent");
            return JW_ACTIVE_GAME_LOAD_ABSENT;
        }
        out->active = out->recovered = out->uncertain = true;
        jw__reason(reason, reason_size, "open-failed");
        return JW_ACTIVE_GAME_LOAD_ERROR;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uintmax_t)st.st_size > JW_ACTIVE_GAME_FILE_MAX) {
        close(fd);
        out->active = out->recovered = out->uncertain = true;
        jw__reason(reason, reason_size, "invalid-record");
        return JW_ACTIVE_GAME_LOAD_UNCERTAIN;
    }
    size_t len = (size_t)st.st_size;
    char *json = malloc(len + 1u);
    if (!json) {
        close(fd);
        out->active = out->recovered = out->uncertain = true;
        jw__reason(reason, reason_size, "out-of-memory");
        return JW_ACTIVE_GAME_LOAD_ERROR;
    }
    size_t read_len = 0;
    while (read_len < len) {
        ssize_t n = read(fd, json + read_len, len - read_len);
        if (n > 0) {
            read_len += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(fd);
    if (read_len != len || memchr(json, '\0', len) != NULL) {
        free(json);
        out->active = out->recovered = out->uncertain = true;
        jw__reason(reason, reason_size, "read-failed");
        return JW_ACTIVE_GAME_LOAD_UNCERTAIN;
    }
    json[len] = '\0';
    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    bool valid = root && cJSON_IsObject(root) && end == json + len &&
                 jw__object_size(root) == 4 &&
                 jw__copy_string(root, "launch_id", out->launch_id,
                                 sizeof(out->launch_id), false) &&
                 jw__copy_string(root, "source_id", out->source_id,
                                 sizeof(out->source_id), false) &&
                 jw__copy_string(root, "saves_path", out->saves_path,
                                 sizeof(out->saves_path), true) &&
                 jw__copy_string(root, "states_path", out->states_path,
                                 sizeof(out->states_path), true);
    cJSON_Delete(root);
    free(json);
    out->active = true;
    out->recovered = true;
    if (!valid) {
        memset(out->launch_id, 0, sizeof(out->launch_id));
        memset(out->source_id, 0, sizeof(out->source_id));
        memset(out->saves_path, 0, sizeof(out->saves_path));
        memset(out->states_path, 0, sizeof(out->states_path));
        out->uncertain = true;
        jw__reason(reason, reason_size, "invalid-record");
        return JW_ACTIVE_GAME_LOAD_UNCERTAIN;
    }
    jw__reason(reason, reason_size, "ok");
    return JW_ACTIVE_GAME_LOAD_VALID;
}

bool jw_active_game_clear(const char *runtime_dir,
                          char *reason, size_t reason_size) {
    jw__reason(reason, reason_size, "invalid-arguments");
    char path[JW_ACTIVE_GAME_PATH_MAX];
    if (!runtime_dir || !runtime_dir[0] ||
        !jw__path(path, sizeof(path), runtime_dir, JW_ACTIVE_GAME_FILENAME)) {
        return false;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        jw__reason(reason, reason_size, "unlink-failed");
        return false;
    }
    if (!jw__fsync_dir(runtime_dir)) {
        jw__reason(reason, reason_size, "directory-fsync-failed");
        return false;
    }
    jw__reason(reason, reason_size, "ok");
    return true;
}

bool jw_active_game_generate_id(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return false;
    }
    struct timespec wall;
    struct timespec mono;
    if (clock_gettime(CLOCK_REALTIME, &wall) != 0 ||
        clock_gettime(CLOCK_MONOTONIC, &mono) != 0) {
        out[0] = '\0';
        return false;
    }
    uint64_t serial = atomic_fetch_add(&jw__active_game_counter, 1);
    int n = snprintf(out, out_size, "%lld-%09ld-%ld-%llu-%lld",
                     (long long)wall.tv_sec, wall.tv_nsec, (long)getpid(),
                     (unsigned long long)serial,
                     (long long)mono.tv_nsec);
    return n > 0 && (size_t)n < out_size;
}
