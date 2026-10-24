#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/ipc/ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop = 0;

static void on_term(int signo) {
    (void)signo;
    g_stop = 1;
}

static int exchange(jw_ipc_client *client, const char *request,
                    const char *expected) {
    if (jw_ipc_client_send(client, request, strlen(request)) != 0) {
        return -1;
    }
    char *reply = NULL;
    size_t reply_len = 0;
    if (jw_ipc_client_recv(client, &reply, &reply_len) != 0) {
        return -1;
    }
    bool matches = reply_len == strlen(expected) &&
                   memcmp(reply, expected, reply_len) == 0;
    free(reply);
    return matches ? 0 : -1;
}

static int connect_and_subscribe(const char *socket_path, const char *service_id,
                                 const char *id, const char *mode,
                                 jw_ipc_client **out) {
    jw_ipc_client *client = NULL;
    if (jw_ipc_client_connect(socket_path, &client) != 0) {
        return -1;
    }
    char request[512];
    int n = snprintf(request, sizeof(request),
        "{\"v\":1,\"op\":\"subscribe\",\"id\":\"%s\","
        "\"events\":[\"game\"],\"service_id\":\"%s\",\"mode\":\"%s\","
        "\"ack_ms\":250,\"wait_ms\":0}", id, service_id, mode);
    char expected[128];
    int e = snprintf(expected, sizeof(expected),
                     "{\"v\":1,\"id\":\"%s\",\"ok\":true}", id);
    if (n < 0 || (size_t)n >= sizeof(request) ||
        e < 0 || (size_t)e >= sizeof(expected) ||
        exchange(client, request, expected) != 0 ||
        jw_ipc_client_set_nonblocking(client, false, 0) != 0) {
        jw_ipc_client_close(client);
        return -1;
    }
    *out = client;
    return 0;
}

static int connect_and_subscribe_wait(const char *socket_path,
                                      const char *service_id,
                                      int ack_ms, int wait_ms,
                                      jw_ipc_client **out) {
    jw_ipc_client *client = NULL;
    if (jw_ipc_client_connect(socket_path, &client) != 0) {
        return -1;
    }
    char request[512];
    int n = snprintf(request, sizeof(request),
        "{\"v\":1,\"op\":\"subscribe\",\"id\":\"1\","
        "\"events\":[\"game\"],\"service_id\":\"%s\",\"mode\":\"notify\","
        "\"ack_ms\":%d,\"wait_ms\":%d}", service_id, ack_ms, wait_ms);
    const char *expected = "{\"v\":1,\"id\":\"1\",\"ok\":true}";
    if (n < 0 || (size_t)n >= sizeof(request) ||
        exchange(client, request, expected) != 0 ||
        jw_ipc_client_set_nonblocking(client, false, 0) != 0) {
        jw_ipc_client_close(client);
        return -1;
    }
    *out = client;
    return 0;
}

static int connect_and_subscribe_check(const char *socket_path,
                                       const char *service_id,
                                       int ack_ms, int wait_ms,
                                       jw_ipc_client **out) {
    jw_ipc_client *client = NULL;
    if (jw_ipc_client_connect(socket_path, &client) != 0) {
        return -1;
    }
    char request[512];
    int n = snprintf(request, sizeof(request),
        "{\"v\":1,\"op\":\"subscribe\",\"id\":\"1\","
        "\"events\":[\"game\"],\"service_id\":\"%s\",\"mode\":\"stop\","
        "\"ack_ms\":%d,\"wait_ms\":%d,\"check_before_stop\":true}",
        service_id, ack_ms, wait_ms);
    const char *expected = "{\"v\":1,\"id\":\"1\",\"ok\":true}";
    if (n < 0 || (size_t)n >= sizeof(request) ||
        exchange(client, request, expected) != 0 ||
        jw_ipc_client_set_nonblocking(client, false, 0) != 0) {
        jw_ipc_client_close(client);
        return -1;
    }
    *out = client;
    return 0;
}

