#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/launch.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
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

#include "cJSON.h"

/* Real (not stubbed) fork()/exec() fixtures. Each test launches a real
 * helper binary through jw_svc_launch() and asserts ONE SVC-1 launch
 * mechanism actually happened in the child: the new process group, the
 * lease on descriptor 3 surviving exec, the environment, the rotating
 * bounded log, the pre-exec error pipe, or (Linux only) PDEATHSIG. A
 * passing suite on macOS plus the same suite inside gcc:13 on Linux is
 * the cross-platform bar; the PDEATHSIG test compiles to a skip on
 * macOS because that kernel has no pdwaitpid-style analogue. */

#define JW__TMP_TEMPLATE "/tmp/jw-launch-test-XXXXXX"

static char JW__TMP[sizeof(JW__TMP_TEMPLATE)];
static char JW__BIN[PATH_MAX];
static char JW__LOGS[PATH_MAX];
static char JW__LEASE[PATH_MAX];

/* Reaps a launched child, asserting it exited (not signalled) with the
 * expected status. The launch primitive hands back an unreaped leader;
 * the test (as its parent) is responsible for reaping it here. */
static void jw__reap_expect(pid_t pid, int expect_status) {
    int status = 0;
    pid_t r;
    do {
        r = waitpid(pid, &status, 0);
    } while (r < 0 && errno == EINTR);
    assert(r == pid);
    assert(WIFEXITED(status));
    assert(WEXITSTATUS(status) == expect_status);
}

static void jw__write_all(int fd, const void *data, size_t size) {
    const char *cursor = data;
    size_t offset = 0;
    while (offset < size) {
        ssize_t written = write(fd, cursor + offset, size - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        assert(written > 0);
        offset += (size_t)written;
    }
}

static void jw__read_exact(int fd, void *data, size_t size) {
    char *cursor = data;
    size_t offset = 0;
    while (offset < size) {
        ssize_t n = read(fd, cursor + offset, size - offset);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        assert(n > 0);
        offset += (size_t)n;
    }
}

/* Writes `text` into `path`, creating it with mode 0700. */
static void jw__write_file(const char *path, const char *text, mode_t mode) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
    assert(fd >= 0);
    size_t len = strlen(text);
    jw__write_all(fd, text, len);
    close(fd);
}

static char *jw__read_file(const char *path) {
    int fd = open(path, O_RDONLY);
    assert(fd >= 0);
    struct stat st;
    assert(fstat(fd, &st) == 0);
    assert(st.st_size >= 0);
    size_t size = (size_t)st.st_size;
    assert(size < SIZE_MAX);
    char *buf = malloc(size + 1u);
    assert(buf);
    jw__read_exact(fd, buf, size);
    buf[size] = '\0';
    close(fd);
    return buf;
}

static void jw__compile_helper(const char *name, const char *source,
                               char *helper, size_t helper_size) {
    int n = snprintf(helper, helper_size, "%s/%s", JW__TMP, name);
    assert(n > 0 && (size_t)n < helper_size);

    char src[PATH_MAX];
    n = snprintf(src, sizeof(src), "%s.c", helper);
    assert(n > 0 && (size_t)n < sizeof(src));
    jw__write_file(src, source, 0600);

    char cmd[PATH_MAX * 2];
    n = snprintf(cmd, sizeof(cmd), "cc -std=c11 -O0 -o '%s' '%s'",
                 helper, src);
    assert(n > 0 && (size_t)n < sizeof(cmd));
    assert(system(cmd) == 0);
}

/* A lease stand-in: a plain open file whose descriptor the child must
 * inherit on fd 3. flock semantics are lease.c's own test suite's job;
 * here we only need an open descriptor to hand over. */
static int jw__open_lease_standin(void) {
    char path[PATH_MAX];
    int n = snprintf(path, sizeof(path), "%s/generation.lease", JW__TMP);
    assert(n > 0 && n < (int)sizeof(path));
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    assert(fd >= 0);
    return fd;
}

