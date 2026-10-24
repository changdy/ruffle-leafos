#include "internal/ipc/life1.h"

#include "cJSON.h"

#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void jw__life1_error(char *out, size_t out_size, const char *value) {
    if (out && out_size > 0) {
        snprintf(out, out_size, "%s", value ? value : "invalid-payload");
    }
}

static bool jw__life1_payload_has_escaped_nul(const char *payload,
                                               size_t payload_len) {
    bool in_string = false;
    for (size_t i = 0; i < payload_len; i++) {
        unsigned char ch = (unsigned char)payload[i];
        if (!in_string) {
            if (ch == (unsigned char)'"') {
                in_string = true;
            }
            continue;
        }
        if (ch == (unsigned char)'"') {
            in_string = false;
            continue;
        }
        if (ch != (unsigned char)'\\') {
            continue;
        }
        if (i + 5u < payload_len && payload[i + 1u] == 'u' &&
            payload[i + 2u] == '0' && payload[i + 3u] == '0' &&
            payload[i + 4u] == '0' && payload[i + 5u] == '0') {
            return true;
        }
        if (i + 1u < payload_len) {
            i++;
        }
    }
    return false;
}

static int jw__life1_object_size(const cJSON *object) {
    int count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, object) {
        if (count == INT_MAX) {
            return -1;
        }
        count++;
    }
    return count;
}

static char *jw__life1_dup_nonempty_string(const cJSON *item) {
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
        return NULL;
    }
    size_t len = strlen(item->valuestring);
    char *copy = malloc(len + 1u);
    if (copy) {
        memcpy(copy, item->valuestring, len + 1u);
    }
    return copy;
}

static bool jw__life1_version_ok(const cJSON *version, bool *unsupported) {
    *unsupported = false;
    if (!cJSON_IsNumber(version) || !isfinite(version->valuedouble) ||
        floor(version->valuedouble) != version->valuedouble) {
        return false;
    }
    if (version->valuedouble != (double)JW_LIFE1_VERSION) {
        *unsupported = true;
        return false;
    }
    return true;
}

static bool jw__life1_nonnegative_clamped(const cJSON *item, int maximum,
                                           int *out) {
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < 0.0) {
        return false;
    }
    *out = item->valuedouble > (double)maximum
               ? maximum : (int)item->valuedouble;
    return true;
}

static bool jw__life1_nonnegative_int64(const cJSON *item, int64_t *out) {
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        floor(item->valuedouble) != item->valuedouble ||
        item->valuedouble < 0.0 || item->valuedouble > (double)INT64_MAX) {
        return false;
    }
    *out = (int64_t)item->valuedouble;
    return true;
}

static bool jw__life1_game_events(const cJSON *events) {
    if (!cJSON_IsArray(events) || cJSON_GetArraySize(events) < 1) {
        return false;
    }
    const cJSON *event = NULL;
    cJSON_ArrayForEach(event, events) {
        if (!cJSON_IsString(event) || !event->valuestring ||
            strcmp(event->valuestring, "game") != 0) {
            return false;
        }
    }
    return true;
}

void jw_life1_request_destroy(jw_life1_request *request) {
    if (!request) {
        return;
    }
    free(request->id);
    free(request->service_id);
    memset(request, 0, sizeof(*request));
}

void jw_life1_status_destroy(jw_life1_status *status) {
    if (!status) {
        return;
    }
    free(status->launch_id);
    free(status->reason);
    memset(status, 0, sizeof(*status));
}

