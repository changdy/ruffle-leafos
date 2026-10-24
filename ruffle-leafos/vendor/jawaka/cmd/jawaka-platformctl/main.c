#include "cJSON.h"
#include "internal/ipc/ipc.h"
#include "internal/ipc/ipc_client.h"
#include "internal/platform/paths.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *stream) {
    fprintf(stream,
            "Usage: jawaka-platformctl [--socket PATH] status\n"
            "       jawaka-platformctl [--socket PATH] brightness PERCENT\n"
            "       jawaka-platformctl [--socket PATH] request JSON\n"
            "       jawaka-platformctl [--socket PATH] capabilities\n"
            "       jawaka-platformctl [--socket PATH] relocate-prepare OP GENERATION ITEMS_JSON\n"
            "       jawaka-platformctl [--socket PATH] relocate-status OP\n"
            "       jawaka-platformctl [--socket PATH] relocate-commit OP\n"
            "       jawaka-platformctl [--socket PATH] relocate-revert OP\n"
            "       jawaka-platformctl [--socket PATH] relocate-abort OP\n"
            "       jawaka-platformctl [--socket PATH] relocate-finish OP\n");
}

static char *join_args(int argc, char **argv, int start) {
    if (start >= argc) return NULL;
    size_t size = 1;
    for (int i = start; i < argc; i++) {
        if (strlen(argv[i]) > JW_IPC_MAX_FRAME - size) return NULL;
        size += strlen(argv[i]) + (i > start ? 1u : 0u);
    }
    char *out = malloc(size);
    if (!out) return NULL;
    out[0] = '\0';
    for (int i = start; i < argc; i++) {
        if (i > start) strcat(out, " ");
        strcat(out, argv[i]);
    }
    return out;
}

static void print_relocation_status(const jw_ipc_relocation_status *status) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "library-relocate-status");
    cJSON_AddStringToObject(root, "operation_id", status->operation_id);
    cJSON_AddStringToObject(root, "state", status->state);
    cJSON_AddNumberToObject(root, "expected_generation", status->expected_generation);
    cJSON_AddNumberToObject(root, "mapping_generation", status->mapping_generation);
    cJSON_AddNumberToObject(root, "scan_ticket_generation",
                           status->scan_ticket_generation);
    cJSON_AddNumberToObject(root, "library_generation", status->library_generation);
    cJSON_AddNumberToObject(root, "item_count", status->item_count);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json) {
        puts(json);
        cJSON_free(json);
    }
}

static int parse_identity(cJSON *object, jw_ipc_relocation_identity *out) {
    cJSON *source = cJSON_GetObjectItemCaseSensitive(object, "source_id");
    cJSON *rom = cJSON_GetObjectItemCaseSensitive(object, "rom_relpath");
    cJSON *kind = cJSON_GetObjectItemCaseSensitive(object, "image_root_kind");
    cJSON *image = cJSON_GetObjectItemCaseSensitive(object, "image_relpath");
    if (!cJSON_IsObject(object) || !cJSON_IsString(source) ||
        !cJSON_IsString(rom) || (kind && !cJSON_IsString(kind)) ||
        (image && !cJSON_IsString(image)) || (!!kind != !!image)) return -1;
    out->source_id = source->valuestring;
    out->rom_relpath = rom->valuestring;
    out->image_root_kind = kind ? kind->valuestring : NULL;
    out->image_relpath = image ? image->valuestring : NULL;
    return 0;
}

static int relocate_prepare(const char *socket, const char *operation,
                            const char *generation_text, const char *items_json) {
    char *end = NULL;
    long generation = strtol(generation_text, &end, 10);
    const char *parse_end = NULL;
    cJSON *array = cJSON_ParseWithOpts(items_json, &parse_end, 1);
    int count = cJSON_IsArray(array) ? cJSON_GetArraySize(array) : 0;
    if (end == generation_text || *end || generation < 0 || count <= 0 || count > 256) {
        cJSON_Delete(array);
        fprintf(stderr, "invalid relocation generation or items JSON\n");
        return 2;
    }
    jw_ipc_relocation_item *items = calloc((size_t)count, sizeof(*items));
    if (!items) {
        cJSON_Delete(array);
        return 1;
    }
    for (int i = 0; i < count; i++) {
        cJSON *item = cJSON_GetArrayItem(array, i);
        if (!cJSON_IsObject(item) ||
            parse_identity(cJSON_GetObjectItemCaseSensitive(item, "old"),
                           &items[i].old_identity) != 0 ||
            parse_identity(cJSON_GetObjectItemCaseSensitive(item, "new"),
                           &items[i].new_identity) != 0) {
            fprintf(stderr, "invalid relocation item at index %d\n", i);
            free(items);
            cJSON_Delete(array);
            return 2;
        }
    }
    jw_ipc_relocation_status status;
    char error[256];
    int rc = jw_ipc_relocation_prepare(socket, operation, (int)generation,
                                        items, count, &status,
                                        error, sizeof(error));
    free(items);
    cJSON_Delete(array);
    if (rc != 0) {
        fprintf(stderr, "relocation prepare failed: %s\n", error);
        return 1;
    }
    print_relocation_status(&status);
    return 0;
}