/* $$ only proves a shell knows its pid, not that setpgid() worked. This
 * helper checks the process-group identity from inside the exec'd image. */
#define JW__PGID_HELPER_SRC                                             \
    "#include <sys/types.h>\n#include <unistd.h>\n"                  \
    "int main(void){return getpid()==getpgrp()?0:20;}\n"

static void jw__test_group_and_exec(void) {
    puts("RUN launch-test: child runs in a new process group and execs");

    char helper[PATH_MAX];
    jw__compile_helper("pgid-helper", JW__PGID_HELPER_SRC, helper,
                       sizeof(helper));

    int lease = jw__open_lease_standin();
    jw_svc_launch_request req = {
        .run_path_abs = helper,
        .args = NULL,
        .args_count = 0,
        .env_json = NULL,
        .lease_fd = lease,
        .log_fd = -1,
    };
    char reason[JW_SVC_LAUNCH_REASON_BUF];
    pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
    assert(pid > 0);
    close(lease);

    jw__reap_expect(pid, 0);
    puts("PASS launch-test: new process group + exec");
}

/* A tiny C helper exec'd by the fd-3 test: asserts, from INSIDE the
 * child after exec, that descriptor 3 is a valid open descriptor with
 * FD_CLOEXEC clear, that UMRK_SERVICE_LEASE_FD names it, and that the
 * caller-supplied env arrived. Exits 0 on success, distinct non-zero
 * codes pinpointing the failing check. */
#define JW__FD3_HELPER_SRC                                              \
    "#include <fcntl.h>\n#include <stdlib.h>\n#include <string.h>\n"    \
    "#include <sys/stat.h>\n#include <unistd.h>\n"                      \
    "int main(void){\n"                                                 \
    " const char*e=getenv(\"UMRK_SERVICE_LEASE_FD\");\n"               \
    " if(!e||strcmp(e,\"3\")!=0) return 10;\n"                          \
    " int fl=fcntl(3,F_GETFD);\n"                                       \
    " if(fl<0) return 11;\n"                                            \
    " if(fl&FD_CLOEXEC) return 12;\n"                                   \
    " const char*m=getenv(\"JW_TEST_MARKER\");\n"                       \
    " if(!m||strcmp(m,\"hello-leaf\")!=0) return 13;\n"                 \
    " const char*o=getenv(\"JW_TEST_OVERRIDE\");\n"                     \
    " if(!o||strcmp(o,\"child\")!=0) return 14;\n"                     \
    " struct stat lease_st,log_st,st;\n"                              \
    " if(fstat(3,&lease_st)!=0||fstat(1,&log_st)!=0) return 15;\n"      \
    " for(int fd=4;fd<64;fd++){\n"                                     \
    "  if(fstat(fd,&st)==0){\n"                                           \
    "   if(st.st_dev==lease_st.st_dev&&st.st_ino==lease_st.st_ino)"    \
    " return 16;\n"                                                       \
    "   if(st.st_dev==log_st.st_dev&&st.st_ino==log_st.st_ino)"        \
    " return 17;\n"                                                       \
    "  }\n"                                                               \
    " }\n"                                                                \
    " return 0;\n"                                                      \
    "}\n"