static int reconcile(jw_ipc_client *client, const char *id) {
    char request[128];
    char expected[128];
    int n = snprintf(request, sizeof(request),
                     "{\"v\":1,\"op\":\"game.state\",\"id\":\"%s\"}", id);
    int e = snprintf(expected, sizeof(expected),
                     "{\"v\":1,\"id\":\"%s\",\"active\":false}", id);
    return n > 0 && (size_t)n < sizeof(request) &&
           e > 0 && (size_t)e < sizeof(expected)
        ? exchange(client, request, expected) : -1;
}

static int write_result(const char *runtime, const char *body) {
    char result_path[4096];
    int n = snprintf(result_path, sizeof(result_path), "%s/life1-fixture-result",
                     runtime);
    if (n < 0 || (size_t)n >= sizeof(result_path)) {
        return -1;
    }
    int fd = open(result_path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return -1;
    }
    size_t len = strlen(body);
    ssize_t wrote = write(fd, body, len);
    int close_rc = close(fd);
    return wrote == (ssize_t)len && close_rc == 0 ? 0 : -1;
}

static int recv_json(jw_ipc_client *client, char **out) {
    char *body = NULL;
    size_t len = 0;
    if (jw_ipc_client_recv(client, &body, &len) != 0 || !body) {
        free(body);
        return -1;
    }
    char *copy = malloc(len + 1u);
    if (!copy) {
        free(body);
        return -1;
    }
    memcpy(copy, body, len);
    copy[len] = '\0';
    free(body);
    *out = copy;
    return 0;
}

