#define _POSIX_C_SOURCE 200809L

/* Host checks for the RAOfflineProxy transient launch bridge:
 *  - proxied prepare -> inject -> backup restores the exact prior shared
 *    lines (foreign/empty custom host, true/false Hardcore) byte-identically;
 *  - injected values and RA-derived cheevos_token never reach the shared cfg;
 *  - direct (NULL / non-proxied) backups persist ordinary setting changes;
 *  - the bounded loopback health probe accepts only the fixed ready body and
 *    never overruns its timeout against a dead listener;
 *  - service liveness (the RetroArch gate's and the Flycast route's shared
 *    predicate) across every supervisor state, STARTING included, and the
 *    UMRK_FLYCAST_RA_ROUTE value the bundled Flycast child gets for each.
 */

#include "internal/platform/paths.h"
#include "internal/platform/raofflineproxy.h"
#include "internal/launcher/ra_account.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <netinet/in.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int fail(const char *message) {
    fprintf(stderr, "raofflineproxy-bridge-test: %s\n", message);
    return 1;
}

static int mkdir_one(const char *path) {
    return (mkdir(path, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int write_text(const char *path, const char *text) {
    FILE *fp = fopen(path, "wb");
    if (!fp) return -1;
    size_t len = strlen(text);
    return fwrite(text, 1, len, fp) == len && fclose(fp) == 0 ? 0 : -1;
}

static int append_text(const char *path, const char *text) {
    FILE *fp = fopen(path, "ab");
    if (!fp) return -1;
    size_t len = strlen(text);
    return fwrite(text, 1, len, fp) == len && fclose(fp) == 0 ? 0 : -1;
}

static char *read_text(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp || fseek(fp, 0, SEEK_END) != 0) {
        if (fp) fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0 || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *text = calloc((size_t)size + 1u, 1u);
    if (!text || fread(text, 1, (size_t)size, fp) != (size_t)size) {
        free(text);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    return text;
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000ll + ts.tv_nsec / 1000000ll;
}

/* -- fake health endpoint ------------------------------------------------ */

typedef struct {
    int listen_fd;
    const char *response;  /* NULL = accept and stay silent */
} fake_health_server;

static void *fake_health_server_run(void *arg) {
    fake_health_server *server = arg;
    int client = accept(server->listen_fd, NULL, NULL);
    if (client < 0) {
        return NULL;
    }
    /* recv() does not NUL-terminate, so the buffer must be terminated before
     * any string walk -- and the header terminator can straddle two reads, so
     * scanning only the latest chunk can miss it. Accumulate, terminate, then
     * search. */
    char drain[1024];
    size_t drained = 0;
    struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    for (;;) {
        if (drained + 1 >= sizeof(drain)) {
            break;
        }
        ssize_t n = recv(client, drain + drained, sizeof(drain) - drained - 1, 0);
        if (n <= 0) {
            break;
        }
        drained += (size_t)n;
        drain[drained] = '\0';
        if (strstr(drain, "\r\n\r\n") != NULL) {
            break;
        }
    }
    if (server->response) {
        (void)send(client, server->response, strlen(server->response), 0);
    }
    /* Silent mode: hold the connection briefly so the probe's own timeout is
     * what decides, not an early close. */
    struct timespec hold = { .tv_sec = 1, .tv_nsec = 0 };
    nanosleep(&hold, NULL);
    close(client);
    return NULL;
}

static int fake_health_server_start(fake_health_server *server,
                                    const char *response,
                                    uint16_t *out_port,
                                    pthread_t *out_thread) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(0x7F000001u); /* 127.0.0.1 */
    addr.sin_port = 0;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
        listen(fd, 4) != 0) {
        close(fd);
        return -1;
    }
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) {
        close(fd);
        return -1;
    }
    server->listen_fd = fd;
    server->response = response;
    *out_port = ntohs(addr.sin_port);
    if (pthread_create(out_thread, NULL, fake_health_server_run, server) != 0) {
        close(fd);
        return -1;
    }
    return 0;
}

static const char ready_response[] =
    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
    "Content-Length: 85\r\nConnection: close\r\n\r\n"
    "{\"service\":\"org.umrk.raofflineproxy\",\"protocol\":\"leaf-health-1\",\"ready\":true}";

static int test_health_probe(void) {
    fake_health_server server;
    uint16_t port = 0;
    pthread_t thread;

    if (fake_health_server_start(&server, ready_response, &port, &thread) != 0) {
        return fail("could not start ready fake server");
    }
    bool ready = jw_raofflineproxy_health_ready("127.0.0.1", port, 500);
    pthread_join(thread, NULL);
    close(server.listen_fd);
    if (!ready) {
        return fail("fixed ready body was rejected");
    }

    static const char not_ready_response[] =
        "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 86\r\n"
        "Connection: close\r\n\r\n"
        "{\"service\":\"org.umrk.raofflineproxy\",\"protocol\":\"leaf-health-1\",\"ready\":false}";
    if (fake_health_server_start(&server, not_ready_response, &port, &thread) != 0) {
        return fail("could not start not-ready fake server");
    }
    ready = jw_raofflineproxy_health_ready("127.0.0.1", port, 500);
    pthread_join(thread, NULL);
    close(server.listen_fd);
    if (ready) {
        return fail("ready:false body was accepted");
    }

    /* A foreign service id is rejected even with ready:true. */
    static const char foreign_response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 64\r\nConnection: close\r\n\r\n"
        "{\"service\":\"org.example.other\",\"protocol\":\"leaf-health-1\",\"ready\":true}";
    if (fake_health_server_start(&server, foreign_response, &port, &thread) != 0) {
        return fail("could not start foreign fake server");
    }
    ready = jw_raofflineproxy_health_ready("127.0.0.1", port, 500);
    pthread_join(thread, NULL);
    close(server.listen_fd);
    if (ready) {
        return fail("foreign service id was accepted");
    }

    /* Dead listener: probe must refuse fast, not wait out the budget. */
    ready = jw_raofflineproxy_health_ready("127.0.0.1", port, 500);
    if (ready) {
        return fail("unbound port was accepted");
    }

    /* Silent listener: never overruns its timeout budget. */
    if (fake_health_server_start(&server, NULL, &port, &thread) != 0) {
        return fail("could not start silent fake server");
    }
    long long start = now_ms();
    ready = jw_raofflineproxy_health_ready("127.0.0.1", port, 300);
    long long elapsed = now_ms() - start;
    pthread_join(thread, NULL);
    close(server.listen_fd);
    if (ready) {
        return fail("silent server was accepted");
    }
    if (elapsed < 250 || elapsed > 1500) {
        fprintf(stderr, "silent probe elapsed=%lldms\n", elapsed);
        return fail("silent probe did not respect its timeout budget");
    }
    return 0;
}

/* -- snapshot / transient backup ----------------------------------------- */

static int count_lines_with(const char *text, const char *needle) {
    int count = 0;
    size_t needle_len = strlen(needle);
    for (const char *line = text; line && *line;) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        if (len >= needle_len && strncmp(line, needle, needle_len) == 0) {
            count++;
        }
        line = next ? next + 1 : NULL;
    }
    return count;
}