static void jw__test_lease_fd_and_env(void) {
    puts("RUN launch-test: lease survives exec on fd 3 (cloexec clear), "
         "env applied");

    char helper[PATH_MAX];
    jw__compile_helper("fd3-helper", JW__FD3_HELPER_SRC, helper,
                       sizeof(helper));

    assert(unsetenv("JW_TEST_MARKER") == 0);
    assert(setenv("JW_TEST_OVERRIDE", "parent", 1) == 0);
    assert(setenv(JW_SVC_LAUNCH_LEASE_ENV, "parent-lease", 1) == 0);

    int lease = jw__open_lease_standin();
    jw_svc_launch_request req = {
        .run_path_abs = helper,
        .args = NULL,
        .args_count = 0,
        .env_json = "{\"JW_TEST_MARKER\":\"hello-leaf\","
                    "\"JW_TEST_OVERRIDE\":\"child\","
                    "\"UMRK_SERVICE_LEASE_FD\":\"99\"}",
        .lease_fd = lease,
        .log_fd = -1,
    };
    char reason[JW_SVC_LAUNCH_REASON_BUF];
    pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
    assert(pid > 0);
    assert(getenv("JW_TEST_MARKER") == NULL);
    const char *parent_override = getenv("JW_TEST_OVERRIDE");
    const char *parent_lease_env = getenv(JW_SVC_LAUNCH_LEASE_ENV);
    assert(parent_override != NULL);
    assert(parent_lease_env != NULL);
    assert(strcmp(parent_override, "parent") == 0);
    assert(strcmp(parent_lease_env, "parent-lease") == 0);
    close(lease);

    /* Exit 0 means every in-child check passed; a 10/11/12/13 names the
     * exact failing one. */
    jw__reap_expect(pid, 0);
    assert(unsetenv("JW_TEST_OVERRIDE") == 0);
    assert(unsetenv(JW_SVC_LAUNCH_LEASE_ENV) == 0);
    puts("PASS launch-test: fd 3 lease (cloexec clear) + env");
}

static void jw__test_log_capture_and_rotation(void) {
    puts("RUN launch-test: stdout/stderr captured; log rotates at cap");

    char reason[JW_SVC_LAUNCH_REASON_BUF];
    int log_fd = jw_svc_launch_open_log(JW__LOGS, "svc.alpha", reason,
                                        sizeof(reason));
    assert(log_fd >= 0);
    int descriptor_flags = fcntl(log_fd, F_GETFD);
    assert(descriptor_flags >= 0 && (descriptor_flags & FD_CLOEXEC) != 0);

    int lease = jw__open_lease_standin();
    const char *script =
        "echo first-gen-stdout; echo first-gen-stderr 1>&2";
    const char *args[] = {"-c", script, "sh"};
    jw_svc_launch_request req = {
        .run_path_abs = "/bin/sh",
        .args = args,
        .args_count = 3,
        .env_json = NULL,
        .lease_fd = lease,
        .log_fd = log_fd,
    };
    pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
    assert(pid > 0);
    close(lease);
    close(log_fd);
    jw__reap_expect(pid, 0);

    char logpath[PATH_MAX];
    int n = snprintf(logpath, sizeof(logpath),
                     "%s/services/svc.alpha/svc.alpha.log", JW__LOGS);
    assert(n > 0 && n < (int)sizeof(logpath));
    char *contents = jw__read_file(logpath);
    assert(strstr(contents, "first-gen-stdout") != NULL);
    assert(strstr(contents, "first-gen-stderr") != NULL);
    free(contents);

    /* Fill the current file beyond the cap, then reopen: its archived
     * generation must be trimmed to the per-file bound before rotation. */
    int fill = open(logpath, O_WRONLY | O_TRUNC);
    assert(fill >= 0);
    static char big[JW_SVC_LOG_MAX_BYTES + 4096];
    memset(big, 'x', sizeof(big));
    jw__write_all(fill, big, sizeof(big));
    close(fill);

    int log_fd2 = jw_svc_launch_open_log(JW__LOGS, "svc.alpha", reason,
                                         sizeof(reason));
    assert(log_fd2 >= 0);
    close(log_fd2);

    char rotated[PATH_MAX];
    n = snprintf(rotated, sizeof(rotated), "%s.1", logpath);
    assert(n > 0 && n < (int)sizeof(rotated));
    struct stat st;
    assert(stat(rotated, &st) == 0);
    assert(st.st_size == JW_SVC_LOG_MAX_BYTES);
    /* The fresh current file is now empty again. */
    assert(stat(logpath, &st) == 0);
    assert(st.st_size == 0);

    /* Drive enough full generations through the rotation to prove the
     * oldest is dropped, no .5 is created, and every archive is bounded. */
    for (int generation = 0; generation < JW_SVC_LOG_MAX_FILES; generation++) {
        fill = open(logpath, O_WRONLY | O_TRUNC);
        assert(fill >= 0);
        jw__write_all(fill, big, JW_SVC_LOG_MAX_BYTES);
        close(fill);

        log_fd2 = jw_svc_launch_open_log(JW__LOGS, "svc.alpha", reason,
                                         sizeof(reason));
        assert(log_fd2 >= 0);
        close(log_fd2);
    }
    for (int generation = 1; generation < JW_SVC_LOG_MAX_FILES; generation++) {
        n = snprintf(rotated, sizeof(rotated), "%s.%d", logpath, generation);
        assert(n > 0 && (size_t)n < sizeof(rotated));
        assert(stat(rotated, &st) == 0);
        assert(st.st_size == JW_SVC_LOG_MAX_BYTES);
    }
    n = snprintf(rotated, sizeof(rotated), "%s.%d", logpath,
                 JW_SVC_LOG_MAX_FILES);
    assert(n > 0 && (size_t)n < sizeof(rotated));
    assert(stat(rotated, &st) != 0 && errno == ENOENT);

    puts("PASS launch-test: log capture + bounded five-file rotation");
}