static int relocate_simple(const char *socket, const char *command,
                           const char *operation) {
    jw_ipc_relocation_status status;
    char error[256];
    int rc =
        strcmp(command, "relocate-status") == 0
            ? jw_ipc_relocation_get_status(socket, operation, &status, error, sizeof(error)) :
        strcmp(command, "relocate-commit") == 0
            ? jw_ipc_relocation_commit(socket, operation, &status, error, sizeof(error)) :
        strcmp(command, "relocate-revert") == 0
            ? jw_ipc_relocation_revert(socket, operation, &status, error, sizeof(error)) :
        strcmp(command, "relocate-abort") == 0
            ? jw_ipc_relocation_abort(socket, operation, &status, error, sizeof(error)) :
        jw_ipc_relocation_finish(socket, operation, &status, error, sizeof(error));
    if (rc != 0) {
        fprintf(stderr, "%s failed: %s\n", command, error);
        return 1;
    }
    print_relocation_status(&status);
    return 0;
}

int main(int argc, char **argv) {
    const char *socket_override = NULL;
    int index = 1;
    while (index < argc && strcmp(argv[index], "--socket") == 0) {
        if (++index >= argc) { usage(stderr); return 2; }
        socket_override = argv[index++];
    }
    if (index >= argc || strcmp(argv[index], "--help") == 0 ||
        strcmp(argv[index], "-h") == 0) {
        usage(index >= argc ? stderr : stdout);
        return index >= argc ? 2 : 0;
    }
    const char *command = argv[index++];
    char *socket_path = socket_override ? NULL : jw_socket_path();
    const char *socket = socket_override ? socket_override : socket_path;
    if (!socket || !socket[0]) {
        fprintf(stderr, "could not resolve jawakad socket path\n");
        free(socket_path);
        return 1;
    }
    int result = 0;
    if (strcmp(command, "capabilities") == 0) {
        bool supported = false;
        if (index != argc ||
            jw_ipc_has_feature(socket, "relocate-games-v1", &supported) != 0) {
            fprintf(stderr, "capability probe failed\n");
            result = 1;
        } else {
            printf("{\"type\":\"capabilities\",\"relocate-games-v1\":%s}\n",
                   supported ? "true" : "false");
        }
    } else if (strcmp(command, "relocate-prepare") == 0) {
        if (index + 2 >= argc) {
            usage(stderr);
            result = 2;
        } else {
            char *json = join_args(argc, argv, index + 2);
            result = json ? relocate_prepare(socket, argv[index], argv[index + 1], json) : 2;
            free(json);
        }
    } else if (strncmp(command, "relocate-", 9) == 0) {
        if (index + 1 != argc ||
            (strcmp(command, "relocate-status") != 0 &&
             strcmp(command, "relocate-commit") != 0 &&
             strcmp(command, "relocate-revert") != 0 &&
             strcmp(command, "relocate-abort") != 0 &&
             strcmp(command, "relocate-finish") != 0)) {
            usage(stderr);
            result = 2;
        } else {
            result = relocate_simple(socket, command, argv[index]);
        }
    } else {
        char *request = NULL;
        if (strcmp(command, "status") == 0 && index == argc) {
            request = strdup("{\"type\":\"platform-status\"}");
        } else if (strcmp(command, "brightness") == 0 && index + 1 == argc) {
            char *end = NULL;
            long value = strtol(argv[index], &end, 10);
            if (end == argv[index] || *end) {
                fprintf(stderr, "invalid brightness percent\n");
                result = 2;
            } else {
                request = malloc(96);
                if (request) snprintf(request, 96,
                    "{\"type\":\"platform-action\",\"action\":\"set-brightness\",\"value\":%ld}",
                    value);
            }
        } else if (strcmp(command, "request") == 0 && index < argc) {
            request = join_args(argc, argv, index);
        } else {
            usage(stderr);
            result = 2;
        }
        if (request) {
            char *response = NULL;
            size_t response_len = 0;
            if (jw_ipc_request(socket, request, strlen(request),
                               &response, &response_len) != 0) {
                fprintf(stderr, "platform request failed: %s\n", socket);
                result = 1;
            } else {
                fwrite(response, 1, response_len, stdout);
                fputc('\n', stdout);
            }
            free(response);
            free(request);
        } else if (result == 0) {
            result = 1;
        }
    }
    free(socket_path);
    return result;
}
