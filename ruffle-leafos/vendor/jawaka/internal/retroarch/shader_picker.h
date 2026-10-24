#ifndef JW_RETROARCH_SHADER_PICKER_H
#define JW_RETROARCH_SHADER_PICKER_H

#include "internal/ipc/ipc_client.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    JW_SHADER_PICKER_GET = 0,
    JW_SHADER_PICKER_SET,
    JW_SHADER_PICKER_RESTORE,
    JW_SHADER_PICKER_CLEAR,
    JW_SHADER_PICKER_SAVE,
    JW_SHADER_PICKER_REMOVE,
} jw_shader_picker_operation;

typedef enum {
    JW_SHADER_PICKER_OK = 0,
    JW_SHADER_PICKER_UNAVAILABLE,
    JW_SHADER_PICKER_UNSUPPORTED_BUILD,
    JW_SHADER_PICKER_MISSING,
    JW_SHADER_PICKER_UNSUPPORTED_TYPE,
    JW_SHADER_PICKER_APPLY_FAILED,
    JW_SHADER_PICKER_UNKNOWN_STATE,
    JW_SHADER_PICKER_RESTORE_FAILED,
    JW_SHADER_PICKER_SAVE_FAILED,
    JW_SHADER_PICKER_REMOVE_ABSENT,
    JW_SHADER_PICKER_REMOVE_FAILED,
} jw_shader_picker_result;

typedef int (*jw_shader_picker_send_fn)(
    void *ctx, jw_shader_picker_operation operation, const char *path,
    jw_ra_shader_scope scope, jw_ipc_retroarch_shader_reply *reply);

typedef struct {
    jw_shader_picker_send_fn send;
    void *ctx;
} jw_shader_picker_transport;

typedef struct {
    bool supported;
    bool original_known;
    bool current_known;
    char original_path[1024]; /* empty means Off */
    char current_path[1024];  /* empty means Off */
} jw_shader_picker_state;

void jw_shader_picker_init(jw_shader_picker_state *state);
jw_shader_picker_result jw_shader_picker_probe(
    jw_shader_picker_state *state, const jw_shader_picker_transport *transport);
/* Apply only on explicit selection; list navigation does not change RetroArch. */
jw_shader_picker_result jw_shader_picker_apply(
    jw_shader_picker_state *state, const jw_shader_picker_transport *transport,
    const char *path);
jw_shader_picker_result jw_shader_picker_cancel(
    jw_shader_picker_state *state, const jw_shader_picker_transport *transport);
jw_shader_picker_result jw_shader_picker_save(
    const jw_shader_picker_transport *transport, jw_ra_shader_scope scope);
jw_shader_picker_result jw_shader_picker_remove(
    const jw_shader_picker_transport *transport, jw_ra_shader_scope scope);

bool jw_shader_picker_scope_enabled(jw_ra_shader_scope scope,
                                    bool fugazi_resolver_present);

#endif
