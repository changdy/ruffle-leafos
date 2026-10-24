#include "internal/ipc/ipc_client.h"
#include "internal/ipc/ctl1.h"
#include "internal/ipc/ipc.h"

#include "cJSON.h"

#include <limits.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

/* Send request (takes ownership of req), parse the JSON response.
 * Caller must cJSON_Delete(*out_response) on success. */
static int ipc__request_max(const char *socket_path, cJSON *req,
                            cJSON **out_resp, size_t max_response,
                            bool require_exact_payload, int timeout_ms) {
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return -1;

    char *resp_json = NULL;
    size_t resp_len = 0;
    size_t body_len = strlen(body);
    if (body_len > max_response) {
        cJSON_free(body);
        return -1;
    }
    int rc = jw_ipc_request_timeout(socket_path, body, body_len,
                                    &resp_json, &resp_len, timeout_ms);
    cJSON_free(body);
    if (rc != 0) return -1;

    if (resp_len > max_response ||
        (require_exact_payload && memchr(resp_json, '\0', resp_len))) {
        free(resp_json);
        return -1;
    }
    const char *parse_end = NULL;
    cJSON *resp = require_exact_payload
        ? cJSON_ParseWithLengthOpts(resp_json, resp_len, &parse_end, false)
        : cJSON_ParseWithOpts(resp_json, &parse_end, 1);
    bool consumed_exactly = !require_exact_payload ||
                            parse_end == resp_json + resp_len;
    free(resp_json);
    if (!resp || !consumed_exactly) {
        cJSON_Delete(resp);
        return -1;
    }

    *out_resp = resp;
    return 0;
}

static int ipc__request(const char *socket_path, cJSON *req, cJSON **out_resp) {
    return ipc__request_max(socket_path, req, out_resp, JW_IPC_MAX_FRAME,
                            false, 30000);
}

static int ipc__type_is(const cJSON *resp, const char *expected) {
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(resp, "type");
    return cJSON_IsString(t) && t->valuestring &&
           strcmp(t->valuestring, expected) == 0;
}

static void ipc__copy_string(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", value ? value : "");
}

static long long ipc__json_ll(const cJSON *obj, const char *name) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    return cJSON_IsNumber(item) ? (long long)item->valuedouble : 0;
}

static void ipc__parse_update_status(const cJSON *resp,
                                     jw_ipc_update_status_info *out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->download_percent = -1;
    out->install_battery_percent = -1;
    out->install_charging = -1;
    out->install_available_free = -1;
    out->selected_option = -1;
    const cJSON *v = NULL;
    v = cJSON_GetObjectItemCaseSensitive(resp, "state");
    if (cJSON_IsString(v)) ipc__copy_string(out->state, sizeof(out->state), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(resp, "platform_id");
    if (cJSON_IsString(v)) ipc__copy_string(out->platform_id, sizeof(out->platform_id), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (cJSON_IsString(v)) ipc__copy_string(out->message, sizeof(out->message), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(resp, "source_manifest");
    if (cJSON_IsString(v)) ipc__copy_string(out->source_manifest, sizeof(out->source_manifest), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(resp, "manifest_url");
    if (cJSON_IsString(v)) ipc__copy_string(out->manifest_url, sizeof(out->manifest_url), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(resp, "artifact_url");
    if (cJSON_IsString(v)) ipc__copy_string(out->artifact_url, sizeof(out->artifact_url), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(resp, "recovery_name");
    if (cJSON_IsString(v)) ipc__copy_string(out->recovery_name, sizeof(out->recovery_name), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(resp, "recovery_url");
    if (cJSON_IsString(v)) ipc__copy_string(out->recovery_url, sizeof(out->recovery_url), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(resp, "download_path");
    if (cJSON_IsString(v)) ipc__copy_string(out->download_path, sizeof(out->download_path), v->valuestring);

    out->compatible = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "compatible"));
    out->has_update = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "has_update"));
    out->downloaded = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "downloaded"));
    out->download_active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "download_active"));
    out->download_received = ipc__json_ll(resp, "download_received");
    out->download_total = ipc__json_ll(resp, "download_total");
    v = cJSON_GetObjectItemCaseSensitive(resp, "download_percent");
    out->download_percent = cJSON_IsNumber(v) ? v->valueint : -1;
    out->managed_apps_count = (int)ipc__json_ll(resp, "managed_apps_count");
    out->migrations_count = (int)ipc__json_ll(resp, "migrations_count");
    v = cJSON_GetObjectItemCaseSensitive(resp, "selected_option");
    if (cJSON_IsNumber(v)) out->selected_option = v->valueint;
    out->options_complete = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "options_complete"));
    out->options_loading = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "options_loading"));

    const cJSON *options = cJSON_GetObjectItemCaseSensitive(resp, "options");
    if (cJSON_IsArray(options)) {
        int count = cJSON_GetArraySize(options);
        if (count > JW_IPC_UPDATE_MAX_OPTIONS) {
            count = JW_IPC_UPDATE_MAX_OPTIONS;
        }
        out->option_count = count;
        for (int i = 0; i < count; i++) {
            const cJSON *item = cJSON_GetArrayItem(options, i);
            jw_ipc_update_option_info *option = &out->options[i];
            option->index = i;
            v = cJSON_GetObjectItemCaseSensitive(item, "index");
            if (cJSON_IsNumber(v)) option->index = v->valueint;
            option->selected = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "selected"));
            option->installed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "installed"));
            v = cJSON_GetObjectItemCaseSensitive(item, "release_id");
            if (cJSON_IsString(v)) ipc__copy_string(option->release_id, sizeof(option->release_id), v->valuestring);
            v = cJSON_GetObjectItemCaseSensitive(item, "version");
            if (cJSON_IsString(v)) ipc__copy_string(option->version, sizeof(option->version), v->valuestring);
            v = cJSON_GetObjectItemCaseSensitive(item, "published_at");
            if (cJSON_IsString(v)) ipc__copy_string(option->published_at, sizeof(option->published_at), v->valuestring);
            v = cJSON_GetObjectItemCaseSensitive(item, "notes_url");
            if (cJSON_IsString(v)) ipc__copy_string(option->notes_url, sizeof(option->notes_url), v->valuestring);
            v = cJSON_GetObjectItemCaseSensitive(item, "artifact_kind");
            if (cJSON_IsString(v)) ipc__copy_string(option->artifact_kind, sizeof(option->artifact_kind), v->valuestring);
            v = cJSON_GetObjectItemCaseSensitive(item, "artifact_name");
            if (cJSON_IsString(v)) ipc__copy_string(option->artifact_name, sizeof(option->artifact_name), v->valuestring);
            option->artifact_size = ipc__json_ll(item, "artifact_size");
            option->installed_size = ipc__json_ll(item, "installed_size");
        }
    }

    const cJSON *current = cJSON_GetObjectItemCaseSensitive(resp, "current");
    out->current_unknown = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(current, "unknown"));
    out->installed_schema = (int)ipc__json_ll(current, "schema");
    v = cJSON_GetObjectItemCaseSensitive(current, "release_id");
    if (cJSON_IsString(v)) ipc__copy_string(out->current_release_id, sizeof(out->current_release_id), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(current, "version");
    if (cJSON_IsString(v)) ipc__copy_string(out->current_version, sizeof(out->current_version), v->valuestring);

    const cJSON *candidate = cJSON_GetObjectItemCaseSensitive(resp, "candidate");
    v = cJSON_GetObjectItemCaseSensitive(candidate, "release_id");
    if (cJSON_IsString(v)) ipc__copy_string(out->release_id, sizeof(out->release_id), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(candidate, "version");
    if (cJSON_IsString(v)) ipc__copy_string(out->version, sizeof(out->version), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(candidate, "published_at");
    if (cJSON_IsString(v)) ipc__copy_string(out->published_at, sizeof(out->published_at), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(candidate, "notes_url");
    if (cJSON_IsString(v)) ipc__copy_string(out->notes_url, sizeof(out->notes_url), v->valuestring);

    const cJSON *artifact = cJSON_GetObjectItemCaseSensitive(resp, "artifact");
    v = cJSON_GetObjectItemCaseSensitive(artifact, "kind");
    if (cJSON_IsString(v)) ipc__copy_string(out->artifact_kind, sizeof(out->artifact_kind), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(artifact, "name");
    if (cJSON_IsString(v)) ipc__copy_string(out->artifact_name, sizeof(out->artifact_name), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(artifact, "sha256");
    if (cJSON_IsString(v)) ipc__copy_string(out->artifact_sha256, sizeof(out->artifact_sha256), v->valuestring);
    out->artifact_size = ipc__json_ll(artifact, "size");
    out->installed_size = ipc__json_ll(artifact, "installed_size");

    const cJSON *handoff = cJSON_GetObjectItemCaseSensitive(resp, "handoff");
    v = cJSON_GetObjectItemCaseSensitive(handoff, "type");
    if (cJSON_IsString(v)) ipc__copy_string(out->handoff_type, sizeof(out->handoff_type), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(handoff, "completion");
    if (cJSON_IsString(v)) ipc__copy_string(out->handoff_completion, sizeof(out->handoff_completion), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(handoff, "trigger_file");
    if (cJSON_IsString(v)) ipc__copy_string(out->handoff_trigger_file, sizeof(out->handoff_trigger_file), v->valuestring);

    const cJSON *install = cJSON_GetObjectItemCaseSensitive(resp, "install");
    out->install_ready = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(install, "ready"));
    out->install_blocked = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(install, "blocked"));
    out->install_needs_confirmation =
        cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(install, "needs_confirmation"));
    out->install_active = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(install, "active"));
    out->install_armed = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(install, "armed"));
    out->install_idle = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(install, "idle"));
    v = cJSON_GetObjectItemCaseSensitive(install, "battery_percent");
    if (cJSON_IsNumber(v)) out->install_battery_percent = v->valueint;
    v = cJSON_GetObjectItemCaseSensitive(install, "charging");
    if (cJSON_IsNumber(v)) out->install_charging = v->valueint;
    v = cJSON_GetObjectItemCaseSensitive(install, "required_free");
    if (cJSON_IsNumber(v)) out->install_required_free = (long long)v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(install, "available_free");
    if (cJSON_IsNumber(v)) out->install_available_free = (long long)v->valuedouble;
    v = cJSON_GetObjectItemCaseSensitive(install, "result_state");
    if (cJSON_IsString(v)) ipc__copy_string(out->install_result_state, sizeof(out->install_result_state), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(install, "result_release_id");
    if (cJSON_IsString(v)) ipc__copy_string(out->install_result_release_id, sizeof(out->install_result_release_id), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(install, "result_message");
    if (cJSON_IsString(v)) ipc__copy_string(out->install_result_message, sizeof(out->install_result_message), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(install, "request_path");
    if (cJSON_IsString(v)) ipc__copy_string(out->install_request_path, sizeof(out->install_request_path), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(install, "result_path");
    if (cJSON_IsString(v)) ipc__copy_string(out->install_result_path, sizeof(out->install_result_path), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(install, "reason");
    if (cJSON_IsString(v)) ipc__copy_string(out->install_reason, sizeof(out->install_reason), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(install, "message");
    if (cJSON_IsString(v)) ipc__copy_string(out->install_message, sizeof(out->install_message), v->valuestring);
}

static int ipc__hello(const char *socket_path, const char *role, bool levels_signal) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "hello");
    cJSON_AddStringToObject(req, "role", role);
    if (levels_signal) {
        cJSON_AddBoolToObject(req, "levels_signal", true);
    }

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) return -1;
    int ok = ipc__type_is(resp, "hello-ok");
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_hello(const char *socket_path, const char *role) {
    return ipc__hello(socket_path, role, false);
}

int jw_ipc_hello_levels_signal(const char *socket_path, const char *role) {
    return ipc__hello(socket_path, role, true);
}

int jw_ipc_has_feature(const char *socket_path, const char *feature,
                       bool *out_supported) {
    if (!feature || !feature[0] || !out_supported) return -1;
    *out_supported = false;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "hello");
    cJSON_AddStringToObject(req, "role", "platformctl");
    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0 ||
        !ipc__type_is(resp, "hello-ok")) {
        if (resp) cJSON_Delete(resp);
        return -1;
    }
    const cJSON *features = cJSON_GetObjectItemCaseSensitive(resp, "features");
    if (!cJSON_IsArray(features)) {
        cJSON_Delete(resp);
        return 0;
    }
    cJSON *entry = NULL;
    cJSON_ArrayForEach(entry, features) {
        if (cJSON_IsString(entry) && entry->valuestring &&
            strcmp(entry->valuestring, feature) == 0) {
            *out_supported = true;
            break;
        }
    }
    cJSON_Delete(resp);
    return 0;
}

static int ipc__relocation_parse(cJSON *resp, const char *operation_id,
                                 const char *expected_state,
                                 jw_ipc_relocation_status *out,
                                 char *error, int error_len) {
    if (!ipc__type_is(resp, "library-relocate-status")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (error && error_len > 0)
            snprintf(error, (size_t)error_len, "%s",
                     cJSON_IsString(message) && message->valuestring
                        ? message->valuestring : "daemon returned relocation error");
        return -1;
    }
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(resp, "operation_id");
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(resp, "state");
    const cJSON *expected = cJSON_GetObjectItemCaseSensitive(resp, "expected_generation");
    const cJSON *mapping = cJSON_GetObjectItemCaseSensitive(resp, "mapping_generation");
    const cJSON *ticket = cJSON_GetObjectItemCaseSensitive(resp, "scan_ticket_generation");
    const cJSON *library = cJSON_GetObjectItemCaseSensitive(resp, "library_generation");
    const cJSON *count = cJSON_GetObjectItemCaseSensitive(resp, "item_count");
    bool known_state = cJSON_IsString(state) && state->valuestring &&
        (strcmp(state->valuestring, "prepared") == 0 ||
         strcmp(state->valuestring, "committed") == 0 ||
         strcmp(state->valuestring, "reverted") == 0 ||
         strcmp(state->valuestring, "finishing") == 0 ||
         strcmp(state->valuestring, "finished") == 0 ||
         strcmp(state->valuestring, "aborted") == 0);
    if (!cJSON_IsString(id) || !id->valuestring ||
        strcmp(id->valuestring, operation_id) != 0 ||
        !known_state ||
        (expected_state && strcmp(state->valuestring, expected_state) != 0) ||
        !cJSON_IsNumber(expected) || expected->valueint < 0 ||
        !cJSON_IsNumber(mapping) || mapping->valueint < 0 ||
        !cJSON_IsNumber(ticket) || ticket->valueint < 0 ||
        !cJSON_IsNumber(library) || library->valueint < 0 ||
        !cJSON_IsNumber(count) || count->valueint <= 0) {
        if (error && error_len > 0)
            snprintf(error, (size_t)error_len, "%s",
                     "malformed or unexpected relocation response");
        return -1;
    }
    if (((strcmp(state->valuestring, "committed") == 0 ||
          strcmp(state->valuestring, "reverted") == 0 ||
          strcmp(state->valuestring, "finishing") == 0 ||
          strcmp(state->valuestring, "finished") == 0) &&
         mapping->valueint <= 0) ||
        ((strcmp(state->valuestring, "finishing") == 0 ||
          strcmp(state->valuestring, "finished") == 0) &&
         ticket->valueint <= mapping->valueint) ||
        (strcmp(state->valuestring, "finished") == 0 &&
         library->valueint < ticket->valueint)) {
        if (error && error_len > 0)
            snprintf(error, (size_t)error_len, "%s", "invalid relocation generation");
        return -1;
    }
    if (out) {
        memset(out, 0, sizeof(*out));
        ipc__copy_string(out->operation_id, sizeof(out->operation_id), id->valuestring);
        ipc__copy_string(out->state, sizeof(out->state), state->valuestring);
        out->expected_generation = expected->valueint;
        out->mapping_generation = mapping->valueint;
        out->scan_ticket_generation = ticket->valueint;
        out->library_generation = library->valueint;
        out->item_count = count->valueint;
    }
    return 0;
}

static int ipc__relocation_request(const char *socket_path, cJSON *req,
                                   const char *operation_id,
                                   const char *expected_state,
                                   jw_ipc_relocation_status *out,
                                   char *error, int error_len) {
    if (error && error_len > 0) error[0] = '\0';
    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (error && error_len > 0)
            snprintf(error, (size_t)error_len, "%s", "daemon unavailable");
        return -1;
    }
    int rc = ipc__relocation_parse(resp, operation_id, expected_state,
                                   out, error, error_len);
    cJSON_Delete(resp);
    return rc;
}

/* ---- CTL-1: service control (control-ipc-v1) ---- */

static atomic_uint ipc__ctl1_sequence = 1u;

static void ipc__ctl1_next_id(char out[JW_CTL1_ID_MAX + 1]) {
    unsigned value = atomic_fetch_add_explicit(&ipc__ctl1_sequence, 1u,
                                                memory_order_relaxed);
    snprintf(out, JW_CTL1_ID_MAX + 1u, "jawaka-%u", value);
}

static int ipc__object_size(const cJSON *object) {
    int count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, object) {
        if (count == INT_MAX) return -1;
        count++;
    }
    return count;
}

static bool ipc__ctl1_version_and_id(const cJSON *response,
                                     const char *expected_id) {
    const cJSON *version =
        cJSON_GetObjectItemCaseSensitive(response, "v");
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(response, "id");
    return cJSON_IsObject(response) && cJSON_IsNumber(version) &&
           version->valuedouble == (double)JW_CTL1_VERSION &&
           cJSON_IsString(id) && id->valuestring &&
           strcmp(id->valuestring, expected_id) == 0;
}

static bool ipc__service_state_is_known(const char *state) {
    static const char *const states[] = {
        "unavailable", "disabled", "stopped", "stale-generation",
        "starting", "running", "stopping", "backoff", "failed",
    };
    if (!state) return false;
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        if (strcmp(state, states[i]) == 0) return true;
    }
    return false;
}

