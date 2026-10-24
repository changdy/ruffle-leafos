#include "cmd/jawaka-osd/osd_backend.h"

#include "cJSON.h"
#include "internal/core/log.h"
#include "internal/i18n/i18n.h"
#include "internal/ipc/ipc.h"
#include "internal/platform/paths.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;
/* A signal that lands between the g_stop check and poll() would otherwise
   wait for the next client, which may be never now that the loop has no
   timeout while nothing is shown. The handler writes here to end the wait. */
static int s_wake_pipe[2] = { -1, -1 };

/* Test hooks for the daemon's late-reply handling: delay a banner before it
   is submitted, or after submission before the reply. Unset in normal use. */
static int s_test_show_delay_ms;
static int s_test_reply_delay_ms;

static int jw__env_delay_ms(const char *name) {
    const char *value = getenv(name);
    if (!value || !value[0]) return 0;
    char *end = NULL;
    long ms = strtol(value, &end, 10);
    return (end && *end == '\0' && ms > 0 && ms <= 10000) ? (int)ms : 0;
}

static void jw__handle_signal(int signo) {
    (void)signo;
    g_stop = 1;
    if (s_wake_pipe[1] >= 0) {
        int saved = errno;
        ssize_t n = write(s_wake_pipe[1], "s", 1);
        (void)n;
        errno = saved;
    }
}

static void jw__wake_pipe_setup(void) {
    if (pipe(s_wake_pipe) != 0) {
        s_wake_pipe[0] = s_wake_pipe[1] = -1;
        return;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(s_wake_pipe[i], F_SETFD, FD_CLOEXEC);
        fcntl(s_wake_pipe[i], F_SETFL, fcntl(s_wake_pipe[i], F_GETFL) | O_NONBLOCK);
    }
}

static uint64_t jw__now_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int jw__reply_json(jw_ipc_client *client, cJSON *root) {
    char *json = cJSON_PrintUnformatted(root);
    if (!json) {
        cJSON_Delete(root);
        return -1;
    }

    int rc = jw_ipc_client_send(client, json, strlen(json));
    cJSON_free(json);
    cJSON_Delete(root);
    return rc;
}

static int jw__reply_ok(jw_ipc_client *client, const char *action) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "ok");
    cJSON_AddStringToObject(root, "action", action ? action : "");
    return jw__reply_json(client, root);
}

static int jw__reply_error(jw_ipc_client *client, const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "error");
    cJSON_AddStringToObject(root, "message", message ? message : "osd error");
    return jw__reply_json(client, root);
}

static int jw__handle_message(jw_ipc_client *client, const char *body) {
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        return jw__reply_error(client, "invalid json");
    }

    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || !type->valuestring) {
        cJSON_Delete(root);
        return jw__reply_error(client, "missing type");
    }

    if (strcmp(type->valuestring, "show-brightness") == 0) {
        cJSON *percent = cJSON_GetObjectItemCaseSensitive(root, "percent");
        if (!cJSON_IsNumber(percent)) {
            cJSON_Delete(root);
            return jw__reply_error(client, "missing brightness percent");
        }
        int rc = jw_osd_backend_show_brightness(percent->valueint, jw__now_ms());
        cJSON_Delete(root);
        return rc == 0 ? jw__reply_ok(client, "show-brightness")
                       : jw__reply_error(client, "could not show brightness");
    }

    if (strcmp(type->valuestring, "show-volume") == 0) {
        cJSON *percent = cJSON_GetObjectItemCaseSensitive(root, "percent");
        if (!cJSON_IsNumber(percent)) {
            cJSON_Delete(root);
            return jw__reply_error(client, "missing volume percent");
        }
        int rc = jw_osd_backend_show_volume(percent->valueint, jw__now_ms());
        cJSON_Delete(root);
        return rc == 0 ? jw__reply_ok(client, "show-volume")
                       : jw__reply_error(client, "could not show volume");
    }

    if (strcmp(type->valuestring, "show-game-launch") == 0) {
        jw_osd_game_stage stage;
        int pending_items = 0;
        uint64_t expires_ms = 0;
        if (!jw_osd_game_launch_parse(root, &stage, &pending_items, &expires_ms)) {
            cJSON_Delete(root);
            return jw__reply_error(client, "invalid game launch status");
        }
        cJSON_Delete(root);
        if (s_test_show_delay_ms) usleep((useconds_t)s_test_show_delay_ms * 1000u);
        /* The daemon has already stopped waiting and disarmed: a prompt shown
           now would ask for a press that can no longer do anything. */
        if (expires_ms && jw__now_ms() >= expires_ms) {
            jw_log_warn("osd: dropped expired %s banner", jw_osd_game_stage_name(stage));
            return jw__reply_error(client, "expired");
        }
        /* The daemon arms the PICO-8 exit confirmation only on ok, so a
           backend that could not submit the banner must say so. */
        int rc = jw_osd_backend_show_game_launch(stage, pending_items, jw__now_ms());
        if (s_test_reply_delay_ms) usleep((useconds_t)s_test_reply_delay_ms * 1000u);
        return rc == 0 ? jw__reply_ok(client, "show-game-launch")
                       : jw__reply_error(client, "could not show game launch status");
    }

    if (strcmp(type->valuestring, "hide-game-launch") == 0) {
        if (cJSON_GetArraySize(root) != 1) {
            cJSON_Delete(root);
            return jw__reply_error(client, "invalid hide game launch request");
        }
        jw_osd_backend_hide_game_launch();
        cJSON_Delete(root);
        return jw__reply_ok(client, "hide-game-launch");
    }

    if (strcmp(type->valuestring, "shutdown") == 0) {
        g_stop = 1;
        cJSON_Delete(root);
        return jw__reply_ok(client, "shutdown");
    }

    cJSON_Delete(root);
    return jw__reply_error(client, "unknown type");
}

