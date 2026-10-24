#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/ipc/ipc.h"
#include "internal/ipc/ipc_client.h"

#include "cJSON.h"

#include <assert.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef enum {
    REPLY_LIST,
    REPLY_ACK,
    REPLY_ERROR,
    REPLY_MALFORMED_ROW,
    REPLY_TOO_LARGE,
    REPLY_TIMEOUT,
} reply_kind;

static int object_size(const cJSON *object) {
    int count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, object) count++;
    return count;
}

static void serve_one(jw_ipc_server *server, const char *expected_op,
                      const char *expected_service_id, reply_kind kind) {
    jw_ipc_client *client = NULL;
    if (jw_ipc_server_accept(server, &client, 5000) != 0 || !client) _exit(20);
    char *body = NULL;
    size_t len = 0;
    if (jw_ipc_client_recv(client, &body, &len) != 0) _exit(21);
    const char *end = NULL;
    cJSON *request = cJSON_ParseWithLengthOpts(body, len, &end, false);
    if (!request || end != body + len) _exit(22);
    cJSON *v = cJSON_GetObjectItemCaseSensitive(request, "v");
    cJSON *op = cJSON_GetObjectItemCaseSensitive(request, "op");
    cJSON *id = cJSON_GetObjectItemCaseSensitive(request, "id");
    cJSON *service_id =
        cJSON_GetObjectItemCaseSensitive(request, "service_id");
    int expected_fields = expected_service_id ? 4 : 3;
    if (object_size(request) != expected_fields || !cJSON_IsNumber(v) ||
        v->valuedouble != 1.0 || !cJSON_IsString(op) || !op->valuestring ||
        strcmp(op->valuestring, expected_op) != 0 ||
        !cJSON_IsString(id) || !id->valuestring || !id->valuestring[0] ||
        (expected_service_id &&
         (!cJSON_IsString(service_id) || !service_id->valuestring ||
          strcmp(service_id->valuestring, expected_service_id) != 0))) {
        _exit(23);
    }
    char correlation[129];
    snprintf(correlation, sizeof(correlation), "%s", id->valuestring);
    cJSON_Delete(request);
    free(body);

    if (kind == REPLY_TIMEOUT) {
        usleep(500000);
        jw_ipc_client_close(client);
        _exit(0);
    }
    if (kind == REPLY_TOO_LARGE) {
        char *large = malloc(JW_IPC_SERVICE_MAX_FRAME + 1u);
        if (!large) _exit(24);
        memset(large, ' ', JW_IPC_SERVICE_MAX_FRAME + 1u);
        if (jw_ipc_client_send(client, large,
                               JW_IPC_SERVICE_MAX_FRAME + 1u) != 0) _exit(25);
        free(large);
        jw_ipc_client_close(client);
        _exit(0);
    }

    cJSON *reply = cJSON_CreateObject();
    cJSON_AddNumberToObject(reply, "v", 1);
    cJSON_AddStringToObject(reply, "id", correlation);
    if (kind == REPLY_ACK) {
        cJSON_AddBoolToObject(reply, "ok", true);
    } else if (kind == REPLY_ERROR) {
        cJSON *error = cJSON_AddObjectToObject(reply, "error");
        cJSON_AddStringToObject(error, "code", "denied");
        cJSON_AddStringToObject(error, "message", "denied by policy");
    } else {
        cJSON *services = cJSON_AddArrayToObject(reply, "services");
        int count = kind == REPLY_LIST ? 40 : 1;
        for (int i = 0; i < count; i++) {
            if (kind == REPLY_MALFORMED_ROW) {
                cJSON_AddItemToArray(services, cJSON_CreateString("bad"));
                continue;
            }
            cJSON *service = cJSON_CreateObject();
            char service_name[64];
            snprintf(service_name, sizeof(service_name),
                     "org.umrk.test.service%02d", i);
            cJSON_AddStringToObject(service, "service_id", service_name);
            cJSON_AddBoolToObject(service, "desired_enabled", i == 0);
            cJSON_AddStringToObject(service, "effective_state",
                                    i == 0 ? "running" : "stopped");
            cJSON_AddItemToArray(services, service);
        }
    }
    char *json = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    if (!json || jw_ipc_client_send(client, json, strlen(json)) != 0) _exit(26);
    cJSON_free(json);
    jw_ipc_client_close(client);
    _exit(0);
}