static bool ipc__service_id_is_valid(const char *id) {
    if (!id) return false;
    size_t len = strlen(id);
    if (len < 3u || len > JW_IPC_SVC_ID_MAX) return false;
    bool saw_dot = false;
    bool component_has_char = false;
    for (size_t i = 0; i < len; i++) {
        unsigned char ch = (unsigned char)id[i];
        if (ch == '.') {
            if (!component_has_char) return false;
            saw_dot = true;
            component_has_char = false;
        } else if ((ch >= (unsigned char)'a' && ch <= (unsigned char)'z') ||
                   (ch >= (unsigned char)'0' && ch <= (unsigned char)'9')) {
            component_has_char = true;
        } else {
            return false;
        }
    }
    return saw_dot && component_has_char;
}

static bool ipc__ctl1_error_message(const cJSON *response,
                                    char *status, int status_len) {
    const cJSON *error =
        cJSON_GetObjectItemCaseSensitive(response, "error");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
    const cJSON *message =
        cJSON_GetObjectItemCaseSensitive(error, "message");
    if (ipc__object_size(response) != 3 || !cJSON_IsObject(error) ||
        (ipc__object_size(error) != 2 && ipc__object_size(error) != 3) ||
        !cJSON_IsString(code) || !code->valuestring ||
        !cJSON_IsString(message) || !message->valuestring) {
        return false;
    }
    if (status && status_len > 0) {
        snprintf(status, (size_t)status_len, "%s", message->valuestring);
    }
    return true;
}

int jw_ipc_service_list(const char *socket_path, jw_ipc_service_info *out,
                        int max, int *out_count) {
    if (out_count) *out_count = 0;
    if (!socket_path || max < 0 || (max > 0 && !out)) return -1;

    char request_id[JW_CTL1_ID_MAX + 1];
    ipc__ctl1_next_id(request_id);
    cJSON *request = cJSON_CreateObject();
    if (!request ||
        !cJSON_AddNumberToObject(request, "v", JW_CTL1_VERSION) ||
        !cJSON_AddStringToObject(request, "op", "list") ||
        !cJSON_AddStringToObject(request, "id", request_id)) {
        cJSON_Delete(request);
        return -1;
    }

    cJSON *response = NULL;
    if (ipc__request_max(socket_path, request, &response,
                         JW_CTL1_MAX_PAYLOAD, true, 250) != 0) {
        return -1;
    }
    const cJSON *services =
        cJSON_GetObjectItemCaseSensitive(response, "services");
    if (!ipc__ctl1_version_and_id(response, request_id) ||
        ipc__object_size(response) != 3 || !cJSON_IsArray(services)) {
        cJSON_Delete(response);
        return -1;
    }

    int copied = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, services) {
        const cJSON *service_id =
            cJSON_GetObjectItemCaseSensitive(item, "service_id");
        const cJSON *desired =
            cJSON_GetObjectItemCaseSensitive(item, "desired_enabled");
        const cJSON *state =
            cJSON_GetObjectItemCaseSensitive(item, "effective_state");
        if (!cJSON_IsObject(item) || ipc__object_size(item) != 3 ||
            !cJSON_IsString(service_id) ||
            !ipc__service_id_is_valid(service_id->valuestring) ||
            !cJSON_IsBool(desired) || !cJSON_IsString(state) ||
            !ipc__service_state_is_known(state->valuestring)) {
            cJSON_Delete(response);
            return -1;
        }
        if (copied < max) {
            jw_ipc_service_info *info = &out[copied++];
            memset(info, 0, sizeof(*info));
            ipc__copy_string(info->id, sizeof(info->id),
                             service_id->valuestring);
            ipc__copy_string(info->state, sizeof(info->state),
                             state->valuestring);
            info->desired_enabled = cJSON_IsTrue(desired);
        }
    }
    cJSON_Delete(response);
    if (out_count) *out_count = copied;
    return 0;
}

int jw_ipc_service_ctl(const char *socket_path, const char *op,
                       const char *service_id, char *status, int status_len) {
    if (status && status_len > 0) status[0] = '\0';
    bool known_op = op &&
        (strcmp(op, "enable") == 0 || strcmp(op, "disable") == 0 ||
         strcmp(op, "run") == 0 || strcmp(op, "stop") == 0 ||
         strcmp(op, "restart") == 0);
    if (!socket_path || !known_op || !ipc__service_id_is_valid(service_id)) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     known_op ? "invalid service id" : "unknown operation");
        }
        return -1;
    }

    char request_id[JW_CTL1_ID_MAX + 1];
    ipc__ctl1_next_id(request_id);
    cJSON *request = cJSON_CreateObject();
    if (!request ||
        !cJSON_AddNumberToObject(request, "v", JW_CTL1_VERSION) ||
        !cJSON_AddStringToObject(request, "op", op) ||
        !cJSON_AddStringToObject(request, "id", request_id) ||
        !cJSON_AddStringToObject(request, "service_id", service_id)) {
        cJSON_Delete(request);
        return -1;
    }

    cJSON *response = NULL;
    /* Settings -> Services calls this from the launcher's render thread, so
     * the deadline has to stay short enough that a busy daemon degrades to
     * "Request failed" instead of a frozen UI. Every CTL-1 control op now
     * replies immediately -- stop/disable/restart initiate the stop sequence
     * and let the daemon's tick complete it -- so no legitimate reply needs
     * anywhere near the old 30 s ceiling. */
    if (ipc__request_max(socket_path, request, &response,
                         JW_CTL1_MAX_PAYLOAD, true, 2000) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "daemon unreachable");
        }
        return -1;
    }
    if (!ipc__ctl1_version_and_id(response, request_id)) {
        cJSON_Delete(response);
        return -1;
    }

    const cJSON *ok = cJSON_GetObjectItemCaseSensitive(response, "ok");
    if (ipc__object_size(response) == 3 && cJSON_IsTrue(ok)) {
        cJSON_Delete(response);
        return 0;
    }
    (void)ipc__ctl1_error_message(response, status, status_len);
    cJSON_Delete(response);
    if (status && status_len > 0 && !status[0]) {
        snprintf(status, (size_t)status_len, "%s", "malformed response");
    }
    return -1;
}

