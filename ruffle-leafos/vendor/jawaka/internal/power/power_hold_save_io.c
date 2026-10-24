#include "internal/power/power_hold_save_io.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool jw_power_hold_save_tmp_path(const char *final_path, const char *request_id,
                                 char *out, size_t out_size) {
    if (!final_path || !final_path[0] || !request_id || !request_id[0] ||
        !out || out_size == 0) {
        return false;
    }
    int n = snprintf(out, out_size, "%s.tmp-%s", final_path, request_id);
    return n > 0 && (size_t)n < out_size;
}

static void jw__phs_error(char *error, size_t error_size, const char *what,
                          const char *path, int err) {
    if (error && error_size > 0) {
        snprintf(error, error_size, "%s %s: %s", what, path ? path : "(null)",
                 strerror(err));
    }
}

static int jw__phs_sync_parent(const char *path) {
    char dir[4096];
    int n = snprintf(dir, sizeof(dir), "%s", path);
    if (n <= 0 || (size_t)n >= sizeof(dir)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char *slash = strrchr(dir, '/');
    if (!slash) {
        snprintf(dir, sizeof(dir), ".");
    } else if (slash == dir) {
        dir[1] = '\0';
    } else {
        *slash = '\0';
    }
    int fd = open(dir, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    int rc = fsync(fd);
    int saved = errno;
    close(fd);
    errno = saved;
    return rc;
}

jw_phs_publish_result jw_power_hold_save_publish(const char *expected_tmp,
                                                 const char *reported_tmp,
                                                 const char *final_path,
                                                 const char *thumb_path,
                                                 char *error, size_t error_size) {
    if (error && error_size > 0) {
        error[0] = '\0';
    }
    if (!expected_tmp || !expected_tmp[0] || !reported_tmp || !final_path ||
        !final_path[0] || strcmp(expected_tmp, reported_tmp) != 0) {
        if (error && error_size > 0) {
            snprintf(error, error_size, "reply path %s is not the pinned %s",
                     reported_tmp ? reported_tmp : "(null)",
                     expected_tmp ? expected_tmp : "(null)");
        }
        return JW_PHS_PUBLISH_REJECTED;
    }
    struct stat st;
    if (lstat(expected_tmp, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        jw__phs_error(error, error_size, "temporary state unusable",
                      expected_tmp, errno ? errno : EINVAL);
        return JW_PHS_PUBLISH_REJECTED;
    }
    if (thumb_path && thumb_path[0] && unlink(thumb_path) != 0 && errno != ENOENT) {
        jw__phs_error(error, error_size, "could not remove old thumbnail",
                      thumb_path, errno);
        return JW_PHS_PUBLISH_THUMB_ERROR;
    }
    if (rename(expected_tmp, final_path) != 0) {
        jw__phs_error(error, error_size, "could not publish", final_path, errno);
        return JW_PHS_PUBLISH_RENAME_ERROR;
    }
    if (jw__phs_sync_parent(final_path) != 0) {
        jw__phs_error(error, error_size, "directory flush failed after publishing",
                      final_path, errno);
        return JW_PHS_PUBLISH_DIR_SYNC_ERROR;
    }
    return JW_PHS_PUBLISH_OK;
}

int jw_power_hold_save_remove_tmp(const char *expected_tmp) {
    if (!expected_tmp || !expected_tmp[0]) {
        return 0;
    }
    if (unlink(expected_tmp) != 0 && errno != ENOENT) {
        return -1;
    }
    return 0;
}

/* "<base>.tmp-<id>" with <id> 1-32 of [A-Za-z0-9_-], the request-id alphabet
   RetroArch accepts. */
static bool jw__phs_is_orphan_name(const char *name, const char *base, size_t base_len) {
    static const char suffix[] = ".tmp-";
    if (strncmp(name, base, base_len) != 0 ||
        strncmp(name + base_len, suffix, sizeof(suffix) - 1) != 0) {
        return false;
    }
    const char *id = name + base_len + sizeof(suffix) - 1;
    size_t id_len = strlen(id);
    if (id_len < 1 || id_len > 32) {
        return false;
    }
    for (size_t i = 0; i < id_len; i++) {
        unsigned char c = (unsigned char)id[i];
        if (!isalnum(c) && c != '_' && c != '-') {
            return false;
        }
    }
    return true;
}

int jw_power_hold_save_remove_orphans(const char *final_path) {
    if (!final_path || !final_path[0]) {
        return -1;
    }
    char dir[4096];
    if (snprintf(dir, sizeof(dir), "%s", final_path) >= (int)sizeof(dir)) {
        return -1;
    }
    const char *base;
    char *slash = strrchr(dir, '/');
    if (!slash) {
        base = final_path;
        snprintf(dir, sizeof(dir), ".");
    } else {
        base = final_path + (slash - dir) + 1;
        if (slash == dir) {
            dir[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
    size_t base_len = strlen(base);
    if (base_len == 0) {
        return -1;
    }
    DIR *dp = opendir(dir);
    if (!dp) {
        return -1;
    }
    int removed = 0;
    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL) {
        if (!jw__phs_is_orphan_name(entry->d_name, base, base_len)) {
            continue;
        }
        char path[4096];
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= (int)sizeof(path)) {
            continue;
        }
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;   /* a symlink or directory is not our leftover */
        }
        if (unlink(path) == 0) {
            removed++;
        }
    }
    closedir(dp);
    return removed;
}
