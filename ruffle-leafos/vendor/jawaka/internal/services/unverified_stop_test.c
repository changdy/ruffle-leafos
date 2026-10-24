#include "internal/services/unverified_stop.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

static void jw__test_shutdown_and_suspend_continue(void) {
    assert(jw_svc_unverified_stop_action_for(JW_SVC_STOP_CALLER_SHUTDOWN) ==
           JW_SVC_UNVERIFIED_STOP_CONTINUE_WITH_WARNING);
    assert(jw_svc_unverified_stop_action_for(JW_SVC_STOP_CALLER_SUSPEND) ==
           JW_SVC_UNVERIFIED_STOP_CONTINUE_WITH_WARNING);

    puts("PASS unverified-stop-test shutdown and suspend continue with a warning");
}

static void jw__test_safe_unmount_and_package_op_fail(void) {
    assert(jw_svc_unverified_stop_action_for(JW_SVC_STOP_CALLER_SAFE_UNMOUNT) ==
           JW_SVC_UNVERIFIED_STOP_FAIL_OPERATION);
    assert(jw_svc_unverified_stop_action_for(JW_SVC_STOP_CALLER_PACKAGE_OP) ==
           JW_SVC_UNVERIFIED_STOP_FAIL_OPERATION);

    puts("PASS unverified-stop-test safe unmount and package operations fail outright");
}

static void jw__test_game_launch_requires_override(void) {
    assert(jw_svc_unverified_stop_action_for(JW_SVC_STOP_CALLER_GAME_LAUNCH) ==
           JW_SVC_UNVERIFIED_STOP_REQUIRE_OVERRIDE);

    puts("PASS unverified-stop-test game launch requires an explicit override");
}

static void jw__test_out_of_range_callers_fail_safe(void) {
    const jw_svc_stop_caller invalid_callers[] = {
        (jw_svc_stop_caller)(JW_SVC_STOP_CALLER_SHUTDOWN - 1),
        (jw_svc_stop_caller)(JW_SVC_STOP_CALLER_PACKAGE_OP + 1),
    };

    /* Neither value is a real jw_svc_stop_caller. Exercise both sides
     * of the named domain: an invalid caller must never be treated as
     * license to continue or to launch, only to refuse. */
    for (size_t i = 0; i < sizeof(invalid_callers) / sizeof(invalid_callers[0]); i++) {
        assert(jw_svc_unverified_stop_action_for(invalid_callers[i]) ==
               JW_SVC_UNVERIFIED_STOP_FAIL_OPERATION);
    }

    puts("PASS unverified-stop-test out-of-range caller values fail safe");
}

static void jw__test_every_caller_has_a_defined_action(void) {
    const jw_svc_stop_caller callers[] = {
        JW_SVC_STOP_CALLER_SHUTDOWN,
        JW_SVC_STOP_CALLER_SUSPEND,
        JW_SVC_STOP_CALLER_SAFE_UNMOUNT,
        JW_SVC_STOP_CALLER_GAME_LAUNCH,
        JW_SVC_STOP_CALLER_PACKAGE_OP,
    };
    for (size_t i = 0; i < sizeof(callers) / sizeof(callers[0]); i++) {
        const jw_svc_unverified_stop_action action =
            jw_svc_unverified_stop_action_for(callers[i]);
        assert(action == JW_SVC_UNVERIFIED_STOP_CONTINUE_WITH_WARNING ||
               action == JW_SVC_UNVERIFIED_STOP_FAIL_OPERATION ||
               action == JW_SVC_UNVERIFIED_STOP_REQUIRE_OVERRIDE);
    }

    puts("PASS unverified-stop-test every real caller value returns one of the three defined actions");
}

int main(void) {
    jw__test_shutdown_and_suspend_continue();
    jw__test_safe_unmount_and_package_op_fail();
    jw__test_game_launch_requires_override();
    jw__test_out_of_range_callers_fail_safe();
    jw__test_every_caller_has_a_defined_action();
    puts("PASS unverified-stop-test");
    return 0;
}