static void jw__test_log_path_safety(void) {
    puts("RUN launch-test: log paths reject links and non-regular files");

    char reason[JW_SVC_LAUNCH_REASON_BUF];
    int log_fd = jw_svc_launch_open_log(JW__LOGS, "svc.hardlink", reason,
                                        sizeof(reason));
    assert(log_fd >= 0);
    close(log_fd);

    char logpath[PATH_MAX];
    int n = snprintf(logpath, sizeof(logpath),
                     "%s/services/svc.hardlink/svc.hardlink.log", JW__LOGS);
    assert(n > 0 && (size_t)n < sizeof(logpath));
    assert(unlink(logpath) == 0);

    char victim[PATH_MAX];
    n = snprintf(victim, sizeof(victim), "%s/hardlink-victim", JW__TMP);
    assert(n > 0 && (size_t)n < sizeof(victim));
    jw__write_file(victim, "must-not-become-a-log", 0600);
    assert(link(victim, logpath) == 0);

    /* O_NOFOLLOW alone does not stop a same-uid hard-link substitution.
     * Refuse multiply-linked current files before append or truncation. */
    assert(jw_svc_launch_open_log(JW__LOGS, "svc.hardlink", reason,
                                  sizeof(reason)) < 0);
    assert(strcmp(reason, "open-failed") == 0);
    char *contents = jw__read_file(victim);
    assert(strcmp(contents, "must-not-become-a-log") == 0);
    free(contents);

    char fifo_dir[PATH_MAX];
    n = snprintf(fifo_dir, sizeof(fifo_dir), "%s/services/svc.fifo", JW__LOGS);
    assert(n > 0 && (size_t)n < sizeof(fifo_dir));
    assert(mkdir(fifo_dir, 0700) == 0);
    n = snprintf(logpath, sizeof(logpath), "%s/svc.fifo.log", fifo_dir);
    assert(n > 0 && (size_t)n < sizeof(logpath));
    assert(mkfifo(logpath, 0600) == 0);
    /* Opening a write-only FIFO normally blocks. The validator must reject
     * it without waiting for a reader. */
    assert(jw_svc_launch_open_log(JW__LOGS, "svc.fifo", reason,
                                  sizeof(reason)) < 0);
    assert(strcmp(reason, "open-failed") == 0);

    puts("PASS launch-test: unsafe log path entries refused");
}