int jw_ipc_relocation_prepare(const char *socket_path, const char *operation_id,
                              int expected_generation,
                              const jw_ipc_relocation_item *items, int item_count,
                              jw_ipc_relocation_status *out,
                              char *error, int error_len) {
    if (!operation_id || !operation_id[0] || expected_generation < 0 ||
        !items || item_count <= 0 || item_count > 256) return -1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "library-relocate-prepare");
    cJSON_AddStringToObject(req, "operation_id", operation_id);
    cJSON_AddNumberToObject(req, "expected_generation", expected_generation);
    cJSON *array = cJSON_AddArrayToObject(req, "items");
    for (int i = 0; i < item_count; i++) {
        const jw_ipc_relocation_identity *identities[2] = {
            &items[i].old_identity, &items[i].new_identity
        };
        const char *names[2] = {"old", "new"};
        cJSON *item = cJSON_CreateObject();
        for (int j = 0; j < 2; j++) {
            cJSON *identity = cJSON_AddObjectToObject(item, names[j]);
            cJSON_AddStringToObject(identity, "source_id",
                                    identities[j]->source_id ? identities[j]->source_id : "");
            cJSON_AddStringToObject(identity, "rom_relpath",
                                    identities[j]->rom_relpath ? identities[j]->rom_relpath : "");
            if (identities[j]->image_root_kind && identities[j]->image_root_kind[0]) {
                cJSON_AddStringToObject(identity, "image_root_kind",
                                        identities[j]->image_root_kind);
                cJSON_AddStringToObject(identity, "image_relpath",
                                        identities[j]->image_relpath
                                            ? identities[j]->image_relpath : "");
            }
        }
        cJSON_AddItemToArray(array, item);
    }
    return ipc__relocation_request(socket_path, req, operation_id, "prepared",
                                   out, error, error_len);
}

static int ipc__relocation_simple(const char *socket_path, const char *type,
                                  const char *operation_id,
                                  const char *expected_state,
                                  jw_ipc_relocation_status *out,
                                  char *error, int error_len) {
    if (!operation_id || !operation_id[0]) return -1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", type);
    cJSON_AddStringToObject(req, "operation_id", operation_id);
    return ipc__relocation_request(socket_path, req, operation_id, expected_state,
                                   out, error, error_len);
}

int jw_ipc_relocation_get_status(const char *s, const char *id,
                                 jw_ipc_relocation_status *out, char *e, int n) {
    return ipc__relocation_simple(s, "library-relocate-status", id, NULL, out, e, n);
}
int jw_ipc_relocation_commit(const char *s, const char *id,
                             jw_ipc_relocation_status *out, char *e, int n) {
    return ipc__relocation_simple(s, "library-relocate-commit", id, "committed", out, e, n);
}
int jw_ipc_relocation_revert(const char *s, const char *id,
                             jw_ipc_relocation_status *out, char *e, int n) {
    return ipc__relocation_simple(s, "library-relocate-revert", id, "reverted", out, e, n);
}
int jw_ipc_relocation_abort(const char *s, const char *id,
                            jw_ipc_relocation_status *out, char *e, int n) {
    return ipc__relocation_simple(s, "library-relocate-abort", id, "aborted", out, e, n);
}
int jw_ipc_relocation_finish(const char *s, const char *id,
                             jw_ipc_relocation_status *out, char *e, int n) {
    int rc = ipc__relocation_simple(s, "library-relocate-finish", id, NULL, out, e, n);
    if (rc == 0 && out && strcmp(out->state, "finishing") != 0 &&
        strcmp(out->state, "finished") != 0) {
        if (e && n > 0) snprintf(e, (size_t)n, "%s", "unexpected finish state");
        return -1;
    }
    if (rc == 0 && out && strcmp(out->state, "finishing") == 0 &&
        out->scan_ticket_generation <= out->mapping_generation) {
        if (e && n > 0) snprintf(e, (size_t)n, "%s", "invalid reconciliation ticket");
        return -1;
    }
    if (rc == 0 && out && strcmp(out->state, "finished") == 0 &&
        (out->scan_ticket_generation <= out->mapping_generation ||
         out->library_generation < out->scan_ticket_generation)) {
        if (e && n > 0) snprintf(e, (size_t)n, "%s",
                                 "invalid completed reconciliation generation");
        return -1;
    }
    return rc;
}

int jw_ipc_scan_library(const char *socket_path, char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "scan-library");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s", "scan failed: daemon unavailable");
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        if (status) snprintf(status, (size_t)status_len, "%s", "scan failed: daemon returned error");
        cJSON_Delete(resp);
        return -1;
    }

    if (status) {
        const cJSON *gc = cJSON_GetObjectItemCaseSensitive(resp, "game_count");
        const cJSON *ac = cJSON_GetObjectItemCaseSensitive(resp, "app_count");
        if (cJSON_IsNumber(gc) && cJSON_IsNumber(ac))
            snprintf(status, (size_t)status_len,
                     "scan complete: %d games, %d apps", gc->valueint, ac->valueint);
        else {
            const cJSON *action = cJSON_GetObjectItemCaseSensitive(resp, "action");
            if (cJSON_IsString(action) && action->valuestring &&
                strstr(action->valuestring, "queued")) {
                snprintf(status, (size_t)status_len, "%s", "scan already running; queued rescan");
            } else {
                snprintf(status, (size_t)status_len, "%s", "scan started");
            }
        }
    }

    cJSON_Delete(resp);
    return 0;
}

/* Send without waiting for the reply. Haptics fire on every cursor move, and a
   full request/response round trip on the UI thread would stall drawing for as
   long as the daemon takes to answer -- worst in a held-direction repeat, where
   it happens once per repeat before the next frame. The daemon ignores SIGPIPE,
   so it is safe for it to answer a socket we have already closed. */
static int ipc__notify(const char *socket_path, cJSON *req) {
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return -1;

    jw_ipc_client *client = NULL;
    int rc = jw_ipc_client_connect(socket_path, &client);
    if (rc == 0) {
        rc = jw_ipc_client_send(client, body, strlen(body));
        jw_ipc_client_close(client);
    }
    cJSON_free(body);
    return rc;
}

int jw_ipc_rumble(const char *socket_path, const char *event) {
    if (!event) return -1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "rumble");
    cJSON_AddStringToObject(req, "event", event);
    return ipc__notify(socket_path, req);
}

int jw_ipc_refresh_osd_appearance(const char *socket_path) {
    if (!socket_path || !socket_path[0]) return -1;
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "refresh-osd-appearance");
    return ipc__notify(socket_path, req);
}

int jw_ipc_rumble_preview(const char *socket_path, int strength) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "rumble");
    cJSON_AddStringToObject(req, "event", "preview");
    cJSON_AddNumberToObject(req, "strength", strength);
    return ipc__notify(socket_path, req);
}

int jw_ipc_set_input_shortcuts(const char *socket_path,
                               const jw_input_shortcuts *shortcuts,
                               bool screenshots_enabled,
                               bool recording_enabled) {
    if (!shortcuts || !jw_input_shortcuts_valid(shortcuts)) {
        return -1;
    }
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "set-input-shortcuts");
    cJSON *buttons = cJSON_AddArrayToObject(req, "buttons");
    for (int i = 0; i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        const char *name = jw_input_shortcut_button_name(shortcuts->buttons[i]);
        if (!name || !buttons) {
            cJSON_Delete(req);
            return -1;
        }
        cJSON_AddItemToArray(buttons, cJSON_CreateString(name));
    }
    cJSON_AddBoolToObject(req, "screenshots_enabled", screenshots_enabled);
    cJSON_AddBoolToObject(req, "recording_enabled", recording_enabled);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    int rc = ipc__type_is(resp, "ok") ? 0 : -1;
    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_library_status_full(const char *socket_path, jw_ipc_library_status_info *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "library-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    if (!ipc__type_is(resp, "library-status")) {
        cJSON_Delete(resp);
        return -1;
    }

    const cJSON *generation = cJSON_GetObjectItemCaseSensitive(resp, "generation");
    if (out) {
        out->generation = cJSON_IsNumber(generation) ? generation->valueint : 0;
        out->scan_running = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "scan_running"));
        out->pending_rescan = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "pending_rescan"));
        out->library_populated = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "library_populated"));
        const cJSON *reason = cJSON_GetObjectItemCaseSensitive(resp, "scan_reason");
        if (cJSON_IsString(reason)) {
            ipc__copy_string(out->scan_reason, sizeof(out->scan_reason), reason->valuestring);
        }
        const cJSON *error = cJSON_GetObjectItemCaseSensitive(resp, "scan_error");
        if (cJSON_IsString(error)) {
            ipc__copy_string(out->scan_error, sizeof(out->scan_error), error->valuestring);
        }
        const cJSON *scrape = cJSON_GetObjectItemCaseSensitive(resp, "scrape_state");
        if (cJSON_IsString(scrape)) {
            ipc__copy_string(out->scrape_state, sizeof(out->scrape_state), scrape->valuestring);
        }
        const cJSON *storage_generation =
            cJSON_GetObjectItemCaseSensitive(resp, "storage_health_generation");
        out->storage_health_generation = cJSON_IsNumber(storage_generation)
            ? storage_generation->valueint : -1;
    }
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_library_status(const char *socket_path, int *out_generation) {
    jw_ipc_library_status_info info;
    if (jw_ipc_library_status_full(socket_path, &info) != 0) {
        return -1;
    }
    if (out_generation) {
        *out_generation = info.generation;
    }
    return 0;
}

int jw_ipc_get_storage_status(const char *socket_path, const char *source,
                              jw_ipc_storage_status_info *out,
                              char *status, int status_len) {
    if (out) {
        memset(out, 0, sizeof(*out));
        out->battery_percent = -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "storage-status");
    cJSON_AddStringToObject(req, "source", source && source[0] ? source : "secondary_sd");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s", "storage status unavailable");
        return -1;
    }
    if (!ipc__type_is(resp, "storage-status")) {
        if (status) snprintf(status, (size_t)status_len, "%s", "storage status failed");
        cJSON_Delete(resp);
        return -1;
    }

    if (out) {
        const cJSON *v = NULL;
        v = cJSON_GetObjectItemCaseSensitive(resp, "source");
        if (cJSON_IsString(v)) ipc__copy_string(out->source, sizeof(out->source), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(resp, "label");
        if (cJSON_IsString(v)) ipc__copy_string(out->label, sizeof(out->label), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(resp, "mount_path");
        if (cJSON_IsString(v)) ipc__copy_string(out->mount_path, sizeof(out->mount_path), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(v)) ipc__copy_string(out->message, sizeof(out->message), v->valuestring);
        out->present = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "present"));
        out->mounted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "mounted"));
        out->busy = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "busy"));
        out->can_unmount = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "can_unmount"));
#define IPC__STORAGE_STRING(field, key) \
        v = cJSON_GetObjectItemCaseSensitive(resp, key); \
        if (cJSON_IsString(v)) ipc__copy_string(out->field, sizeof(out->field), v->valuestring)
        IPC__STORAGE_STRING(access, "access");
        IPC__STORAGE_STRING(cause, "cause");
        IPC__STORAGE_STRING(repair, "repair");
        IPC__STORAGE_STRING(fs_type, "fs_type");
        IPC__STORAGE_STRING(uuid, "uuid");
        IPC__STORAGE_STRING(volume_label, "volume_label");
        IPC__STORAGE_STRING(kernel_message, "kernel_message");
        IPC__STORAGE_STRING(repair_unavailable_reason, "repair_unavailable_reason");
        IPC__STORAGE_STRING(repair_mode, "repair_mode");
        out->dirty_at_boot = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "dirty_at_boot"));
        out->block_write_protected =
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "block_write_protected"));
        out->repair_supported =
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "repair_supported"));
        out->warning_pending =
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "warning_pending"));
        out->external_power =
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "external_power"));
        v = cJSON_GetObjectItemCaseSensitive(resp, "battery_percent");
        if (cJSON_IsNumber(v)) out->battery_percent = v->valueint;
        IPC__STORAGE_STRING(hold_trigger, "hold_trigger");
        v = cJSON_GetObjectItemCaseSensitive(resp, "health_generation");
        out->health_generation = cJSON_IsNumber(v) ? v->valueint : -1;
        const cJSON *last = cJSON_GetObjectItemCaseSensitive(resp, "last_repair");
        if (cJSON_IsObject(last)) {
            const cJSON *saved_resp = resp;
            resp = (cJSON *)last;
            out->last_repair_valid = true;
            IPC__STORAGE_STRING(last_repair_request_id, "request_id");
            IPC__STORAGE_STRING(last_repair_outcome, "outcome");
            IPC__STORAGE_STRING(last_repair_mount_state, "mount_state");
            IPC__STORAGE_STRING(last_repair_mode, "mode");
            IPC__STORAGE_STRING(last_repair_origin, "origin");
            IPC__STORAGE_STRING(last_repair_trigger, "trigger");
            out->last_repair_acknowledged =
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "acknowledged"));
            out->last_repair_changes_complete =
                cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "changes_complete"));
            v = cJSON_GetObjectItemCaseSensitive(resp, "reported_changes");
            out->last_repair_reported_changes = cJSON_IsNumber(v) ? v->valueint : 0;
            resp = (cJSON *)saved_resp;
        }
