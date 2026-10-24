#include "internal/core/log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <unistd.h>

#define JW__LOG_FALLBACK_MAX_BYTES (2L * 1024L * 1024L)

static bool jw__log_fd_read_only(int fd) {
    struct statvfs vfs;
    return fstatvfs(fd, &vfs) == 0 && (vfs.f_flag & ST_RDONLY) != 0;
}

/* Opens <fallback_dir>/<name>.log for append, capped at 2 MB with one
   rotation. Returns the descriptor, or -1 when the fallback is unusable. */
static int jw__log_open_fallback(const char *fallback_dir, const char *name) {
    if (!fallback_dir || !fallback_dir[0] || !name || !name[0]) {
        return -1;
    }
    char path[512];
    char rotated[520];
    if (snprintf(path, sizeof(path), "%s/%s.log", fallback_dir, name) >= (int)sizeof(path)) {
        return -1;
    }
    snprintf(rotated, sizeof(rotated), "%s.1", path);
    (void)mkdir(fallback_dir, 0755);
    struct stat st;
    if (stat(path, &st) == 0 && st.st_size > JW__LOG_FALLBACK_MAX_BYTES) {
        (void)rename(path, rotated);
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd < 0) {
        return -1;
    }
    if (jw__log_fd_read_only(fd)) {
        close(fd);
        return -1;
    }
    return fd;
}

int jw_log_redirect_if_read_only(const char *fallback_dir, const char *name) {
    if (!fallback_dir || !fallback_dir[0] || !name || !name[0] ||
        !jw__log_fd_read_only(STDERR_FILENO)) {
        return 0;
    }
    int fd = jw__log_open_fallback(fallback_dir, name);
    if (fd < 0) {
        return -1;
    }
    fflush(stdout);
    fflush(stderr);
    int rc = (dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) ? -1 : 1;
    close(fd);
    clearerr(stdout);
    clearerr(stderr);
    return rc;
}

/* A real byte, not a zero-length write: a 0-byte write can succeed without
   touching the device and would not detect EIO/EFBIG. */
static bool jw__log_fd_takes_byte(int fd) {
    ssize_t n;
    do {
        n = write(fd, "\n", 1);
    } while (n < 0 && errno == EINTR);
    return n == 1;
}

int jw_log_heal_unwritable_stdio(const char *fallback_dir, const char *name) {
    fflush(stdout);
    fflush(stderr);
    bool broken_out = !jw__log_fd_takes_byte(STDOUT_FILENO);
    bool broken_err = !jw__log_fd_takes_byte(STDERR_FILENO);
    if (!broken_out && !broken_err) {
        return 0;
    }

    int rc = 1;
    int fd = jw__log_open_fallback(fallback_dir, name);
    if (fd >= 0 && !jw__log_fd_takes_byte(fd)) {
        close(fd);
        fd = -1;
    }
    if (fd < 0) {
        fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
        rc = 2;
    }
    /* A closed stdout or stderr leaves its slot free, and open() hands out the
       lowest free descriptor; move the target above stdio so the dup2 below
       cannot be undone by the close. */
    if (fd >= 0 && fd <= STDERR_FILENO) {
        int moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        close(fd);
        fd = moved;
    }
    if (fd < 0) {
        return -1;
    }
    if ((broken_out && dup2(fd, STDOUT_FILENO) < 0) ||
        (broken_err && dup2(fd, STDERR_FILENO) < 0)) {
        rc = -1;
    }
    close(fd);
    clearerr(stdout);
    clearerr(stderr);
    return rc;
}

void jw_log_impl(const char *level, const char *fmt, ...) {
    char timestamp[32];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", &tm_now);

    fprintf(stderr, "%s %s ", timestamp, level);

    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    va_end(args);

    fputc('\n', stderr);
    fflush(stderr);
}
