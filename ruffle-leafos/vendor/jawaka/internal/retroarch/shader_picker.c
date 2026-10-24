#include "internal/retroarch/shader_picker.h"

#include <stdio.h>
#include <string.h>

static bool jw_sp__same(const char *a, const char *b) {
    return strcmp(a ? a : "", b ? b : "") == 0;
}

static bool jw_sp__get_path(const jw_shader_picker_transport *transport,
                            char *path, size_t path_size) {
    jw_ipc_retroarch_shader_reply reply;
    if (!transport || !transport->send ||
        transport->send(transport->ctx, JW_SHADER_PICKER_GET, NULL,
                        JW_RA_SHADER_SCOPE_GAME, &reply) != 0 ||
        reply.result != JW_RA_OK ||
        (reply.outcome != JW_RA_SHADER_OK &&
         reply.outcome != JW_RA_SHADER_NONE)) {
        return false;
    }
    if (reply.outcome == JW_RA_SHADER_OK && !reply.path[0]) return false;
    if (path && path_size > 0)
        snprintf(path, path_size, "%s",
                 reply.outcome == JW_RA_SHADER_OK ? reply.path : "");
    return true;
}

static bool jw_sp__restore_path(const jw_shader_picker_transport *transport,
                                const char *path) {
    jw_ipc_retroarch_shader_reply reply;
    jw_shader_picker_operation op = path && path[0]
                                  ? JW_SHADER_PICKER_RESTORE
                                  : JW_SHADER_PICKER_CLEAR;
    return transport && transport->send &&
           transport->send(transport->ctx, op, path,
                           JW_RA_SHADER_SCOPE_GAME, &reply) == 0 &&
           reply.result == JW_RA_OK && reply.outcome == JW_RA_SHADER_OK;
}

void jw_shader_picker_init(jw_shader_picker_state *state) {
    if (state) memset(state, 0, sizeof(*state));
}

jw_shader_picker_result jw_shader_picker_probe(
    jw_shader_picker_state *state, const jw_shader_picker_transport *transport) {
    jw_ipc_retroarch_shader_reply reply;
    if (!state || !transport || !transport->send ||
        transport->send(transport->ctx, JW_SHADER_PICKER_GET, NULL,
                        JW_RA_SHADER_SCOPE_GAME, &reply) != 0) {
        return JW_SHADER_PICKER_UNAVAILABLE;
    }
    if (reply.result == JW_RA_UNSUPPORTED)
        return JW_SHADER_PICKER_UNSUPPORTED_BUILD;
    if (reply.result != JW_RA_OK)
        return JW_SHADER_PICKER_UNAVAILABLE;
    if (reply.outcome != JW_RA_SHADER_OK && reply.outcome != JW_RA_SHADER_NONE)
        return JW_SHADER_PICKER_UNAVAILABLE;
    if (reply.outcome == JW_RA_SHADER_OK && !reply.path[0])
        return JW_SHADER_PICKER_UNAVAILABLE;

    const char *path = reply.outcome == JW_RA_SHADER_OK ? reply.path : "";
    snprintf(state->original_path, sizeof(state->original_path), "%s", path);
    snprintf(state->current_path, sizeof(state->current_path), "%s", path);
    state->supported = true;
    state->original_known = true;
    state->current_known = true;
    return JW_SHADER_PICKER_OK;
}

static jw_shader_picker_result jw_sp__restore(
    jw_shader_picker_state *state, const jw_shader_picker_transport *transport,
    const char *path) {
    if (!jw_sp__restore_path(transport, path)) {
        state->current_known = false;
        return JW_SHADER_PICKER_RESTORE_FAILED;
    }
    snprintf(state->current_path, sizeof(state->current_path), "%s",
             path ? path : "");
    state->current_known = true;
    return JW_SHADER_PICKER_OK;
}