jw_life1_parse_result jw_life1_parse_request(const char *payload,
                                             size_t payload_len,
                                             jw_life1_request *request,
                                             char *error_code,
                                             size_t error_code_size) {
    if (request) {
        memset(request, 0, sizeof(*request));
    }
    jw__life1_error(error_code, error_code_size, "invalid-payload");
    if (!payload || !request || payload_len == 0 ||
        payload_len > JW_LIFE1_MAX_PAYLOAD ||
        memchr(payload, '\0', payload_len) != NULL ||
        jw__life1_payload_has_escaped_nul(payload, payload_len)) {
        return JW_LIFE1_PARSE_NOT_LIFE1;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(payload, payload_len,
                                            &parse_end, false);
    if (!root || !cJSON_IsObject(root) || parse_end != payload + payload_len) {
        cJSON_Delete(root);
        return JW_LIFE1_PARSE_NOT_LIFE1;
    }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
    if (!cJSON_IsString(op) || !op->valuestring ||
        (strcmp(op->valuestring, "subscribe") != 0 &&
         strcmp(op->valuestring, "game.state") != 0)) {
        cJSON_Delete(root);
        return JW_LIFE1_PARSE_NOT_LIFE1;
    }

    request->kind = strcmp(op->valuestring, "subscribe") == 0
                        ? JW_LIFE1_REQUEST_SUBSCRIBE
                        : JW_LIFE1_REQUEST_GAME_STATE;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    request->id = jw__life1_dup_nonempty_string(id);
    if (!request->id) {
        cJSON_Delete(root);
        return JW_LIFE1_PARSE_INVALID;
    }
    bool unsupported = false;
    if (!jw__life1_version_ok(
            cJSON_GetObjectItemCaseSensitive(root, "v"), &unsupported)) {
        jw__life1_error(error_code, error_code_size,
                        unsupported ? "unsupported-version" : "invalid-payload");
        cJSON_Delete(root);
        return JW_LIFE1_PARSE_INVALID;
    }

    if (request->kind == JW_LIFE1_REQUEST_GAME_STATE) {
        bool valid = jw__life1_object_size(root) == 3;
        cJSON_Delete(root);
        if (!valid) {
            return JW_LIFE1_PARSE_INVALID;
        }
        jw__life1_error(error_code, error_code_size, "");
        return JW_LIFE1_PARSE_OK;
    }

    const cJSON *service_id =
        cJSON_GetObjectItemCaseSensitive(root, "service_id");
    request->service_id = jw__life1_dup_nonempty_string(service_id);
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(root, "mode");
    const cJSON *check_before_stop =
        cJSON_GetObjectItemCaseSensitive(root, "check_before_stop");
    bool mode_valid = cJSON_IsString(mode) && mode->valuestring;
    if (mode_valid && strcmp(mode->valuestring, "notify") == 0) {
        request->mode = JW_LIFE1_MODE_NOTIFY;
    } else if (mode_valid && strcmp(mode->valuestring, "stop") == 0) {
        request->mode = JW_LIFE1_MODE_STOP;
    } else {
        mode_valid = false;
    }
    int object_size = jw__life1_object_size(root);
    bool check_valid = check_before_stop == NULL ||
                       cJSON_IsBool(check_before_stop);
    request->check_before_stop = cJSON_IsTrue(check_before_stop);
    bool valid = (object_size == 8 || object_size == 9) && check_valid &&
        (check_before_stop == NULL || request->mode == JW_LIFE1_MODE_STOP) &&
        request->service_id && mode_valid &&
        jw__life1_game_events(
            cJSON_GetObjectItemCaseSensitive(root, "events")) &&
        jw__life1_nonnegative_clamped(
            cJSON_GetObjectItemCaseSensitive(root, "ack_ms"),
            JW_LIFE1_ACK_MS_MAX, &request->ack_ms) &&
        jw__life1_nonnegative_clamped(
            cJSON_GetObjectItemCaseSensitive(root, "wait_ms"),
            JW_LIFE1_WAIT_MS_MAX, &request->wait_ms);
    cJSON_Delete(root);
    if (!valid) {
        return JW_LIFE1_PARSE_INVALID;
    }
    jw__life1_error(error_code, error_code_size, "");
    return JW_LIFE1_PARSE_OK;
}

jw_life1_parse_result jw_life1_parse_status(const char *payload,
                                            size_t payload_len,
                                            jw_life1_status *status,
                                            char *error_code,
                                            size_t error_code_size) {
    if (status) {
        memset(status, 0, sizeof(*status));
    }
    jw__life1_error(error_code, error_code_size, "invalid-payload");
    if (!payload || !status || payload_len == 0 ||
        payload_len > JW_LIFE1_MAX_PAYLOAD ||
        memchr(payload, '\0', payload_len) != NULL ||
        jw__life1_payload_has_escaped_nul(payload, payload_len)) {
        return JW_LIFE1_PARSE_INVALID;
    }
    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(payload, payload_len,
                                            &parse_end, false);
    if (!root || !cJSON_IsObject(root) || parse_end != payload + payload_len) {
        cJSON_Delete(root);
        return JW_LIFE1_PARSE_INVALID;
    }
    bool unsupported = false;
    const cJSON *status_item =
        cJSON_GetObjectItemCaseSensitive(root, "status");
    if (!jw__life1_version_ok(
            cJSON_GetObjectItemCaseSensitive(root, "v"), &unsupported) ||
        !cJSON_IsString(status_item) || !status_item->valuestring) {
        jw__life1_error(error_code, error_code_size,
                        unsupported ? "unsupported-version" :
                                      "invalid-payload");
        cJSON_Delete(root);
        return JW_LIFE1_PARSE_INVALID;
    }
    if (strcmp(status_item->valuestring, "waiting") == 0) {
        status->kind = JW_LIFE1_STATUS_WAITING;
    } else if (strcmp(status_item->valuestring, "ready") == 0) {
        status->kind = JW_LIFE1_STATUS_READY;
    } else if (strcmp(status_item->valuestring, "stop") == 0) {
        status->kind = JW_LIFE1_STATUS_STOP;
    } else if (strcmp(status_item->valuestring, "error") == 0) {
        status->kind = JW_LIFE1_STATUS_ERROR;
    } else {
        cJSON_Delete(root);
        return JW_LIFE1_PARSE_INVALID;
    }
    status->launch_id = jw__life1_dup_nonempty_string(
        cJSON_GetObjectItemCaseSensitive(root, "launch_id"));
    bool valid = status->launch_id != NULL;
    if (status->kind == JW_LIFE1_STATUS_WAITING) {
        const cJSON *pending =
            cJSON_GetObjectItemCaseSensitive(root, "pending_items");
        const cJSON *pending_bytes =
            cJSON_GetObjectItemCaseSensitive(root, "pending_bytes");
        int object_size = jw__life1_object_size(root);
        status->has_pending_bytes = pending_bytes != NULL;
        valid = valid && (object_size == 4 || object_size == 5) &&
                jw__life1_nonnegative_clamped(pending, INT_MAX,
                                               &status->pending_items) &&
                (!status->has_pending_bytes ||
                 jw__life1_nonnegative_int64(pending_bytes,
                                              &status->pending_bytes));
    } else if (status->kind == JW_LIFE1_STATUS_READY ||
               status->kind == JW_LIFE1_STATUS_STOP) {
        valid = valid && jw__life1_object_size(root) == 3;
    } else {
        status->reason = jw__life1_dup_nonempty_string(
            cJSON_GetObjectItemCaseSensitive(root, "reason"));
        valid = valid && status->reason && jw__life1_object_size(root) == 4;
    }
    cJSON_Delete(root);
    if (!valid) {
        return JW_LIFE1_PARSE_INVALID;
    }
    jw__life1_error(error_code, error_code_size, "");
    return JW_LIFE1_PARSE_OK;
}

static char *jw__life1_print(cJSON *root) {
    if (!root) {
        return NULL;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

char *jw_life1_build_ok(const char *id) {
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddNumberToObject(root, "v", JW_LIFE1_VERSION) ||
        !cJSON_AddStringToObject(root, "id", id ? id : "") ||
        !cJSON_AddBoolToObject(root, "ok", true)) {
        cJSON_Delete(root);
        return NULL;
    }
    return jw__life1_print(root);
}

char *jw_life1_build_error(const char *id, const char *code,
                           const char *message) {
    cJSON *root = cJSON_CreateObject();
    cJSON *error = NULL;
    if (!root || !cJSON_AddNumberToObject(root, "v", JW_LIFE1_VERSION) ||
        !cJSON_AddStringToObject(root, "id", id ? id : "") ||
        !(error = cJSON_AddObjectToObject(root, "error")) ||
        !cJSON_AddStringToObject(error, "code",
                                code ? code : "internal-error") ||
        !cJSON_AddStringToObject(error, "message",
                                message ? message : code)) {
        cJSON_Delete(root);
        return NULL;
    }
    if (code && strcmp(code, "unsupported-version") == 0) {
        cJSON *versions = cJSON_AddArrayToObject(error, "supported_versions");
        if (!versions ||
            !cJSON_AddItemToArray(versions,
                                  cJSON_CreateNumber(JW_LIFE1_VERSION))) {
            cJSON_Delete(root);
            return NULL;
        }
    }
    return jw__life1_print(root);
}

char *jw_life1_build_game_state_inactive(const char *id) {
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddNumberToObject(root, "v", JW_LIFE1_VERSION) ||
        !cJSON_AddStringToObject(root, "id", id ? id : "") ||
        !cJSON_AddBoolToObject(root, "active", false)) {
        cJSON_Delete(root);
        return NULL;
    }
    return jw__life1_print(root);
}

char *jw_life1_build_game_state_active(const char *id,
                                       const char *launch_id,
                                       const char *source_id,
                                       const char *saves_path,
                                       const char *states_path) {
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddNumberToObject(root, "v", JW_LIFE1_VERSION) ||
        !cJSON_AddStringToObject(root, "id", id ? id : "") ||
        !cJSON_AddBoolToObject(root, "active", true) ||
        !cJSON_AddStringToObject(root, "launch_id", launch_id ? launch_id : "") ||
        !cJSON_AddStringToObject(root, "source_id", source_id ? source_id : "") ||
        !cJSON_AddStringToObject(root, "saves_path", saves_path ? saves_path : "") ||
        !cJSON_AddStringToObject(root, "states_path", states_path ? states_path : "")) {
        cJSON_Delete(root);
        return NULL;
    }
    return jw__life1_print(root);
}

