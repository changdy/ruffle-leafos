#include "cJSON.h"
#include "internal/ipc/ipc.h"
#include "internal/power/suspend_inhibit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void usage(FILE *out) {
    fprintf(out,
        "usage: jawaka-inhibitctl --socket PATH status\n"
        "       jawaka-inhibitctl --socket PATH hold --reason LABEL [--scope SCOPE]\n"
        "                         [--seconds N] [--heartbeat PATH]\n"
        "  --scope block-suspend  defer deep suspend only (default)\n"
        "  --scope block-screen   also defer the idle backlight blank\n");
}

static int request(const char *socket, cJSON *root, cJSON **out) {
    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!body) return -1;
    char *reply = NULL;
    size_t reply_len = 0;
    int rc = jw_ipc_request(socket, body, strlen(body), &reply, &reply_len);
    free(body);
    if (rc != 0) return -1;
    *out = cJSON_ParseWithLength(reply, reply_len);
    free(reply);
    return *out ? 0 : -1;
}

static int heartbeat(const char *path, int second) {
    if (!path || !path[0]) return 0;
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "%d\n", second);
    return fclose(fp);
}

int main(int argc, char **argv) {
    const char *socket = NULL, *reason = "smoke helper", *heartbeat_path = NULL;
    const char *scope = JW_SUSPEND_SCOPE_SUSPEND;
    int seconds = 60, command_index = -1;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) socket = argv[++i];
        else if (strcmp(argv[i], "--scope") == 0 && i + 1 < argc) scope = argv[++i];
        else if (strcmp(argv[i], "--reason") == 0 && i + 1 < argc) reason = argv[++i];
        else if (strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (strcmp(argv[i], "--heartbeat") == 0 && i + 1 < argc) heartbeat_path = argv[++i];
        else if (strcmp(argv[i], "status") == 0 || strcmp(argv[i], "hold") == 0) command_index = i;
        else { usage(stderr); return 2; }
    }
    if (!socket || command_index < 0 || seconds < 1) { usage(stderr); return 2; }
    const char *command = argv[command_index];
    if (strcmp(command, "status") == 0) {
        cJSON *request_json = cJSON_CreateObject(), *reply = NULL;
        cJSON_AddStringToObject(request_json, "type", "suspend-inhibit-status");
        if (request(socket, request_json, &reply) != 0) return 1;
        char *printed = cJSON_PrintUnformatted(reply);
        cJSON_Delete(reply);
        if (!printed) return 1;
        puts(printed);
        free(printed);
        return 0;
    }

    cJSON *acquire = cJSON_CreateObject(), *reply = NULL;
    cJSON_AddStringToObject(acquire, "type", "suspend-inhibit-acquire");
    cJSON_AddStringToObject(acquire, "scope", scope);
    cJSON_AddStringToObject(acquire, "reason", reason);
    if (request(socket, acquire, &reply) != 0) return 1;
    cJSON *token_json = cJSON_GetObjectItemCaseSensitive(reply, "token");
    if (!cJSON_IsString(token_json) || !token_json->valuestring) {
        char *printed = cJSON_PrintUnformatted(reply);
        fprintf(stderr, "acquire failed: %s\n", printed ? printed : "invalid reply");
        free(printed);
        cJSON_Delete(reply);
        return 1;
    }
    char token[64];
    snprintf(token, sizeof(token), "%s", token_json->valuestring);
    cJSON_Delete(reply);
    printf("acquired pid=%d reason=%s\n", (int)getpid(), reason);
    fflush(stdout);
    for (int second = 0; second < seconds; second++) {
        if (heartbeat(heartbeat_path, second) != 0)
            fprintf(stderr, "heartbeat write failed: %s\n", strerror(errno));
        sleep(1);
    }

    cJSON *release = cJSON_CreateObject();
    cJSON_AddStringToObject(release, "type", "suspend-inhibit-release");
    cJSON_AddStringToObject(release, "token", token);
    reply = NULL;
    if (request(socket, release, &reply) != 0) return 1;
    cJSON_Delete(reply);
    puts("released");
    return 0;
}
