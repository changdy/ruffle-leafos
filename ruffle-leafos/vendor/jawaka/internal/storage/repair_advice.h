#ifndef JW_STORAGE_REPAIR_ADVICE_H
#define JW_STORAGE_REPAIR_ADVICE_H

#include <stdbool.h>

#include "internal/ipc/ipc_client.h"

/* What to tell the user about a read-only or held SD card, and which on-device
   action can actually help. Kept free of UI code so every rule is testable:
   the screens in internal/settings/storage_ui.c only turn an advice into text.

   The order matters most after a check. "check" mode runs a read-only
   fsck.fat -n: it can prove a card clean, but it never fixes anything. Once a
   check has finished and found errors, checking again gives the same answer
   forever, so the only useful next step is a repair. */
typedef enum {
    JW_STORAGE_ADVICE_NONE = 0,             /* writable, nothing to act on */
    JW_STORAGE_ADVICE_WRITE_PROTECTED,      /* lock switch or card protection */
    JW_STORAGE_ADVICE_REPAIR_FOUND_ERRORS,  /* a finished check found errors */
    JW_STORAGE_ADVICE_REPAIR_FAILED,        /* an on-device repair did not fix it */
    JW_STORAGE_ADVICE_CHECK_PAUSED_SHUTDOWN,/* held after a shutdown that didn't finish */
    JW_STORAGE_ADVICE_CHECK_TIMED_OUT,      /* the last check or repair ran out of time */
    JW_STORAGE_ADVICE_CHECK_INTERRUPTED,    /* the last check or repair did not finish */
    JW_STORAGE_ADVICE_FILESYSTEM_ERROR,     /* read-only after a file system error */
    JW_STORAGE_ADVICE_READ_ONLY_UNKNOWN,    /* read-only, cause unknown */
} jw_storage_advice;

jw_storage_advice jw_storage_repair_advice(const jw_ipc_storage_status_info *card);

/* The on-device request that helps: "repair", "check", or NULL when nothing
   on the device can (write protection, or a writable card). */
const char *jw_storage_advice_next_mode(jw_storage_advice advice);

/* True when the last result is a finished check that found errors, which the
   warning for REPAIR_FOUND_ERRORS already reports. */
bool jw_storage_advice_check_found_errors(const jw_ipc_storage_status_info *card);

/* "Needs repair" or "Needs check" for a held card, NULL otherwise. Untranslated
   keys; the caller translates. */
const char *jw_storage_advice_state_key(jw_storage_advice advice);

/* The actions Settings > SD Cards offers for one card. A held card can be
   unmounted (hotplug refuses to mount it), and the daemon still accepts a
   check or repair for it when its identity is known, so repair is not only
   for mounted cards. A re-check is hidden only when a repair is actually on
   offer: a card must never be left with no way forward. */
typedef struct {
    bool repair;
    bool check;
    bool unmount;
} jw_storage_card_actions;

jw_storage_card_actions jw_storage_advice_card_actions(
    const jw_ipc_storage_status_info *card);

#endif /* JW_STORAGE_REPAIR_ADVICE_H */