static int json_launch_id(const char *json, char *out, size_t out_size) {
    const char *key = "\"launch_id\":\"";
    const char *start = json ? strstr(json, key) : NULL;
    if (!start) {
        return -1;
    }
    start += strlen(key);
    const char *end = strchr(start, '"');
    size_t len = end ? (size_t)(end - start) : 0;
    if (len == 0 || len >= out_size) {
        return -1;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

static int send_status(jw_ipc_client *client, const char *status,
                       const char *launch_id, int pending_items);

static int send_check_waiting(jw_ipc_client *client, const char *launch_id,
                              int pending_items, long long pending_bytes) {
    char json[320];
    int n = snprintf(json, sizeof(json),
                     "{\"v\":1,\"status\":\"waiting\",\"launch_id\":\"%s\","
                     "\"pending_items\":%d,\"pending_bytes\":%lld}",
                     launch_id, pending_items, pending_bytes);
    return n > 0 && (size_t)n < sizeof(json)
        ? jw_ipc_client_send(client, json, (size_t)n) : -1;
}

static int run_game_check_fixture(const char *socket_path,
                                  const char *service_id,
                                  const char *runtime,
                                  const char *scenario) {
    int ack_ms = strcmp(scenario, "game-check-expiry") == 0 ? 1000 : 250;
    int wait_ms = strcmp(scenario, "game-check-expiry") == 0 ? 200 : 600;
    jw_ipc_client *client = NULL;
    if (connect_and_subscribe_check(socket_path, service_id, ack_ms, wait_ms,
                                    &client) != 0 ||
        reconcile(client, "7") != 0 ||
        write_result(runtime, "ready=1\n") != 0) {
        jw_ipc_client_close(client);
        return 110;
    }

    char *check = NULL;
    char launch_id[128];
    if (recv_json(client, &check) != 0 ||
        !strstr(check, "\"event\":\"game.check\"") ||
        !strstr(check, "\"wait_budget_ms\":") ||
        json_launch_id(check, launch_id, sizeof(launch_id)) != 0) {
        free(check);
        jw_ipc_client_close(client);
        return 111;
    }
    free(check);

    if (strcmp(scenario, "game-check-current") == 0) {
        if (send_status(client, "stop", launch_id, 0) != 0 ||
            write_result(runtime, "ready=1\ncheck=1\nstop=1\n") != 0) {
            jw_ipc_client_close(client);
            return 112;
        }
    } else if (strcmp(scenario, "game-check-timeout") == 0) {
        /* No initial status: Jawaka must abort, never infer current. */
    } else if (strcmp(scenario, "game-check-unsafe-card") == 0) {
        char error[320];
        int n = snprintf(error, sizeof(error),
                         "{\"v\":1,\"status\":\"error\",\"launch_id\":\"%s\","
                         "\"reason\":\"unsafe-card-binding\"}", launch_id);
        if (n <= 0 || (size_t)n >= sizeof(error) ||
            jw_ipc_client_send(client, error, (size_t)n) != 0 ||
            write_result(runtime, "ready=1\ncheck=1\nunsafe_card=1\n") != 0) {
            jw_ipc_client_close(client);
            return 121;
        }
    } else if (strcmp(scenario, "game-check-malformed") == 0) {
        if (send_status(client, "ready", launch_id, 0) != 0) {
            jw_ipc_client_close(client);
            return 113;
        }
    } else {
        if (send_check_waiting(client, launch_id, 3, 49152) != 0 ||
            write_result(runtime, "ready=1\ncheck=1\nwaiting=1\n") != 0) {
            jw_ipc_client_close(client);
            return 114;
        }
        if (strcmp(scenario, "game-check-wait") == 0) {
            char selected_path[4096];
            int selected_n = snprintf(selected_path, sizeof(selected_path),
                                      "%s/check-wait-selected", runtime);
            if (selected_n < 0 || (size_t)selected_n >= sizeof(selected_path)) {
                jw_ipc_client_close(client);
                return 115;
            }
            while (!g_stop && access(selected_path, F_OK) != 0) {
                usleep(10000);
            }
            /* Deliberately exceed ack_ms. An explicit check wait is bounded by
               wait_ms and must not require artificial progress heartbeats. */
            usleep(350000);
            if (send_check_waiting(client, launch_id, 2, 32768) != 0) {
                jw_ipc_client_close(client);
                return 120;
            }
            usleep(100000);
            if (send_status(client, "stop", launch_id, 0) != 0) {
                jw_ipc_client_close(client);
                return 116;
            }
        } else {
            char *event = NULL;
            if (recv_json(client, &event) != 0) {
                free(event);
                jw_ipc_client_close(client);
                return 117;
            }
            bool cancel = strstr(event, "\"event\":\"game.cancel\"") != NULL;
            bool abort = strstr(event, "\"event\":\"game.abort\"") != NULL;
            free(event);
            if ((strcmp(scenario, "game-check-play") == 0 && !cancel) ||
                ((strcmp(scenario, "game-check-cancel") == 0 ||
                  strcmp(scenario, "game-check-expiry") == 0) && !abort)) {
                jw_ipc_client_close(client);
                return 118;
            }
            if (write_result(runtime,
                    cancel ? "ready=1\ncheck=1\nwaiting=1\ncancel=1\n"
                           : "ready=1\ncheck=1\nwaiting=1\nabort=1\n") != 0) {
                jw_ipc_client_close(client);
                return 119;
            }
        }
    }
    while (!g_stop) {
        pause();
    }
    jw_ipc_client_close(client);
    return 0;
}

static int run_game_exchange_fixture(const char *socket_path,
                                     const char *service_id,
                                     const char *runtime,
                                     const char *scenario) {
    jw_ipc_client *client = NULL;
    if (connect_and_subscribe(socket_path, service_id, "1", "notify",
                              &client) != 0 ||
        reconcile(client, "7") != 0 ||
        write_result(runtime, "ready=1\n") != 0) {
        jw_ipc_client_close(client);
        return 80;
    }

    char *start = NULL;
    char *cancel = NULL;
    char *finish = NULL;
    char launch_id[128];
    if (recv_json(client, &start) != 0 ||
        !strstr(start, "\"event\":\"game.start\"") ||
        !strstr(start, "\"source_id\":\"primary\"") ||
        !strstr(start, "\"saves_path\":") ||
        !strstr(start, "\"states_path\":") ||
        json_launch_id(start, launch_id, sizeof(launch_id)) != 0 ||
        recv_json(client, &cancel) != 0 ||
        !strstr(cancel, "\"event\":\"game.cancel\"") ||
        !strstr(cancel, launch_id)) {
        free(start);
        free(cancel);
        jw_ipc_client_close(client);
        return 81;
    }
    char ready[256];
    int n = snprintf(ready, sizeof(ready),
                     "{\"v\":1,\"status\":\"ready\",\"launch_id\":\"%s\"}",
                     launch_id);
    if (strcmp(scenario, "game-slow-ready") == 0) {
        usleep(120000);
    }
    if (n < 0 || (size_t)n >= sizeof(ready) ||
        jw_ipc_client_send(client, ready, (size_t)n) != 0 ||
        recv_json(client, &finish) != 0 ||
        !strstr(finish, "\"event\":\"game.finish\"") ||
        !strstr(finish, launch_id) ||
        write_result(runtime, "ready=1\nstart=1\ncancel=1\nfinish=1\n") != 0) {
        free(start);
        free(cancel);
        free(finish);
        jw_ipc_client_close(client);
        return 82;
    }
    free(start);
    free(cancel);
    free(finish);
    while (!g_stop) {
        pause();
    }
    jw_ipc_client_close(client);
    return 0;
}

static int run_game_unsubscribed_fixture(const char *socket_path,
                                         const char *service_id,
                                         const char *runtime,
                                         const char *scenario) {
    if (write_result(runtime, "ready=1\n") != 0) {
        return 83;
    }
    if (strcmp(scenario, "game-late-subscribe") == 0) {
        usleep(350000);
        if (!g_stop) {
            jw_ipc_client *late = NULL;
            if (connect_and_subscribe(socket_path, service_id, "late", "notify",
                                      &late) == 0) {
                (void)reconcile(late, "late-state");
            }
            jw_ipc_client_close(late);
        }
    }
    while (!g_stop) {
        pause();
    }
    return 0;
}

static int reconcile_active(jw_ipc_client *client, const char *launch_id) {
    const char *request = "{\"v\":1,\"op\":\"game.state\",\"id\":\"reconnect\"}";
    if (jw_ipc_client_send(client, request, strlen(request)) != 0) {
        return -1;
    }
    char *reply = NULL;
    int rc = recv_json(client, &reply);
    bool ok = rc == 0 && strstr(reply, "\"id\":\"reconnect\"") &&
              strstr(reply, "\"active\":true") && strstr(reply, launch_id) &&
              strstr(reply, "\"source_id\":") &&
              strstr(reply, "\"saves_path\":") &&
              strstr(reply, "\"states_path\":");
    free(reply);
    return ok ? 0 : -1;
}

static int run_game_reconnect_fixture(const char *socket_path,
                                      const char *service_id,
                                      const char *runtime) {
    jw_ipc_client *client = NULL;
    if (connect_and_subscribe(socket_path, service_id, "1", "notify",
                              &client) != 0 ||
        reconcile(client, "7") != 0 ||
        write_result(runtime, "ready=1\n") != 0) {
        jw_ipc_client_close(client);
        return 84;
    }

    char *start = NULL;
    char *cancel = NULL;
    char launch_id[128];
    if (recv_json(client, &start) != 0 ||
        json_launch_id(start, launch_id, sizeof(launch_id)) != 0 ||
        recv_json(client, &cancel) != 0 ||
        !strstr(cancel, "\"event\":\"game.cancel\"") ||
        send_status(client, "ready", launch_id, 0) != 0) {
        free(start);
        free(cancel);
        jw_ipc_client_close(client);
        return 85;
    }
    free(start);
    free(cancel);
    jw_ipc_client_close(client);
    client = NULL;

    if (connect_and_subscribe(socket_path, service_id, "2", "notify",
                              &client) != 0 ||
        reconcile_active(client, launch_id) != 0) {
        jw_ipc_client_close(client);
        return 86;
    }
    char *finish = NULL;
    if (recv_json(client, &finish) != 0 ||
        !strstr(finish, "\"event\":\"game.finish\"") ||
        !strstr(finish, launch_id) ||
        write_result(runtime,
                     "ready=1\nstart=1\ncancel=1\nreconnected=1\nfinish=1\n") != 0) {
        free(finish);
        jw_ipc_client_close(client);
        return 87;
    }
    free(finish);
    while (!g_stop) {
        pause();
    }
    jw_ipc_client_close(client);
    return 0;
}

static int run_game_failure_fixture(const char *socket_path,
                                    const char *service_id,
                                    const char *runtime,
                                    const char *scenario) {
    jw_ipc_client *client = NULL;
    if (connect_and_subscribe(socket_path, service_id, "1", "notify",
                              &client) != 0 ||
        reconcile(client, "7") != 0 ||
        write_result(runtime, "ready=1\n") != 0) {
        jw_ipc_client_close(client);
        return 90;
    }
    char *start = NULL;
    char *cancel = NULL;
    if (recv_json(client, &start) != 0 ||
        !strstr(start, "\"event\":\"game.start\"") ||
        recv_json(client, &cancel) != 0 ||
        !strstr(cancel, "\"event\":\"game.cancel\"")) {
        free(start);
        free(cancel);
        jw_ipc_client_close(client);
        return 91;
    }
    free(start);
    free(cancel);
    if (strcmp(scenario, "game-malformed") == 0) {
        if (jw_ipc_client_send(client, "{", 1) != 0) {
            jw_ipc_client_close(client);
            return 92;
        }
    } else if (strcmp(scenario, "game-drop") == 0) {
        jw_ipc_client_close(client);
        client = NULL;
    }
    /* timeout sends nothing. Every branch must be contained by Jawaka's
     * verified-stop fallback, which delivers SIGTERM to this whole group. */
    while (!g_stop) {
        pause();
    }
    jw_ipc_client_close(client);
    return 0;
}

static int send_status(jw_ipc_client *client, const char *status,
                       const char *launch_id, int pending_items) {
    char json[320];
    int n = strcmp(status, "waiting") == 0
        ? snprintf(json, sizeof(json),
                   "{\"v\":1,\"status\":\"waiting\",\"launch_id\":\"%s\","
                   "\"pending_items\":%d}", launch_id, pending_items)
        : snprintf(json, sizeof(json),
                   "{\"v\":1,\"status\":\"%s\",\"launch_id\":\"%s\"}",
                   status, launch_id);
    return n > 0 && (size_t)n < sizeof(json)
        ? jw_ipc_client_send(client, json, (size_t)n) : -1;
}

static int run_game_wait_fixture(const char *socket_path,
                                 const char *service_id,
                                 const char *runtime,
                                 const char *scenario) {
    jw_ipc_client *client = NULL;
    if (connect_and_subscribe_wait(socket_path, service_id, 250, 600,
                                   &client) != 0 ||
        reconcile(client, "7") != 0 ||
        write_result(runtime, "ready=1\n") != 0) {
        jw_ipc_client_close(client);
        return 100;
    }
    char *start = NULL;
    char launch_id[128];
    if (recv_json(client, &start) != 0 ||
        !strstr(start, "\"event\":\"game.start\"") ||
        json_launch_id(start, launch_id, sizeof(launch_id)) != 0 ||
        send_status(client, "waiting", launch_id, 3) != 0 ||
        (strcmp(scenario, "game-start-now") == 0 &&
         write_result(runtime, "ready=1\nwaiting=1\n") != 0)) {
        free(start);
        jw_ipc_client_close(client);
        return 101;
    }
    free(start);

    if (strcmp(scenario, "game-waiting") == 0) {
        usleep(30000);
        if (send_status(client, "waiting", launch_id, 2) != 0) {
            jw_ipc_client_close(client);
            return 102;
        }
        usleep(30000);
        if (send_status(client, "ready", launch_id, 0) != 0) {
            jw_ipc_client_close(client);
            return 103;
        }
    } else if (strcmp(scenario, "game-stalled") == 0) {
        usleep(30000);
        if (send_status(client, "waiting", launch_id, 3) != 0) {
            jw_ipc_client_close(client);
            return 104;
        }
    }

    char *next = NULL;
    if (recv_json(client, &next) != 0) {
        free(next);
        jw_ipc_client_close(client);
        return 105;
    }
    if (strcmp(scenario, "game-waiting") == 0) {
        if (!strstr(next, "\"event\":\"game.finish\"") ||
            write_result(runtime,
                         "ready=1\nstart=1\nwaiting=1\nfinish=1\n") != 0) {
            free(next);
            jw_ipc_client_close(client);
            return 106;
        }
    } else {
        if (!strstr(next, "\"event\":\"game.cancel\"")) {
            free(next);
            jw_ipc_client_close(client);
            return 107;
        }
        if (strcmp(scenario, "game-wait-expiry") == 0 ||
            strcmp(scenario, "game-start-now") == 0) {
            if (send_status(client, "ready", launch_id, 0) != 0) {
                free(next);
                jw_ipc_client_close(client);
                return 108;
            }
            free(next);
            next = NULL;
            if (recv_json(client, &next) != 0 ||
                !strstr(next, "\"event\":\"game.finish\"") ||
                write_result(runtime,
                    "ready=1\nstart=1\nwaiting=1\ncancel=1\nfinish=1\n") != 0) {
                free(next);
                jw_ipc_client_close(client);
                return 109;
            }
        }
        /* game-stalled intentionally never sends ready: timeout -> verified
         * stop fallback. */
    }
    free(next);
    while (!g_stop) {
        pause();
    }
    jw_ipc_client_close(client);
    return 0;
}

int main(void) {
    const char *socket_path = getenv("UMRK_DAEMON_SOCKET");
    const char *runtime = getenv("UMRK_SERVICE_RUNTIME_DIR");
    const char *service_id = getenv("UMRK_LIFE1_FIXTURE_SERVICE_ID");
    const char *lease = getenv("UMRK_SERVICE_LEASE_FD");
    if (!socket_path || !runtime || !service_id || !lease ||
        strcmp(lease, "3") != 0 || fcntl(3, F_GETFD) < 0) {
        return 64;
    }
    signal(SIGTERM, on_term);
    signal(SIGINT, on_term);
    signal(SIGPIPE, SIG_IGN);

    const char *scenario = getenv("UMRK_LIFE1_FIXTURE_SCENARIO");
    if (scenario && strncmp(scenario, "game-check-", 11) == 0) {
        return run_game_check_fixture(socket_path, service_id, runtime,
                                      scenario);
    }
    if (scenario &&
        (strcmp(scenario, "game-exchange") == 0 ||
         strcmp(scenario, "game-slow-ready") == 0)) {
        return run_game_exchange_fixture(socket_path, service_id, runtime,
                                         scenario);
    }
    if (scenario &&
        (strcmp(scenario, "game-never-subscribe") == 0 ||
         strcmp(scenario, "game-late-subscribe") == 0)) {
        return run_game_unsubscribed_fixture(socket_path, service_id, runtime,
                                              scenario);
    }
    if (scenario && strcmp(scenario, "game-reconnect") == 0) {
        return run_game_reconnect_fixture(socket_path, service_id, runtime);
    }
    if (scenario &&
        (strcmp(scenario, "game-malformed") == 0 ||
         strcmp(scenario, "game-drop") == 0 ||
         strcmp(scenario, "game-timeout") == 0)) {
        return run_game_failure_fixture(socket_path, service_id, runtime,
                                        scenario);
    }
    if (scenario &&
        (strcmp(scenario, "game-waiting") == 0 ||
         strcmp(scenario, "game-wait-expiry") == 0 ||
         strcmp(scenario, "game-start-now") == 0 ||
         strcmp(scenario, "game-stalled") == 0)) {
        return run_game_wait_fixture(socket_path, service_id, runtime,
                                     scenario);
    }

    jw_ipc_client *first = NULL;
    jw_ipc_client *second = NULL;
    if (connect_and_subscribe(socket_path, service_id, "1", "notify",
                              &first) != 0 ||
        reconcile(first, "7") != 0 ||
        connect_and_subscribe(socket_path, service_id, "2", "stop",
                              &second) != 0) {
        jw_ipc_client_close(first);
        jw_ipc_client_close(second);
        return 70;
    }

    /* The accepted re-subscription must replace and close the prior socket. */
    char *unexpected = NULL;
    size_t unexpected_len = 0;
    if (jw_ipc_client_recv(first, &unexpected, &unexpected_len) == 0) {
        free(unexpected);
        jw_ipc_client_close(first);
        jw_ipc_client_close(second);
        return 71;
    }
    jw_ipc_client_close(first);
    if (reconcile(second, "8") != 0) {
        jw_ipc_client_close(second);
        return 72;
    }

    if (write_result(runtime,
                     "subscribed=2\nreconciled=2\nreplaced=1\n") != 0) {
        jw_ipc_client_close(second);
        return 74;
    }

    while (!g_stop) {
        pause();
    }
    jw_ipc_client_close(second);
    return 0;
}