static char *jw__life1_build_game_begin(const char *event,
                                        const char *launch_id,
                                        const char *source_id,
                                        const char *saves_path,
                                        const char *states_path,
                                        int wait_budget_ms) {
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddNumberToObject(root, "v", JW_LIFE1_VERSION) ||
        !cJSON_AddStringToObject(root, "event", event ? event : "") ||
        !cJSON_AddStringToObject(root, "launch_id", launch_id ? launch_id : "") ||
        !cJSON_AddStringToObject(root, "source_id", source_id ? source_id : "") ||
        !cJSON_AddStringToObject(root, "saves_path", saves_path ? saves_path : "") ||
        !cJSON_AddStringToObject(root, "states_path", states_path ? states_path : "") ||
        !cJSON_AddNumberToObject(root, "wait_budget_ms", wait_budget_ms)) {
        cJSON_Delete(root);
        return NULL;
    }
    return jw__life1_print(root);
}

char *jw_life1_build_game_start(const char *launch_id,
                                const char *source_id,
                                const char *saves_path,
                                const char *states_path,
                                int wait_budget_ms) {
    return jw__life1_build_game_begin("game.start", launch_id, source_id,
                                      saves_path, states_path, wait_budget_ms);
}

char *jw_life1_build_game_check(const char *launch_id,
                                const char *source_id,
                                const char *saves_path,
                                const char *states_path,
                                int wait_budget_ms) {
    return jw__life1_build_game_begin("game.check", launch_id, source_id,
                                      saves_path, states_path, wait_budget_ms);
}

static char *jw__life1_build_game_event(const char *event,
                                        const char *launch_id) {
    cJSON *root = cJSON_CreateObject();
    if (!root || !cJSON_AddNumberToObject(root, "v", JW_LIFE1_VERSION) ||
        !cJSON_AddStringToObject(root, "event", event ? event : "") ||
        !cJSON_AddStringToObject(root, "launch_id", launch_id ? launch_id : "")) {
        cJSON_Delete(root);
        return NULL;
    }
    return jw__life1_print(root);
}

char *jw_life1_build_game_cancel(const char *launch_id) {
    return jw__life1_build_game_event("game.cancel", launch_id);
}

char *jw_life1_build_game_abort(const char *launch_id) {
    return jw__life1_build_game_event("game.abort", launch_id);
}

char *jw_life1_build_game_finish(const char *launch_id) {
    return jw__life1_build_game_event("game.finish", launch_id);
}