static void jw__test_log_modes(void) {
    puts("RUN launch-test: read-only group/world log bits are accepted");

    char service_dir[PATH_MAX];
    int n = snprintf(service_dir, sizeof(service_dir),
                     "%s/services/svc.modes", JW__LOGS);
    assert(n > 0 && (size_t)n < sizeof(service_dir));
    assert(mkdir(service_dir, 0700) == 0);
    assert(chmod(service_dir, 0755) == 0);

    char reason[JW_SVC_LAUNCH_REASON_BUF];
    int log_fd = jw_svc_launch_open_log(JW__LOGS, "svc.modes", reason,
                                        sizeof(reason));
    assert(log_fd >= 0);
    close(log_fd);

    char logpath[PATH_MAX];
    n = snprintf(logpath, sizeof(logpath), "%s/svc.modes.log", service_dir);
    assert(n > 0 && (size_t)n < sizeof(logpath));
    assert(chmod(logpath, 0644) == 0);
    log_fd = jw_svc_launch_open_log(JW__LOGS, "svc.modes", reason,
                                    sizeof(reason));
    assert(log_fd >= 0);
    close(log_fd);

    assert(chmod(service_dir, 0775) == 0);
    assert(jw_svc_launch_open_log(JW__LOGS, "svc.modes", reason,
                                  sizeof(reason)) < 0);
    assert(strcmp(reason, "mkdir-failed") == 0);
    assert(chmod(service_dir, 0777) == 0);
    assert(jw_svc_launch_open_log(JW__LOGS, "svc.modes", reason,
                                  sizeof(reason)) < 0);
    assert(strcmp(reason, "mkdir-failed") == 0);

    assert(chmod(service_dir, 0755) == 0);
    assert(chmod(logpath, 0660) == 0);
    assert(jw_svc_launch_open_log(JW__LOGS, "svc.modes", reason,
                                  sizeof(reason)) < 0);
    assert(strcmp(reason, "open-failed") == 0);
    assert(chmod(logpath, 0666) == 0);
    assert(jw_svc_launch_open_log(JW__LOGS, "svc.modes", reason,
                                  sizeof(reason)) < 0);
    assert(strcmp(reason, "open-failed") == 0);

    puts("PASS launch-test: writable group/world log paths refused");
}

/* Regression for a real bug: the child used to dup2() the caller's raw
 * lease_fd and log_fd straight onto their reserved slots (3, stdout,
 * stderr) without relocating them first. When a caller-supplied log_fd
 * happened to already equal JW_SVC_LAUNCH_LEASE_FD (3) -- which happens
 * in practice, since jw_svc_launch_open_log() is often the very first
 * fd a minimal-fd process allocates -- dup2(lease_fd, 3) silently closed
 * the log descriptor before it could be duplicated onto stdout/stderr,
 * so the child's stdout/stderr ended up pointing at the LEASE file
 * instead of the log, and the real log stayed empty. Forces that exact
 * collision here regardless of ambient fd state (via dup2, not
 * allocation order), plus the symmetric case of the caller's lease_fd
 * itself already being 3. */