static bool has_line(const char *text, const char *exact_line) {
    size_t want = strlen(exact_line);
    for (const char *line = text; line && *line;) {
        const char *next = strchr(line, '\n');
        size_t len = next ? (size_t)(next - line) : strlen(line);
        if (len == want && strncmp(line, exact_line, len) == 0) {
            return true;
        }
        line = next ? next + 1 : NULL;
    }
    return false;
}

/* -- service liveness and the bundled Flycast route ---------------------- */

static int write_record(const char *dir, const char *name, const char *text) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    return write_text(path, text);
}

static int test_liveness_and_flycast_route(void) {
    char root[] = "/tmp/jw-rop-route-XXXXXX";
    if (!mkdtemp(root)) return fail("route mkdtemp failed");
    char platform[PATH_MAX], emulators[PATH_MAX], flycast[PATH_MAX];
    char launcher[PATH_MAX];
    snprintf(platform, sizeof(platform), "%s/platform", root);
    snprintf(emulators, sizeof(emulators), "%s/emulators", platform);
    snprintf(flycast, sizeof(flycast), "%s/flycast", emulators);
    snprintf(launcher, sizeof(launcher), "%s/launch.sh", flycast);
    if (mkdir_one(platform) || mkdir_one(emulators) || mkdir_one(flycast) ||
        write_text(launcher, "#!/bin/sh\n") ||
        write_record(flycast, "ra-account-v1", "standalone-ra-account-v1\n") ||
        write_record(flycast, "ra-route-v1", "umrk-flycast-ra-route-v1\n")) {
        return fail("route fixture write failed");
    }

    /* The bundled Flycast child: the release-owned launcher with both
       capability records. */
    const jw_standalone_policy flycast_policy = {
        .provider_bound = false, .release = JW_STANDALONE_RELEASE_FLYCAST,
    };
    if (!jw_flycast_ra_route_target_authorized(launcher, "flycast_standalone",
                                               &flycast_policy, NULL, platform)) {
        return fail("bundled Flycast child is not route-authorized");
    }

    static const struct {
        jw_svc_effective_state state;
        pid_t pgid;
        bool pak_present;
        bool manifest_valid;
        bool live;
        const char *what;
    } cases[] = {
        { JW_SVC_STATE_RUNNING,          4242, true,  true,  true,  "running" },
        { JW_SVC_STATE_STARTING,         4242, true,  true,  true,  "starting" },
        { JW_SVC_STATE_STARTING,         0,    true,  true,  false, "starting without a group" },
        { JW_SVC_STATE_RUNNING,          0,    true,  true,  false, "running without a group" },
        { JW_SVC_STATE_STOPPING,         4242, true,  true,  false, "stopping" },
        { JW_SVC_STATE_STOPPED,          0,    true,  true,  false, "stopped" },
        { JW_SVC_STATE_DISABLED,         0,    true,  true,  false, "disabled" },
        { JW_SVC_STATE_BACKOFF,          0,    true,  true,  false, "backoff" },
        { JW_SVC_STATE_FAILED,           0,    true,  true,  false, "failed" },
        { JW_SVC_STATE_STALE_GENERATION, 4242, true,  true,  false, "stale generation" },
        { JW_SVC_STATE_UNAVAILABLE,      0,    false, false, false, "unavailable" },
        { JW_SVC_STATE_RUNNING,          4242, false, true,  false, "running, pak gone" },
        { JW_SVC_STATE_STARTING,         4242, true,  false, false, "starting, manifest invalid" },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        jw_svc_supervised entry;
        memset(&entry, 0, sizeof(entry));
        entry.state = cases[i].state;
        entry.pgid = cases[i].pgid;
        entry.pak_present = cases[i].pak_present;
        entry.manifest_valid = cases[i].manifest_valid;
        bool live = jw_raofflineproxy_entry_live(&entry);
        const char *route = jw_flycast_ra_route_value(live);
        const char *want = cases[i].live ? "service-live" : "native";
        if (live != cases[i].live || strcmp(route, want) != 0) {
            fprintf(stderr, "raofflineproxy-bridge-test: %s: live=%d route=%s, "
                            "want live=%d route=%s\n",
                    cases[i].what, live, route, cases[i].live, want);
            return 1;
        }
    }
    if (jw_raofflineproxy_entry_live(NULL) ||
        strcmp(jw_flycast_ra_route_value(false), "native") != 0) {
        return fail("an absent service must route native");
    }

    /* A Flycast build without the route record keeps its native path however
       live the service is. */
    char record[PATH_MAX];
    snprintf(record, sizeof(record), "%s/ra-route-v1", flycast);
    unlink(record);
    if (jw_flycast_ra_route_target_authorized(launcher, "flycast_standalone",
                                              &flycast_policy, NULL, platform)) {
        return fail("account-only Flycast build was route-authorized");
    }
    return 0;
}

