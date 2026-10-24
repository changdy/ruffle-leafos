#include "internal/ipc/delete_client.h"
#include "internal/ipc/ipc.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A preview reads one game's descriptors; allow for a slow card. */
#define JW_DELETE_PREVIEW_TIMEOUT_MS 10000

struct jw_ipc_delete_session { jw_ipc_client *client; bool failed; };

void jw_ipc_delete_status_free(jw_ipc_delete_status *status) {
    if (!status) return;
    free(status->files);
    memset(status, 0, sizeof(*status));
}

static bool jw__string(const cJSON *object, const char *key, char *out, size_t size) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(object, key);
    if (!value) { out[0] = '\0'; return true; }
    if (!cJSON_IsString(value) || strlen(value->valuestring) >= size) return false;
    memcpy(out, value->valuestring, strlen(value->valuestring) + 1);
    return true;
}

static bool jw__number(const cJSON *object, const char *key, uint64_t limit,
                        bool required, uint64_t *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
    *out = 0;
    if (!v) return !required;
    if (!cJSON_IsNumber(v) || !(v->valuedouble >= 0) ||
        !(v->valuedouble < 9007199254740992.0)) return false;
    uint64_t number = (uint64_t)v->valuedouble;
    if ((double)number != v->valuedouble || number > limit) return false;
    *out = number;
    return true;
}

static bool jw__boolean(const cJSON *object, const char *key, bool required, bool *out) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(object, key);
    *out = false;
    if (!v) return !required;
    if (!cJSON_IsBool(v)) return false;
    *out = cJSON_IsTrue(v);
    return true;
}

static int jw__parse(const char *raw, size_t length, jw_ipc_delete_status *status) {
    cJSON *reply = cJSON_ParseWithLength(raw, length);
    int rc = -1;
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(reply, "type");
    const cJSON *phase = cJSON_GetObjectItemCaseSensitive(reply, "phase");
    if (!cJSON_IsObject(reply) || !cJSON_IsString(type) || strcmp(type->valuestring, "rom-delete-status") ||
        !cJSON_IsNumber(phase) || phase->valuedouble != phase->valueint ||
        phase->valueint < JW_IPC_DELETE_READY || phase->valueint > JW_IPC_DELETE_ERROR)
        goto done;
    status->phase = (jw_ipc_delete_phase)phase->valueint;
#define READ_STRING(field) if (!jw__string(reply, #field, status->field, sizeof(status->field))) goto done
    READ_STRING(name); READ_STRING(source_id); READ_STRING(digest); READ_STRING(error);
    READ_STRING(readonly_source);
#undef READ_STRING
    bool full = status->phase == JW_IPC_DELETE_READY || status->phase == JW_IPC_DELETE_DONE;
    if ((full && (!status->name[0] || !status->source_id[0])) ||
        (status->phase == JW_IPC_DELETE_READY) != (status->digest[0] != '\0') ||
        (status->phase == JW_IPC_DELETE_COMMITTING && !status->source_id[0]) ||
        (status->phase == JW_IPC_DELETE_ERROR && !status->error[0])) goto done;
    uint64_t number;
    if (!jw__number(reply, "removed_count", SIZE_MAX, full, &number)) goto done;
    status->removed_count = (size_t)number;
    if (!jw__number(reply, "absent_count", SIZE_MAX, full, &number)) goto done;
    status->absent_count = (size_t)number;
    if (!jw__number(reply, "bytes", UINT64_MAX, full, &status->bytes) ||
        !jw__boolean(reply, "writable_progress", full, &status->writable_progress)) goto done;
    const cJSON *files = cJSON_GetObjectItemCaseSensitive(reply, "files");
    if ((files && !cJSON_IsArray(files)) || (full && !files)) goto done;
    int count = cJSON_GetArraySize(files);
    if (count > 65536 || (full && !count)) goto done;
    if (count) {
        status->files = calloc((size_t)count, sizeof(*status->files));
        if (!status->files) goto done;
    }
    status->files_count = (size_t)count;
    size_t removed = 0;
    uint64_t present_bytes = 0;
    for (int i = 0; i < count; ++i) {
        const cJSON *item = cJSON_GetArrayItem(files, i);
        jw_ipc_delete_file *file = &status->files[i];
        if (!cJSON_IsObject(item) ||
            !jw__string(item, "source_id", file->source_id, sizeof(file->source_id)) ||
            !jw__string(item, "rom_relpath", file->rom_relpath, sizeof(file->rom_relpath)) ||
            !file->source_id[0] || !file->rom_relpath[0] ||
            !jw__number(item, "size", UINT64_MAX, true, &file->size) ||
            !jw__boolean(item, "missing", true, &file->missing) ||
            !jw__boolean(item, "removed", true, &file->removed)) goto done;
        if (file->removed && file->missing) goto done;
        removed += file->removed;
        if (!file->missing) {
            if (UINT64_MAX - present_bytes < file->size) goto done;
            present_bytes += file->size;
        }
    }
    if (files && (status->removed_count != removed ||
                  status->absent_count > status->files_count - removed)) goto done;
    /* A preview's total is exactly what its listed files hold. */
    if (status->phase == JW_IPC_DELETE_READY &&
        (status->removed_count || status->absent_count || status->bytes != present_bytes)) goto done;
    rc = 0;
done:
    cJSON_Delete(reply);
    return rc;
}

