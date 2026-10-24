/* mkdtemp(), kill(), and PATH_MAX need broader-than-bare-C11 visibility on
 * glibc; see the matching comment in lease.c. Must precede every
 * #include. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/lease.h"

/* This test deliberately uses assert() for both checks and fixture setup.
 * Jawaka's device profile defines NDEBUG, so force assertions on before
 * including <assert.h>; otherwise the profile silently removes forks,
 * reads, closes, and the checks that make this test meaningful. */
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define JW_TEST_REASON_BUF 64

static void jw__test_mkdtemp(const char *suffix, char *out, size_t out_size) {
    int n = snprintf(out, out_size, "/tmp/jw-lease-test-%s.XXXXXX", suffix);
    assert(n > 0 && (size_t)n < out_size);
    assert(mkdtemp(out) != NULL);
}

static ino_t jw__test_fd_inode(int fd) {
    struct stat st;
    assert(fstat(fd, &st) == 0);
    return st.st_ino;
}

static void jw__test_path(char *out, size_t out_size, const char *parent,
                          const char *child) {
    int n = snprintf(out, out_size, "%s/%s", parent, child);
    assert(n > 0 && (size_t)n < out_size);
}

static void jw__test_basic_acquire_and_contention(void) {
    char runtime_dir[512]; jw__test_mkdtemp("basic", runtime_dir, sizeof(runtime_dir));

    char reason[JW_TEST_REASON_BUF] = {0};
    int fd1 = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd1 >= 0);
    assert((fcntl(fd1, F_GETFD) & FD_CLOEXEC) != 0);

    /* Same-process double open: flock() treats two independently open()'d
     * descriptors on the same file as unrelated, even within one process,
     * so this genuinely exercises the conflict path without forking. */
    memset(reason, 0, sizeof(reason));
    int fd2 = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd2 == -1);
    assert(strcmp(reason, "stale-generation") == 0);

    char services_dir[600], service_dir[600], lease_path[600];
    jw__test_path(services_dir, sizeof(services_dir), runtime_dir, "services");
    jw__test_path(service_dir, sizeof(service_dir), services_dir, "org.umrk.test");
    jw__test_path(lease_path, sizeof(lease_path), service_dir, "generation.lease");

    /* Keep an independent reference to the original inode after releasing
     * the flock. If the implementation unlinks and recreates the pathname,
     * the still-open old inode cannot be recycled and this comparison must
     * differ; a bare close/reopen comparison can miss immediate inode-number
     * reuse. */
    int observer_fd = open(lease_path, O_RDONLY);
    assert(observer_fd >= 0);
    ino_t inode_before = jw__test_fd_inode(observer_fd);
    assert(close(fd1) == 0); /* releases the flock: last (only) holder closed it */

    memset(reason, 0, sizeof(reason));
    int fd3 = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd3 >= 0);
    /* The lease file must never be recreated -- same inode across the
     * release/reacquire cycle, not a fresh file that happens to have the
     * same path. */
    assert(jw__test_fd_inode(fd3) == inode_before);
    assert(close(fd3) == 0);
    assert(close(observer_fd) == 0);

    puts("PASS lease-test basic acquire, same-process contention, inode preserved");
}

static void jw__test_invalid_service_ids(void) {
    char runtime_dir[512]; jw__test_mkdtemp("invalid-id", runtime_dir, sizeof(runtime_dir));
    char reason[JW_TEST_REASON_BUF];

    static const char *bad_ids[] = {"", "a/b", "..", ".", "/etc"};
    for (size_t i = 0; i < sizeof(bad_ids) / sizeof(bad_ids[0]); i++) {
        memset(reason, 0, sizeof(reason));
        int fd = jw_svc_lease_acquire(runtime_dir, bad_ids[i], reason, sizeof(reason));
        if (fd != -1 || strcmp(reason, "invalid-service-id") != 0) {
            fprintf(stderr, "FAIL invalid service_id %s: fd=%d reason=%s\n",
                    bad_ids[i], fd, reason);
            assert(false);
        }
    }

    memset(reason, 0, sizeof(reason));
    int fd_null = jw_svc_lease_acquire(runtime_dir, NULL, reason, sizeof(reason));
    assert(fd_null == -1);
    assert(strcmp(reason, "invalid-service-id") == 0);

    puts("PASS lease-test rejects invalid service ids");
}

static void jw__test_runtime_dir_unavailable(void) {
    char reason[JW_TEST_REASON_BUF] = {0};
    int fd = jw_svc_lease_acquire("/nonexistent/jw-lease-test-root", "org.umrk.test",
                                  reason, sizeof(reason));
    assert(fd == -1);
    assert(strcmp(reason, "runtime-dir-unavailable") == 0);

    memset(reason, 0, sizeof(reason));
    fd = jw_svc_lease_acquire("", "org.umrk.test", reason, sizeof(reason));
    assert(fd == -1);
    assert(strcmp(reason, "runtime-dir-unavailable") == 0);

    memset(reason, 0, sizeof(reason));
    fd = jw_svc_lease_acquire(NULL, "org.umrk.test", reason, sizeof(reason));
    assert(fd == -1);
    assert(strcmp(reason, "runtime-dir-unavailable") == 0);
    puts("PASS lease-test rejects an unavailable runtime_dir");
}