#undef IPC__STORAGE_STRING
    }
    if (status) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s", "storage status ready");
        }
    }

    cJSON_Delete(resp);
    return 0;
}

static int ipc__storage_simple_request(const char *socket_path, cJSON *req,
                                       char *status, int status_len) {
    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s", "daemon unavailable");
        return -1;
    }
    int rc = ipc__type_is(resp, "ok") ? 0 : -1;
    if (status) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (!cJSON_IsString(message)) {
            message = cJSON_GetObjectItemCaseSensitive(resp, "error");
        }
        snprintf(status, (size_t)status_len, "%s",
                 cJSON_IsString(message) && message->valuestring ? message->valuestring : "");
    }
    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_storage_warning_ack(const char *socket_path, const char *source) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "storage-warning-ack");
    cJSON_AddStringToObject(req, "source", source && source[0] ? source : "launcher_sd");
    return ipc__storage_simple_request(socket_path, req, NULL, 0);
}

int jw_ipc_storage_repair_request(const char *socket_path, const char *source,
                                  const char *mode, bool allow_battery,
                                  char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "storage-repair-request");
    cJSON_AddStringToObject(req, "source", source && source[0] ? source : "launcher_sd");
    cJSON_AddStringToObject(req, "mode", mode && mode[0] ? mode : "repair");
    cJSON_AddBoolToObject(req, "allow_battery", allow_battery);
    return ipc__storage_simple_request(socket_path, req, status, status_len);
}

int jw_ipc_storage_repair_result_ack(const char *socket_path, const char *request_id) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "storage-repair-result-ack");
    cJSON_AddStringToObject(req, "request_id", request_id ? request_id : "");
    return ipc__storage_simple_request(socket_path, req, NULL, 0);
}

int jw_ipc_safe_unmount_storage(const char *socket_path, const char *source,
                                char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "storage-action");
    cJSON_AddStringToObject(req, "source", source && source[0] ? source : "secondary_sd");
    cJSON_AddStringToObject(req, "action", "safe-unmount");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s", "unmount failed: daemon unavailable");
        return -1;
    }

    bool ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s",
                     ok ? "Secondary SD unmounted" : "unmount failed");
        }
    }
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_open_menu(const char *socket_path) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "open-menu");
    cJSON *resp = NULL;
    int rc = ipc__request(socket_path, req, &resp);
    if (resp) cJSON_Delete(resp);
    return rc;
}

static int ipc__launch_game(const char *socket_path, const char *system,
                            const char *rom_path, const char *resume_policy,
                            char *status, int status_len) {
    if (!system || !system[0] || !rom_path || !rom_path[0]) {
        if (status) snprintf(status, (size_t)status_len, "%s", "launch failed: missing game");
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "launch-game");
    cJSON_AddStringToObject(req, "system", system);
    cJSON_AddStringToObject(req, "rom_path", rom_path);
    if (resume_policy && resume_policy[0]) {
        cJSON_AddStringToObject(req, "resume_policy", resume_policy);
    }

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s", "launch failed: daemon unavailable");
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "launch failed: %s", message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "launch failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (status) snprintf(status, (size_t)status_len, "%s", "launch requested");
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_launch_game(const char *socket_path, const char *system,
                       const char *rom_path, char *status, int status_len) {
    return ipc__launch_game(socket_path, system, rom_path, NULL, status, status_len);
}

int jw_ipc_launch_game_switcher(const char *socket_path, const char *system,
                                const char *rom_path, char *status,
                                int status_len) {
    return ipc__launch_game(socket_path, system, rom_path, "switcher-latest",
                            status, status_len);
}

int jw_ipc_open_switcher(const char *socket_path, char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "open-switcher");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s",
                             "open-switcher failed: daemon unavailable");
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "open-switcher failed: %s",
                         message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "open-switcher failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (status) snprintf(status, (size_t)status_len, "%s", "switcher requested");
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_switch_game(const char *socket_path, const char *system,
                       const char *rom_path, char *status, int status_len) {
    if (!system || !system[0] || !rom_path || !rom_path[0]) {
        if (status) snprintf(status, (size_t)status_len, "%s",
                             "switch failed: missing game");
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "switch-game");
    cJSON_AddStringToObject(req, "system", system);
    cJSON_AddStringToObject(req, "rom_path", rom_path);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s",
                             "switch failed: daemon unavailable");
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "switch failed: %s",
                         message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "switch failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (status) snprintf(status, (size_t)status_len, "%s", "switch requested");
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_launch_app(const char *socket_path, const char *pak_dir,
                      char *status, int status_len) {
    if (!pak_dir || !pak_dir[0]) {
        if (status) snprintf(status, (size_t)status_len, "%s", "launch failed: missing app");
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "launch-app");
    cJSON_AddStringToObject(req, "pak_dir", pak_dir);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status) snprintf(status, (size_t)status_len, "%s", "launch failed: daemon unavailable");
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "launch failed: %s", message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "launch failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (status) snprintf(status, (size_t)status_len, "%s", "app launch requested");
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_get_retroarch_session(const char *socket_path,
                                 jw_ipc_retroarch_session_info *out,
                                 char *status, int status_len) {
    if (!out) {
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->disk_count = 0;
    out->disk_slot = -1;
    out->state_slot = -1;
    ipc__copy_string(out->command_result, sizeof(out->command_result), "unavailable");

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "retroarch-session");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "RetroArch session unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "retroarch-session")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status && status_len > 0) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "RetroArch session failed: %s",
                         message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "RetroArch session failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    const cJSON *active = cJSON_GetObjectItemCaseSensitive(resp, "active");
    const cJSON *command_ok = cJSON_GetObjectItemCaseSensitive(resp, "command_ok");
    const cJSON *command_result = cJSON_GetObjectItemCaseSensitive(resp, "command_result");
    const cJSON *system = cJSON_GetObjectItemCaseSensitive(resp, "system");
    const cJSON *rom_path = cJSON_GetObjectItemCaseSensitive(resp, "rom_path");
    const cJSON *core_path = cJSON_GetObjectItemCaseSensitive(resp, "core_path");
    const cJSON *core_id = cJSON_GetObjectItemCaseSensitive(resp, "core_id");
    const cJSON *core_config_folder =
        cJSON_GetObjectItemCaseSensitive(resp, "core_config_folder");
    const cJSON *disk_count = cJSON_GetObjectItemCaseSensitive(resp, "disk_count");
    const cJSON *disk_slot = cJSON_GetObjectItemCaseSensitive(resp, "disk_slot");
    const cJSON *savestate_supported =
        cJSON_GetObjectItemCaseSensitive(resp, "savestate_supported");
    const cJSON *state_slot = cJSON_GetObjectItemCaseSensitive(resp, "state_slot");

    out->active = cJSON_IsTrue(active);
    out->command_ok = cJSON_IsTrue(command_ok);
    if (cJSON_IsString(command_result) && command_result->valuestring) {
        ipc__copy_string(out->command_result, sizeof(out->command_result),
                         command_result->valuestring);
    }
    if (cJSON_IsString(system) && system->valuestring) {
        ipc__copy_string(out->system, sizeof(out->system), system->valuestring);
    }
    if (cJSON_IsString(rom_path) && rom_path->valuestring) {
        ipc__copy_string(out->rom_path, sizeof(out->rom_path), rom_path->valuestring);
    }
    if (cJSON_IsString(core_path) && core_path->valuestring) {
        ipc__copy_string(out->core_path, sizeof(out->core_path), core_path->valuestring);
    }
    if (cJSON_IsString(core_id) && core_id->valuestring) {
        ipc__copy_string(out->core_id, sizeof(out->core_id), core_id->valuestring);
    }
    if (cJSON_IsString(core_config_folder) && core_config_folder->valuestring) {
        ipc__copy_string(out->core_config_folder,
                         sizeof(out->core_config_folder),
                         core_config_folder->valuestring);
    }
    if (cJSON_IsNumber(disk_count)) {
        out->disk_count = disk_count->valueint;
    }
    if (cJSON_IsNumber(disk_slot)) {
        out->disk_slot = disk_slot->valueint;
    }
    out->savestate_supported = cJSON_IsTrue(savestate_supported);
    if (cJSON_IsNumber(state_slot)) {
        out->state_slot = state_slot->valueint;
    }

    if (status && status_len > 0) {
        snprintf(status, (size_t)status_len, "%s",
                 out->active ? "RetroArch session active" : "No active RetroArch session");
    }

    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_retroarch_action(const char *socket_path, const char *action,
                            int value, char *status, int status_len) {
    if (!action || !action[0]) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "RetroArch action missing");
        }
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "retroarch-action");
    cJSON_AddStringToObject(req, "action", action);
    cJSON_AddNumberToObject(req, "value", value);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "RetroArch action failed: daemon unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status && status_len > 0) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "RetroArch action failed: %s",
                         message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "RetroArch action failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (status && status_len > 0) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s", "RetroArch action requested");
        }
    }

    cJSON_Delete(resp);
    return 0;
}

static bool ipc__shader_result(const char *text, jw_ra_result *out) {
    if (!text || !out) return false;
    if (strcmp(text, "ok") == 0) *out = JW_RA_OK;
    else if (strcmp(text, "timeout") == 0) *out = JW_RA_TIMEOUT;
    else if (strcmp(text, "unsupported") == 0) *out = JW_RA_UNSUPPORTED;
    else if (strcmp(text, "parse_error") == 0) *out = JW_RA_PARSE_ERROR;
    else if (strcmp(text, "socket_error") == 0) *out = JW_RA_SOCKET_ERROR;
    else return false;
    return true;
}

static const char *ipc__shader_result_name(jw_ra_result result) {
    switch (result) {
        case JW_RA_OK: return "ok";
        case JW_RA_TIMEOUT: return "timeout";
        case JW_RA_UNSUPPORTED: return "unsupported";
        case JW_RA_PARSE_ERROR: return "parse_error";
        case JW_RA_SOCKET_ERROR: return "socket_error";
    }
    return "unknown";
}

static bool ipc__shader_outcome(const char *text,
                                jw_ra_shader_outcome *out) {
    if (!text || !out) return false;
    if (strcmp(text, "ok") == 0) *out = JW_RA_SHADER_OK;
    else if (strcmp(text, "none") == 0) *out = JW_RA_SHADER_NONE;
    else if (strcmp(text, "absent") == 0) *out = JW_RA_SHADER_ABSENT;
    else if (strcmp(text, "missing") == 0) *out = JW_RA_SHADER_ERR_MISSING;
    else if (strcmp(text, "unsupported") == 0) *out = JW_RA_SHADER_ERR_UNSUPPORTED;
    else if (strcmp(text, "apply") == 0) *out = JW_RA_SHADER_ERR_APPLY;
    else if (strcmp(text, "error") == 0) *out = JW_RA_SHADER_ERR;
    else return false;
    return true;
}

static const char *ipc__shader_scope(jw_ra_shader_scope scope) {
    switch (scope) {
        case JW_RA_SHADER_SCOPE_GAME: return "game";
        case JW_RA_SHADER_SCOPE_PARENT: return "parent";
        case JW_RA_SHADER_SCOPE_CORE: return "core";
        case JW_RA_SHADER_SCOPE_GLOBAL: return "global";
    }
    return NULL;
}

