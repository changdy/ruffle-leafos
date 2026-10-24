/* The SD card advice rules: which situation a held or read-only card is in,
   and which on-device action can help. The case that motivated this module
   is first: a check that finished and found errors must lead to a repair,
   never to another check that can only fail the same way. */

#include "internal/storage/repair_advice.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void expect(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "storage-repair-advice-test: %s\n", what);
        failures++;
    }
}

static void expect_str(const char *got, const char *want, const char *what) {
    int ok = (!got && !want) || (got && want && strcmp(got, want) == 0);
    if (!ok) {
        fprintf(stderr, "storage-repair-advice-test: %s: got %s, want %s\n", what,
                got ? got : "(null)", want ? want : "(null)");
        failures++;
    }
}

static jw_ipc_storage_status_info card(const char *access, const char *repair) {
    jw_ipc_storage_status_info c;
    memset(&c, 0, sizeof(c));
    snprintf(c.source, sizeof(c.source), "launcher_sd");
    c.present = true;
    c.mounted = true;
    snprintf(c.access, sizeof(c.access), "%s", access);
    snprintf(c.cause, sizeof(c.cause), "unknown");
    snprintf(c.repair, sizeof(c.repair), "%s", repair);
    c.repair_supported = true;
    return c;
}

static void last(jw_ipc_storage_status_info *c, const char *mode, const char *outcome) {
    c->last_repair_valid = true;
    snprintf(c->last_repair_mode, sizeof(c->last_repair_mode), "%s", mode);
    snprintf(c->last_repair_outcome, sizeof(c->last_repair_outcome), "%s", outcome);
}