static void jw__test_directories_are_owner_only(void) {
    char runtime_dir[512]; jw__test_mkdtemp("perms", runtime_dir, sizeof(runtime_dir));
    char reason[JW_TEST_REASON_BUF] = {0};
    int fd = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd >= 0);

    char services_dir[600], service_dir[600], lease_path[600];
    jw__test_path(services_dir, sizeof(services_dir), runtime_dir, "services");
    jw__test_path(service_dir, sizeof(service_dir), services_dir, "org.umrk.test");
    jw__test_path(lease_path, sizeof(lease_path), service_dir, "generation.lease");

    struct stat st;
    assert(lstat(services_dir, &st) == 0);
    assert(S_ISDIR(st.st_mode));
    assert(st.st_uid == geteuid());
    assert((st.st_mode & 0777) == 0700);
    assert(lstat(service_dir, &st) == 0);
    assert(S_ISDIR(st.st_mode));
    assert(st.st_uid == geteuid());
    assert((st.st_mode & 0777) == 0700);
    assert(lstat(lease_path, &st) == 0);
    assert(S_ISREG(st.st_mode));
    assert(st.st_uid == geteuid());
    assert((st.st_mode & 0777) == 0600);

    assert(close(fd) == 0);
    puts("PASS lease-test creates owner-only directories");
}

static void jw__test_rejects_unsafe_existing_directories(void) {
    char reason[JW_TEST_REASON_BUF] = {0};
    char runtime_dir[512], services_dir[600], service_dir[600];

    jw__test_mkdtemp("unsafe-services-mode", runtime_dir, sizeof(runtime_dir));
    jw__test_path(services_dir, sizeof(services_dir), runtime_dir, "services");
    assert(mkdir(services_dir, 0700) == 0);
    assert(chmod(services_dir, 0777) == 0);
    int fd = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd == -1);
    assert(strcmp(reason, "mkdir-failed") == 0);

    jw__test_mkdtemp("unsafe-service-mode", runtime_dir, sizeof(runtime_dir));
    jw__test_path(services_dir, sizeof(services_dir), runtime_dir, "services");
    jw__test_path(service_dir, sizeof(service_dir), services_dir, "org.umrk.test");
    assert(mkdir(services_dir, 0700) == 0);
    assert(mkdir(service_dir, 0700) == 0);
    assert(chmod(service_dir, 0777) == 0);
    memset(reason, 0, sizeof(reason));
    fd = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd == -1);
    assert(strcmp(reason, "mkdir-failed") == 0);

    puts("PASS lease-test rejects unsafe existing directory permissions");
}

static void jw__test_rejects_symlinked_paths(void) {
    char reason[JW_TEST_REASON_BUF] = {0};
    char runtime_dir[512], outside_dir[512];
    char services_dir[600], service_dir[600], lease_path[600], outside_child[600];

    jw__test_mkdtemp("services-symlink", runtime_dir, sizeof(runtime_dir));
    jw__test_mkdtemp("services-target", outside_dir, sizeof(outside_dir));
    jw__test_path(services_dir, sizeof(services_dir), runtime_dir, "services");
    assert(symlink(outside_dir, services_dir) == 0);
    int fd = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd == -1);
    assert(strcmp(reason, "mkdir-failed") == 0);
    jw__test_path(outside_child, sizeof(outside_child), outside_dir, "org.umrk.test");
    assert(lstat(outside_child, &(struct stat){0}) != 0 && errno == ENOENT);

    jw__test_mkdtemp("service-symlink", runtime_dir, sizeof(runtime_dir));
    jw__test_mkdtemp("service-target", outside_dir, sizeof(outside_dir));
    jw__test_path(services_dir, sizeof(services_dir), runtime_dir, "services");
    jw__test_path(service_dir, sizeof(service_dir), services_dir, "org.umrk.test");
    assert(mkdir(services_dir, 0700) == 0);
    assert(symlink(outside_dir, service_dir) == 0);
    memset(reason, 0, sizeof(reason));
    fd = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd == -1);
    assert(strcmp(reason, "mkdir-failed") == 0);
    jw__test_path(outside_child, sizeof(outside_child), outside_dir, "generation.lease");
    assert(lstat(outside_child, &(struct stat){0}) != 0 && errno == ENOENT);

    jw__test_mkdtemp("lease-symlink", runtime_dir, sizeof(runtime_dir));
    jw__test_mkdtemp("lease-target-dir", outside_dir, sizeof(outside_dir));
    jw__test_path(services_dir, sizeof(services_dir), runtime_dir, "services");
    jw__test_path(service_dir, sizeof(service_dir), services_dir, "org.umrk.test");
    jw__test_path(lease_path, sizeof(lease_path), service_dir, "generation.lease");
    jw__test_path(outside_child, sizeof(outside_child), outside_dir, "target");
    assert(mkdir(services_dir, 0700) == 0);
    assert(mkdir(service_dir, 0700) == 0);
    int target_fd = open(outside_child, O_RDWR | O_CREAT, 0600);
    assert(target_fd >= 0);
    assert(close(target_fd) == 0);
    assert(symlink(outside_child, lease_path) == 0);
    memset(reason, 0, sizeof(reason));
    fd = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd == -1);
    assert(strcmp(reason, "open-failed") == 0);

    puts("PASS lease-test rejects symlinked lease paths");
}