int main(void) {
    if (test_health_probe() != 0) {
        return 1;
    }
    if (test_liveness_and_flycast_route() != 0) {
        return 1;
    }

    char root[] = "/tmp/jw-rop-bridge-XXXXXX";
    int fd = mkstemp(root);
    if (fd < 0) return fail("mkstemp failed");
    close(fd);
    if (unlink(root) != 0 || mkdir_one(root) != 0) return fail("root mkdir failed");

    char platform[PATH_MAX], defaults[PATH_MAX], internal[PATH_MAX];
    char runtime[PATH_MAX], cores[PATH_MAX], shaders[PATH_MAX];
    char retroarch_dir[PATH_MAX], shared_cfg[PATH_MAX], default_cfg[PATH_MAX];
    snprintf(platform, sizeof(platform), "%s/platform", root);
    snprintf(defaults, sizeof(defaults), "%s/defaults", platform);
    snprintf(internal, sizeof(internal), "%s/internal", root);
    snprintf(runtime, sizeof(runtime), "%s/runtime", root);
    snprintf(cores, sizeof(cores), "%s/cores", platform);
    snprintf(shaders, sizeof(shaders), "%s/shaders", platform);
    snprintf(retroarch_dir, sizeof(retroarch_dir), "%s/retroarch", internal);
    snprintf(shared_cfg, sizeof(shared_cfg), "%s/retroarch.cfg", retroarch_dir);
    snprintf(default_cfg, sizeof(default_cfg), "%s/retroarch.cfg", defaults);
    if (mkdir_one(platform) || mkdir_one(defaults) || mkdir_one(internal) ||
        mkdir_one(runtime) || mkdir_one(cores) || mkdir_one(shaders) ||
        mkdir_one(retroarch_dir)) {
        return fail("fixture mkdir failed");
    }
    if (write_text(default_cfg, "video_vsync = \"true\"\n") != 0) {
        return fail("defaults write failed");
    }

    setenv("SDCARD_PATH", root, 1);
    setenv("UMRK_PLATFORM_PATH", platform, 1);
    setenv("UMRK_INTERNAL_DATA_PATH", internal, 1);
    setenv("UMRK_RETROARCH_SHADERS_DIR", shaders, 1);

    char core[PATH_MAX];
    snprintf(core, sizeof(core), "%s/mgba_libretro.so", cores);
    char error[256];

    /* Durable Hardcore flag is read from the shared config only. */
    if (write_text(shared_cfg,
                   "menu_driver = \"rgui\"\n"
                   "cheevos_custom_host = \"foreign.example:9999\"\n"
                   "cheevos_hardcore_mode_enable = \"true\"\n") != 0) {
        return fail("hardcore fixture write failed");
    }
    if (!jw_retroarch_shared_hardcore_enabled(root)) {
        return fail("durable Hardcore not detected");
    }

    /* Duplicate-order regression. RetroArch keeps the FIRST occurrence of a
     * key, so a shared config carrying Hardcore "true" before a later "false"
     * IS hardcore as far as RetroArch is concerned. A last-wins read reports
     * it off and routes a hardcore session through the casual-only proxy,
     * silently breaking the durable-Hardcore/direct-play guarantee. Duplicates
     * are reachable: the shared text is passed through undeduplicated, so
     * save-on-exit churn can leave two lines for one key. */
    if (write_text(shared_cfg,
                   "menu_driver = \"rgui\"\n"
                   "cheevos_hardcore_mode_enable = \"true\"\n"
                   "cheevos_hardcore_mode_enable = \"false\"\n") != 0) {
        return fail("duplicate hardcore fixture write failed");
    }
    if (!jw_retroarch_shared_hardcore_enabled(root)) {
        return fail("Hardcore gate honored the last duplicate, not the first");
    }
    /* And the mirror case: "false" first means Hardcore really is off, even
     * with a later "true" that RetroArch will ignore. */
    if (write_text(shared_cfg,
                   "menu_driver = \"rgui\"\n"
                   "cheevos_hardcore_mode_enable = \"false\"\n"
                   "cheevos_hardcore_mode_enable = \"true\"\n") != 0) {
        return fail("mirror duplicate hardcore fixture write failed");
    }
    if (jw_retroarch_shared_hardcore_enabled(root)) {
        return fail("Hardcore gate honored a later duplicate over the first");
    }

    /* Read failure must fail CLOSED. jw__read_text_file returns NULL for
     * "absent" and for "there but unreadable" alike, and the gate used to
     * report both as Hardcore off -- which routes a possibly-hardcore session
     * through the casual-only proxy and earns its achievements as casual.
     *
     * Oversize is the reachable form of unreadable and the one worth pinning:
     * RetroArch rewrites this file on every exit with every option it knows,
     * and the qualification device already sits at 112 KB. chmod would not do
     * as a fixture -- these tests can run as root, where it proves nothing. */
    {
        FILE *fp = fopen(shared_cfg, "wb");
        if (!fp) {
            return fail("oversize hardcore fixture open failed");
        }
        /* One byte past the reader's ceiling. */
        size_t target = (4u * 1024u * 1024u) + 1u;
        char chunk[4096];
        memset(chunk, 'x', sizeof(chunk));
        for (size_t written = 0; written < target; written += sizeof(chunk)) {
            if (fwrite(chunk, 1, sizeof(chunk), fp) != sizeof(chunk)) {
                fclose(fp);
                return fail("oversize hardcore fixture write failed");
            }
        }
        fclose(fp);
    }
    if (!jw_retroarch_shared_hardcore_enabled(root)) {
        return fail("unreadable shared config reported Hardcore off; "
                    "a hardcore session would route through the casual proxy");
    }
    {
        jw_shared_config_status status = JW_SHARED_CFG_OK;
        char *text = jw_retroarch_shared_config_read_status(root, &status);
        if (text || status != JW_SHARED_CFG_UNREADABLE) {
            free(text);
            return fail("oversize shared config not reported as unreadable");
        }
    }

    /* Absent is the opposite case and must stay open: a device with no shared
     * config has no durable Hardcore setting, so casual play may proxy. */
    if (remove(shared_cfg) != 0) {
        return fail("could not remove shared config fixture");
    }
    if (jw_retroarch_shared_hardcore_enabled(root)) {
        return fail("absent shared config reported Hardcore on");
    }
    {
        jw_shared_config_status status = JW_SHARED_CFG_OK;
        char *text = jw_retroarch_shared_config_read_status(root, &status);
        if (text || status != JW_SHARED_CFG_ABSENT) {
            free(text);
            return fail("absent shared config not reported as absent");
        }
    }

    /* Restore the single-value hardcore fixture for the cycle below. */
    if (write_text(shared_cfg,
                   "menu_driver = \"rgui\"\n"
                   "cheevos_custom_host = \"foreign.example:9999\"\n"
                   "cheevos_hardcore_mode_enable = \"true\"\n") != 0) {
        return fail("hardcore fixture restore failed");
    }

    /* Proxied launch cycle with foreign values: everything comes back
     * byte-identical, and neither the injected host, the RA-changed host, nor
     * the derived token ever persist. */
    char *shared_text = jw_retroarch_shared_config_read(root);
    if (!shared_text) {
        return fail("could not read shared config");
    }
    jw_retroarch_launch_snapshot snapshot;
    jw_retroarch_launch_snapshot_init(&snapshot);
    jw_retroarch_launch_snapshot_capture(&snapshot, shared_text);
    free(shared_text);
    if (!snapshot.custom_host_present || !snapshot.hardcore_present) {
        return fail("snapshot missed foreign shared lines");
    }
    snapshot.proxied = true;

    char *runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                    true, true,
                                                    error, sizeof(error));
    if (!runtime_cfg) {
        return fail(error[0] ? error : "prepare failed");
    }
    char *proxied_runtime = read_text(runtime_cfg);
    /* Count the KEY, not the proxy value. RetroArch keeps the first
     * occurrence of a key and drops later duplicates, so "the proxy line is
     * present" is not the property that matters -- "the proxy line is the
     * ONLY line for that key" is. Counting values instead of keys is exactly
     * how an override that never took effect could look correct here. */
    if (!proxied_runtime ||
        count_lines_with(proxied_runtime, "cheevos_custom_host") != 1 ||
        count_lines_with(proxied_runtime,
                         "cheevos_custom_host = \"127.0.0.1:8080\"") != 1 ||
        count_lines_with(proxied_runtime, "cheevos_hardcore_mode_enable") != 1 ||
        count_lines_with(proxied_runtime,
                         "cheevos_hardcore_mode_enable = \"false\"") != 1) {
        free(proxied_runtime);
        return fail("proxied runtime config must carry each cheevos key exactly once");
    }
    free(proxied_runtime);

    /* Emulate RetroArch save-on-exit churn: foreign host replaced, token
     * derived. None of it may persist. */
    if (append_text(runtime_cfg,
                    "cheevos_custom_host = \"changed-during-play:1\"\n"
                    "cheevos_hardcore_mode_enable = \"true\"\n"
                    "cheevos_token = \"derived-plaintext-token\"\n") != 0) {
        return fail("save-on-exit churn fixture failed");
    }
    if (jw_backup_retroarch_config(runtime_cfg, root, &snapshot,
                                   error, sizeof(error)) != 0) {
        return fail(error[0] ? error : "proxied backup failed");
    }
    unlink(runtime_cfg);
    free(runtime_cfg);

    char *shared = read_text(shared_cfg);
    if (!shared) {
        return fail("could not read restored shared config");
    }
    int ok =
        has_line(shared, "cheevos_custom_host = \"foreign.example:9999\"") &&
        has_line(shared, "cheevos_hardcore_mode_enable = \"true\"") &&
        count_lines_with(shared, "cheevos_custom_host") == 1 &&
        count_lines_with(shared, "cheevos_hardcore_mode_enable") == 1 &&
        !strstr(shared, "127.0.0.1:8080") &&
        !strstr(shared, "changed-during-play") &&
        count_lines_with(shared, "cheevos_token") == 0;
    free(shared);
    if (!ok) {
        return fail("proxied backup did not restore the exact shared lines");
    }

    /* Absent shared values stay absent through a proxied session. */
    if (write_text(shared_cfg, "menu_driver = \"rgui\"\n") != 0) {
        return fail("absent fixture write failed");
    }
    shared_text = jw_retroarch_shared_config_read(root);
    jw_retroarch_launch_snapshot_init(&snapshot);
    jw_retroarch_launch_snapshot_capture(&snapshot, shared_text ? shared_text : "");
    free(shared_text);
    if (snapshot.custom_host_present || snapshot.hardcore_present) {
        return fail("snapshot invented absent shared lines");
    }
    snapshot.proxied = true;
    runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                              true, true,
                                              error, sizeof(error));
    if (!runtime_cfg) {
        return fail("absent-case prepare/inject failed");
    }
    if (jw_backup_retroarch_config(runtime_cfg, root, &snapshot,
                                   error, sizeof(error)) != 0) {
        return fail("absent-case backup failed");
    }
    unlink(runtime_cfg);
    free(runtime_cfg);
    shared = read_text(shared_cfg);
    ok = shared &&
         count_lines_with(shared, "cheevos_custom_host") == 0 &&
         count_lines_with(shared, "cheevos_hardcore_mode_enable") == 0;
    free(shared);
    if (!ok) {
        return fail("absent shared values leaked through a proxied session");
    }

    /* Direct sessions (NULL snapshot, and non-proxied snapshots alike) keep
     * persisting ordinary changes: a user-chosen host survives; the derived
     * token still never reaches the shared config. The RetroArch pak runner
     * always takes exactly this path. */
    jw_retroarch_launch_snapshot_init(&snapshot);
    runtime_cfg = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                              true, false,
                                              error, sizeof(error));
    if (!runtime_cfg ||
        append_text(runtime_cfg,
                    "cheevos_custom_host = \"user-chose-this:7777\"\n"
                    "cheevos_hardcore_mode_enable = \"true\"\n"
                    "cheevos_token = \"derived-plaintext-token\"\n") != 0) {
        return fail("direct fixture failed");
    }
    if (jw_backup_retroarch_config(runtime_cfg, root, NULL,
                                   error, sizeof(error)) != 0 ||
        jw_backup_retroarch_config(runtime_cfg, root, &snapshot,
                                   error, sizeof(error)) != 0) {
        return fail("direct backup failed");
    }
    unlink(runtime_cfg);
    free(runtime_cfg);
    shared = read_text(shared_cfg);
    ok = shared &&
         has_line(shared, "cheevos_custom_host = \"user-chose-this:7777\"") &&
         has_line(shared, "cheevos_hardcore_mode_enable = \"true\"") &&
         count_lines_with(shared, "cheevos_token") == 0;
    free(shared);
    if (!ok) {
        return fail("direct session changes did not persist correctly");
    }

    printf("raofflineproxy-bridge-test: ok\n");
    return 0;
}