static int ipc__retroarch_shader(const char *socket_path,
                                 const char *operation,
                                 const char *path,
                                 const char *scope,
                                 jw_ipc_retroarch_shader_reply *out,
                                 char *status, int status_len) {
    if (!operation || !out) return -1;
    memset(out, 0, sizeof(*out));
    out->result = JW_RA_PARSE_ERROR;
    out->outcome = JW_RA_SHADER_ERR;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "retroarch-shader");
    cJSON_AddStringToObject(req, "operation", operation);
    if (path) cJSON_AddStringToObject(req, "path", path);
    if (scope) cJSON_AddStringToObject(req, "scope", scope);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0)
            snprintf(status, (size_t)status_len, "%s", "RetroArch is not available");
        return -1;
    }

    const cJSON *reply_op = cJSON_GetObjectItemCaseSensitive(resp, "operation");
    const cJSON *result = cJSON_GetObjectItemCaseSensitive(resp, "result");
    const cJSON *outcome = cJSON_GetObjectItemCaseSensitive(resp, "outcome");
    const cJSON *reply_path = cJSON_GetObjectItemCaseSensitive(resp, "path");
    bool valid = (ipc__type_is(resp, "ok") || ipc__type_is(resp, "error")) &&
                 cJSON_IsString(reply_op) &&
                 strcmp(reply_op->valuestring, operation) == 0 &&
                 cJSON_IsString(result) &&
                 ipc__shader_result(result->valuestring, &out->result);
    if (valid && out->result == JW_RA_OK) {
        valid = cJSON_IsString(outcome) &&
                ipc__shader_outcome(outcome->valuestring, &out->outcome);
    }
    if (valid && cJSON_IsString(reply_path)) {
        ipc__copy_string(out->path, sizeof(out->path), reply_path->valuestring);
    }
    if (!valid) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status && status_len > 0)
            snprintf(status, (size_t)status_len, "%s",
                     cJSON_IsString(message) ? message->valuestring
                                             : "Malformed shader reply");
        cJSON_Delete(resp);
        return -1;
    }
    if (status && status_len > 0) {
        snprintf(status, (size_t)status_len, "%s",
                 ipc__shader_result_name(out->result));
    }
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_retroarch_shader_get(const char *socket_path,
                                jw_ipc_retroarch_shader_reply *out,
                                char *status, int status_len) {
    return ipc__retroarch_shader(socket_path, "get", NULL, NULL,
                                 out, status, status_len);
}

int jw_ipc_retroarch_shader_set(const char *socket_path, const char *path,
                                jw_ipc_retroarch_shader_reply *out,
                                char *status, int status_len) {
    if (!path || !path[0]) return -1;
    return ipc__retroarch_shader(socket_path, "set", path, NULL,
                                 out, status, status_len);
}

int jw_ipc_retroarch_shader_restore(const char *socket_path, const char *path,
                                    jw_ipc_retroarch_shader_reply *out,
                                    char *status, int status_len) {
    if (!path || !path[0]) return -1;
    return ipc__retroarch_shader(socket_path, "restore", path, NULL,
                                 out, status, status_len);
}

int jw_ipc_retroarch_shader_clear(const char *socket_path,
                                  jw_ipc_retroarch_shader_reply *out,
                                  char *status, int status_len) {
    return ipc__retroarch_shader(socket_path, "clear", NULL, NULL,
                                 out, status, status_len);
}

int jw_ipc_retroarch_shader_save(const char *socket_path,
                                 jw_ra_shader_scope scope,
                                 jw_ipc_retroarch_shader_reply *out,
                                 char *status, int status_len) {
    const char *token = ipc__shader_scope(scope);
    if (!token) return -1;
    return ipc__retroarch_shader(socket_path, "save", NULL, token,
                                 out, status, status_len);
}

int jw_ipc_retroarch_shader_remove(const char *socket_path,
                                   jw_ra_shader_scope scope,
                                   jw_ipc_retroarch_shader_reply *out,
                                   char *status, int status_len) {
    const char *token = ipc__shader_scope(scope);
    if (!token) return -1;
    return ipc__retroarch_shader(socket_path, "remove", NULL, token,
                                 out, status, status_len);
}

int jw_ipc_reset_retroarch_config(const char *socket_path,
                                  char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "reset-retroarch-config");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "RetroArch reset failed: daemon unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status && status_len > 0) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "RetroArch reset failed: %s", message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "RetroArch reset failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (status && status_len > 0) {
        snprintf(status, (size_t)status_len, "%s", "RetroArch config reset");
    }
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_shutdown(const char *socket_path) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "shutdown");
    cJSON *resp = NULL;
    int rc = ipc__request(socket_path, req, &resp);
    if (resp) cJSON_Delete(resp);
    return rc;
}

int jw_ipc_exit_stock(const char *socket_path) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "exit-stock");
    cJSON *resp = NULL;
    int rc = ipc__request(socket_path, req, &resp);
    if (resp) cJSON_Delete(resp);
    return rc;
}

int jw_ipc_platform_action(const char *socket_path, const char *action, int value) {
    if (!action || !action[0]) {
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", action);
    cJSON_AddNumberToObject(req, "value", value);
    cJSON *resp = NULL;
    int rc = ipc__request(socket_path, req, &resp);
    if (resp) cJSON_Delete(resp);
    return rc;
}

int jw_ipc_frontend_ready(const char *socket_path, const char *role) {
    if (!role || !role[0]) {
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "frontend-ready");
    cJSON_AddStringToObject(req, "role", role);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int ok = ipc__type_is(resp, "ok");
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_platform_levels_full(const char *socket_path, int *out_brightness,
                                int *out_volume, int *out_hdmi_revert_seconds) {
    if (out_brightness) {
        *out_brightness = -1;
    }
    if (out_volume) {
        *out_volume = -1;
    }
    if (out_hdmi_revert_seconds) {
        *out_hdmi_revert_seconds = -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-levels");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int rc = -1;
    const cJSON *brightness = cJSON_GetObjectItemCaseSensitive(resp, "brightness_percent");
    const cJSON *volume = cJSON_GetObjectItemCaseSensitive(resp, "volume_percent");
    const cJSON *revert = cJSON_GetObjectItemCaseSensitive(resp, "hdmi_revert_seconds");
    if (cJSON_IsNumber(brightness) && cJSON_IsNumber(volume)) {
        if (out_brightness) {
            *out_brightness = brightness->valueint;
        }
        if (out_volume) {
            *out_volume = volume->valueint;
        }
        if (out_hdmi_revert_seconds && cJSON_IsNumber(revert)) {
            *out_hdmi_revert_seconds = revert->valueint;
        }
        rc = 0;
    }

    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_platform_levels(const char *socket_path, int *out_brightness,
                           int *out_volume) {
    return jw_ipc_platform_levels_full(socket_path, out_brightness, out_volume, NULL);
}

int jw_ipc_platform_brightness(const char *socket_path, int *out_percent) {
    return jw_ipc_platform_levels(socket_path, out_percent, NULL);
}

int jw_ipc_platform_power_status(const char *socket_path,
                                 int *out_battery_percent,
                                 int *out_charging) {
    return jw_ipc_platform_power_status_full(socket_path, out_battery_percent,
                                             out_charging, NULL, NULL);
}

int jw_ipc_platform_power_status_full(const char *socket_path, int *out_battery_percent,
                                      int *out_charging, int *out_volume_percent,
                                      int *out_hdmi_revert_seconds) {
    if (out_hdmi_revert_seconds) {
        *out_hdmi_revert_seconds = -1;
    }
    if (out_battery_percent) {
        *out_battery_percent = -1;
    }
    if (out_charging) {
        *out_charging = -1;
    }
    if (out_volume_percent) {
        *out_volume_percent = -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int rc = -1;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    const cJSON *battery = cJSON_GetObjectItemCaseSensitive(status, "battery_percent");
    const cJSON *charging = cJSON_GetObjectItemCaseSensitive(status, "charging");
    const cJSON *volume = cJSON_GetObjectItemCaseSensitive(status, "volume_percent");
    if (cJSON_IsNumber(battery) || cJSON_IsNumber(charging)) {
        if (out_battery_percent) {
            *out_battery_percent = cJSON_IsNumber(battery) ? battery->valueint : -1;
        }
        if (out_charging) {
            *out_charging = cJSON_IsNumber(charging) ? charging->valueint : -1;
        }
        rc = 0;
    }
    if (out_volume_percent && cJSON_IsNumber(volume)) {
        *out_volume_percent = volume->valueint;
    }
    const cJSON *revert = cJSON_GetObjectItemCaseSensitive(resp, "hdmi_revert_seconds");
    if (out_hdmi_revert_seconds && cJSON_IsNumber(revert)) {
        *out_hdmi_revert_seconds = revert->valueint;
    }

    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_set_brightness(const char *socket_path, int percent,
                          int *out_percent, char *status, int status_len) {
    if (out_percent) {
        *out_percent = -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", "set-brightness");
    cJSON_AddNumberToObject(req, "value", percent);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "brightness failed: daemon unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status && status_len > 0) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "brightness failed: %s", message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "brightness failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    const cJSON *value = cJSON_GetObjectItemCaseSensitive(resp, "value");
    int resolved = cJSON_IsNumber(value) ? value->valueint : percent;
    if (out_percent) {
        *out_percent = resolved;
    }
    if (status && status_len > 0) {
        snprintf(status, (size_t)status_len, "brightness: %d%%", resolved);
    }

    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_platform_volume(const char *socket_path, int *out_percent) {
    return jw_ipc_platform_levels(socket_path, NULL, out_percent);
}

int jw_ipc_hdmi_revert_status(const char *socket_path, int *out_seconds) {
    if (out_seconds) {
        *out_seconds = 0;
    }
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "hdmi-revert-status");
    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    int rc = -1;
    const cJSON *secs = cJSON_GetObjectItemCaseSensitive(resp, "seconds");
    if (cJSON_IsNumber(secs)) {
        if (out_seconds) {
            *out_seconds = secs->valueint;
        }
        rc = 0;
    }
    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_hdmi_revert_keep(const char *socket_path) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "hdmi-revert-keep");
    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_set_volume(const char *socket_path, int percent,
                      int *out_percent, char *status, int status_len) {
    if (out_percent) {
        *out_percent = -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", "set-volume");
    cJSON_AddNumberToObject(req, "value", percent);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "volume failed: daemon unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "ok")) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status && status_len > 0) {
            if (cJSON_IsString(message) && message->valuestring) {
                snprintf(status, (size_t)status_len, "volume failed: %s", message->valuestring);
            } else {
                snprintf(status, (size_t)status_len, "%s", "volume failed");
            }
        }
        cJSON_Delete(resp);
        return -1;
    }

    const cJSON *value = cJSON_GetObjectItemCaseSensitive(resp, "value");
    int resolved = cJSON_IsNumber(value) ? value->valueint : percent;
    if (out_percent) {
        *out_percent = resolved;
    }
    if (status && status_len > 0) {
        snprintf(status, (size_t)status_len, "volume: %d%%", resolved);
    }

    cJSON_Delete(resp);
    return 0;
}

static void ipc__audio_status_init(jw_ipc_audio_status *out_status) {
    if (!out_status) {
        return;
    }
    out_status->output = JW_PLATFORM_AUDIO_OUTPUT_UNKNOWN;
    out_status->available_outputs = 0;
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        out_status->volume_percent[i] = -1;
    }
    out_status->test_playing = 0;
}

int jw_ipc_platform_audio_status(const char *socket_path,
                                 jw_ipc_audio_status *out_status) {
    ipc__audio_status_init(out_status);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-audio-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int rc = -1;
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    if (cJSON_IsObject(status)) {
        const cJSON *output = cJSON_GetObjectItemCaseSensitive(status, "audio_output");
        jw_platform_audio_output parsed = JW_PLATFORM_AUDIO_OUTPUT_UNKNOWN;
        if (cJSON_IsString(output) && output->valuestring &&
            jw_platform_parse_audio_output(output->valuestring, &parsed)) {
            out_status->output = parsed;
        }

        const cJSON *available = cJSON_GetObjectItemCaseSensitive(status,
                                                                  "audio_available_outputs");
        if (cJSON_IsArray(available)) {
            const cJSON *item = NULL;
            cJSON_ArrayForEach(item, available) {
                if (!cJSON_IsString(item) || !item->valuestring) {
                    continue;
                }
                if (jw_platform_parse_audio_output(item->valuestring, &parsed) &&
                    parsed >= 0 && parsed < JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
                    out_status->available_outputs |= JW_PLATFORM_AUDIO_OUTPUT_BIT(parsed);
                }
            }
        }

        const cJSON *volumes = cJSON_GetObjectItemCaseSensitive(status, "audio_volumes");
        if (cJSON_IsObject(volumes)) {
            for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
                const char *name = jw_platform_audio_output_name((jw_platform_audio_output)i);
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(volumes, name);
                if (cJSON_IsNumber(v)) {
                    int percent = v->valueint;
                    if (percent < 0) percent = 0;
                    if (percent > 100) percent = 100;
                    out_status->volume_percent[i] = percent;
                }
            }
        }

        const cJSON *test_playing = cJSON_GetObjectItemCaseSensitive(status,
                                                                     "audio_test_playing");
        if (cJSON_IsNumber(test_playing)) {
            out_status->test_playing = test_playing->valueint ? 1 : 0;
        }

        rc = 0;
    }

    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_set_audio_output(const char *socket_path,
                            jw_platform_audio_output output,
                            char *status, int status_len) {
    if (output < 0 || output >= JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "audio output invalid");
        }
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", "set-audio-output");
    cJSON_AddStringToObject(req, "output", jw_platform_audio_output_name(output));
    cJSON_AddNumberToObject(req, "value", (int)output);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "audio output failed: daemon unavailable");
        }
        return -1;
    }

    int ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s",
                     ok ? "audio output set" : "audio output failed");
        }
    }

    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