int main(void) {
    /* The loop seen on MLP1: a finished check found errors, the card stays
       held, and "Restart and check" can never clear it. */
    jw_ipc_storage_status_info c = card("read-only", "failed");
    last(&c, "check", "verify-failed");
    expect(jw_storage_repair_advice(&c) == JW_STORAGE_ADVICE_REPAIR_FOUND_ERRORS,
           "check found errors -> repair");
    expect_str(jw_storage_advice_next_mode(jw_storage_repair_advice(&c)), "repair",
               "check found errors offers repair");
    expect_str(jw_storage_advice_state_key(jw_storage_repair_advice(&c)), "Needs repair",
               "check found errors reads as needs repair");
    expect(jw_storage_advice_check_found_errors(&c), "check found errors is detected");

    /* The same result while the hold still names the paused shutdown: the
       finished check wins over the precautionary wording. */
    snprintf(c.hold_trigger, sizeof(c.hold_trigger), "paused-shutdown");
    expect(jw_storage_repair_advice(&c) == JW_STORAGE_ADVICE_REPAIR_FOUND_ERRORS,
           "paused-shutdown hold with a failed check -> repair");

    /* Held after a paused shutdown, not yet checked: check first. */
    c = card("read-only", "failed");
    snprintf(c.hold_trigger, sizeof(c.hold_trigger), "paused-shutdown");
    expect(jw_storage_repair_advice(&c) == JW_STORAGE_ADVICE_CHECK_PAUSED_SHUTDOWN,
           "unchecked paused shutdown -> check");
    expect_str(jw_storage_advice_next_mode(jw_storage_repair_advice(&c)), "check",
               "unchecked paused shutdown offers check");
    expect_str(jw_storage_advice_state_key(jw_storage_repair_advice(&c)), "Needs check",
               "unchecked paused shutdown reads as needs check");

    /* A check that ran out of time proves nothing: check again. */
    c = card("read-only", "failed");
    last(&c, "check", "timed-out");
    expect(jw_storage_repair_advice(&c) == JW_STORAGE_ADVICE_CHECK_TIMED_OUT,
           "timed-out check -> check again");
    expect_str(jw_storage_advice_next_mode(jw_storage_repair_advice(&c)), "check",
               "timed-out check offers check");

    /* Interrupted (power loss mid-attempt) or pending without a result. */
    c = card("read-only", "failed");
    last(&c, "repair", "interrupted");
    expect(jw_storage_repair_advice(&c) == JW_STORAGE_ADVICE_CHECK_INTERRUPTED,
           "interrupted repair -> check");
    c = card("read-only", "pending");
    expect(jw_storage_repair_advice(&c) == JW_STORAGE_ADVICE_CHECK_INTERRUPTED,
           "pending without a result -> check");

    /* A repair that ran and still failed: the device can't fix it; after a
       computer repair, a check lifts the hold. */
    c = card("read-only", "failed");
    last(&c, "repair", "verify-failed");
    expect(jw_storage_repair_advice(&c) == JW_STORAGE_ADVICE_REPAIR_FAILED,
           "failed repair -> computer, then check");
    expect_str(jw_storage_advice_next_mode(jw_storage_repair_advice(&c)), "check",
               "failed repair offers check");
    expect_str(jw_storage_advice_state_key(jw_storage_repair_advice(&c)), "Needs repair",
               "failed repair reads as needs repair");
    expect(!jw_storage_advice_check_found_errors(&c), "a repair result is not a check result");

    /* A check verify failure that was later cleared is not an error state. */
    c = card("read-write", "none");
    last(&c, "check", "verify-failed");
    expect(jw_storage_repair_advice(&c) == JW_STORAGE_ADVICE_NONE,
           "cleared card with an old failed check -> nothing");
    expect(!jw_storage_advice_check_found_errors(&c), "old result on a cleared card is ignored");

    /* Read-only without a hold: kernel error or unknown, both repairable. */
    c = card("read-only", "none");
    snprintf(c.cause, sizeof(c.cause), "filesystem-error");
    expect(jw_storage_repair_advice(&c) == JW_STORAGE_ADVICE_FILESYSTEM_ERROR,
           "file system error -> repair");
    expect_str(jw_storage_advice_next_mode(jw_storage_repair_advice(&c)), "repair",
               "file system error offers repair");
    c = card("read-only", "none");
    expect(jw_storage_repair_advice(&c) == JW_STORAGE_ADVICE_READ_ONLY_UNKNOWN,
           "unknown read-only -> repair");

    /* Write protection is outside Leaf's reach whatever else is true. */
    c = card("read-only", "failed");
    last(&c, "check", "verify-failed");
    c.block_write_protected = true;
    expect(jw_storage_repair_advice(&c) == JW_STORAGE_ADVICE_WRITE_PROTECTED,
           "write protection wins");
    expect(jw_storage_advice_next_mode(JW_STORAGE_ADVICE_WRITE_PROTECTED) == NULL,
           "write protection offers nothing");

    /* Healthy and missing cards. */
    c = card("read-write", "none");
    expect(jw_storage_repair_advice(&c) == JW_STORAGE_ADVICE_NONE, "writable -> nothing");
    expect(jw_storage_repair_advice(NULL) == JW_STORAGE_ADVICE_NONE, "no card -> nothing");
    expect(jw_storage_advice_state_key(JW_STORAGE_ADVICE_NONE) == NULL, "no state key");

    /* Settings actions. The regression this guards: an unmounted held card
       after a failed check had Check hidden and Repair gated on mounted, so
       Settings offered nothing at all. */
    jw_storage_card_actions a;
    c = card("read-only", "failed");
    last(&c, "check", "verify-failed");
    c.mounted = false;
    snprintf(c.uuid, sizeof(c.uuid), "5C8A-1EF4");
    a = jw_storage_advice_card_actions(&c);
    expect(a.repair && !a.check && !a.unmount,
           "unmounted held card after a failed check offers repair");
    c.uuid[0] = '\0';
    a = jw_storage_advice_card_actions(&c);
    expect(!a.repair && a.check,
           "unidentified unmounted held card keeps check as its way forward");

    c = card("read-only", "failed");
    last(&c, "check", "verify-failed");
    a = jw_storage_advice_card_actions(&c);
    expect(a.repair && !a.check, "mounted card after a failed check offers repair only");

    c = card("read-only", "failed");
    last(&c, "repair", "interrupted");
    a = jw_storage_advice_card_actions(&c);
    expect(a.repair && a.check, "interrupted: repair and check, as before");

    c = card("read-only", "failed");
    snprintf(c.hold_trigger, sizeof(c.hold_trigger), "paused-shutdown");
    c.mounted = false;
    snprintf(c.uuid, sizeof(c.uuid), "5C8A-1EF4");
    a = jw_storage_advice_card_actions(&c);
    expect(!a.repair && a.check, "unmounted paused-shutdown hold: check first");

    c = card("read-only", "pending");
    a = jw_storage_advice_card_actions(&c);
    expect(!a.repair && !a.check, "pending request: nothing to start");

    c = card("read-write", "none");
    snprintf(c.source, sizeof(c.source), "secondary_sd");
    a = jw_storage_advice_card_actions(&c);
    expect(!a.repair && !a.check && a.unmount, "healthy secondary: unmount only");

    /* Every held card keeps at least one action. */
    const char *const modes[] = { "check", "repair" };
    const char *const outcomes[] = { "verify-failed", "timed-out", "interrupted", "failed" };
    for (int mounted = 0; mounted < 2; mounted++)
    for (int m = 0; m < 2; m++)
    for (int o = 0; o < 4; o++)
    for (int id = 0; id < 2; id++) {
        c = card("read-only", "failed");
        c.mounted = mounted;
        last(&c, modes[m], outcomes[o]);
        if (id) snprintf(c.uuid, sizeof(c.uuid), "5C8A-1EF4");
        a = jw_storage_advice_card_actions(&c);
        expect(a.repair || a.check, "held card left with no action");
    }

    if (failures) {
        fprintf(stderr, "storage-repair-advice-test: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("storage-repair-advice-test: ok\n");
    return 0;
}