jw_shader_picker_result jw_shader_picker_apply(
    jw_shader_picker_state *state, const jw_shader_picker_transport *transport,
    const char *path) {
    if (!state || !state->supported || !transport || !transport->send)
        return JW_SHADER_PICKER_UNAVAILABLE;
    const char *desired = path ? path : "";
    if (state->current_known && jw_sp__same(state->current_path, desired))
        return JW_SHADER_PICKER_OK; /* already applied */

    char previous[sizeof(state->current_path)];
    bool previous_known = state->current_known;
    snprintf(previous, sizeof(previous), "%s", state->current_path);

    jw_ipc_retroarch_shader_reply reply;
    jw_shader_picker_operation op = desired[0]
                                  ? JW_SHADER_PICKER_SET
                                  : JW_SHADER_PICKER_CLEAR;
    if (transport->send(transport->ctx, op, desired,
                        JW_RA_SHADER_SCOPE_GAME, &reply) != 0)
        return JW_SHADER_PICKER_UNAVAILABLE;

    if (reply.result == JW_RA_TIMEOUT) {
        char actual[sizeof(state->current_path)];
        if (!jw_sp__get_path(transport, actual, sizeof(actual))) {
            state->current_known = false;
            return JW_SHADER_PICKER_UNKNOWN_STATE; /* never restore blindly */
        }
        snprintf(state->current_path, sizeof(state->current_path), "%s", actual);
        state->current_known = true;
        if (jw_sp__same(actual, desired)) return JW_SHADER_PICKER_OK;
        if (!previous_known) return JW_SHADER_PICKER_UNKNOWN_STATE;
        if (jw_sp__same(actual, previous)) return JW_SHADER_PICKER_APPLY_FAILED;
        return jw_sp__restore(state, transport, previous) == JW_SHADER_PICKER_OK
             ? JW_SHADER_PICKER_APPLY_FAILED
             : JW_SHADER_PICKER_RESTORE_FAILED;
    }
    if (reply.result == JW_RA_UNSUPPORTED)
        return JW_SHADER_PICKER_UNSUPPORTED_BUILD;
    if (reply.result != JW_RA_OK)
        return JW_SHADER_PICKER_UNAVAILABLE;
    if (reply.outcome == JW_RA_SHADER_OK) {
        snprintf(state->current_path, sizeof(state->current_path), "%s", desired);
        state->current_known = true;
        return JW_SHADER_PICKER_OK;
    }

    jw_shader_picker_result failure = JW_SHADER_PICKER_APPLY_FAILED;
    if (reply.outcome == JW_RA_SHADER_ERR_MISSING)
        failure = JW_SHADER_PICKER_MISSING;
    else if (reply.outcome == JW_RA_SHADER_ERR_UNSUPPORTED)
        failure = JW_SHADER_PICKER_UNSUPPORTED_TYPE;
    if (previous_known &&
        jw_sp__restore(state, transport, previous) != JW_SHADER_PICKER_OK)
        return JW_SHADER_PICKER_RESTORE_FAILED;
    return failure;
}

jw_shader_picker_result jw_shader_picker_cancel(
    jw_shader_picker_state *state, const jw_shader_picker_transport *transport) {
    if (!state || !state->original_known)
        return JW_SHADER_PICKER_UNKNOWN_STATE;
    if (!state->current_known) {
        char actual[sizeof(state->current_path)];
        if (!jw_sp__get_path(transport, actual, sizeof(actual)))
            return JW_SHADER_PICKER_UNKNOWN_STATE;
        snprintf(state->current_path, sizeof(state->current_path), "%s", actual);
        state->current_known = true;
    }
    if (jw_sp__same(state->current_path, state->original_path))
        return JW_SHADER_PICKER_OK;
    return jw_sp__restore(state, transport, state->original_path);
}

jw_shader_picker_result jw_shader_picker_save(
    const jw_shader_picker_transport *transport, jw_ra_shader_scope scope) {
    jw_ipc_retroarch_shader_reply reply;
    if (!transport || !transport->send ||
        transport->send(transport->ctx, JW_SHADER_PICKER_SAVE, NULL,
                        scope, &reply) != 0 ||
        reply.result != JW_RA_OK || reply.outcome != JW_RA_SHADER_OK)
        return JW_SHADER_PICKER_SAVE_FAILED;
    return JW_SHADER_PICKER_OK;
}

jw_shader_picker_result jw_shader_picker_remove(
    const jw_shader_picker_transport *transport, jw_ra_shader_scope scope) {
    jw_ipc_retroarch_shader_reply reply;
    if (!transport || !transport->send ||
        transport->send(transport->ctx, JW_SHADER_PICKER_REMOVE, NULL,
                        scope, &reply) != 0 || reply.result != JW_RA_OK)
        return JW_SHADER_PICKER_REMOVE_FAILED;
    if (reply.outcome == JW_RA_SHADER_ABSENT)
        return JW_SHADER_PICKER_REMOVE_ABSENT;
    if (reply.outcome != JW_RA_SHADER_OK)
        return JW_SHADER_PICKER_REMOVE_FAILED;
    /* The preset was removed, but a lost daemon must not let the UI follow an
       unconfirmed result with a contradictory success notice. */
    return jw_sp__get_path(transport, NULL, 0)
         ? JW_SHADER_PICKER_OK : JW_SHADER_PICKER_UNKNOWN_STATE;
}

bool jw_shader_picker_scope_enabled(jw_ra_shader_scope scope,
                                    bool fugazi_resolver_present) {
    return scope != JW_RA_SHADER_SCOPE_GLOBAL || fugazi_resolver_present;
}
