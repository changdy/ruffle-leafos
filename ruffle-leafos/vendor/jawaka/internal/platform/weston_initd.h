#ifndef JW_WESTON_INITD_H
#define JW_WESTON_INITD_H

/* Shell command prefix for every stop, start and restart of the stock
   compositor. S49weston backgrounds Weston, so it reparents to init and outlives
   the Leaf generation that restarted it, together with its clients and the log
   tee. Run it from / and without the loader variables: jawakad's
   LD_LIBRARY_PATH points at the launcher's lib/ on the SD card, so those
   processes would map libz and libatomic from the card for the rest of the boot.
   Once a launcher update replaces the files, the mappings pin deleted inodes and
   the shutdown barrier cannot remount the card read-only. A card cwd does the
   same.

   The rest of the environment is kept on purpose: the session exports the
   WESTON_DRM_* panel-first settings when HDMI is connected at boot, and a
   restart must not fall back to mirroring onto the TV. Extra settings go after
   the prefix as NAME=VALUE words, before the script path. */
#define JW_WESTON_INITD_SCRIPT "/etc/init.d/S49weston"
#define JW_WESTON_ROOTFS_ENV "cd / && env -u LD_LIBRARY_PATH -u LD_PRELOAD"
#define JW_WESTON_INITD(verb) JW_WESTON_ROOTFS_ENV " " JW_WESTON_INITD_SCRIPT " " verb

/* Run these commands only through the helpers below, never system() or a bare
   exec. The environment is half of the leak; inherited descriptors are the
   other half. Any descriptor a jawakad thread has open without O_CLOEXEC at
   fork time (the content catalog hashing a pak's core, say) would otherwise stay
   open in Weston, its clients and the log tee for the rest of the boot, and
   pins a deleted inode once that file is replaced. The helpers give the shell
   /dev/null for stdio and close everything from 3 up. */

#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif

/* Upper bound for the close loop; read before fork, sysconf is not
   async-signal-safe. */
static inline long jw_weston_initd_fd_limit(void) {
    long limit = sysconf(_SC_OPEN_MAX);
    return limit > 0 && limit <= 65536 ? limit : 65536;
}

/* In a forked child only: async-signal-safe calls until the exec. */
static inline void jw_weston_initd_child_setup(long fd_limit) {
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
    }
#if defined(__linux__) && defined(SYS_close_range)
    if (syscall(SYS_close_range, 3u, ~0u, 0u) == 0) {
        return;
    }
#endif
    for (long fd = 3; fd < fd_limit; fd++) {
        close((int)fd);
    }
}

/* system() for a JW_WESTON_INITD command: waits and returns the wait status,
   or -1 when the shell could not be started. */
static inline int jw_weston_initd_run(const char *command) {
    long fd_limit = jw_weston_initd_fd_limit();
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        jw_weston_initd_child_setup(fd_limit);
        execl("/bin/sh", "sh", "-c", command, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return status;
}

/* Detached variant for actions that must return at once. Double-fork so the
   grandchild reparents to init (jawakad never reaps unknown pids); only the
   intermediate child is waited for. Returns -1 when the fork failed. */
static inline int jw_weston_initd_spawn_detached(const char *command) {
    long fd_limit = jw_weston_initd_fd_limit();
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        pid_t grandchild = fork();
        if (grandchild == 0) {
            setsid();
            jw_weston_initd_child_setup(fd_limit);
            execl("/bin/sh", "sh", "-c", command, (char *)NULL);
            _exit(127);
        }
        _exit(grandchild < 0 ? 1 : 0);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return -1;
        }
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

#endif