static void jw__lost(jw_ipc_delete_status *status) {
    jw_ipc_delete_status_free(status);
    status->phase = JW_IPC_DELETE_ERROR;
    snprintf(status->error, sizeof(status->error), "%s",
             "Couldn't confirm the result. Check your library.");
}

static cJSON *jw__request(const char *type, const char *source_id, const char *rom_relpath) {
    cJSON *request = cJSON_CreateObject();
    if (request && (!cJSON_AddStringToObject(request, "type", type) ||
                    (source_id && !cJSON_AddStringToObject(request, "source_id", source_id)) ||
                    (rom_relpath && !cJSON_AddStringToObject(request, "rom_relpath", rom_relpath)))) {
        cJSON_Delete(request);
        return NULL;
    }
    return request;
}

static int jw__exchange(jw_ipc_delete_session *session, cJSON *request,
                         jw_ipc_delete_status *status) {
    jw_ipc_delete_status_free(status);
    char *encoded = request ? cJSON_PrintUnformatted(request) : NULL;
    char *raw = NULL;
    size_t length = 0;
    int rc = -1;
    if (session && !session->failed && encoded &&
        jw_ipc_client_send(session->client, encoded, strlen(encoded)) == 0 &&
        jw_ipc_client_recv(session->client, &raw, &length) == 0)
        rc = jw__parse(raw, length, status);
    free(raw);
    cJSON_free(encoded);
    cJSON_Delete(request);
    if (rc != 0) {
        if (session) session->failed = true;
        jw__lost(status);
    }
    return rc;
}

int jw_ipc_delete_preview(const char *socket_path, const char *source_id,
                          const char *rom_relpath, jw_ipc_delete_status *status) {
    jw_ipc_delete_status_free(status);
    cJSON *request = jw__request("rom-delete-preview", source_id, rom_relpath);
    char *encoded = request ? cJSON_PrintUnformatted(request) : NULL;
    char *raw = NULL;
    size_t length = 0;
    int rc = encoded && jw_ipc_request_timeout(socket_path, encoded, strlen(encoded), &raw, &length,
                                               JW_DELETE_PREVIEW_TIMEOUT_MS) == 0
        ? jw__parse(raw, length, status) : -1;
    if (rc == 0 && status->phase != JW_IPC_DELETE_READY && status->phase != JW_IPC_DELETE_ERROR) rc = -1;
    free(raw);
    cJSON_free(encoded);
    cJSON_Delete(request);
    if (rc != 0) {
        jw_ipc_delete_status_free(status);
        status->phase = JW_IPC_DELETE_ERROR;
        snprintf(status->error, sizeof(status->error), "%s",
                 "Couldn't check your game's files. Try again.");
    }
    return rc;
}

int jw_ipc_delete_begin(const char *socket_path, const char *source_id,
                        const char *rom_relpath, const char *digest,
                        jw_ipc_delete_session **out, jw_ipc_delete_status *status) {
    *out = calloc(1, sizeof(**out));
    cJSON *request = jw__request("rom-delete", source_id, rom_relpath);
    if (request && !cJSON_AddStringToObject(request, "digest", digest ? digest : "")) {
        cJSON_Delete(request);
        request = NULL;
    }
    if (!*out || jw_ipc_client_connect(socket_path, &(*out)->client) != 0) {
        if (*out) (*out)->failed = true;
    } else if (jw_ipc_client_set_nonblocking((*out)->client, false, 500) != 0) {
        (*out)->failed = true;
    }
    int rc = jw__exchange(*out, request, status);
    if (rc == 0 && status->phase != JW_IPC_DELETE_COMMITTING && status->phase != JW_IPC_DELETE_ERROR) {
        (*out)->failed = true;
        jw__lost(status);
        rc = -1;
    }
    return rc;
}

int jw_ipc_delete_poll(jw_ipc_delete_session *session, jw_ipc_delete_status *status) {
    int rc = jw__exchange(session, jw__request("rom-delete-status", NULL, NULL), status);
    if (rc == 0 && status->phase == JW_IPC_DELETE_READY) {
        session->failed = true;
        jw__lost(status);
        rc = -1;
    }
    return rc;
}

void jw_ipc_delete_close(jw_ipc_delete_session *session) {
    if (!session) return;
    if (session->client) jw_ipc_client_close(session->client);
    free(session);
}
