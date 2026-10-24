#include "internal/retroarch/shader_picker.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    jw_shader_picker_operation op;
    jw_ra_shader_scope scope;
    const char *path;
    int rc;
    jw_ra_result result;
    jw_ra_shader_outcome outcome;
    const char *reply_path;
} step;

typedef struct {
    const step *steps;
    int count;
    int next;
    int failed;
} fake;

static int send_fake(void *ctx, jw_shader_picker_operation op,
                     const char *path, jw_ra_shader_scope scope,
                     jw_ipc_retroarch_shader_reply *reply) {
    fake *f = ctx;
    if (f->next >= f->count) {
        fprintf(stderr, "unexpected operation %d\n", op);
        f->failed++;
        return -1;
    }
    const step *s = &f->steps[f->next++];
    if (s->op != op || s->scope != scope ||
        strcmp(s->path ? s->path : "", path ? path : "") != 0) {
        fprintf(stderr, "step %d mismatch: op=%d scope=%d path=%s\n",
                f->next, op, scope, path ? path : "");
        f->failed++;
    }
    memset(reply, 0, sizeof(*reply));
    reply->result = s->result;
    reply->outcome = s->outcome;
    snprintf(reply->path, sizeof(reply->path), "%s",
             s->reply_path ? s->reply_path : "");
    return s->rc;
}

static int failures;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", message);
        failures++;
    }
}

static jw_shader_picker_transport transport(fake *f) {
    jw_shader_picker_transport t = { .send = send_fake, .ctx = f };
    return t;
}

static void check_fake(fake *f, const char *name) {
    check(f->failed == 0, name);
    check(f->next == f->count, "all expected operations consumed");
}

static void test_apply_duplicate_and_cancel(void) {
    const step steps[] = {
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_OK, "/old.glslp" },
        { JW_SHADER_PICKER_SET, JW_RA_SHADER_SCOPE_GAME, "/new.glslp", 0,
          JW_RA_OK, JW_RA_SHADER_OK, NULL },
        { JW_SHADER_PICKER_RESTORE, JW_RA_SHADER_SCOPE_GAME, "/old.glslp", 0,
          JW_RA_OK, JW_RA_SHADER_OK, NULL },
    };
    fake f = { steps, 3, 0, 0 };
    jw_shader_picker_transport t = transport(&f);
    jw_shader_picker_state state;
    jw_shader_picker_init(&state);
    check(jw_shader_picker_probe(&state, &t) == JW_SHADER_PICKER_OK,
          "probe succeeds");
    check(jw_shader_picker_apply(&state, &t, "/new.glslp") == JW_SHADER_PICKER_OK,
          "explicit selection applies");
    check(jw_shader_picker_apply(&state, &t, "/new.glslp") == JW_SHADER_PICKER_OK,
          "duplicate application is suppressed");
    check(jw_shader_picker_cancel(&state, &t) == JW_SHADER_PICKER_OK,
          "cancel restores original");
    check(strcmp(state.current_path, "/old.glslp") == 0,
          "cancel tracks restored path");
    check_fake(&f, "apply/cancel operation sequence");
}

static void test_cancel_without_apply(void) {
    const step steps[] = {
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_OK, "/old.glslp" },
    };
    fake f = { steps, 1, 0, 0 };
    jw_shader_picker_transport t = transport(&f);
    jw_shader_picker_state state;
    jw_shader_picker_init(&state);
    check(jw_shader_picker_probe(&state, &t) == JW_SHADER_PICKER_OK,
          "probe succeeds before browsing");
    check(jw_shader_picker_cancel(&state, &t) == JW_SHADER_PICKER_OK,
          "Back without a selection leaves the shader alone");
    check(strcmp(state.current_path, "/old.glslp") == 0,
          "browsing preserves the current shader");
    check_fake(&f, "Back without applying sends no mutation");
}