static void ipc__performance_status_init(jw_ipc_performance_status_info *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->soc_temp_c = -1;
    for (int i = 0; i < JW_PLATFORM_PERF_DOMAIN_COUNT; i++) {
        out->domains[i].current_freq = -1;
        out->domains[i].set_freq = -1;
    }
}

static void ipc__parse_performance_domain(const cJSON *domains,
                                          const char *key,
                                          jw_ipc_performance_domain_status *out) {
    if (!domains || !key || !out) {
        return;
    }
    const cJSON *domain = cJSON_GetObjectItemCaseSensitive(domains, key);
    if (!cJSON_IsObject(domain)) {
        return;
    }
    const cJSON *v = NULL;
    out->supported = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(domain, "supported"));
    v = cJSON_GetObjectItemCaseSensitive(domain, "name");
    if (cJSON_IsString(v)) ipc__copy_string(out->name, sizeof(out->name), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(domain, "governor");
    if (cJSON_IsString(v)) ipc__copy_string(out->governor, sizeof(out->governor), v->valuestring);
    v = cJSON_GetObjectItemCaseSensitive(domain, "current_freq");
    out->current_freq = cJSON_IsNumber(v) ? v->valueint : -1;
    v = cJSON_GetObjectItemCaseSensitive(domain, "set_freq");
    out->set_freq = cJSON_IsNumber(v) ? v->valueint : -1;
    v = cJSON_GetObjectItemCaseSensitive(domain, "available_governors");
    if (cJSON_IsString(v)) {
        ipc__copy_string(out->available_governors,
                         sizeof(out->available_governors), v->valuestring);
    }
    v = cJSON_GetObjectItemCaseSensitive(domain, "available_frequencies");
    if (cJSON_IsString(v)) {
        ipc__copy_string(out->available_frequencies,
                         sizeof(out->available_frequencies), v->valuestring);
    }
}

int jw_ipc_get_performance_status(const char *socket_path,
                                  jw_ipc_performance_status_info *out,
                                  char *status, int status_len) {
    ipc__performance_status_init(out);

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "performance-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     "performance status unavailable");
        }
        return -1;
    }
    if (!ipc__type_is(resp, "performance-status")) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     "performance status failed");
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (out) {
        const cJSON *v = NULL;
        out->supported = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "supported"));
        out->session_override =
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "session_override"));
        v = cJSON_GetObjectItemCaseSensitive(resp, "active_profile");
        if (cJSON_IsString(v)) ipc__copy_string(out->active_profile, sizeof(out->active_profile), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(resp, "global_profile");
        if (cJSON_IsString(v)) ipc__copy_string(out->global_profile, sizeof(out->global_profile), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(resp, "session_profile");
        if (cJSON_IsString(v)) ipc__copy_string(out->session_profile, sizeof(out->session_profile), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(resp, "soc_temp_c");
        out->soc_temp_c = cJSON_IsNumber(v) ? v->valueint : -1;
        v = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(v)) ipc__copy_string(out->message, sizeof(out->message), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(resp, "last_error");
        if (cJSON_IsString(v)) ipc__copy_string(out->last_error, sizeof(out->last_error), v->valuestring);

        const cJSON *domains = cJSON_GetObjectItemCaseSensitive(resp, "domains");
        ipc__parse_performance_domain(domains, "cpu",
                                      &out->domains[JW_PLATFORM_PERF_DOMAIN_CPU]);
        ipc__parse_performance_domain(domains, "gpu",
                                      &out->domains[JW_PLATFORM_PERF_DOMAIN_GPU]);
        ipc__parse_performance_domain(domains, "dmc",
                                      &out->domains[JW_PLATFORM_PERF_DOMAIN_DMC]);
    }

    if (status && status_len > 0) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s", "performance status ready");
        }
    }
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_set_performance_profile(const char *socket_path,
                                   const char *scope,
                                   const char *profile,
                                   char *status, int status_len) {
    if (!profile || !profile[0]) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "performance profile missing");
        }
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "performance-set-profile");
    cJSON_AddStringToObject(req, "scope", scope && scope[0] ? scope : "session");
    cJSON_AddStringToObject(req, "profile", profile);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     "performance profile failed: daemon unavailable");
        }
        return -1;
    }
    int ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s",
                     ok ? "performance profile set" : "performance profile failed");
        }
    }
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

static void ipc__add_perf_domain_request(cJSON *req,
                                         const char *prefix,
                                         const jw_platform_perf_domain_request *domain) {
    if (!req || !prefix || !domain) {
        return;
    }
    char key[64];
    if (domain->governor[0]) {
        snprintf(key, sizeof(key), "%s_governor", prefix);
        cJSON_AddStringToObject(req, key, domain->governor);
    }
    if (domain->frequency >= 0) {
        snprintf(key, sizeof(key), "%s_frequency", prefix);
        cJSON_AddNumberToObject(req, key, domain->frequency);
    }
}

int jw_ipc_set_performance_custom(const char *socket_path,
                                  const jw_platform_perf_request *request,
                                  char *status, int status_len) {
    if (!request) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "performance custom missing");
        }
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "performance-set-custom");
    ipc__add_perf_domain_request(req, "cpu",
                                 &request->domains[JW_PLATFORM_PERF_DOMAIN_CPU]);
    ipc__add_perf_domain_request(req, "gpu",
                                 &request->domains[JW_PLATFORM_PERF_DOMAIN_GPU]);
    ipc__add_perf_domain_request(req, "dmc",
                                 &request->domains[JW_PLATFORM_PERF_DOMAIN_DMC]);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     "performance custom failed: daemon unavailable");
        }
        return -1;
    }
    int ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s",
                     ok ? "performance custom set" : "performance custom failed");
        }
    }
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_reset_performance_session(const char *socket_path,
                                     char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "performance-reset-session");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     "performance reset failed: daemon unavailable");
        }
        return -1;
    }
    int ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s",
                     ok ? "performance reset" : "performance reset failed");
        }
    }
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

static int ipc__update_request(const char *socket_path,
                               const char *manifest_path,
                               bool check,
                               jw_ipc_update_status_info *out,
                               char *status,
                               int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", check ? "update-check" : "update-status");
    if (check && manifest_path && manifest_path[0]) {
        cJSON_AddStringToObject(req, "manifest_path", manifest_path);
    }

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update status unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "update-status")) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update status failed");
        }
        cJSON_Delete(resp);
        return -1;
    }

    ipc__parse_update_status(resp, out);
    if (status && status_len > 0) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s", "update status ready");
        }
    }

    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_update_status(const char *socket_path,
                         jw_ipc_update_status_info *out,
                         char *status,
                         int status_len) {
    return ipc__update_request(socket_path, NULL, false, out, status, status_len);
}

int jw_ipc_update_check(const char *socket_path,
                        const char *manifest_path,
                        jw_ipc_update_status_info *out,
                        char *status,
                        int status_len) {
    return ipc__update_request(socket_path, manifest_path, true, out, status, status_len);
}

int jw_ipc_update_releases(const char *socket_path,
                           bool refresh,
                           jw_ipc_update_status_info *out,
                           char *status,
                           int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "update-releases");
    cJSON_AddBoolToObject(req, "refresh", refresh);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update releases unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "update-status")) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update releases failed");
        }
        cJSON_Delete(resp);
        return -1;
    }

    ipc__parse_update_status(resp, out);
    if (status && status_len > 0) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        snprintf(status, (size_t)status_len, "%s",
                 cJSON_IsString(message) && message->valuestring
                     ? message->valuestring : "update releases ready");
    }
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_update_select(const char *socket_path,
                         int option_index,
                         jw_ipc_update_status_info *out,
                         char *status,
                         int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "update-select");
    cJSON_AddNumberToObject(req, "option_index", option_index);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update selection unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "update-status")) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update selection failed");
        }
        cJSON_Delete(resp);
        return -1;
    }

    ipc__parse_update_status(resp, out);
    if (status && status_len > 0) {
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s", "update selected");
        }
    }

    bool ok = out && out->compatible && out->artifact_name[0];
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_update_download(const char *socket_path,
                           jw_ipc_update_status_info *out,
                           char *status,
                           int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "update-download");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update download unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "update-status")) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update download failed");
        }
        cJSON_Delete(resp);
        return -1;
    }

    ipc__parse_update_status(resp, out);
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s", "update download finished");
        }
    }

    bool ok = false;
    const cJSON *state = cJSON_GetObjectItemCaseSensitive(resp, "state");
    if (cJSON_IsString(state) && state->valuestring &&
        (strcmp(state->valuestring, "downloaded") == 0 ||
         strcmp(state->valuestring, "downloading") == 0)) {
        ok = true;
    }
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_update_cancel(const char *socket_path,
                         jw_ipc_update_status_info *out,
                         char *status,
                         int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "update-cancel");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update cancel unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "update-status")) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update cancel failed");
        }
        cJSON_Delete(resp);
        return -1;
    }

    ipc__parse_update_status(resp, out);
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s", "update download cancelled");
        }
    }

    const cJSON *state = cJSON_GetObjectItemCaseSensitive(resp, "state");
    bool ok = cJSON_IsString(state) && state->valuestring &&
              strcmp(state->valuestring, "cancelled") == 0;
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_update_install_preflight(const char *socket_path,
                                    bool confirm_unknown_battery,
                                    jw_ipc_update_status_info *out,
                                    char *status,
                                    int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "update-install-preflight");
    if (confirm_unknown_battery) {
        cJSON_AddBoolToObject(req, "confirm_unknown_battery", true);
    }

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update ready check unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "update-status")) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update ready check failed");
        }
        cJSON_Delete(resp);
        return -1;
    }

    ipc__parse_update_status(resp, out);
    if (status && status_len > 0) {
        const cJSON *install = cJSON_GetObjectItemCaseSensitive(resp, "install");
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(install, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s", "update ready check complete");
        }
    }

    bool ok = out && (out->install_ready ||
                      out->install_blocked ||
                      out->install_needs_confirmation);
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_update_install(const char *socket_path,
                          bool confirm_unknown_battery,
                          jw_ipc_update_status_info *out,
                          char *status,
                          int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "update-install");
    if (confirm_unknown_battery) {
        cJSON_AddBoolToObject(req, "confirm_unknown_battery", true);
    }

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update install unavailable");
        }
        return -1;
    }

    if (!ipc__type_is(resp, "update-status")) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "update install failed");
        }
        cJSON_Delete(resp);
        return -1;
    }

    ipc__parse_update_status(resp, out);
    if (status && status_len > 0) {
        const cJSON *install = cJSON_GetObjectItemCaseSensitive(resp, "install");
        const cJSON *message = cJSON_GetObjectItemCaseSensitive(install, "message");
        const cJSON *top_message = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else if (cJSON_IsString(top_message) && top_message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", top_message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s", "update install started");
        }
    }

    bool ok = out && (out->install_active ||
                      out->install_armed ||
                      out->install_blocked ||
                      out->install_needs_confirmation);
    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_get_adb(const char *socket_path, int *out_enabled,
                   int *out_intent_enabled, bool *out_supported) {
    if (out_enabled) {
        *out_enabled = -1;
    }
    if (out_intent_enabled) {
        *out_intent_enabled = -1;
    }
    if (out_supported) {
        *out_supported = false;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int rc = -1;
    const cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(resp, "capabilities");
    const cJSON *adb_cap = cJSON_GetObjectItemCaseSensitive(capabilities, "adb");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(status, "adb_enabled");
    const cJSON *intent = cJSON_GetObjectItemCaseSensitive(status, "adb_intent_enabled");
    if (out_supported) {
        *out_supported = cJSON_IsTrue(adb_cap);
    }
    if (cJSON_IsNumber(enabled) || cJSON_IsNumber(intent)) {
        if (out_enabled) {
            *out_enabled = cJSON_IsNumber(enabled) ? enabled->valueint : -1;
        }
        if (out_intent_enabled) {
            *out_intent_enabled = cJSON_IsNumber(intent) ? intent->valueint : -1;
        }
        rc = 0;
    } else if (cJSON_IsBool(adb_cap)) {
        rc = 0;
    }

    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_set_adb(const char *socket_path, int enabled,
                   char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", enabled ? "enable-adb" : "disable-adb");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s", "ADB failed: daemon unavailable");
        }
        return -1;
    }

    bool ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s",
                     ok ? (enabled ? "ADB enabled" : "ADB disabled") : "ADB failed");
        }
    }

    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_get_boot_splash(const char *socket_path, int *out_enabled,
                           bool *out_supported) {
    if (out_enabled) {
        *out_enabled = -1;
    }
    if (out_supported) {
        *out_supported = false;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int rc = -1;
    const cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(resp, "capabilities");
    const cJSON *splash_cap = cJSON_GetObjectItemCaseSensitive(capabilities, "boot_splash");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    const cJSON *enabled = cJSON_GetObjectItemCaseSensitive(status, "boot_splash_enabled");
    if (out_supported) {
        *out_supported = cJSON_IsTrue(splash_cap);
    }
    if (cJSON_IsNumber(enabled)) {
        if (out_enabled) {
            *out_enabled = enabled->valueint;
        }
        rc = 0;
    } else if (cJSON_IsBool(splash_cap)) {
        rc = 0;
    }

    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_set_boot_splash(const char *socket_path, int enabled,
                           char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", "set-boot-splash");
    cJSON_AddNumberToObject(req, "value", enabled ? 1 : 0);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     "boot splash failed: daemon unavailable");
        }
        return -1;
    }

    bool ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s",
                     ok ? (enabled ? "boot splash enabled" : "boot splash disabled")
                        : "boot splash failed");
        }
    }

    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