static void jw__test_reserved_fd_collision(void) {
    puts("RUN launch-test: caller fds landing on the reserved lease slot "
         "(3) do not collide");

    char reason[JW_SVC_LAUNCH_REASON_BUF];

    /* Case 1: log_fd == 3, the originally-observed collision. */
    {
        int log_raw = jw_svc_launch_open_log(JW__LOGS, "svc.collide",
                                             reason, sizeof(reason));
        assert(log_raw >= 0);
        int lease_raw = jw__open_lease_standin();

        /* Preserve the lease before dup2() can clobber fd 3. This ordering
         * keeps the fixture deterministic even when log_raw is not itself
         * 3 (for example, openat-based directory traversal can return 4
         * after closing its temporary directory descriptors). */
        int lease = fcntl(lease_raw, F_DUPFD, 10);
        assert(lease >= 10);
        if (lease_raw != lease) close(lease_raw);
        assert(dup2(log_raw, 3) == 3);
        if (log_raw != 3) close(log_raw);

        const char *args[] = {"-c", "echo collide-stdout", "sh"};
        jw_svc_launch_request req = {
            .run_path_abs = "/bin/sh",
            .args = args,
            .args_count = 3,
            .env_json = NULL,
            .lease_fd = lease,
            .log_fd = 3,
        };
        pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
        assert(pid > 0);
        close(3);
        close(lease);
        jw__reap_expect(pid, 0);

        char logpath[PATH_MAX];
        int n = snprintf(logpath, sizeof(logpath),
                         "%s/services/svc.collide/svc.collide.log",
                         JW__LOGS);
        assert(n > 0 && n < (int)sizeof(logpath));
        char *contents = jw__read_file(logpath);
        assert(strstr(contents, "collide-stdout") != NULL);
        free(contents);
    }

    /* Case 2: the caller's lease_fd is already 3 (a dup2(x, x) no-op in
     * the naive version -- must still end up correctly placed). */
    {
        int lease_raw = jw__open_lease_standin();
        assert(dup2(lease_raw, 3) == 3);
        if (lease_raw != 3) close(lease_raw);

        int log_fd = jw_svc_launch_open_log(JW__LOGS, "svc.collide2",
                                            reason, sizeof(reason));
        assert(log_fd >= 0);

        const char *args[] = {"-c", "echo collide2-stdout", "sh"};
        jw_svc_launch_request req = {
            .run_path_abs = "/bin/sh",
            .args = args,
            .args_count = 3,
            .env_json = NULL,
            .lease_fd = 3,
            .log_fd = log_fd,
        };
        pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
        assert(pid > 0);
        close(3);
        close(log_fd);
        jw__reap_expect(pid, 0);

        char logpath[PATH_MAX];
        int n = snprintf(logpath, sizeof(logpath),
                         "%s/services/svc.collide2/svc.collide2.log",
                         JW__LOGS);
        assert(n > 0 && n < (int)sizeof(logpath));
        char *contents = jw__read_file(logpath);
        assert(strstr(contents, "collide2-stdout") != NULL);
        free(contents);
    }

    puts("PASS launch-test: reserved fd (3) collisions handled");
}

/* Run one launch from a disposable process whose low descriptors have been
 * deliberately vacated. That makes pipe() deterministically allocate its
 * write end on a descriptor that the child setup later reserves. */
static void jw__error_pipe_collision_probe(bool close_all_stdio) {
    int lease_raw = jw__open_lease_standin();
    int lease = fcntl(lease_raw, F_DUPFD, 10);
    assert(lease >= 10);
    close(lease_raw);

    if (close_all_stdio) {
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        close(JW_SVC_LAUNCH_LEASE_FD);
    } else {
        /* pipe read == 2 and pipe write == 3: lease placement used to
         * destroy the write end before execve() could report failure. */
        close(STDERR_FILENO);
        close(JW_SVC_LAUNCH_LEASE_FD);
    }

    jw_svc_launch_request req = {
        .run_path_abs = "/definitely/not/a/service",
        .args = NULL,
        .args_count = 0,
        .env_json = NULL,
        .lease_fd = lease,
        .log_fd = -1,
    };
    char reason[JW_SVC_LAUNCH_REASON_BUF];
    pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
    close(lease);
    if (pid > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
        _exit(20);
    }
    if (strcmp(reason, "fork-failed") != 0) {
        _exit(21);
    }
    _exit(0);
}

static void jw__test_error_pipe_reserved_fd_collision(void) {
    puts("RUN launch-test: error pipe survives reserved fd collisions");
    assert(fflush(NULL) == 0);

    pid_t probe = fork();
    assert(probe >= 0);
    if (probe == 0) {
        /* pipe read == 0 and pipe write == 1: stdout placement used to
         * destroy the write end before execve() failure reporting. */
        jw__error_pipe_collision_probe(true);
    }
    jw__reap_expect(probe, 0);

    probe = fork();
    assert(probe >= 0);
    if (probe == 0) {
        jw__error_pipe_collision_probe(false);
    }
    jw__reap_expect(probe, 0);
    puts("PASS launch-test: error pipe reserved fd collisions handled");
}

