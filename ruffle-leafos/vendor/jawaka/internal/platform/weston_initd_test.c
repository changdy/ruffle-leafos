/* Runs the real S49weston command strings through the real helpers against a
   fake init script, from a jawakad-like process: card cwd, card LD_LIBRARY_PATH,
   session settings, and a card file open without O_CLOEXEC. */
#include "internal/platform/weston_initd.h"
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Not assert(): several checks carry the side effect under test. */
#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "weston-initd-test: %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
} while (0)

static char g_root[] = "/tmp/jawaka-weston-initd-XXXXXX";
static char g_script[PATH_MAX];
static char g_out[PATH_MAX];

/* Swap the stock script path for the fake one, keeping the rest verbatim. */
static void with_fake_script(const char *command, char *buf, size_t size) {
    const char *at = strstr(command, JW_WESTON_INITD_SCRIPT);
    CHECK(at);
    int n = snprintf(buf, size, "%.*s%s%s", (int)(at - command), command, g_script,
                     at + strlen(JW_WESTON_INITD_SCRIPT));
    CHECK(n > 0 && (size_t)n < size);
}

static void run(const char *command) {
    char buf[1024];
    with_fake_script(command, buf, sizeof(buf));
    unlink(g_out);
    CHECK(jw_weston_initd_run(buf) == 0);
}

/* The detached helper returns at once; the fake script publishes atomically. */
static void run_detached(const char *command) {
    char buf[1024];
    with_fake_script(command, buf, sizeof(buf));
    unlink(g_out);
    CHECK(jw_weston_initd_spawn_detached(buf) == 0);
    struct timespec pause = { 0, 20 * 1000 * 1000 };
    for (int i = 0; i < 250 && access(g_out, F_OK) != 0; i++) {
        nanosleep(&pause, NULL);
    }
    CHECK(access(g_out, F_OK) == 0);
}

static char *slurp(void) {
    static char text[16384];
    FILE *f = fopen(g_out, "r");
    CHECK(f);
    size_t n = fread(text, 1, sizeof(text) - 1, f);
    fclose(f);
    text[n] = '\0';
    return text;
}

static int has_line(const char *text, const char *line) {
    size_t len = strlen(line);
    for (const char *p = text; (p = strstr(p, line)); p += len) {
        if ((p == text || p[-1] == '\n') && (p[len] == '\n' || p[len] == '\0')) return 1;
    }
    return 0;
}

static int has_name(const char *text, const char *name) {
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "%s=", name);
    for (const char *p = text; (p = strstr(p, prefix)); p++) {
        if (p == text || p[-1] == '\n') return 1;
    }
    return 0;
}

/* What every started shell must look like. */
static void check_rootfs(const char *seen) {
    CHECK(has_line(seen, "cwd=/"));
    CHECK(!has_name(seen, "LD_LIBRARY_PATH"));
    CHECK(!has_name(seen, "LD_PRELOAD"));
    CHECK(has_line(seen, "inherited=closed"));
}

int main(void) {
    CHECK(mkdtemp(g_root));
    snprintf(g_script, sizeof(g_script), "%s/S49weston", g_root);
    snprintf(g_out, sizeof(g_out), "%s/seen", g_root);
    FILE *f = fopen(g_script, "w");
    CHECK(f);
    /* `true >&N` succeeds only while descriptor N is open in the script. Not
       `:`: a redirection error on a special builtin exits dash. */
    fprintf(f,
            "#!/bin/sh\n"
            "{ echo \"verb=$1\"; echo \"cwd=$(pwd)\";\n"
            "  if { true >&\"$LEAK_FD\"; } 2>/dev/null; then echo inherited=open;"
            " else echo inherited=closed; fi\n"
            "  env; } >'%s.tmp' && mv '%s.tmp' '%s'\n",
            g_out, g_out, g_out);
    fclose(f);
    CHECK(chmod(g_script, 0755) == 0);

    /* What jawakad carries on the device. */
    CHECK(chdir(g_root) == 0);
    CHECK(setenv("LD_LIBRARY_PATH", "/media/sdcard1/.system/leaf/platforms/mlp1/launcher/lib:", 1) == 0);
    CHECK(setenv("LD_PRELOAD", "/media/sdcard1/shim.so", 1) == 0);
    CHECK(setenv("WESTON_DRM_PRIMARY", "DSI-1", 1) == 0);
    /* A descriptor another thread has open at fork time, as the content
       catalog had a pak's core open on the device. 9 because dash rejects
       multi-digit descriptors in a redirection. */
    int leaked = open(g_script, O_RDONLY);
    CHECK(leaked >= 0);
    CHECK(dup2(leaked, 9) == 9);
    if (leaked != 9) close(leaked);
    CHECK(setenv("LEAK_FD", "9", 1) == 0);

    const char *verbs[] = { "start", "stop", "restart" };
    const char *commands[] = { JW_WESTON_INITD("start"), JW_WESTON_INITD("stop"),
                               JW_WESTON_INITD("restart") };
    for (size_t i = 0; i < 3; i++) {
        run(commands[i]);
        char *seen = slurp();
        char verb[32];
        snprintf(verb, sizeof(verb), "verb=%s", verbs[i]);
        CHECK(has_line(seen, verb));
        check_rootfs(seen);
        /* The boot-time panel-first choice still reaches Weston. */
        CHECK(has_line(seen, "WESTON_DRM_PRIMARY=DSI-1"));
    }

    /* The detached restart used by the refresh-rate and HDMI actions. */
    run_detached(JW_WESTON_INITD("restart"));
    check_rootfs(slurp());

    /* The HDMI switch passes its Weston settings after the prefix. */
    run_detached(JW_WESTON_ROOTFS_ENV " WESTON_DRM_SINGLE_HEAD=1 WESTON_DRM_PRIMARY=HDMI-A-1 "
                 JW_WESTON_INITD_SCRIPT " restart");
    char *seen = slurp();
    CHECK(has_line(seen, "WESTON_DRM_SINGLE_HEAD=1"));
    CHECK(has_line(seen, "WESTON_DRM_PRIMARY=HDMI-A-1"));
    check_rootfs(seen);

    /* The descriptor stayed open here the whole time: only the children lost it. */
    CHECK(fcntl(9, F_GETFD) != -1);

    unlink(g_out);
    unlink(g_script);
    rmdir(g_root);
    printf("weston-initd-test: ok\n");
    return 0;
}
