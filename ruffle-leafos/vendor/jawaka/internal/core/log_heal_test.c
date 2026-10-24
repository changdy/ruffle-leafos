/* jw_log_heal_unwritable_stdio: the daemon must never hand a game child a
   stdout/stderr that cannot take a byte (the Leaf 0.11 session-log failure).
   Each case runs in its own child process because it rewires fd 1 and 2. */

#include "internal/core/log.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char g_tmp[256];
static int g_failures = 0;

static int run_case(const char *name, int (*body)(void)) {
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        exit(2);
    }
    if (pid == 0) {
        _exit(body());
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL %s (status %d)\n", name, WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        g_failures++;
        return 1;
    }
    printf("ok   %s\n", name);
    return 0;
}

static int point_stdio_at(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0 || dup2(fd, STDOUT_FILENO) < 0 || dup2(fd, STDERR_FILENO) < 0) {
        return -1;
    }
    close(fd);
    return 0;
}

static int case_healthy(void) {
    char path[300];
    snprintf(path, sizeof(path), "%s/healthy.out", g_tmp);
    if (point_stdio_at(path) != 0) return 10;
    return jw_log_heal_unwritable_stdio(g_tmp, "healthy") == 0 ? 0 : 11;
}

static int case_closed_fds_use_fallback(void) {
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    if (jw_log_heal_unwritable_stdio(g_tmp, "closed") != 1) return 20;
    if (write(STDERR_FILENO, "after-heal\n", 11) != 11) return 21;
    if (write(STDOUT_FILENO, "stdout-too\n", 11) != 11) return 22;
    return 0;
}

static int case_read_only_fds_without_fallback_use_devnull(void) {
    int ro = open("/dev/null", O_RDONLY);
    if (ro < 0 || dup2(ro, STDOUT_FILENO) < 0 || dup2(ro, STDERR_FILENO) < 0) return 30;
    close(ro);
    if (jw_log_heal_unwritable_stdio(NULL, "unused") != 2) return 31;
    if (write(STDOUT_FILENO, "x", 1) != 1) return 32;
    if (write(STDERR_FILENO, "x", 1) != 1) return 33;
    return 0;
}

static int case_unusable_fallback_uses_devnull(void) {
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    if (jw_log_heal_unwritable_stdio("/nonexistent-leaf-log-heal/logs", "x") != 2) return 40;
    if (write(STDERR_FILENO, "x", 1) != 1) return 41;
    return 0;
}

static int case_only_the_broken_descriptor_moves(void) {
    char path[300];
    snprintf(path, sizeof(path), "%s/split.out", g_tmp);
    if (point_stdio_at(path) != 0) return 50;
    close(STDERR_FILENO);
    if (jw_log_heal_unwritable_stdio(g_tmp, "split") != 1) return 51;
    if (write(STDOUT_FILENO, "kept-stdout\n", 12) != 12) return 52;
    return 0;
}

static int file_contains(const char *path, const char *needle) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

int main(void) {
    const char *base = getenv("TMPDIR");
    snprintf(g_tmp, sizeof(g_tmp), "%s/jw-log-heal-XXXXXX", base && base[0] ? base : "/tmp");
    if (!mkdtemp(g_tmp)) {
        perror("mkdtemp");
        return 2;
    }

    run_case("healthy stdio is left alone", case_healthy);
    char path[300];

    if (run_case("closed stdio moves to the fallback log", case_closed_fds_use_fallback) == 0) {
        snprintf(path, sizeof(path), "%s/closed.log", g_tmp);
        if (!file_contains(path, "after-heal") || !file_contains(path, "stdout-too")) {
            printf("FAIL closed stdio: fallback log missing writes\n");
            g_failures++;
        }
    }
    run_case("read-only stdio without a fallback moves to /dev/null",
             case_read_only_fds_without_fallback_use_devnull);
    run_case("unusable fallback moves to /dev/null", case_unusable_fallback_uses_devnull);
    if (run_case("only the broken descriptor moves", case_only_the_broken_descriptor_moves) == 0) {
        snprintf(path, sizeof(path), "%s/split.out", g_tmp);
        if (!file_contains(path, "kept-stdout")) {
            printf("FAIL split: healthy stdout was repointed\n");
            g_failures++;
        }
    }

    char cmd[320];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_tmp);
    (void)system(cmd);
    if (g_failures) {
        printf("%d log-heal case(s) failed\n", g_failures);
        return 1;
    }
    printf("log-heal test: ok\n");
    return 0;
}