static void jw__test_exec_failure_reported(void) {
    puts("RUN launch-test: a non-executable run.path fails the launch "
         "synchronously");

    char bad[PATH_MAX];
    int n = snprintf(bad, sizeof(bad), "%s/not-there", JW__TMP);
    assert(n > 0 && n < (int)sizeof(bad));

    int lease = jw__open_lease_standin();
    jw_svc_launch_request req = {
        .run_path_abs = bad,
        .args = NULL,
        .args_count = 0,
        .env_json = NULL,
        .lease_fd = lease,
        .log_fd = -1,
    };
    char reason[JW_SVC_LAUNCH_REASON_BUF];
    pid_t pid = jw_svc_launch(&req, reason, sizeof(reason));
    /* The error pipe must report the pre-exec execv() failure as a
     * failed launch, not a pid that then dies. */
    assert(pid < 0);
    assert(strcmp(reason, "fork-failed") == 0);
    close(lease);
    puts("PASS launch-test: exec failure reported via error pipe");
}

static void jw__test_invalid_requests(void) {
    puts("RUN launch-test: invalid requests are refused before fork");

    char reason[JW_SVC_LAUNCH_REASON_BUF];
    jw_svc_launch_request req = {
        .run_path_abs = "/bin/true",
        .args = NULL,
        .args_count = 0,
        .env_json = NULL,
        .lease_fd = -1, /* no lease: must be refused */
        .log_fd = -1,
    };
    assert(jw_svc_launch(&req, reason, sizeof(reason)) < 0);
    assert(strcmp(reason, "invalid-request") == 0);

    req.lease_fd = jw__open_lease_standin();
    req.log_fd = -2;
    assert(jw_svc_launch(&req, reason, sizeof(reason)) < 0);
    assert(strcmp(reason, "invalid-request") == 0);
    close(req.lease_fd);
    req.log_fd = -1;

    req.lease_fd = jw__open_lease_standin();
    req.log_fd = req.lease_fd;
    assert(jw_svc_launch(&req, reason, sizeof(reason)) < 0);
    assert(strcmp(reason, "invalid-request") == 0);
    close(req.lease_fd);
    req.log_fd = -1;

    req.lease_fd = jw__open_lease_standin();
    req.run_path_abs = NULL;
    assert(jw_svc_launch(&req, reason, sizeof(reason)) < 0);
    assert(strcmp(reason, "invalid-request") == 0);
    close(req.lease_fd);

    /* Malformed env fails before fork. */
    req.run_path_abs = "/bin/true";
    req.lease_fd = jw__open_lease_standin();
    req.env_json = "not json";
    assert(jw_svc_launch(&req, reason, sizeof(reason)) < 0);
    assert(strcmp(reason, "env-parse-failed") == 0);

    req.env_json = "{} trailing-garbage";
    assert(jw_svc_launch(&req, reason, sizeof(reason)) < 0);
    assert(strcmp(reason, "env-parse-failed") == 0);

    req.env_json = "{\"BAD=NAME\":\"value\"}";
    assert(jw_svc_launch(&req, reason, sizeof(reason)) < 0);
    assert(strcmp(reason, "env-parse-failed") == 0);
    close(req.lease_fd);
    puts("PASS launch-test: invalid requests refused");
}

