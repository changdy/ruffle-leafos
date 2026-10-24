#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

static int write_marker(const char *runtime, const char *leaf,
                        const char *value) {
    char path[4096];
    int n = snprintf(path, sizeof(path), "%s/%s", runtime, leaf);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }
    size_t len = strlen(value);
    ssize_t wrote = write(fd, value, len);
    int close_rc = close(fd);
    return wrote == (ssize_t)len && close_rc == 0 ? 0 : -1;
}

int main(void) {
    const char *runtime = getenv("UMRK_RUNTIME_PATH");
    if (!runtime || !runtime[0]) {
        return 64;
    }
    pid_t writer = fork();
    if (writer < 0) {
        return 65;
    }
    if (writer == 0) {
        char active_path[4096];
        int active_n = snprintf(active_path, sizeof(active_path),
                                "%s/active-game.json", runtime);
        if (active_n < 0 || (size_t)active_n >= sizeof(active_path) ||
            access(active_path, F_OK) != 0) {
            _exit(69);
        }
        if (write_marker(runtime, "game-writer-live", "1\n") != 0) {
            _exit(70);
        }
        struct timespec delay = {.tv_sec = 1, .tv_nsec = 200000000L};
        while (nanosleep(&delay, &delay) != 0) {
        }
        if (write_marker(runtime, "game-writer-done", "1\n") != 0) {
            _exit(71);
        }
        _exit(0);
    }
    /* The direct child deliberately exits before its writer descendant. */
    return 0;
}
