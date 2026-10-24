#include "internal/storage/repair_advice.h"

#include <string.h>

static bool jw__advice_eq(const char *a, const char *b) {
    return a && b && strcmp(a, b) == 0;
}

bool jw_storage_advice_check_found_errors(const jw_ipc_storage_status_info *card) {
    return card && card->last_repair_valid &&
           jw__advice_eq(card->last_repair_mode, "check") &&
           jw__advice_eq(card->last_repair_outcome, "verify-failed") &&
           jw__advice_eq(card->repair, "failed");
}

/* A repair that ran and still left the card failing. Trying the same repair
   again on the device will not change the answer. */
static bool jw__advice_repair_failed(const jw_ipc_storage_status_info *card) {
    return card->last_repair_valid &&
           jw__advice_eq(card->last_repair_mode, "repair") &&
           (jw__advice_eq(card->last_repair_outcome, "verify-failed") ||
            jw__advice_eq(card->last_repair_outcome, "failed")) &&
           jw__advice_eq(card->repair, "failed");
}

jw_storage_advice jw_storage_repair_advice(const jw_ipc_storage_status_info *card) {
    if (!card) {
        return JW_STORAGE_ADVICE_NONE;
    }
    bool held = jw__advice_eq(card->repair, "failed") ||
                jw__advice_eq(card->repair, "pending");
    bool read_only = card->mounted && jw__advice_eq(card->access, "read-only");
    if (jw__advice_eq(card->cause, "write-protected") || card->block_write_protected) {
        return (held || read_only) ? JW_STORAGE_ADVICE_WRITE_PROTECTED
                                   : JW_STORAGE_ADVICE_NONE;
    }
    if (jw_storage_advice_check_found_errors(card)) {
        return JW_STORAGE_ADVICE_REPAIR_FOUND_ERRORS;
    }
    if (jw__advice_repair_failed(card)) {
        return JW_STORAGE_ADVICE_REPAIR_FAILED;
    }
    if (held) {
        if (jw__advice_eq(card->hold_trigger, "paused-shutdown") &&
            !card->last_repair_valid) {
            return JW_STORAGE_ADVICE_CHECK_PAUSED_SHUTDOWN;
        }
        if (card->last_repair_valid &&
            jw__advice_eq(card->last_repair_outcome, "timed-out")) {
            return JW_STORAGE_ADVICE_CHECK_TIMED_OUT;
        }
        if (jw__advice_eq(card->hold_trigger, "paused-shutdown")) {
            return JW_STORAGE_ADVICE_CHECK_PAUSED_SHUTDOWN;
        }
        return JW_STORAGE_ADVICE_CHECK_INTERRUPTED;
    }
    if (!read_only) {
        return JW_STORAGE_ADVICE_NONE;
    }
    return jw__advice_eq(card->cause, "filesystem-error")
               ? JW_STORAGE_ADVICE_FILESYSTEM_ERROR
               : JW_STORAGE_ADVICE_READ_ONLY_UNKNOWN;
}

const char *jw_storage_advice_next_mode(jw_storage_advice advice) {
    switch (advice) {
    case JW_STORAGE_ADVICE_REPAIR_FOUND_ERRORS:
    case JW_STORAGE_ADVICE_FILESYSTEM_ERROR:
    case JW_STORAGE_ADVICE_READ_ONLY_UNKNOWN:
        return "repair";
    case JW_STORAGE_ADVICE_REPAIR_FAILED:
        /* After the card is repaired on a computer, a check proves it and
           lifts the hold. */
    case JW_STORAGE_ADVICE_CHECK_PAUSED_SHUTDOWN:
    case JW_STORAGE_ADVICE_CHECK_TIMED_OUT:
    case JW_STORAGE_ADVICE_CHECK_INTERRUPTED:
        return "check";
    case JW_STORAGE_ADVICE_NONE:
    case JW_STORAGE_ADVICE_WRITE_PROTECTED:
        break;
    }
    return NULL;
}

jw_storage_card_actions jw_storage_advice_card_actions(
    const jw_ipc_storage_status_info *card) {
    jw_storage_card_actions actions = { false, false, false };
    if (!card) {
        return actions;
    }
    jw_storage_advice advice = jw_storage_repair_advice(card);
    bool pending = jw__advice_eq(card->repair, "pending");
    bool failed = jw__advice_eq(card->repair, "failed");
    const char *next = jw_storage_advice_next_mode(advice);
    if (card->mounted) {
        actions.repair = jw__advice_eq(card->access, "read-only") && !pending;
    } else {
        /* The daemon's rule for an unmounted card: held, with a known
           identity. It re-checks the device itself before running. */
        actions.repair = failed && card->uuid[0] && next &&
                         strcmp(next, "repair") == 0;
    }
    actions.check = failed &&
                    !(advice == JW_STORAGE_ADVICE_REPAIR_FOUND_ERRORS && actions.repair);
    actions.unmount = card->mounted && jw__advice_eq(card->source, "secondary_sd");
    return actions;
}

const char *jw_storage_advice_state_key(jw_storage_advice advice) {
    switch (advice) {
    case JW_STORAGE_ADVICE_REPAIR_FOUND_ERRORS:
    case JW_STORAGE_ADVICE_REPAIR_FAILED:
        return "Needs repair";
    case JW_STORAGE_ADVICE_CHECK_PAUSED_SHUTDOWN:
    case JW_STORAGE_ADVICE_CHECK_TIMED_OUT:
    case JW_STORAGE_ADVICE_CHECK_INTERRUPTED:
        return "Needs check";
    default:
        break;
    }
    return NULL;
}