/* platform-status field readers, shared by the one-row getters below and by
   jw_ipc_get_display_status(), which fills all three rows from one reply. The
   outputs are already initialized to "unknown". */
static int ipc__parse_refresh_rate(const cJSON *resp, int *out_hz, bool *out_supported) {
    int rc = -1;
    const cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(resp, "capabilities");
    const cJSON *rate_cap = cJSON_GetObjectItemCaseSensitive(capabilities, "refresh_rate");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    const cJSON *hz = cJSON_GetObjectItemCaseSensitive(status, "refresh_rate_hz");
    if (out_supported) {
        *out_supported = cJSON_IsTrue(rate_cap);
    }
    if (cJSON_IsNumber(hz)) {
        if (out_hz) {
            *out_hz = hz->valueint;
        }
        rc = 0;
    } else if (cJSON_IsBool(rate_cap)) {
        rc = 0;
    }
    return rc;
}

static int ipc__parse_color_temp(const cJSON *resp, int *out_kelvin, bool *out_supported) {
    int rc = -1;
    const cJSON *capabilities = cJSON_GetObjectItemCaseSensitive(resp, "capabilities");
    const cJSON *temp_cap = cJSON_GetObjectItemCaseSensitive(capabilities, "color_temperature");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    const cJSON *kelvin = cJSON_GetObjectItemCaseSensitive(status, "color_temp_kelvin");
    if (out_supported) {
        *out_supported = cJSON_IsTrue(temp_cap);
    }
    if (cJSON_IsNumber(kelvin)) {
        if (out_kelvin) {
            *out_kelvin = kelvin->valueint;
        }
        rc = 0;
    } else if (cJSON_IsBool(temp_cap)) {
        rc = 0;
    }
    return rc;
}

static int ipc__parse_hdmi_status(const cJSON *resp, int *out_connected, int *out_mode,
                                  bool *out_supported) {
    const cJSON *cap = cJSON_GetObjectItemCaseSensitive(resp, "capabilities");
    const cJSON *hdmi_cap = cJSON_GetObjectItemCaseSensitive(cap, "hdmi_output");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    const cJSON *conn = cJSON_GetObjectItemCaseSensitive(status, "hdmi_connected");
    const cJSON *mode = cJSON_GetObjectItemCaseSensitive(status, "hdmi_output_mode");
    if (out_supported) *out_supported = cJSON_IsTrue(hdmi_cap);
    if (out_connected && cJSON_IsNumber(conn)) *out_connected = conn->valueint;
    if (out_mode && cJSON_IsNumber(mode)) *out_mode = mode->valueint;
    return cJSON_IsBool(hdmi_cap) ? 0 : -1;
}

int jw_ipc_get_refresh_rate(const char *socket_path, int *out_hz,
                            bool *out_supported) {
    if (out_hz) {
        *out_hz = -1;
    }
    if (out_supported) {
        *out_supported = false;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int rc = ipc__parse_refresh_rate(resp, out_hz, out_supported);
    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_get_display_status(const char *socket_path, jw_ipc_display_status *out) {
    if (!out) {
        return -1;
    }
    out->refresh_rate_hz = -1;
    out->refresh_rate_supported = false;
    out->color_temp_kelvin = -1;
    out->color_temp_supported = false;
    out->hdmi_connected = -1;
    out->hdmi_output_mode = -1;
    out->hdmi_supported = false;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    (void)ipc__parse_refresh_rate(resp, &out->refresh_rate_hz, &out->refresh_rate_supported);
    (void)ipc__parse_color_temp(resp, &out->color_temp_kelvin, &out->color_temp_supported);
    (void)ipc__parse_hdmi_status(resp, &out->hdmi_connected, &out->hdmi_output_mode,
                                 &out->hdmi_supported);
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_set_refresh_rate(const char *socket_path, int hz,
                            char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", "set-refresh-rate");
    cJSON_AddNumberToObject(req, "value", hz);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     "refresh rate failed: daemon unavailable");
        }
        return -1;
    }

    bool ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s",
                     ok ? (hz >= 90 ? "switching to 90 Hz" : "switching to 60 Hz")
                        : "refresh rate failed");
        }
    }

    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_get_color_temp(const char *socket_path, int *out_kelvin,
                          bool *out_supported) {
    if (out_kelvin) {
        *out_kelvin = -1;
    }
    if (out_supported) {
        *out_supported = false;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int rc = ipc__parse_color_temp(resp, out_kelvin, out_supported);
    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_set_color_temp(const char *socket_path, int kelvin,
                          char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", "set-color-temp");
    cJSON_AddNumberToObject(req, "value", kelvin);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     "Color temperature failed: daemon unavailable");
        }
        return -1;
    }

    bool ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            snprintf(status, (size_t)status_len, "%s",
                     ok ? "Color temperature updated" : "Color temperature failed");
        }
    }

    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_set_language(const char *socket_path, const char *lang,
                        char *status, int status_len) {
    return jw_ipc_set_language_ex(socket_path, lang, false, status, status_len);
}

int jw_ipc_set_language_ex(const char *socket_path, const char *lang,
                           bool keep_running, char *status, int status_len) {
    if (!lang || !lang[0]) {
        if (status && status_len > 0)
            snprintf(status, (size_t)status_len, "%s", "no language given");
        return -1;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "set-language");
    cJSON_AddStringToObject(req, "language", lang);
    if (keep_running) cJSON_AddBoolToObject(req, "keep_running", true);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     "language change failed: daemon unavailable");
        }
        return -1;
    }

    bool ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        if (cJSON_IsString(message) && message->valuestring) {
            snprintf(status, (size_t)status_len, "%s", message->valuestring);
        } else {
            /* On success the launcher is already being torn down, so this string
               rarely survives long enough to be read. Set it anyway: if the
               restart does not happen, this is the only thing the user sees. */
            snprintf(status, (size_t)status_len, "%s",
                     ok ? "restarting" : "language change failed");
        }
    }

    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_get_hdmi_status(const char *socket_path, int *out_connected,
                           int *out_mode, bool *out_supported) {
    if (out_connected) *out_connected = -1;
    if (out_mode) *out_mode = -1;
    if (out_supported) *out_supported = false;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }

    int rc = ipc__parse_hdmi_status(resp, out_connected, out_mode, out_supported);
    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_set_hdmi_output(const char *socket_path, int mode,
                           char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-action");
    cJSON_AddStringToObject(req, "action", "set-hdmi-output");
    cJSON_AddNumberToObject(req, "value", mode);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     "HDMI output failed: daemon unavailable");
        }
        return -1;
    }

    bool ok = ipc__type_is(resp, "ok");
    const cJSON *message = cJSON_GetObjectItemCaseSensitive(resp, "message");
    if (status && status_len > 0) {
        snprintf(status, (size_t)status_len, "%s",
                 (cJSON_IsString(message) && message->valuestring)
                     ? message->valuestring
                     : (ok ? "switching HDMI output" : "HDMI output failed"));
    }

    cJSON_Delete(resp);
    return ok ? 0 : -1;
}

int jw_ipc_scrape_validate(const char *socket_path, const char *username,
                           const char *password,
                           jw_ipc_scrape_validate_info *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "scrape-validate");
    cJSON_AddStringToObject(req, "username", username ? username : "");
    cJSON_AddStringToObject(req, "password", password ? password : "");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    if (!ipc__type_is(resp, "scrape-validate")) {
        cJSON_Delete(resp);
        return -1;
    }

    if (out) {
        out->valid = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "valid"));
        out->rejected = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "rejected"));
        out->max_threads = (int)ipc__json_ll(resp, "max_threads");
        out->requests_today = (int)ipc__json_ll(resp, "requests_today");
        out->max_requests = (int)ipc__json_ll(resp, "max_requests");
        out->user_level = (int)ipc__json_ll(resp, "user_level");
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(msg)) {
            ipc__copy_string(out->message, sizeof(out->message), msg->valuestring);
        }
    }

    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_scrape_start_full(const char *socket_path, const char *scope,
                             const char *system, const char *rom_path,
                             bool missing_only,
                             jw_ipc_scrape_start_info *out,
                             char *status, int status_len) {
    if (out) memset(out, 0, sizeof(*out));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "scrape-start");
    cJSON_AddStringToObject(req, "scope", scope ? scope : "");
    cJSON_AddStringToObject(req, "system", system ? system : "");
    if (rom_path && rom_path[0]) {
        cJSON_AddStringToObject(req, "rom_path", rom_path);
    }
    cJSON_AddStringToObject(req, "mode", missing_only ? "missing" : "all");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     "scrape failed: daemon unavailable");
        }
        return -1;
    }
    if (!ipc__type_is(resp, "ok")) {
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (status && status_len > 0) {
            snprintf(status, (size_t)status_len, "%s",
                     cJSON_IsString(msg) ? msg->valuestring
                                         : "scrape failed");
        }
        cJSON_Delete(resp);
        return -1;
    }

    if (out) {
        out->requested = (int)ipc__json_ll(resp, "requested");
        out->enqueued = (int)ipc__json_ll(resp, "enqueued");
        out->already_queued = (int)ipc__json_ll(resp, "already_queued");
        out->skipped_existing = (int)ipc__json_ll(resp, "skipped_existing");
        out->queue_full =
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "queue_full"));
    }
    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_scrape_start(const char *socket_path, const char *scope,
                        const char *system, const char *rom_path,
                        bool missing_only, int *out_enqueued,
                        char *status, int status_len) {
    jw_ipc_scrape_start_info info;
    int rc = jw_ipc_scrape_start_full(socket_path, scope, system, rom_path,
                                      missing_only, &info, status, status_len);
    if (out_enqueued) *out_enqueued = rc == 0 ? info.enqueued : 0;
    return rc;
}