static void test_apply_failure_and_restore_failure(void) {
    const step restored[] = {
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_OK, "/old.glslp" },
        { JW_SHADER_PICKER_SET, JW_RA_SHADER_SCOPE_GAME, "/bad.glslp", 0,
          JW_RA_OK, JW_RA_SHADER_ERR_APPLY, NULL },
        { JW_SHADER_PICKER_RESTORE, JW_RA_SHADER_SCOPE_GAME, "/old.glslp", 0,
          JW_RA_OK, JW_RA_SHADER_OK, NULL },
    };
    fake f = { restored, 3, 0, 0 };
    jw_shader_picker_transport t = transport(&f);
    jw_shader_picker_state state;
    jw_shader_picker_init(&state);
    jw_shader_picker_probe(&state, &t);
    check(jw_shader_picker_apply(&state, &t, "/bad.glslp") ==
              JW_SHADER_PICKER_APPLY_FAILED,
          "apply failure is reported after restoration");
    check(strcmp(state.current_path, "/old.glslp") == 0,
          "failed candidate does not stay selected");
    check_fake(&f, "apply failure restoration sequence");

    const step broken[] = {
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_OK, "/old.glslp" },
        { JW_SHADER_PICKER_SET, JW_RA_SHADER_SCOPE_GAME, "/bad.glslp", 0,
          JW_RA_OK, JW_RA_SHADER_ERR_APPLY, NULL },
        { JW_SHADER_PICKER_RESTORE, JW_RA_SHADER_SCOPE_GAME, "/old.glslp", 0,
          JW_RA_OK, JW_RA_SHADER_ERR_APPLY, NULL },
    };
    f = (fake){ broken, 3, 0, 0 };
    t = transport(&f);
    jw_shader_picker_init(&state);
    jw_shader_picker_probe(&state, &t);
    check(jw_shader_picker_apply(&state, &t, "/bad.glslp") ==
              JW_SHADER_PICKER_RESTORE_FAILED,
          "restore failure is distinct");
    check(!state.current_known, "restore failure marks current state unknown");
    check_fake(&f, "restore failure sequence");
}

static void test_timeout_reconciliation(void) {
    const step applied[] = {
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_NONE, NULL },
        { JW_SHADER_PICKER_SET, JW_RA_SHADER_SCOPE_GAME, "/new.glslp", 0,
          JW_RA_TIMEOUT, JW_RA_SHADER_ERR, NULL },
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_OK, "/new.glslp" },
    };
    fake f = { applied, 3, 0, 0 };
    jw_shader_picker_transport t = transport(&f);
    jw_shader_picker_state state;
    jw_shader_picker_init(&state);
    jw_shader_picker_probe(&state, &t);
    check(jw_shader_picker_apply(&state, &t, "/new.glslp") == JW_SHADER_PICKER_OK,
          "late success is accepted after state query");
    check_fake(&f, "timeout success reconciliation sequence");

    const step wrong[] = {
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_OK, "/old.glslp" },
        { JW_SHADER_PICKER_SET, JW_RA_SHADER_SCOPE_GAME, "/new.glslp", 0,
          JW_RA_TIMEOUT, JW_RA_SHADER_ERR, NULL },
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_OK, "/unexpected.glslp" },
        { JW_SHADER_PICKER_RESTORE, JW_RA_SHADER_SCOPE_GAME, "/old.glslp", 0,
          JW_RA_OK, JW_RA_SHADER_OK, NULL },
    };
    f = (fake){ wrong, 4, 0, 0 };
    t = transport(&f);
    jw_shader_picker_init(&state);
    jw_shader_picker_probe(&state, &t);
    check(jw_shader_picker_apply(&state, &t, "/new.glslp") ==
              JW_SHADER_PICKER_APPLY_FAILED,
          "unexpected timeout state is restored but never called success");
    check(strcmp(state.current_path, "/old.glslp") == 0,
          "timeout mismatch restores the previous path");
    check_fake(&f, "timeout mismatch restoration sequence");

    const step unknown[] = {
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_NONE, NULL },
        { JW_SHADER_PICKER_SET, JW_RA_SHADER_SCOPE_GAME, "/new.glslp", 0,
          JW_RA_TIMEOUT, JW_RA_SHADER_ERR, NULL },
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_TIMEOUT, JW_RA_SHADER_ERR, NULL },
    };
    f = (fake){ unknown, 3, 0, 0 };
    t = transport(&f);
    jw_shader_picker_init(&state);
    jw_shader_picker_probe(&state, &t);
    check(jw_shader_picker_apply(&state, &t, "/new.glslp") ==
              JW_SHADER_PICKER_UNKNOWN_STATE,
          "double timeout reports unknown state");
    check(!state.current_known, "double timeout never claims a state");
    check_fake(&f, "double timeout does not issue blind restore");
}

