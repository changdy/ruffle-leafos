#include "internal/ipc/ctl1.h"

#include "cJSON.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static void jw__ctl1_error(char *out, size_t out_size, const char *value) {
    if (out && out_size > 0) {
        snprintf(out, out_size, "%s", value ? value : "invalid-payload");
    }
}

static bool jw__ctl1_copy_string(const cJSON *item, char *out,
                                  size_t out_size, size_t max_len) {
    if (!cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    size_t len = strlen(item->valuestring);
    if (len == 0 || len > max_len || len + 1u > out_size) {
        return false;
    }
    memcpy(out, item->valuestring, len + 1u);
    return true;
}

/* cJSON stores decoded strings as NUL-terminated C strings, so a JSON
 * `\u0000` would hide every byte after it from strlen()/strcmp(). Reject the
 * escape lexically before decoding. The small scanner distinguishes it from
 * `\\u0000`, whose doubled backslash represents ordinary literal text. */
static bool jw__ctl1_payload_has_escaped_nul(const char *payload,
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
        /* Skip the escaped byte. A valid \u escape cannot accidentally match
         * after this point because only the escape-introducing slash matters. */
        if (i + 1u < payload_len) {
            i++;
        }
    }
    return false;
}

static int jw__ctl1_object_size(const cJSON *object) {
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

static jw_ctl1_operation jw__ctl1_operation(const char *name) {
    if (!name) return JW_CTL1_OP_INVALID;
    if (strcmp(name, "list") == 0) return JW_CTL1_OP_LIST;
    if (strcmp(name, "capabilities") == 0) return JW_CTL1_OP_CAPABILITIES;
    if (strcmp(name, "enable") == 0) return JW_CTL1_OP_ENABLE;
    if (strcmp(name, "disable") == 0) return JW_CTL1_OP_DISABLE;
    if (strcmp(name, "run") == 0) return JW_CTL1_OP_RUN;
    if (strcmp(name, "stop") == 0) return JW_CTL1_OP_STOP;
    if (strcmp(name, "restart") == 0) return JW_CTL1_OP_RESTART;
    if (strcmp(name, "status") == 0) return JW_CTL1_OP_STATUS;
    if (strcmp(name, "logs") == 0) return JW_CTL1_OP_LOGS;
    if (strcmp(name, "export-logs") == 0) return JW_CTL1_OP_EXPORT_LOGS;
    return JW_CTL1_OP_INVALID;
}

const char *jw_ctl1_operation_name(jw_ctl1_operation operation) {
    switch (operation) {
    case JW_CTL1_OP_LIST: return "list";
    case JW_CTL1_OP_CAPABILITIES: return "capabilities";
    case JW_CTL1_OP_ENABLE: return "enable";
    case JW_CTL1_OP_DISABLE: return "disable";
    case JW_CTL1_OP_RUN: return "run";
    case JW_CTL1_OP_STOP: return "stop";
    case JW_CTL1_OP_RESTART: return "restart";
    case JW_CTL1_OP_STATUS: return "status";
    case JW_CTL1_OP_LOGS: return "logs";
    case JW_CTL1_OP_EXPORT_LOGS: return "export-logs";
    case JW_CTL1_OP_INVALID: break;
    }
    return "invalid";
}

bool jw_ctl1_parse_request(const char *payload, size_t payload_len,
                           jw_ctl1_request *request,
                           char *error_code, size_t error_code_size) {
    if (request) {
        memset(request, 0, sizeof(*request));
    }
    jw__ctl1_error(error_code, error_code_size, "invalid-payload");
    if (!payload || !request || payload_len == 0 ||
        payload_len > JW_CTL1_MAX_PAYLOAD ||
        memchr(payload, '\0', payload_len) != NULL ||
        jw__ctl1_payload_has_escaped_nul(payload, payload_len)) {
        return false;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(payload, payload_len,
                                            &parse_end, false);
    if (!root || !cJSON_IsObject(root) || parse_end != payload + payload_len) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (!jw__ctl1_copy_string(id, request->id, sizeof(request->id),
                              JW_CTL1_ID_MAX)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "v");
    if (!cJSON_IsNumber(version) || !isfinite(version->valuedouble) ||
        floor(version->valuedouble) != version->valuedouble) {
        cJSON_Delete(root);
        return false;
    }
    if (version->valuedouble != (double)JW_CTL1_VERSION) {
        jw__ctl1_error(error_code, error_code_size, "unsupported-version");
        cJSON_Delete(root);
        return false;
    }

    const cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
    if (!cJSON_IsString(op) || !op->valuestring) {
        cJSON_Delete(root);
        return false;
    }
    request->operation = jw__ctl1_operation(op->valuestring);
    if (request->operation == JW_CTL1_OP_INVALID) {
        jw__ctl1_error(error_code, error_code_size, "unknown-op");
        cJSON_Delete(root);
        return false;
    }

    bool unscoped = request->operation == JW_CTL1_OP_LIST ||
                    request->operation == JW_CTL1_OP_CAPABILITIES;
    bool logs = request->operation == JW_CTL1_OP_LOGS;
    int expected_fields = unscoped ? 3 : 4;
    if (logs && cJSON_GetObjectItemCaseSensitive(root, "tail")) {
        expected_fields++;
    }
    if (jw__ctl1_object_size(root) != expected_fields) {
        cJSON_Delete(root);
        return false;
    }

    if (!unscoped) {
        const cJSON *service_id =
            cJSON_GetObjectItemCaseSensitive(root, "service_id");
        if (!jw__ctl1_copy_string(service_id, request->service_id,
                                  sizeof(request->service_id),
                                  JW_CTL1_SERVICE_ID_MAX)) {
            cJSON_Delete(root);
            return false;
        }
    }

    const cJSON *tail = cJSON_GetObjectItemCaseSensitive(root, "tail");
    if (tail) {
        if (!logs || !cJSON_IsNumber(tail) ||
            !isfinite(tail->valuedouble) || tail->valuedouble < 1.0 ||
            tail->valuedouble > (double)INT_MAX ||
            floor(tail->valuedouble) != tail->valuedouble) {
            cJSON_Delete(root);
            return false;
        }
        request->tail = (int)tail->valuedouble;
        request->has_tail = true;
    }

    jw__ctl1_error(error_code, error_code_size, "");
    cJSON_Delete(root);
    return true;
}