/* Sleeps until a client connects, the backend has events, a signal arrives,
   or the view's timer (a level toast ending) runs out. With nothing timed on
   screen there is no timeout at all. Returns true when a client is waiting. */
static bool jw__wait(jw_ipc_server *server) {
    struct pollfd fds[3];
    nfds_t count = 0;
    fds[count++] = (struct pollfd){ .fd = jw_ipc_server_fd(server), .events = POLLIN };
    int backend_fd = jw_osd_backend_event_fd();
    if (backend_fd >= 0) {
        fds[count++] = (struct pollfd){ .fd = backend_fd, .events = POLLIN };
    }
    if (s_wake_pipe[0] >= 0) {
        fds[count++] = (struct pollfd){ .fd = s_wake_pipe[0], .events = POLLIN };
    }
    int timeout = jw_osd_backend_timeout_ms(jw__now_ms());
    if (s_wake_pipe[0] < 0 && (timeout < 0 || timeout > 1000)) {
        timeout = 1000;   /* no pipe: a missed signal waits at most a second */
    }
    int ready = poll(fds, count, timeout);
    if (ready < 0 && errno != EINTR) {
        jw_log_warn("osd: poll failed: %s", strerror(errno));
        usleep(50000);
        return false;
    }
    return ready > 0 && (fds[0].revents & POLLIN);
}

/* Readiness is one byte on the pipe the daemon passed down, written only once
   fonts, the backend and the socket are all up. */
static void jw__announce_ready(void) {
    const char *value = getenv("JAWAKA_OSD_READY_FD");
    if (!value || !value[0]) return;
    char *end = NULL;
    long fd = strtol(value, &end, 10);
    unsetenv("JAWAKA_OSD_READY_FD");
    if (!end || *end != '\0' || fd < 3 || fd > 65535) return;
    ssize_t n;
    do {
        n = write((int)fd, "R", 1);
    } while (n < 0 && errno == EINTR);
    close((int)fd);
}

int main(void) {
    uint64_t started_ms = jw__now_ms();
    jw__wake_pipe_setup();
    signal(SIGINT, jw__handle_signal);
    signal(SIGTERM, jw__handle_signal);
    signal(SIGPIPE, SIG_IGN);

    char *socket_path = jw_osd_socket_path();
    if (!socket_path) {
        jw_log_error("osd: could not resolve socket path");
        return 1;
    }

    s_test_show_delay_ms = jw__env_delay_ms("JAWAKA_OSD_TEST_SHOW_DELAY_MS");
    s_test_reply_delay_ms = jw__env_delay_ms("JAWAKA_OSD_TEST_REPLY_DELAY_MS");
    const char *language = getenv("UMRK_LANGUAGE");
    if (!language || !language[0]) language = getenv("JAWAKA_LANGUAGE");
    (void)jw_i18n_load(language);

    if (jw_osd_backend_init() != 0) {
        jw_log_error("osd: backend init failed");
        free(socket_path);
        return 1;
    }

    jw_ipc_server *server = NULL;
    if (jw_ipc_server_listen(socket_path, &server) != 0) {
        jw_log_error("osd: could not bind socket: %s", socket_path);
        jw_osd_backend_shutdown();
        free(socket_path);
        return 1;
    }

    jw__announce_ready();
    jw_log_info("osd: listening on %s language=%s ready_ms=%llu", socket_path,
                jw_i18n_language(), (unsigned long long)(jw__now_ms() - started_ms));
    while (!g_stop) {
        jw_osd_backend_tick(jw__now_ms());
        if (!jw__wait(server)) {
            continue;
        }

        jw_ipc_client *client = NULL;
        int rc = jw_ipc_server_accept(server, &client, 0);
        if (rc == 1) {
            continue;
        }
        if (rc < 0) {
            jw_log_warn("osd: accept failed");
            continue;
        }

        char *body = NULL;
        size_t len = 0;
        if (jw_ipc_client_recv(client, &body, &len) == 0) {
            jw__handle_message(client, body);
        }
        free(body);
        jw_ipc_client_close(client);
    }

    jw_ipc_server_close(server);
    jw_osd_backend_shutdown();
    for (int i = 0; i < 2; i++) {
        if (s_wake_pipe[i] >= 0) close(s_wake_pipe[i]);
    }
    jw_i18n_shutdown();
    free(socket_path);
    return 0;
}