int jw_ipc_scrape_status(const char *socket_path,
                         jw_ipc_scrape_status_info *out) {
    if (out) memset(out, 0, sizeof(*out));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "scrape-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    if (!ipc__type_is(resp, "scrape-status")) {
        cJSON_Delete(resp);
        return -1;
    }

    if (out) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "state");
        if (cJSON_IsString(v)) ipc__copy_string(out->state, sizeof(out->state), v->valuestring);
        out->total = (int)ipc__json_ll(resp, "total");
        out->done = (int)ipc__json_ll(resp, "done");
        out->found = (int)ipc__json_ll(resp, "found");
        out->not_found = (int)ipc__json_ll(resp, "not_found");
        out->failed = (int)ipc__json_ll(resp, "failed");
        out->cancelled = (int)ipc__json_ll(resp, "cancelled");
        out->queued = (int)ipc__json_ll(resp, "queued");
        out->active = (int)ipc__json_ll(resp, "active");
        v = cJSON_GetObjectItemCaseSensitive(resp, "current_name");
        if (cJSON_IsString(v)) ipc__copy_string(out->current_name, sizeof(out->current_name), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(resp, "current_system");
        if (cJSON_IsString(v)) ipc__copy_string(out->current_system, sizeof(out->current_system), v->valuestring);
        v = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(v)) ipc__copy_string(out->message, sizeof(out->message), v->valuestring);
    }
    cJSON_Delete(resp);
    return 0;
}

static jw_ipc_scrape_row_state ipc__parse_scrape_row_state(const char *state) {
    if (!state) return JW_IPC_SCRAPE_ROW_QUEUED;
    if (strcmp(state, "hashing") == 0 || strcmp(state, "hash") == 0)
        return JW_IPC_SCRAPE_ROW_HASH;
    if (strcmp(state, "searching") == 0 || strcmp(state, "search") == 0)
        return JW_IPC_SCRAPE_ROW_SEARCH;
    if (strcmp(state, "downloading") == 0 || strcmp(state, "download") == 0)
        return JW_IPC_SCRAPE_ROW_DOWNLOAD;
    if (strcmp(state, "saving") == 0 || strcmp(state, "save") == 0)
        return JW_IPC_SCRAPE_ROW_SAVE;
    if (strcmp(state, "done") == 0) return JW_IPC_SCRAPE_ROW_DONE;
    if (strcmp(state, "not-found") == 0) return JW_IPC_SCRAPE_ROW_NOT_FOUND;
    if (strcmp(state, "error") == 0) return JW_IPC_SCRAPE_ROW_ERROR;
    if (strcmp(state, "cancelled") == 0) return JW_IPC_SCRAPE_ROW_CANCELLED;
    return JW_IPC_SCRAPE_ROW_QUEUED;
}

int jw_ipc_scrape_queue(const char *socket_path, int offset, int limit,
                        jw_ipc_scrape_queue_info *out) {
    if (out) {
        memset(out, 0, sizeof(*out));
        out->eta_seconds = -1;
    }

    if (limit <= 0 || limit > JW_IPC_SCRAPE_QUEUE_MAX_ROWS) {
        limit = JW_IPC_SCRAPE_QUEUE_MAX_ROWS;
    }

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "scrape-queue");
    cJSON_AddNumberToObject(req, "offset", offset);
    cJSON_AddNumberToObject(req, "limit", limit);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    if (!ipc__type_is(resp, "scrape-queue")) {
        cJSON_Delete(resp);
        return -1;
    }

    if (out) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(resp, "state");
        if (cJSON_IsString(v)) ipc__copy_string(out->state, sizeof(out->state), v->valuestring);
        out->total = (int)ipc__json_ll(resp, "total");
        out->done = (int)ipc__json_ll(resp, "done");
        out->found = (int)ipc__json_ll(resp, "found");
        out->not_found = (int)ipc__json_ll(resp, "not_found");
        out->failed = (int)ipc__json_ll(resp, "failed");
        out->cancelled = (int)ipc__json_ll(resp, "cancelled");
        out->queued = (int)ipc__json_ll(resp, "queued");
        out->active = (int)ipc__json_ll(resp, "active");
        out->requests_today = (int)ipc__json_ll(resp, "requests_today");
        out->max_requests = (int)ipc__json_ll(resp, "max_requests");
        out->max_threads = (int)ipc__json_ll(resp, "max_threads");
        out->permits = (int)ipc__json_ll(resp, "permits");
        out->eta_seconds = (int)ipc__json_ll(resp, "eta_seconds");
        v = cJSON_GetObjectItemCaseSensitive(resp, "message");
        if (cJSON_IsString(v)) ipc__copy_string(out->message, sizeof(out->message), v->valuestring);

        const cJSON *rows = cJSON_GetObjectItemCaseSensitive(resp, "rows");
        if (cJSON_IsArray(rows)) {
            int count = cJSON_GetArraySize(rows);
            if (count > JW_IPC_SCRAPE_QUEUE_MAX_ROWS) {
                count = JW_IPC_SCRAPE_QUEUE_MAX_ROWS;
            }
            out->row_count = count;
            for (int i = 0; i < count; i++) {
                const cJSON *item = cJSON_GetArrayItem(rows, i);
                jw_ipc_scrape_queue_row *row = &out->rows[i];
                row->id = (unsigned)ipc__json_ll(item, "id");
                v = cJSON_GetObjectItemCaseSensitive(item, "state");
                row->state = ipc__parse_scrape_row_state(
                    cJSON_IsString(v) ? v->valuestring : NULL);
                v = cJSON_GetObjectItemCaseSensitive(item, "display_name");
                if (cJSON_IsString(v)) ipc__copy_string(row->display_name, sizeof(row->display_name), v->valuestring);
                v = cJSON_GetObjectItemCaseSensitive(item, "system");
                if (cJSON_IsString(v)) ipc__copy_string(row->system, sizeof(row->system), v->valuestring);
                v = cJSON_GetObjectItemCaseSensitive(item, "rom_path");
                if (cJSON_IsString(v)) ipc__copy_string(row->rom_path, sizeof(row->rom_path), v->valuestring);
                v = cJSON_GetObjectItemCaseSensitive(item, "output_path");
                if (cJSON_IsString(v)) ipc__copy_string(row->output_path, sizeof(row->output_path), v->valuestring);
                v = cJSON_GetObjectItemCaseSensitive(item, "message");
                if (cJSON_IsString(v)) ipc__copy_string(row->message, sizeof(row->message), v->valuestring);
            }
        }
    }

    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_scrape_missing_counts(const char *socket_path,
                                 jw_ipc_scrape_missing_info *out) {
    if (out) memset(out, 0, sizeof(*out));

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "scrape-missing-counts");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    if (!ipc__type_is(resp, "ok")) {
        cJSON_Delete(resp);
        return -1;
    }

    if (out) {
        out->total_missing = (int)ipc__json_ll(resp, "total_missing");
        const cJSON *arr = cJSON_GetObjectItemCaseSensitive(resp, "systems");
        if (cJSON_IsArray(arr)) {
            int count = cJSON_GetArraySize(arr);
            if (count > JW_IPC_MISSING_MAX_SYSTEMS) count = JW_IPC_MISSING_MAX_SYSTEMS;
            out->system_count = count;
            for (int i = 0; i < count; i++) {
                const cJSON *item = cJSON_GetArrayItem(arr, i);
                jw_ipc_scrape_missing_row *row = &out->systems[i];
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(item, "system");
                if (cJSON_IsString(v))
                    ipc__copy_string(row->system, sizeof(row->system), v->valuestring);
                row->missing = (int)ipc__json_ll(item, "missing");
                row->total   = (int)ipc__json_ll(item, "total");
            }
        }
    }

    cJSON_Delete(resp);
    return 0;
}

int jw_ipc_scrape_pending(const char *socket_path, const char *system,
                          const char *rom_path, bool *out_pending) {
    if (out_pending) *out_pending = false;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "scrape-status");
    cJSON_AddStringToObject(req, "system", system ? system : "");
    if (rom_path && rom_path[0]) {
        cJSON_AddStringToObject(req, "rom_path", rom_path);
    }

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    int rc = ipc__type_is(resp, "scrape-status") ? 0 : -1;
    if (rc == 0 && out_pending) {
        *out_pending = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(resp, "pending"));
    }
    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_scrape_cancel(const char *socket_path, const char *scope,
                         const char *system, const char *rom_path,
                         int *out_removed) {
    if (out_removed) *out_removed = 0;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "scrape-cancel");
    cJSON_AddStringToObject(req, "scope", scope ? scope : "all");
    if (system && system[0]) cJSON_AddStringToObject(req, "system", system);
    if (rom_path && rom_path[0]) cJSON_AddStringToObject(req, "rom_path", rom_path);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    int rc = ipc__type_is(resp, "ok") ? 0 : -1;
    if (rc == 0 && out_removed) {
        *out_removed = (int)ipc__json_ll(resp, "removed");
    }
    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_scrape_stop_all(const char *socket_path, int *out_stopped) {
    if (out_stopped) *out_stopped = 0;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "scrape-stop-all");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    int rc = ipc__type_is(resp, "ok") ? 0 : -1;
    if (rc == 0 && out_stopped) {
        *out_stopped = (int)ipc__json_ll(resp, "stopped");
    }
    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_scrape_clear_done(const char *socket_path, int *out_cleared) {
    if (out_cleared) *out_cleared = 0;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "scrape-clear-done");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        return -1;
    }
    int rc = ipc__type_is(resp, "ok") ? 0 : -1;
    if (rc == 0 && out_cleared) {
        *out_cleared = (int)ipc__json_ll(resp, "cleared");
    }
    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_set_led(const char *socket_path, int enabled, const char *mode,
                   int r, int g, int b, int brightness, int speed,
                   char *status, int status_len) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "set-led");
    cJSON_AddBoolToObject(req, "enabled", enabled ? 1 : 0);
    cJSON_AddStringToObject(req, "mode", mode ? mode : "FOREVER");
    cJSON_AddNumberToObject(req, "r", r);
    cJSON_AddNumberToObject(req, "g", g);
    cJSON_AddNumberToObject(req, "b", b);
    cJSON_AddNumberToObject(req, "brightness", brightness);
    cJSON_AddNumberToObject(req, "speed", speed);

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) {
        if (status && status_len > 0)
            snprintf(status, (size_t)status_len, "%s", "led failed: daemon unavailable");
        return -1;
    }
    int rc = ipc__type_is(resp, "ok") ? 0 : -1;
    if (status && status_len > 0)
        snprintf(status, (size_t)status_len, "%s", rc == 0 ? "led applied" : "led failed");
    cJSON_Delete(resp);
    return rc;
}

int jw_ipc_get_led(const char *socket_path, int *enabled, char *mode, int mode_len,
                   int *r, int *g, int *b, int *brightness, int *speed) {
    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "platform-status");

    cJSON *resp = NULL;
    if (ipc__request(socket_path, req, &resp) != 0) return -1;

    const cJSON *status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    const cJSON *led = cJSON_GetObjectItemCaseSensitive(status, "led");
    int rc = -1;
    if (cJSON_IsObject(led)) {
        const cJSON *v;
        if (enabled    && (v = cJSON_GetObjectItemCaseSensitive(led, "enabled")))    *enabled = cJSON_IsTrue(v) ? 1 : (cJSON_IsNumber(v) ? v->valueint : 0);
        if (mode && mode_len > 0 && (v = cJSON_GetObjectItemCaseSensitive(led, "mode")) && cJSON_IsString(v)) snprintf(mode, (size_t)mode_len, "%s", v->valuestring);
        if (r          && (v = cJSON_GetObjectItemCaseSensitive(led, "r")))           *r = v->valueint;
        if (g          && (v = cJSON_GetObjectItemCaseSensitive(led, "g")))           *g = v->valueint;
        if (b          && (v = cJSON_GetObjectItemCaseSensitive(led, "b")))           *b = v->valueint;
        if (brightness && (v = cJSON_GetObjectItemCaseSensitive(led, "brightness")))  *brightness = v->valueint;
        if (speed      && (v = cJSON_GetObjectItemCaseSensitive(led, "speed")))       *speed = v->valueint;
        rc = 0;
    }
    cJSON_Delete(resp);
    return rc;
}
