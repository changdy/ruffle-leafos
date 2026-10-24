#ifndef JW_POWER_REQUEST_H
#define JW_POWER_REQUEST_H

#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The rootfs supervisor creates a private tmpfs directory for each daemon
   generation. No capability means no safe shutdown implementation is installed. */
static inline int jw_power_request_dir(void) {
    const char *path = getenv("UMRK_POWER_REQUEST_DIR");
    if (!path || path[0] != '/') return -1;
    int dir = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return -1;
    struct stat st;
    int ready = -1;
    char version[8] = {0};
    bool valid = fstat(dir, &st) == 0 && st.st_uid == geteuid() &&
                 (st.st_mode & 0777) == 0700;
    if (valid) {
        ready = openat(dir, "ready", O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        valid = ready >= 0 && read(ready, version, sizeof(version)) == 2 &&
                memcmp(version, "1\n", 2) == 0;
    }
    if (ready >= 0) close(ready);
    if (!valid) { close(dir); return -1; }
    return dir;
}

static inline bool jw_power_request_available(void) {
    int dir = jw_power_request_dir();
    if (dir < 0) return false;
    close(dir);
    return true;
}

static inline int jw_power_request_write(const char *name, const char *value) {
    int dir = jw_power_request_dir();
    if (dir < 0) return -1;
    char temp[64];
    snprintf(temp, sizeof(temp), "%s.tmp.%ld", name, (long)getpid());
    int fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    int rc = -1;
    if (fd >= 0) {
        size_t len = strlen(value);
        bool written = write(fd, value, len) == (ssize_t)len && fsync(fd) == 0;
        if (close(fd) != 0) written = false;
        if (written && renameat(dir, temp, dir, name) == 0) rc = 0;
        (void)unlinkat(dir, temp, 0);
    }
    close(dir);
    return rc;
}

static inline int jw_power_request_publish(const char *action) {
    if (!action || (strcmp(action, "reboot") != 0 && strcmp(action, "poweroff") != 0))
        return -1;
    return jw_power_request_write("request", action);
}

static inline int jw_power_request_complete(void) {
    return jw_power_request_write("complete", "1\n");
}

/* Why the transition was requested. Informational for the supervisor's log and
   screens; it never changes what the supervisor is allowed to do. */
static inline bool jw_power_request_reason_valid(const char *reason) {
    static const char *const allowed[] = {
        "menu", "power-button", "low-battery", "storage-check", NULL,
    };
    if (!reason) return false;
    for (int i = 0; allowed[i]; i++) {
        if (strcmp(reason, allowed[i]) == 0) return true;
    }
    return false;
}

static inline int jw_power_request_reason(const char *reason) {
    if (!jw_power_request_reason_valid(reason)) return -1;
    char line[32];
    snprintf(line, sizeof(line), "%s\n", reason);
    return jw_power_request_write("reason", line);
}

/* Published instead of `complete` when shutdown could not prove every writer
   gone (SVC-1 unverified-stop table). The first line is
   reason=unverified-service|game-child|cleanup-failed, then one line per item:
   "service=<id> pgid=<n> lease=<path>" or "game pid=<n> start=<ticks>". The
   supervisor never signals these; it only checks whether they are gone. */
static inline int jw_power_request_incomplete(const char *record) {
    if (!record || strncmp(record, "reason=", 7) != 0) return -1;
    return jw_power_request_write("incomplete", record);
}
#endif
