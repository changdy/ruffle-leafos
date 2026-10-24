/* flock() (sys/file.h) is a BSD extension, not POSIX; glibc hides it
 * (along with PATH_MAX) under a bare -std=c11 unless a broader feature-
 * test macro is set. Matches the existing convention elsewhere in this
 * codebase (internal/platform/input_proxy_mlp1.c,
 * internal/retroarch/legacy_migration.c). Must precede every #include,
 * including the paired header. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/lease.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

static void jw__lease_set_reason(char *reason, size_t reason_size, const char *slug) {
    if (reason && reason_size > 0) {
        snprintf(reason, reason_size, "%s", slug);
    }
}

/* service_id is used as a single path component (runtime_dir/services/
 * <service_id>/generation.lease). It is expected to already be a
 * validated reverse-DNS id (see internal/services/manifest.c), but this
 * function does not assume that -- it independently refuses anything
 * that could escape the intended directory or resolve to something other
 * than a plain component. */
static bool jw__lease_service_id_is_safe(const char *service_id) {
    if (!service_id || !service_id[0]) {
        return false;
    }
    if (strchr(service_id, '/') != NULL) {
        return false;
    }
    return strcmp(service_id, ".") != 0 && strcmp(service_id, "..") != 0;
}

/* Creates `path` as a directory if it does not exist; if it already
 * exists, succeeds only when it is a real (not symlinked), owner-owned,
 * owner-only directory. The services tree is part of the lease's trust
 * boundary: accepting a writable-by-others directory would allow the
 * stable lease pathname to be replaced, while following a directory
 * symlink would move the lock outside the runtime tree. Never removes,
 * replaces, or silently chmods an existing object; an unsafe pre-existing
 * path fails closed for the caller/environment to repair. */
static bool jw__lease_mkdir_if_missing(const char *path, mode_t mode) {
    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        return false;
    }

    struct stat st;
    return lstat(path, &st) == 0 &&
           S_ISDIR(st.st_mode) &&
           st.st_uid == geteuid() &&
           (st.st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

static bool jw__lease_file_is_safe(const struct stat *st) {
    return S_ISREG(st->st_mode) &&
           st->st_uid == geteuid() &&
           (st->st_mode & (S_IRWXG | S_IRWXO)) == 0;
}

int jw_svc_lease_acquire(const char *runtime_dir, const char *service_id,
                         char *reason, size_t reason_size) {
    if (!jw__lease_service_id_is_safe(service_id)) {
        jw__lease_set_reason(reason, reason_size, "invalid-service-id");
        return -1;
    }
    if (!runtime_dir || !runtime_dir[0]) {
        jw__lease_set_reason(reason, reason_size, "runtime-dir-unavailable");
        return -1;
    }

    struct stat root_st;
    if (stat(runtime_dir, &root_st) != 0 || !S_ISDIR(root_st.st_mode)) {
        jw__lease_set_reason(reason, reason_size, "runtime-dir-unavailable");
        return -1;
    }

    char services_dir[PATH_MAX];
    int n = snprintf(services_dir, sizeof(services_dir), "%s/services", runtime_dir);
    if (n < 0 || (size_t)n >= sizeof(services_dir)) {
        jw__lease_set_reason(reason, reason_size, "path-too-long");
        return -1;
    }
    if (!jw__lease_mkdir_if_missing(services_dir, 0700)) {
        jw__lease_set_reason(reason, reason_size, "mkdir-failed");
        return -1;
    }

    char service_dir[PATH_MAX];
    n = snprintf(service_dir, sizeof(service_dir), "%s/%s", services_dir, service_id);
    if (n < 0 || (size_t)n >= sizeof(service_dir)) {
        jw__lease_set_reason(reason, reason_size, "path-too-long");
        return -1;
    }
    if (!jw__lease_mkdir_if_missing(service_dir, 0700)) {
        jw__lease_set_reason(reason, reason_size, "mkdir-failed");
        return -1;
    }

    char lease_path[PATH_MAX];
    n = snprintf(lease_path, sizeof(lease_path), "%s/generation.lease", service_dir);
    if (n < 0 || (size_t)n >= sizeof(lease_path)) {
        jw__lease_set_reason(reason, reason_size, "path-too-long");
        return -1;
    }

    /* Refuse an unsafe existing object before open() so, for example, a
     * FIFO or device is not opened for I/O. O_NOFOLLOW and the post-open
     * fstat() close the symlink/type race around this advisory check. */
    struct stat lease_st;
    if (lstat(lease_path, &lease_st) == 0) {
        if (!jw__lease_file_is_safe(&lease_st)) {
            jw__lease_set_reason(reason, reason_size, "open-failed");
            return -1;
        }
    } else if (errno != ENOENT) {
        jw__lease_set_reason(reason, reason_size, "open-failed");
        return -1;
    }

    /* O_CREAT without O_TRUNC and without O_EXCL: create it if absent,
     * but an EXISTING lease file's inode identity and any lock a still-
     * running old generation holds on it must survive completely
     * untouched by this open() call -- that is the entire no-overlap
     * mechanism this function exists to implement. O_NOFOLLOW prevents a
     * same-named symlink from redirecting the stable lock to another
     * inode. */
    /* The daemon may supervise several services concurrently. Its own copy
     * must never leak through an unrelated service's exec; launch.c makes a
     * deliberate duplicate of only the selected lease onto descriptor 3 and
     * clears CLOEXEC there. Without this flag, service B inherits service A's
     * lock and can keep A falsely stale after A's group is gone. */
    int fd = open(lease_path,
                  O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        jw__lease_set_reason(reason, reason_size, "open-failed");
        return -1;
    }
    if (fstat(fd, &lease_st) != 0 || !jw__lease_file_is_safe(&lease_st)) {
        close(fd);
        jw__lease_set_reason(reason, reason_size, "open-failed");
        return -1;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int saved_errno = errno;
        close(fd);
        if (saved_errno == EWOULDBLOCK) {
            jw__lease_set_reason(reason, reason_size, "stale-generation");
        } else {
            jw__lease_set_reason(reason, reason_size, "lock-failed");
        }
        return -1;
    }

    return fd;
}
