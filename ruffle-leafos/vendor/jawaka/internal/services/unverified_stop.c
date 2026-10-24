#include "internal/services/unverified_stop.h"

jw_svc_unverified_stop_action jw_svc_unverified_stop_action_for(jw_svc_stop_caller caller) {
    switch (caller) {
        case JW_SVC_STOP_CALLER_SHUTDOWN:
        case JW_SVC_STOP_CALLER_SUSPEND:
            return JW_SVC_UNVERIFIED_STOP_CONTINUE_WITH_WARNING;
        case JW_SVC_STOP_CALLER_SAFE_UNMOUNT:
        case JW_SVC_STOP_CALLER_PACKAGE_OP:
            return JW_SVC_UNVERIFIED_STOP_FAIL_OPERATION;
        case JW_SVC_STOP_CALLER_GAME_LAUNCH:
            return JW_SVC_UNVERIFIED_STOP_REQUIRE_OVERRIDE;
        default:
            /* An unknown caller cannot inherit the exceptional
             * availability behavior of shutdown or suspend, or the game
             * launch override path. Refusing the caller's operation is
             * the most conservative behavior available in this table. */
            return JW_SVC_UNVERIFIED_STOP_FAIL_OPERATION;
    }
}
