/* clock_gettime()/CLOCK_MONOTONIC, openat(), and O_NOFOLLOW need
 * broader-than-bare-C11 visibility on glibc. Must precede every
 * #include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/reservation.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define JW_RES_MAX_RECORD_BYTES 4096U
#define JW_RES_MAX_SAFE_JSON_INTEGER 9007199254740991LL
#define JW_RES_TEMP_ATTEMPTS 128U

static void jw__res_set_reason(char *reason, size_t reason_size, const char *slug) {
    if (reason && reason_size > 0) {
        snprintf(reason, reason_size, "%s", slug);
    }
}

/* service_id is used as a single path component. Independently checked
 * here rather than trusted from a caller, matching the same posture
 * internal/services/lease.c takes for the same reason. */
static bool jw__res_service_id_is_safe(const char *service_id) {
    if (!service_id || !service_id[0]) {
        return false;
    }
    if (strchr(service_id, '/') != NULL) {
        return false;
    }
    return strcmp(service_id, ".") != 0 && strcmp(service_id, "..") != 0;
}

static const char *jw__res_game_policy_to_str(jw_svc_reservation_game_policy policy) {
    switch (policy) {
        case JW_SVC_RESERVATION_GAME_IGNORE:
            return "ignore";
        case JW_SVC_RESERVATION_GAME_STOP:
            return "stop";
        case JW_SVC_RESERVATION_GAME_NOTIFY:
            return "notify";
        default:
            return NULL;
    }
}

static bool jw__res_game_policy_from_str(const char *s, jw_svc_reservation_game_policy *out) {
    if (!s || !out) {
        return false;
    }
    if (strcmp(s, "ignore") == 0) {
        *out = JW_SVC_RESERVATION_GAME_IGNORE;
        return true;
    }
    if (strcmp(s, "stop") == 0) {
        *out = JW_SVC_RESERVATION_GAME_STOP;
        return true;
    }
    if (strcmp(s, "notify") == 0) {
        *out = JW_SVC_RESERVATION_GAME_NOTIFY;
        return true;
    }
    return false;
}

long long jw_svc_reservation_now_us(void) {
    struct timespec ts = {0, 0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 ||
        ts.tv_sec < 0 ||
        ts.tv_nsec < 0 ||
        ts.tv_nsec >= 1000000000L ||
        (long long)ts.tv_sec >
            (JW_RES_MAX_SAFE_JSON_INTEGER / 1000000LL)) {
        return -1;
    }
    long long instant = (long long)ts.tv_sec * 1000000LL +
                        (long long)ts.tv_nsec / 1000LL;
    return instant <= JW_RES_MAX_SAFE_JSON_INTEGER ? instant : -1;
}

/* Construct the public record path up front so callers receive the
 * documented path-too-long result consistently. The actual I/O below is
 * directory-relative, which also closes intermediate-component symlink
 * and service-directory replacement races. */
static bool jw__res_paths(const char *runtime_dir, const char *service_id,
                          char *dir_out, size_t dir_out_size) {
    int n = snprintf(dir_out, dir_out_size, "%s/services/%s", runtime_dir, service_id);
    if (n < 0 || (size_t)n >= dir_out_size) {
        return false;
    }

    char checked_path[PATH_MAX];
    n = snprintf(checked_path, sizeof(checked_path), "%s/reservation", dir_out);
    return n >= 0 && (size_t)n < sizeof(checked_path);
}

/* Opens each directory component independently. In particular, O_NOFOLLOW
 * applies to both "services" and the service id; applying it only to the
 * final full path would still follow a symlink in an intermediate
 * component. The runtime root itself is caller-selected and may be a
 * platform-provided symlink. */