static void test_save_remove_off_and_reload(void) {
    const step steps[] = {
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_OK, "/auto.glslp" },
        { JW_SHADER_PICKER_CLEAR, JW_RA_SHADER_SCOPE_GAME, "", 0,
          JW_RA_OK, JW_RA_SHADER_OK, NULL },
        { JW_SHADER_PICKER_SAVE, JW_RA_SHADER_SCOPE_PARENT, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_ERR, NULL },
        { JW_SHADER_PICKER_REMOVE, JW_RA_SHADER_SCOPE_CORE, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_ABSENT, NULL },
        /* A content reload is a fresh picker: RetroArch reevaluates auto presets. */
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_OK, "/auto.glslp" },
    };
    fake f = { steps, 5, 0, 0 };
    jw_shader_picker_transport t = transport(&f);
    jw_shader_picker_state state;
    jw_shader_picker_init(&state);
    jw_shader_picker_probe(&state, &t);
    check(jw_shader_picker_apply(&state, &t, NULL) == JW_SHADER_PICKER_OK,
          "Off clears only the live session");
    check(jw_shader_picker_save(&t, JW_RA_SHADER_SCOPE_PARENT) ==
              JW_SHADER_PICKER_SAVE_FAILED,
          "save failure leaves a session-only result");
    check(jw_shader_picker_remove(&t, JW_RA_SHADER_SCOPE_CORE) ==
              JW_SHADER_PICKER_REMOVE_ABSENT,
          "absent scoped preset is distinct");
    jw_shader_picker_init(&state);
    check(jw_shader_picker_probe(&state, &t) == JW_SHADER_PICKER_OK &&
              strcmp(state.current_path, "/auto.glslp") == 0,
          "content reload ends the Off session choice");
    check_fake(&f, "save/remove/session sequence");
}

static void test_global_gate_and_unpatched_build(void) {
    check(!jw_shader_picker_scope_enabled(JW_RA_SHADER_SCOPE_GLOBAL, false),
          "global is gated without Fugazi resolver");
    check(jw_shader_picker_scope_enabled(JW_RA_SHADER_SCOPE_GLOBAL, true),
          "global enables when Fugazi resolver is assembled");
    check(jw_shader_picker_scope_enabled(JW_RA_SHADER_SCOPE_GAME, false),
          "game scope is independent of Fugazi");

    const step steps[] = {
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_UNSUPPORTED, JW_RA_SHADER_ERR, NULL },
    };
    fake f = { steps, 1, 0, 0 };
    jw_shader_picker_transport t = transport(&f);
    jw_shader_picker_state state;
    jw_shader_picker_init(&state);
    check(jw_shader_picker_probe(&state, &t) ==
              JW_SHADER_PICKER_UNSUPPORTED_BUILD,
          "unpatched RetroArch is unavailable without crashing");
    check_fake(&f, "unpatched build probe");
}

static void test_remove_verification(void) {
    const step unconfirmed[] = {
        { JW_SHADER_PICKER_REMOVE, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_OK, NULL },
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, -1,
          JW_RA_TIMEOUT, JW_RA_SHADER_ERR, NULL },
    };
    fake f = { unconfirmed, 2, 0, 0 };
    jw_shader_picker_transport t = transport(&f);
    check(jw_shader_picker_remove(&t, JW_RA_SHADER_SCOPE_GAME) ==
              JW_SHADER_PICKER_UNKNOWN_STATE,
          "successful remove without verification is not reported as success");
    check_fake(&f, "remove verification failure sequence");

    const step confirmed[] = {
        { JW_SHADER_PICKER_REMOVE, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_OK, NULL },
        { JW_SHADER_PICKER_GET, JW_RA_SHADER_SCOPE_GAME, NULL, 0,
          JW_RA_OK, JW_RA_SHADER_NONE, NULL },
    };
    f = (fake){ confirmed, 2, 0, 0 };
    t = transport(&f);
    check(jw_shader_picker_remove(&t, JW_RA_SHADER_SCOPE_GAME) ==
              JW_SHADER_PICKER_OK,
          "verified remove is reported as success");
    check_fake(&f, "remove verification success sequence");
}

int main(void) {
    test_cancel_without_apply();
    test_apply_duplicate_and_cancel();
    test_apply_failure_and_restore_failure();
    test_timeout_reconciliation();
    test_save_remove_off_and_reload();
    test_global_gate_and_unpatched_build();
    test_remove_verification();
    if (failures) {
        fprintf(stderr, "shader-picker-test: %d failure(s)\n", failures);
        return 1;
    }
    puts("PASS shader-picker-test");
    return 0;
}