static jw_ipc_server *make_server(char *dir, size_t dir_size,
                                  char *socket_path, size_t socket_size) {
    snprintf(dir, dir_size, "/tmp/jw-ctl1-client-XXXXXX");
    assert(mkdtemp(dir));
    int n = snprintf(socket_path, socket_size, "%s/socket", dir);
    assert(n > 0 && (size_t)n < socket_size);
    jw_ipc_server *server = NULL;
    assert(jw_ipc_server_listen(socket_path, &server) == 0);
    return server;
}

static pid_t spawn(jw_ipc_server *server, const char *op,
                   const char *service_id, reply_kind kind) {
    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) serve_one(server, op, service_id, kind);
    return child;
}

static void finish(jw_ipc_server *server, const char *dir, pid_t child) {
    int status = 0;
    assert(waitpid(child, &status, 0) == child);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    jw_ipc_server_close(server);
    assert(rmdir(dir) == 0);
}

static void test_list_caps_and_validates(void) {
    char dir[128], socket_path[160];
    jw_ipc_server *server =
        make_server(dir, sizeof(dir), socket_path, sizeof(socket_path));
    pid_t child = spawn(server, "list", NULL, REPLY_LIST);
    jw_ipc_service_info services[JW_IPC_SVC_LIST_MAX];
    int count = -1;
    assert(jw_ipc_service_list(socket_path, services,
                               JW_IPC_SVC_LIST_MAX, &count) == 0);
    finish(server, dir, child);
    assert(count == JW_IPC_SVC_LIST_MAX);
    assert(strcmp(services[0].id, "org.umrk.test.service00") == 0);
    assert(strcmp(services[0].state, "running") == 0);
    assert(services[0].desired_enabled);

    server = make_server(dir, sizeof(dir), socket_path, sizeof(socket_path));
    child = spawn(server, "list", NULL, REPLY_MALFORMED_ROW);
    count = 99;
    assert(jw_ipc_service_list(socket_path, services, 1, &count) == -1);
    finish(server, dir, child);
    assert(count == 0);
}

static void test_control_ack_and_error(void) {
    const char *service_id = "org.umrk.test.service";
    char dir[128], socket_path[160], message[64];
    jw_ipc_server *server =
        make_server(dir, sizeof(dir), socket_path, sizeof(socket_path));
    pid_t child = spawn(server, "run", service_id, REPLY_ACK);
    assert(jw_ipc_service_ctl(socket_path, "run", service_id,
                              message, sizeof(message)) == 0);
    finish(server, dir, child);

    server = make_server(dir, sizeof(dir), socket_path, sizeof(socket_path));
    child = spawn(server, "stop", service_id, REPLY_ERROR);
    assert(jw_ipc_service_ctl(socket_path, "stop", service_id,
                              message, sizeof(message)) == -1);
    finish(server, dir, child);
    assert(strcmp(message, "denied by policy") == 0);
}

static void test_list_ceiling_and_timeout(void) {
    char dir[128], socket_path[160];
    jw_ipc_server *server =
        make_server(dir, sizeof(dir), socket_path, sizeof(socket_path));
    pid_t child = spawn(server, "list", NULL, REPLY_TOO_LARGE);
    assert(jw_ipc_service_list(socket_path, NULL, 0, NULL) == -1);
    finish(server, dir, child);

    server = make_server(dir, sizeof(dir), socket_path, sizeof(socket_path));
    child = spawn(server, "list", NULL, REPLY_TIMEOUT);
    assert(jw_ipc_service_list(socket_path, NULL, 0, NULL) == -1);
    finish(server, dir, child);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    test_list_caps_and_validates();
    test_control_ack_and_error();
    test_list_ceiling_and_timeout();
    puts("PASS service-client-test");
    return 0;
}