static int jw__res_open_service_dir(const char *runtime_dir, const char *service_id,
                                    bool *missing_out) {
    *missing_out = false;

    int runtime_fd = open(runtime_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (runtime_fd < 0) {
        *missing_out = errno == ENOENT;
        return -1;
    }

    int services_fd = openat(runtime_fd, "services",
                             O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int services_errno = errno;
    (void)close(runtime_fd);
    if (services_fd < 0) {
        *missing_out = services_errno == ENOENT;
        errno = services_errno;
        return -1;
    }

    int service_fd = openat(services_fd, service_id,
                            O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    int service_errno = errno;
    (void)close(services_fd);
    if (service_fd < 0) {
        *missing_out = service_errno == ENOENT;
        errno = service_errno;
        return -1;
    }

    struct stat st;
    if (fstat(service_fd, &st) != 0 || !S_ISDIR(st.st_mode)) {
        int saved_errno = errno;
        (void)close(service_fd);
        errno = saved_errno;
        return -1;
    }
    return service_fd;
}

static int jw__res_fsync(int fd) {
    int rc;
    do {
        rc = fsync(fd);
    } while (rc != 0 && errno == EINTR);
    return rc;
}

/* The atomic-write recipe contracts.md's LIFE-1 section specifies for its
 * own active-launch record, reused here: a unique same-directory
 * temporary, complete write, file fsync, rename, then parent-directory
 * fsync. Directory-relative operations keep the whole sequence pinned to
 * the directory that was safely opened above. */
static bool jw__res_write_atomic(int dir_fd, const char *content,
                                 char *reason, size_t reason_size) {
    char temp_name[96];
    int fd = -1;

    for (unsigned int attempt = 0; attempt < JW_RES_TEMP_ATTEMPTS; attempt++) {
        int n = snprintf(temp_name, sizeof(temp_name), ".reservation.tmp.%ld.%u",
                         (long)getpid(), attempt);
        if (n < 0 || (size_t)n >= sizeof(temp_name)) {
            jw__res_set_reason(reason, reason_size, "write-failed");
            return false;
        }

        fd = openat(dir_fd, temp_name,
                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                    0600);
        if (fd >= 0) {
            break;
        }
        if (errno != EEXIST) {
            jw__res_set_reason(reason, reason_size, "write-failed");
            return false;
        }
    }
    if (fd < 0) {
        jw__res_set_reason(reason, reason_size, "write-failed");
        return false;
    }

    bool ok = true;
    size_t len = strlen(content);
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, content + written, len - written);
        if (n > 0) {
            written += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else {
            ok = false;
            break;
        }
    }

    if (ok && jw__res_fsync(fd) != 0) {
        ok = false;
    }
    if (close(fd) != 0) {
        ok = false;
    }

    if (ok && renameat(dir_fd, temp_name, dir_fd, "reservation") != 0) {
        ok = false;
    }
    if (!ok) {
        (void)unlinkat(dir_fd, temp_name, 0);
        jw__res_set_reason(reason, reason_size, "write-failed");
        return false;
    }

    if (jw__res_fsync(dir_fd) != 0) {
        jw__res_set_reason(reason, reason_size, "write-failed");
        return false;
    }
    return true;
}

static bool jw__res_pgid_from_json(const cJSON *item, pid_t *out) {
    if (!cJSON_IsNumber(item) ||
        !isfinite(item->valuedouble) ||
        item->valuedouble < 1.0 ||
        item->valuedouble > (double)INT_MAX) {
        return false;
    }

    int value = (int)item->valuedouble;
    if ((double)value != item->valuedouble) {
        return false;
    }
    *out = (pid_t)value;
    return true;
}

static bool jw__res_instant_from_json(const cJSON *item, long long *out) {
    if (!cJSON_IsNumber(item) ||
        !isfinite(item->valuedouble) ||
        item->valuedouble < 0.0 ||
        item->valuedouble > (double)JW_RES_MAX_SAFE_JSON_INTEGER) {
        return false;
    }

    long long value = (long long)item->valuedouble;
    if ((double)value != item->valuedouble) {
        return false;
    }
    *out = value;
    return true;
}

bool jw_svc_reservation_write(const char *runtime_dir, const char *service_id,
                              const jw_svc_reservation *reservation,
                              char *reason, size_t reason_size) {
    const char *game_policy = reservation
        ? jw__res_game_policy_to_str(reservation->game_policy)
        : NULL;
    /* No upper bound against INT_MAX here: pid_t is `int` on every
     * platform this builds for (macOS, Linux, and MLP1's aarch64-linux
     * cross target), so reservation->pgid can never exceed INT_MAX --
     * gcc's -Wtype-limits correctly flags that comparison as tautological
     * (always false) under -Werror on Linux, where clang stays silent. */
    if (!runtime_dir || !runtime_dir[0] || !jw__res_service_id_is_safe(service_id) ||
        !reservation || reservation->pgid <= 0 ||
        reservation->launch_instant_us < 0 ||
        reservation->launch_instant_us > JW_RES_MAX_SAFE_JSON_INTEGER ||
        !game_policy) {
        jw__res_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    char dir[PATH_MAX];
    if (!jw__res_paths(runtime_dir, service_id, dir, sizeof(dir))) {
        jw__res_set_reason(reason, reason_size, "path-too-long");
        return false;
    }

    bool missing = false;
    int dir_fd = jw__res_open_service_dir(runtime_dir, service_id, &missing);
    if (dir_fd < 0) {
        (void)missing;
        jw__res_set_reason(reason, reason_size, "service-dir-unavailable");
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        (void)close(dir_fd);
        jw__res_set_reason(reason, reason_size, "out-of-memory");
        return false;
    }

    /* cJSON's ordinary number printer accepts a 15-digit rendering when
     * it is within one DBL_EPSILON of the input, which is weaker than an
     * exact integer round trip near 2^53. Add the instant as raw decimal
     * JSON so every accepted microsecond value is emitted losslessly. */
    char instant_json[32];
    int instant_len = snprintf(instant_json, sizeof(instant_json), "%lld",
                               reservation->launch_instant_us);
    bool json_ok =
        instant_len > 0 && (size_t)instant_len < sizeof(instant_json) &&
        cJSON_AddNumberToObject(root, "pgid", (double)reservation->pgid) != NULL &&
        cJSON_AddRawToObject(root, "launch_instant_us", instant_json) != NULL &&
        cJSON_AddStringToObject(root, "game", game_policy) != NULL &&
        cJSON_AddBoolToObject(root, "stop_on_storage_change",
                              reservation->stop_on_storage_change) != NULL &&
        cJSON_AddBoolToObject(root, "stop_on_suspend",
                              reservation->stop_on_suspend) != NULL;
    if (!json_ok) {
        cJSON_Delete(root);
        (void)close(dir_fd);
        jw__res_set_reason(reason, reason_size, "out-of-memory");
        return false;
    }

    char *json_text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json_text) {
        (void)close(dir_fd);
        jw__res_set_reason(reason, reason_size, "out-of-memory");
        return false;
    }

    bool ok = jw__res_write_atomic(dir_fd, json_text, reason, reason_size);
    cJSON_free(json_text);
    if (close(dir_fd) != 0 && ok) {
        jw__res_set_reason(reason, reason_size, "write-failed");
        return false;
    }
    return ok;
}

bool jw_svc_reservation_read(const char *runtime_dir, const char *service_id,
                             jw_svc_reservation *out,
                             char *reason, size_t reason_size) {
    if (!out) {
        jw__res_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (!runtime_dir || !runtime_dir[0] || !jw__res_service_id_is_safe(service_id)) {
        jw__res_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }

    char dir[PATH_MAX];
    if (!jw__res_paths(runtime_dir, service_id, dir, sizeof(dir))) {
        jw__res_set_reason(reason, reason_size, "path-too-long");
        return false;
    }

    bool service_dir_missing = false;
    int dir_fd = jw__res_open_service_dir(runtime_dir, service_id, &service_dir_missing);
    if (dir_fd < 0) {
        jw__res_set_reason(reason, reason_size,
                           service_dir_missing ? "reservation-missing"
                                               : "reservation-corrupt");
        return false;
    }

    int fd = openat(dir_fd, "reservation",
                    O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    int open_errno = errno;
    (void)close(dir_fd);
    if (fd < 0) {
        jw__res_set_reason(reason, reason_size,
                           open_errno == ENOENT ? "reservation-missing"
                                                : "reservation-corrupt");
        return false;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        jw__res_set_reason(reason, reason_size, "reservation-corrupt");
        return false;
    }

    char buf[JW_RES_MAX_RECORD_BYTES + 1U];
    size_t used = 0;
    bool read_failed = false;
    while (used < sizeof(buf)) {
        ssize_t n = read(fd, buf + used, sizeof(buf) - used);
        if (n > 0) {
            used += (size_t)n;
        } else if (n == 0) {
            break;
        } else if (errno != EINTR) {
            read_failed = true;
            break;
        }
    }
    if (close(fd) != 0) {
        read_failed = true;
    }
    if (read_failed || used == 0 || used > JW_RES_MAX_RECORD_BYTES ||
        memchr(buf, '\0', used) != NULL) {
        jw__res_set_reason(reason, reason_size, "reservation-corrupt");
        return false;
    }
    buf[used] = '\0';

    /* cJSON stores decoded strings as NUL-terminated C strings without a
     * separate length. Reject an escaped NUL before parsing so a key such
     * as "pgid\u0000suffix" or policy "ignore\u0000suffix" cannot compare
     * equal to its trusted prefix. Generated records never need NULs. */
    if (strstr(buf, "\\u0000") != NULL) {
        jw__res_set_reason(reason, reason_size, "reservation-corrupt");
        return false;
    }

    cJSON *root = cJSON_ParseWithLengthOpts(buf, used + 1U, NULL, true);
    if (!root) {
        jw__res_set_reason(reason, reason_size, "reservation-corrupt");
        return false;
    }

    cJSON *pgid_item = cJSON_GetObjectItemCaseSensitive(root, "pgid");
    cJSON *instant_item = cJSON_GetObjectItemCaseSensitive(root, "launch_instant_us");
    cJSON *game_item = cJSON_GetObjectItemCaseSensitive(root, "game");
    cJSON *storage_item = cJSON_GetObjectItemCaseSensitive(root, "stop_on_storage_change");
    cJSON *suspend_item = cJSON_GetObjectItemCaseSensitive(root, "stop_on_suspend");

    pid_t pgid = 0;
    long long instant = 0;
    jw_svc_reservation_game_policy policy = JW_SVC_RESERVATION_GAME_IGNORE;
    bool shape_ok =
        cJSON_IsObject(root) &&
        cJSON_GetArraySize(root) == 5 &&
        jw__res_pgid_from_json(pgid_item, &pgid) &&
        jw__res_instant_from_json(instant_item, &instant) &&
        cJSON_IsString(game_item) &&
        game_item->valuestring &&
        jw__res_game_policy_from_str(game_item->valuestring, &policy) &&
        cJSON_IsBool(storage_item) &&
        cJSON_IsBool(suspend_item);
    if (!shape_ok) {
        cJSON_Delete(root);
        jw__res_set_reason(reason, reason_size, "reservation-corrupt");
        return false;
    }

    out->pgid = pgid;
    out->launch_instant_us = instant;
    out->game_policy = policy;
    out->stop_on_storage_change = cJSON_IsTrue(storage_item);
    out->stop_on_suspend = cJSON_IsTrue(suspend_item);
    cJSON_Delete(root);
    return true;
}
