#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/launch.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__linux__)
#include <sys/prctl.h>
#endif

#include "cJSON.h"

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

extern char **environ;

/* jw__child_fail uses async-signal-safe operations only: the child between
 * fork() and exec() may call nothing that takes a lock or allocates (no
 * stdio, setenv, or malloc), because the parent may have held one of those
 * locks in another thread at the instant of fork. */

static void jw__set_slug(char *reason, size_t reason_size, const char *slug) {
    if (reason && reason_size > 0) {
        snprintf(reason, reason_size, "%s", slug);
    }
}

/* Creates and opens one owner-only child directory without a check/use gap.
 * Holding the parent directory descriptor keeps a concurrent path rename
 * from redirecting later rotation operations through a symlink. */
static int jw__open_log_subdir(int parent_fd, const char *name,
                               char *reason, size_t reason_size) {
    if (mkdirat(parent_fd, name, 0700) != 0 && errno != EEXIST) {
        jw__set_slug(reason, reason_size,
                     errno == ENAMETOOLONG ? "path-too-long" :
                                             "mkdir-failed");
        return -1;
    }

    int fd = openat(parent_fd, name,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        jw__set_slug(reason, reason_size,
                     errno == ENAMETOOLONG ? "path-too-long" :
                                             "mkdir-failed");
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != geteuid() ||
        (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
        close(fd);
        jw__set_slug(reason, reason_size, "mkdir-failed");
        return -1;
    }
    return fd;
}

/* Builds and opens logs_dir/services/<service_id>/: each created level is a
 * real directory (not a symlink), owner-owned, and not group/world-writable.
 * Read/execute bits may be synthesized by filesystems such as vfat. */
static int jw__open_service_log_dir(const char *logs_dir,
                                    const char *service_id,
                                    char *reason, size_t reason_size) {
    if (!service_id || service_id[0] == '\0' ||
        strchr(service_id, '/') != NULL || strcmp(service_id, ".") == 0 ||
        strcmp(service_id, "..") == 0) {
        jw__set_slug(reason, reason_size, "invalid-service-id");
        return -1;
    }
    if (!logs_dir || logs_dir[0] == '\0') {
        jw__set_slug(reason, reason_size, "logs-dir-unavailable");
        return -1;
    }

    int logs_fd = open(logs_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (logs_fd < 0) {
        jw__set_slug(reason, reason_size,
                     errno == ENAMETOOLONG ? "path-too-long" :
                                             "logs-dir-unavailable");
        return -1;
    }

    int services_fd =
        jw__open_log_subdir(logs_fd, "services", reason, reason_size);
    close(logs_fd);
    if (services_fd < 0) {
        return -1;
    }

    int service_fd = jw__open_log_subdir(services_fd, service_id, reason,
                                         reason_size);
    close(services_fd);
    return service_fd;
}

/* Renames <id>.log -> .log.1 -> ... -> .log.4, dropping .log.4. Best
 * effort per generation: a missing intermediate file is fine (ENOTENT),
 * anything else fails the rotation. */
static int jw__log_rotate(int dir_fd, const char *base,
                          char *reason, size_t reason_size) {
    char from[PATH_MAX];
    char to[PATH_MAX];

    /* Drop the oldest generation outright. */
    int written = snprintf(to, sizeof(to), "%s.%d", base,
                           JW_SVC_LOG_MAX_FILES - 1);
    if (written < 0 || (size_t)written >= sizeof(to)) {
        jw__set_slug(reason, reason_size, "path-too-long");
        return -1;
    }
    if (unlinkat(dir_fd, to, 0) != 0 && errno != ENOENT) {
        jw__set_slug(reason, reason_size, "rotate-failed");
        return -1;
    }

    for (int i = JW_SVC_LOG_MAX_FILES - 1; i >= 1; i--) {
        written = snprintf(from, sizeof(from), "%s.%d", base, i - 1);
        if (written < 0 || (size_t)written >= sizeof(from)) {
            jw__set_slug(reason, reason_size, "path-too-long");
            return -1;
        }
        written = snprintf(to, sizeof(to), "%s.%d", base, i);
        if (written < 0 || (size_t)written >= sizeof(to)) {
            jw__set_slug(reason, reason_size, "path-too-long");
            return -1;
        }
        if (i == 1) {
            /* The current file has no numeric suffix. */
            written = snprintf(from, sizeof(from), "%s", base);
            if (written < 0 || (size_t)written >= sizeof(from)) {
                jw__set_slug(reason, reason_size, "path-too-long");
                return -1;
            }
        }
        if (renameat(dir_fd, from, dir_fd, to) != 0 && errno != ENOENT) {
            jw__set_slug(reason, reason_size, "rotate-failed");
            return -1;
        }
    }
    return 0;
}

int jw_svc_launch_open_log(const char *logs_dir, const char *service_id,
                           char *reason, size_t reason_size) {
    int dir_fd =
        jw__open_service_log_dir(logs_dir, service_id, reason, reason_size);
    if (dir_fd < 0) {
        return -1;
    }

    char base[PATH_MAX];
    int written = snprintf(base, sizeof(base), "%s.log", service_id);
    if (written < 0 || (size_t)written >= sizeof(base)) {
        close(dir_fd);
        jw__set_slug(reason, reason_size, "path-too-long");
        return -1;
    }

    /* O_NONBLOCK prevents a malicious pre-existing FIFO from hanging the
     * daemon before fstat() can reject it. It has no effect on regular
     * files and is cleared before returning the descriptor. */
    int fd = openat(dir_fd, base,
                    O_WRONLY | O_APPEND | O_NONBLOCK | O_NOFOLLOW |
                        O_CLOEXEC);
    if (fd < 0 && errno != ENOENT) {
        close(dir_fd);
        jw__set_slug(reason, reason_size, "open-failed");
        return -1;
    }

    struct stat st;
    if (fd >= 0) {
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1 ||
            st.st_uid != geteuid() ||
            (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
            close(fd);
            close(dir_fd);
            jw__set_slug(reason, reason_size, "open-failed");
            return -1;
        }

        int status_flags = fcntl(fd, F_GETFL);
        if (status_flags < 0 ||
            fcntl(fd, F_SETFL, status_flags & ~O_NONBLOCK) != 0) {
            close(fd);
            close(dir_fd);
            jw__set_slug(reason, reason_size, "open-failed");
            return -1;
        }

        if (st.st_size < JW_SVC_LOG_MAX_BYTES) {
            close(dir_fd);
            return fd;
        }

        /* Rotation is intentionally open-time only. Cap an oversized
         * completed generation before archiving it; otherwise one noisy
         * run could leave .log.1 larger than the advertised per-file
         * bound for four more generations. */
        if (st.st_size > JW_SVC_LOG_MAX_BYTES &&
            ftruncate(fd, (off_t)JW_SVC_LOG_MAX_BYTES) != 0) {
            close(fd);
            close(dir_fd);
            jw__set_slug(reason, reason_size, "rotate-failed");
            return -1;
        }
        close(fd);
        if (jw__log_rotate(dir_fd, base, reason, reason_size) != 0) {
            close(dir_fd);
            return -1;
        }
    }

    fd = openat(dir_fd, base,
                O_WRONLY | O_CREAT | O_EXCL | O_APPEND | O_NOFOLLOW |
                    O_CLOEXEC,
                0600);
    close(dir_fd);
    if (fd < 0) {
        jw__set_slug(reason, reason_size, "open-failed");
        return -1;
    }
    return fd;
}

typedef struct {
    char **entries;
    size_t count;
    size_t capacity;
} jw__child_env;

static void jw__child_env_free(jw__child_env *env) {
    if (!env || !env->entries) {
        return;
    }
    for (size_t i = 0; i < env->count; i++) {
        free(env->entries[i]);
    }
    free(env->entries);
    env->entries = NULL;
    env->count = 0;
    env->capacity = 0;
}

static char *jw__string_dup(const char *value) {
    size_t len = strlen(value);
    if (len > SIZE_MAX - 1u) {
        return NULL;
    }
    char *copy = malloc(len + 1u);
    if (copy) {
        memcpy(copy, value, len + 1u);
    }
    return copy;
}

static bool jw__env_name_matches(const char *entry, const char *name,
                                 size_t name_len) {
    return strncmp(entry, name, name_len) == 0 && entry[name_len] == '=';
}

/* Adds or replaces one environment entry in memory owned by `env`. This
 * reproduces setenv(..., 1) semantics without ever mutating the daemon's
 * process-global environment. */
static int jw__child_env_set(jw__child_env *env, const char *name,
                             const char *value) {
    if (!name || name[0] == '\0' || strchr(name, '=') != NULL || !value) {
        return -1;
    }

    size_t name_len = strlen(name);
    size_t value_len = strlen(value);
    if (value_len > SIZE_MAX - 2u ||
        name_len > SIZE_MAX - value_len - 2u) {
        return -1;
    }
    size_t entry_size = name_len + value_len + 2u;
    char *entry = malloc(entry_size);
    if (!entry) {
        return -1;
    }
    memcpy(entry, name, name_len);
    entry[name_len] = '=';
    memcpy(entry + name_len + 1u, value, value_len + 1u);

    for (size_t i = 0; i < env->count; i++) {
        if (jw__env_name_matches(env->entries[i], name, name_len)) {
            free(env->entries[i]);
            env->entries[i] = entry;
            return 0;
        }
    }

    if (env->count + 1u >= env->capacity) {
        free(entry);
        return -1;
    }
    env->entries[env->count] = entry;
    env->count++;
    env->entries[env->count] = NULL;
    return 0;
}

/* Snapshots the inherited environment, applies env_json, and reserves the
 * lease variable in the PARENT. The child can then use execve() directly;
 * no post-fork setenv() or allocator call is needed. */
static int jw__child_env_build(const char *env_json, jw__child_env *env) {
    memset(env, 0, sizeof(*env));

    cJSON *root = NULL;
    if (env_json && env_json[0] != '\0') {
        root = cJSON_ParseWithOpts(env_json, NULL, true);
        if (!root || !cJSON_IsObject(root)) {
            cJSON_Delete(root);
            return -1;
        }
    }

    size_t inherited_count = 0;
    if (environ) {
        while (environ[inherited_count]) {
            if (inherited_count == SIZE_MAX - 1u) {
                cJSON_Delete(root);
                return -1;
            }
            inherited_count++;
        }
    }

    size_t json_count = 0;
    if (root) {
        for (const cJSON *entry = root->child; entry; entry = entry->next) {
            if (!entry->string || entry->string[0] == '\0' ||
                strchr(entry->string, '=') != NULL ||
                !cJSON_IsString(entry) || !entry->valuestring ||
                json_count == SIZE_MAX - 1u) {
                cJSON_Delete(root);
                return -1;
            }
            json_count++;
        }
    }

    /* One slot per inherited/JSON entry, one forced lease entry, and one
     * NULL terminator. Replacements leave some capacity unused. */
    if (json_count > SIZE_MAX - 2u ||
        inherited_count > SIZE_MAX - json_count - 2u) {
        cJSON_Delete(root);
        return -1;
    }
    env->capacity = inherited_count + json_count + 2u;
    env->entries = calloc(env->capacity, sizeof(char *));
    if (!env->entries) {
        cJSON_Delete(root);
        return -1;
    }

    for (size_t i = 0; i < inherited_count; i++) {
        char *copy = jw__string_dup(environ[i]);
        if (!copy) {
            cJSON_Delete(root);
            jw__child_env_free(env);
            return -1;
        }
        env->entries[env->count] = copy;
        env->count++;
    }
    env->entries[env->count] = NULL;

    if (root) {
        for (const cJSON *entry = root->child; entry; entry = entry->next) {
            if (jw__child_env_set(env, entry->string, entry->valuestring) !=
                0) {
                cJSON_Delete(root);
                jw__child_env_free(env);
                return -1;
            }
        }
    }
    cJSON_Delete(root);

    /* This reserved value always wins over both the inherited environment
     * and an accidentally supplied JSON entry. */
    if (jw__child_env_set(env, JW_SVC_LAUNCH_LEASE_ENV, "3") != 0) {
        jw__child_env_free(env);
        return -1;
    }
    return 0;
}

/* Child-side pre-exec failure: report a single byte tag into the error
 * pipe, then exit. Never returns. Async-signal-safe. */
static _Noreturn void jw__child_fail(int err_fd, char tag) {
    if (err_fd >= 0) {
        ssize_t written;
        do {
            written = write(err_fd, &tag, 1);
        } while (written < 0 && errno == EINTR);
    }
    _exit(127);
}

pid_t jw_svc_launch(const jw_svc_launch_request *req,
                    char *reason, size_t reason_size) {
    if (!req || !req->run_path_abs || req->run_path_abs[0] == '\0' ||
        req->args_count < 0 || req->args_count > JW_SVC_LAUNCH_MAX_ARGS ||
        (req->args_count > 0 && !req->args) || req->lease_fd < 0 ||
        req->log_fd < -1 ||
        (req->log_fd >= 0 && req->log_fd == req->lease_fd)) {
        jw__set_slug(reason, reason_size, "invalid-request");
        return -1;
    }

    /* Build argv in the parent (it may allocate) so the child only
     * exec()s. argv[0] is run_path_abs; run.args follow. */
    int argc = 1 + req->args_count;
    char **argv = calloc((size_t)argc + 1u, sizeof(char *));
    if (!argv) {
        jw__set_slug(reason, reason_size, "fork-failed");
        return -1;
    }
    argv[0] = (char *)req->run_path_abs;
    for (int i = 0; i < req->args_count; i++) {
        argv[1 + i] = req->args[i] ? (char *)req->args[i] : (char *)"";
    }
    argv[argc] = NULL;

    jw__child_env child_env;
    if (jw__child_env_build(req->env_json, &child_env) != 0) {
        free(argv);
        jw__set_slug(reason, reason_size, "env-parse-failed");
        return -1;
    }

    /* The error pipe lets the child report a pre-exec setup failure
     * synchronously. Its write end is atomically CLOEXEC and deliberately
     * relocated above descriptors 0..3: otherwise a caller with closed
     * stdio can make pipe() return 1, 2, or 3 and a later dup2() silently
     * destroys the only failure-reporting channel. */
    int err_pipe[2] = {-1, -1};
#if defined(__linux__)
    int pipe_rc = pipe2(err_pipe, O_CLOEXEC);
#else
    int pipe_rc = pipe(err_pipe);
#endif
    if (pipe_rc != 0) {
        jw__child_env_free(&child_env);
        free(argv);
        jw__set_slug(reason, reason_size, "fork-failed");
        return -1;
    }
    int private_err_fd;
    do {
        private_err_fd =
            fcntl(err_pipe[1], F_DUPFD_CLOEXEC,
                  JW_SVC_LAUNCH_LEASE_FD + 1);
    } while (private_err_fd < 0 && errno == EINTR);
    if (private_err_fd < 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        jw__child_env_free(&child_env);
        free(argv);
        jw__set_slug(reason, reason_size, "fork-failed");
        return -1;
    }
    close(err_pipe[1]);
    err_pipe[1] = private_err_fd;

#if defined(__linux__)
    pid_t parent_pid = getpid();
    struct sigaction pdeath_action;
    memset(&pdeath_action, 0, sizeof(pdeath_action));
    pdeath_action.sa_handler = SIG_DFL;
    sigset_t pdeath_mask;
    if (sigemptyset(&pdeath_action.sa_mask) != 0 ||
        sigemptyset(&pdeath_mask) != 0 ||
        sigaddset(&pdeath_mask, SIGTERM) != 0) {
        close(err_pipe[0]);
        close(err_pipe[1]);
        jw__child_env_free(&child_env);
        free(argv);
        jw__set_slug(reason, reason_size, "fork-failed");
        return -1;
    }
#endif
    pid_t pid = fork();
    if (pid < 0) {
        jw__child_env_free(&child_env);
        free(argv);
        close(err_pipe[0]);
        close(err_pipe[1]);
        jw__set_slug(reason, reason_size, "fork-failed");
        return -1;
    }

    if (pid == 0) {
        /* ---- child: async-signal-safe operations only until exec ---- */
        close(err_pipe[0]);

        if (setpgid(0, 0) != 0) {
            jw__child_fail(err_pipe[1], 'g');
        }

        /* Descriptor 3 is RESERVED for the lease, but the parent's own
         * lease_fd / log_fd may themselves numerically be 3 (a parent
         * with only stdio open hands out fd 3 for the first open). Move
         * each working descriptor to a private high number FIRST so the
         * later dup2(...,3) for the lease can never clobber one. */
        int lease_work =
            fcntl(req->lease_fd, F_DUPFD, JW_SVC_LAUNCH_LEASE_FD + 1);
        if (lease_work < 0) {
            jw__child_fail(err_pipe[1], 'l');
        }
        int log_work;
        if (req->log_fd >= 0) {
            log_work = fcntl(req->log_fd, F_DUPFD, JW_SVC_LAUNCH_LEASE_FD + 1);
            if (log_work < 0) {
                jw__child_fail(err_pipe[1], 'o');
            }
        } else {
            int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
            if (null_fd < 0) {
                jw__child_fail(err_pipe[1], 'o');
            }
            log_work =
                fcntl(null_fd, F_DUPFD, JW_SVC_LAUNCH_LEASE_FD + 1);
            close(null_fd);
            if (log_work < 0) {
                jw__child_fail(err_pipe[1], 'o');
            }
        }

        /* Only the deliberately placed targets may survive exec. Closing
         * the inherited caller descriptors also covers sources without
         * FD_CLOEXEC; the private working duplicates are closed below. */
        close(req->lease_fd);
        if (req->log_fd >= 0 && req->log_fd != req->lease_fd) {
            close(req->log_fd);
        }

        /* The inherited generation lease on reserved descriptor 3,
         * FD_CLOEXEC cleared so it rides across exec. */
        if (dup2(lease_work, JW_SVC_LAUNCH_LEASE_FD) < 0) {
            jw__child_fail(err_pipe[1], 'l');
        }
        int lflags = fcntl(JW_SVC_LAUNCH_LEASE_FD, F_GETFD);
        if (lflags < 0 ||
            fcntl(JW_SVC_LAUNCH_LEASE_FD, F_SETFD, lflags & ~FD_CLOEXEC) <
                0) {
            jw__child_fail(err_pipe[1], 'l');
        }
        close(lease_work);

#if defined(__linux__)
        /* SVC-1 "Descendant survival": die when Jawaka does. Set, then
         * close the set-then-recheck race: if our parent already
         * changed (died and we were reparented) between fork() and this
         * prctl(), PDEATHSIG was armed against the NEW parent and may
         * never fire the way SVC-1 requires, so we exit rather than run
         * unsupervised. This must compare against parent_pid (captured
         * before fork(), so both processes agree on the value) rather
         * than the constant 1: a bare "getppid() == 1" reparent check is
         * wrong whenever the real parent already IS pid 1 in its
         * namespace (e.g. this process is a container's init), which
         * misfires exit(127) on every launch despite a live parent. */
        /* Caught dispositions reset on exec, but SIG_IGN and a blocked
         * mask survive it. Normalize SIGTERM before arming PDEATHSIG so
         * an inherited daemon policy cannot make the death signal inert
         * (or run a non-async-safe parent handler in this child window). */
        if (sigaction(SIGTERM, &pdeath_action, NULL) != 0 ||
            sigprocmask(SIG_UNBLOCK, &pdeath_mask, NULL) != 0) {
            jw__child_fail(err_pipe[1], 'p');
        }
        /* prctl() is variadic. glibc reads the optional arguments at
         * unsigned-long width, so bare int constants are undefined on
         * ABIs where the widths differ; prctl(2) explicitly requires 0L. */
        if (prctl(PR_SET_PDEATHSIG, (unsigned long)SIGTERM, 0L, 0L, 0L) !=
            0) {
            jw__child_fail(err_pipe[1], 'p');
        }
        if (getppid() != parent_pid) {
            jw__child_fail(err_pipe[1], 'p');
        }
#endif

        /* stdout/stderr into the rotating log (or /dev/null). */
        if (dup2(log_work, STDOUT_FILENO) < 0 ||
            dup2(log_work, STDERR_FILENO) < 0) {
            jw__child_fail(err_pipe[1], 'o');
        }
        close(log_work);

        execve(req->run_path_abs, argv, child_env.entries);
        /* Only reached if execve fails. */
        jw__child_fail(err_pipe[1], 'x');
    }

    /* ---- parent ---- */
    jw__child_env_free(&child_env);
    free(argv);
    close(err_pipe[1]);

    /* Block until the child either execs (read returns 0) or reports a
     * pre-exec failure (read returns 1). This is bounded: the child
     * reaches exec or a fail path without waiting on anything external. */
    char tag = 0;
    ssize_t n;
    do {
        n = read(err_pipe[0], &tag, 1);
    } while (n < 0 && errno == EINTR);
    close(err_pipe[0]);

    if (n > 0) {
        /* The child failed before exec and already _exit()ed. Reap it so
         * it does not linger as a zombie the caller never asked for --
         * the reservation rule applies to a LAUNCHED service, not to one
         * that never got past setup. */
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        const char *slug = "fork-failed";
        if (tag == 'o') {
            slug = "open-log-failed";
        }
        jw__set_slug(reason, reason_size, slug);
        return -1;
    }

    /* n == 0: exec succeeded (CLOEXEC closed the pipe). A non-EINTR read
     * failure cannot be distinguished from success safely, but the child
     * exists either way and is now the caller's reservation responsibility,
     * so report the pid. */
    return pid;
}