static void jw__test_rejects_non_regular_or_unsafe_lease_files(void) {
    char reason[JW_TEST_REASON_BUF] = {0};
    char runtime_dir[512], services_dir[600], service_dir[600], lease_path[600];

    jw__test_mkdtemp("lease-fifo", runtime_dir, sizeof(runtime_dir));
    jw__test_path(services_dir, sizeof(services_dir), runtime_dir, "services");
    jw__test_path(service_dir, sizeof(service_dir), services_dir, "org.umrk.test");
    jw__test_path(lease_path, sizeof(lease_path), service_dir, "generation.lease");
    assert(mkdir(services_dir, 0700) == 0);
    assert(mkdir(service_dir, 0700) == 0);
    assert(mkfifo(lease_path, 0600) == 0);
    int fd = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd == -1);
    assert(strcmp(reason, "open-failed") == 0);

    jw__test_mkdtemp("lease-mode", runtime_dir, sizeof(runtime_dir));
    jw__test_path(services_dir, sizeof(services_dir), runtime_dir, "services");
    jw__test_path(service_dir, sizeof(service_dir), services_dir, "org.umrk.test");
    jw__test_path(lease_path, sizeof(lease_path), service_dir, "generation.lease");
    assert(mkdir(services_dir, 0700) == 0);
    assert(mkdir(service_dir, 0700) == 0);
    fd = open(lease_path, O_RDWR | O_CREAT, 0600);
    assert(fd >= 0);
    assert(close(fd) == 0);
    assert(chmod(lease_path, 0666) == 0);
    memset(reason, 0, sizeof(reason));
    fd = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd == -1);
    assert(strcmp(reason, "open-failed") == 0);

    puts("PASS lease-test rejects unsafe existing lease files");
}

/* The scenario contracts.md's daemon-restart-overlap section exists for:
 * a real separate process (a stand-in for an old Jawaka generation, or a
 * cooperating service descendant) holds the lease; a new attempt must see
 * stale-generation while it lives, and must succeed only once it is
 * actually gone -- not merely once some fixed time has elapsed. */
static void jw__test_cross_process_contention(void) {
    char runtime_dir[512]; jw__test_mkdtemp("cross-proc", runtime_dir, sizeof(runtime_dir));

    int ready_pipe[2];
    assert(pipe(ready_pipe) == 0);

    pid_t holder = fork();
    assert(holder >= 0);
    if (holder == 0) {
        close(ready_pipe[0]);
        char reason[JW_TEST_REASON_BUF] = {0};
        int fd = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
        char result = (fd >= 0) ? 'Y' : 'N';
        assert(write(ready_pipe[1], &result, 1) == 1);
        close(ready_pipe[1]);
        if (fd < 0) {
            _exit(1);
        }
        pause(); /* hold the lease open until killed */
        _exit(0);
    }

    close(ready_pipe[1]);
    char result = '\0';
    assert(read(ready_pipe[0], &result, 1) == 1);
    close(ready_pipe[0]);
    assert(result == 'Y');

    char reason[JW_TEST_REASON_BUF] = {0};
    int fd_contended = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd_contended == -1);
    assert(strcmp(reason, "stale-generation") == 0);

    assert(kill(holder, SIGKILL) == 0);
    int status = 0;
    assert(waitpid(holder, &status, 0) == holder);
    /* The kernel releases flock()s held by a process automatically when
     * its last descriptor closes, including on process death -- no
     * separate cleanup step is needed or expected here. */

    memset(reason, 0, sizeof(reason));
    int fd_after = jw_svc_lease_acquire(runtime_dir, "org.umrk.test", reason, sizeof(reason));
    assert(fd_after >= 0);
    assert(close(fd_after) == 0);

    puts("PASS lease-test cross-process contention: stale while held, "
         "acquirable once the holder is gone");
}

int main(void) {
    jw__test_basic_acquire_and_contention();
    jw__test_invalid_service_ids();
    jw__test_runtime_dir_unavailable();
    jw__test_directories_are_owner_only();
    jw__test_rejects_unsafe_existing_directories();
    jw__test_rejects_symlinked_paths();
    jw__test_rejects_non_regular_or_unsafe_lease_files();
    jw__test_cross_process_contention();
    puts("PASS lease-test");
    return 0;
}