#if defined(__linux__)
static void jw__test_pdeathsig(void) {
    puts("RUN launch-test (linux): PDEATHSIG kills the service when the "
         "parent dies");

    /* A child that blocks forever; we then kill THIS test process's
     * intermediate child (the service's parent) and assert the service
     * is signalled. jw_svc_launch sets the service's parent to this
     * process, so we fork an intermediate to act as the parent we can
     * kill. */
    int sync_pipe[2];
    assert(pipe(sync_pipe) == 0);

    pid_t middle = fork();
    assert(middle >= 0);
    if (middle == 0) {
        close(sync_pipe[0]);

        /* SIG_IGN and a blocked signal mask both survive exec. A launcher
         * that merely arms PDEATHSIG would therefore leave /bin/sleep
         * alive after this process exits. The pre-exec path must normalize
         * SIGTERM without invoking any inherited handler. */
        struct sigaction ignored;
        memset(&ignored, 0, sizeof(ignored));
        ignored.sa_handler = SIG_IGN;
        assert(sigemptyset(&ignored.sa_mask) == 0);
        assert(sigaction(SIGTERM, &ignored, NULL) == 0);
        sigset_t blocked;
        assert(sigemptyset(&blocked) == 0);
        assert(sigaddset(&blocked, SIGTERM) == 0);
        assert(sigprocmask(SIG_BLOCK, &blocked, NULL) == 0);

        int lease = jw__open_lease_standin();
        jw_svc_launch_request req = {
            .run_path_abs = "/bin/sleep",
            .args = (const char *const[]){"60"},
            .args_count = 1,
            .env_json = NULL,
            .lease_fd = lease,
            .log_fd = -1,
        };
        pid_t svc = jw_svc_launch(&req, NULL, 0);
        /* Tell the real parent the service pid, then die so the service
         * is orphaned-by-us -- PDEATHSIG should fire. */
        jw__write_all(sync_pipe[1], &svc, sizeof(svc));
        close(sync_pipe[1]);
        _exit(0); /* the service's parent exits here */
    }

    close(sync_pipe[1]);
    pid_t svc = 0;
    jw__read_exact(sync_pipe[0], &svc, sizeof(svc));
    close(sync_pipe[0]);
    assert(svc > 0);

    /* Wait for the middle child to exit, then give PDEATHSIG a moment to
     * be delivered; the sleep should die on SIGTERM without us signalling
     * it directly. */
    int status = 0;
    while (waitpid(middle, &status, 0) < 0 && errno == EINTR) {
    }

    bool gone = false;
    for (int i = 0; i < 100; i++) {
        int svc_status = 0;
        pid_t reaped = waitpid(svc, &svc_status, WNOHANG);
        if (reaped == svc) {
            assert(WIFSIGNALED(svc_status));
            assert(WTERMSIG(svc_status) == SIGTERM);
            gone = true;
            break;
        }
        assert(reaped == 0 || (reaped < 0 && errno == ECHILD));
        if (kill(svc, 0) != 0 && errno == ESRCH) {
            gone = true;
            break;
        }
        usleep(10000);
    }
    if (!gone) {
        /* Do not strand a minute-long sleeper when this regression fires. */
        kill(svc, SIGKILL);
        while (waitpid(svc, &status, 0) < 0 && errno == EINTR) {
        }
    }
    assert(gone);
    puts("PASS launch-test (linux): PDEATHSIG");
}
#endif

int main(void) {
    strcpy(JW__TMP, JW__TMP_TEMPLATE);
    assert(mkdtemp(JW__TMP) != NULL);
    int n = snprintf(JW__LOGS, sizeof(JW__LOGS), "%s/logs", JW__TMP);
    assert(n > 0 && n < (int)sizeof(JW__LOGS));
    assert(mkdir(JW__LOGS, 0700) == 0);
    n = snprintf(JW__BIN, sizeof(JW__BIN), "%s/bin", JW__TMP);
    assert(n > 0 && n < (int)sizeof(JW__BIN));
    n = snprintf(JW__LEASE, sizeof(JW__LEASE), "%s/lease", JW__TMP);
    assert(n > 0 && n < (int)sizeof(JW__LEASE));

    jw__test_group_and_exec();
    jw__test_lease_fd_and_env();
    jw__test_log_capture_and_rotation();
    jw__test_log_modes();
    jw__test_log_path_safety();
    jw__test_reserved_fd_collision();
    jw__test_error_pipe_reserved_fd_collision();
    jw__test_exec_failure_reported();
    jw__test_invalid_requests();
#if defined(__linux__)
    jw__test_pdeathsig();
#else
    puts("SKIP launch-test: PDEATHSIG (no macOS analogue; runs on Linux)");
#endif

    puts("PASS launch-test");
    return 0;
}
