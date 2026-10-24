#include "catastrophe.h"
#include "catastrophe_widgets.h"

#include "internal/settings/storage_ui.h"
#include "internal/core/log.h"
#include "internal/i18n/i18n.h"
#include "internal/storage/health.h"
#include "internal/storage/repair_advice.h"

#include <stdio.h>
#include <string.h>

const char *const jw_storage_ui_sources[JW_STORAGE_UI_SOURCE_COUNT] = {
    "launcher_sd",
    "secondary_sd",
};

bool jw_storage_ui_is_read_only(const jw_ipc_storage_status_info *card) {
    return card && card->mounted && strcmp(card->access, "read-only") == 0;
}

bool jw_storage_ui_needs_repair(const jw_ipc_storage_status_info *card) {
    return card && (strcmp(card->repair, "failed") == 0 ||
                    strcmp(card->repair, "pending") == 0);
}

void jw_storage_ui_card_name(const jw_ipc_storage_status_info *card,
                             char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    const char *role = card && strcmp(card->source, "secondary_sd") == 0
                           ? T("second SD card") : T("launcher SD card");
    if (card && card->volume_label[0]) {
        snprintf(out, out_size, "%s (%s)", role, card->volume_label);
    } else {
        snprintf(out, out_size, "%s", role);
    }
}

/* The same states as jw_storage_ui_card_state, untranslated. A caller that
   stores the result for later drawing must store this: a translated string
   kept in a buffer keeps its old language after a language change, which is
   exactly what the SD Cards row did. The literals are deliberately the same
   as the ones below, whose T() calls are what put them in the string table. */
const char *jw_storage_ui_card_state_key(const jw_ipc_storage_status_info *card) {
    if (card && strcmp(card->repair, "failed") == 0) {
        const char *key = jw_storage_advice_state_key(jw_storage_repair_advice(card));
        return key ? key : "Needs repair";
    }
    if (!card || !card->mounted) return "Not mounted";
    if (strcmp(card->repair, "pending") == 0 || strcmp(card->repair, "running") == 0)
        return "Repair pending";
    if (strcmp(card->access, "read-only") == 0) return "Read-only";
    if (strcmp(card->access, "unknown") == 0) return "Unknown";
    return card->busy && strcmp(card->source, "secondary_sd") == 0 ? "Busy" : "Mounted";
}

const char *jw_storage_ui_card_state(const jw_ipc_storage_status_info *card) {
    if (card && strcmp(card->repair, "failed") == 0) {
        /* Also when unmounted: hotplug refuses to mount a held card. */
        const char *key = jw_storage_advice_state_key(jw_storage_repair_advice(card));
        return key && strcmp(key, "Needs check") == 0 ? T("Needs check") : T("Needs repair");
    }
    if (!card || !card->mounted) {
        return T("Not mounted");
    }
    if (strcmp(card->repair, "pending") == 0 || strcmp(card->repair, "running") == 0) {
        return T("Repair pending");
    }
    if (strcmp(card->access, "read-only") == 0) {
        return T("Read-only");
    }
    if (strcmp(card->access, "unknown") == 0) {
        return T("Unknown");
    }
    return card->busy && strcmp(card->source, "secondary_sd") == 0 ? T("Busy") : T("Mounted");
}

static void jw__storage_ui_message(const char *message) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_A, .label = T("OK"), .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 1,
    };
    cat_confirm_result result;
    (void)cat_confirmation(&opts, &result);
}

bool jw_storage_ui_show_library_read_only(const char *socket_path) {
    jw_ipc_storage_status_info card = {0};
    if (socket_path && socket_path[0] &&
        jw_ipc_get_storage_status(socket_path, jw_storage_ui_sources[0],
                                  &card, NULL, 0) == 0 &&
        (jw_storage_ui_is_read_only(&card) || jw_storage_ui_needs_repair(&card))) {
        return jw_storage_ui_show_warning(socket_path, &card);
    }
    jw__storage_ui_message(T("Your library is read-only. You can't save changes. Open Settings > System > SD Cards to check your card."));
    return false;
}

static bool jw__storage_ui_confirm(const char *message, const char *cancel,
                                   const char *confirm) {
    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = cancel, .is_confirm = false },
        { .button = CAT_BTN_A, .label = confirm, .is_confirm = true },
    };
    cat_message_opts opts = {
        .message = message,
        .footer = footer,
        .footer_count = 2,
    };
    cat_confirm_result result;
    return cat_confirmation(&opts, &result) == CAT_OK && result.confirmed;
}

/* Why repair on the device isn't offered, for a daemon reason key. */
static const char *jw__storage_ui_unavailable_text(const char *reason) {
    if (!reason || !reason[0] || strcmp(reason, "unsupported-platform") == 0) {
        return "";
    }
    if (strcmp(reason, "exfat-not-validated") == 0) {
        return T("exFAT cards can't be repaired on this device yet.");
    }
    if (strcmp(reason, "unsupported-filesystem") == 0) {
        return T("This card's file system can't be repaired on this device.");
    }
    if (strcmp(reason, "tool-missing") == 0 || strcmp(reason, "repair-runner-missing") == 0) {
        return T("Repair isn't installed on this device.");
    }
    if (strcmp(reason, "power-handoff-missing") == 0) {
        return T("Install the matching Leaf system update before checking this card on your device.");
    }
    if (strcmp(reason, "unknown-identity") == 0) {
        return T("Leaf couldn't identify this card.");
    }
    return "";
}

/* User text for a daemon refusal of a repair request. */
static const char *jw__storage_ui_refusal_text(const char *reason) {
    if (reason && strcmp(reason, "power-required") == 0) {
        return T("Connect to power or charge the battery to at least 30 percent to repair this card.");
    }
    if (reason && strcmp(reason, "busy") == 0) {
        return T("Close the game or app first, then try again.");
    }
    if (reason && strcmp(reason, "internal-storage-unavailable") == 0) {
        return T("Your device's internal storage can't record the repair. Check the card on a computer instead.");
    }
    if (reason && (strcmp(reason, "not-found") == 0 || strcmp(reason, "ambiguous") == 0 ||
                   strcmp(reason, "unknown-identity") == 0)) {
        return T("Leaf couldn't identify this card safely. Check the card on a computer instead.");
    }
    if (reason && strcmp(reason, "write-protected") == 0) {
        return T("Your device can't write to this card, so it can't be repaired here.");
    }
    return T("The repair couldn't be started. Check the card on a computer instead.");
}

bool jw_storage_ui_request_repair(const char *socket_path,
                                  const jw_ipc_storage_status_info *card,
                                  const char *mode) {
    if (!socket_path || !card) {
        return false;
    }
    bool check = mode && strcmp(mode, "check") == 0;
    if (!card->repair_supported) {
        char message[512];
        const char *why = jw__storage_ui_unavailable_text(card->repair_unavailable_reason);
        snprintf(message, sizeof(message), "%s%s%s", why, why[0] ? "\n\n" : "",
                 T("Turn off your device and check the card on a computer."));
        jw__storage_ui_message(message);
        return false;
    }
    bool on_battery = !check && !card->external_power;
    if (on_battery && card->battery_percent < JW_STORAGE_REPAIR_MIN_BATTERY_PERCENT) {
        jw__storage_ui_message(T("Connect to power or charge the battery to at least 30 percent to repair this card."));
        return false;
    }
    const char *message = check
        ? T("Your device will restart to check this card. If the check passes, you can save files again.")
        : on_battery
        ? T("Your device will restart to check and repair this card on battery power. If the battery drops below 30 percent before repair starts, it won't run. Damaged files may be shortened, renamed, or recovered under new names. Back up important files on a computer first.")
        : T("Your device will restart to check and repair this card. Damaged files may be shortened, renamed, or recovered under new names. Back up important files on a computer first if you need to recover them. Keep your device connected to power until the check finishes.");
    if (!jw__storage_ui_confirm(message, T("Cancel"),
                                check ? T("Restart and check") :
                                on_battery ? T("Repair on battery") : T("Restart and repair"))) {
        return false;
    }
    char status[256] = "";
    if (jw_ipc_storage_repair_request(socket_path, card->source, check ? "check" : "repair",
                                      on_battery, status, (int)sizeof(status)) != 0) {
        jw_log_warn("storage: %s request for %s refused: %s", check ? "check" : "repair",
                    card->source, status[0] ? status : "no reply");
        jw__storage_ui_message(jw__storage_ui_refusal_text(status));
        return false;
    }
    return true;
}

bool jw_storage_ui_show_warning(const char *socket_path,
                                const jw_ipc_storage_status_info *card) {
    if (!socket_path || !card) {
        return false;
    }
    char name[128];
    char message[1024];
    jw_storage_ui_card_name(card, name, sizeof(name));
    jw_storage_advice advice = jw_storage_repair_advice(card);
    bool write_protected = advice == JW_STORAGE_ADVICE_WRITE_PROTECTED ||
                           strcmp(card->cause, "write-protected") == 0 ||
                           card->block_write_protected;
    const char *title;
    const char *body;
    switch (advice) {
    case JW_STORAGE_ADVICE_WRITE_PROTECTED:
        title = T("Your SD card is write-protected");
        body = T("Your device can't write to this card. If you use an adapter with a lock switch, check that it isn't set to lock.");
        break;
    case JW_STORAGE_ADVICE_REPAIR_FOUND_ERRORS:
        /* The check finished. Checking again gives the same answer, so this
           is the one case that must lead to a repair, not another check. */
        title = T("Your SD card needs repair");
        body = T("Leaf checked this card and found file system errors, usually left by a shutdown that didn't finish saving. To keep your files safe, you can't save to this card until it's repaired.");
        break;
    case JW_STORAGE_ADVICE_REPAIR_FAILED:
        title = T("Your SD card needs repair");
        body = T("Leaf couldn't repair this card on your device. Turn off your device, repair the card on a computer, then check it here to save to it again.");
        break;
    case JW_STORAGE_ADVICE_CHECK_PAUSED_SHUTDOWN:
        title = T("Your SD card is protected");
        body = T("Your SD card is protected because your last shutdown didn't finish saving. Restart your device to check it.");
        break;
    case JW_STORAGE_ADVICE_CHECK_TIMED_OUT:
        title = T("Your SD card is protected");
        body = T("The last check took too long and stopped. Restart your device to check the card again before saving is enabled.");
        break;
    case JW_STORAGE_ADVICE_CHECK_INTERRUPTED:
        title = T("Your SD card is protected");
        body = T("The last check or repair did not finish. Restart your device to check the card before saving is enabled again.");
        break;
    case JW_STORAGE_ADVICE_FILESYSTEM_ERROR:
        title = T("Your SD card needs repair");
        body = T("Your SD card is read-only after a file system error. You can't save new files or changes to this card. This can happen after an unexpected shutdown.");
        break;
    default:
        title = T("Your SD card is read-only");
        body = T("Your SD card is read-only. The cause couldn't be determined. You can't save new files or changes to this card.");
        break;
    }
    snprintf(message, sizeof(message), "%s\n\n%s\n\n%s %s", title, body, T("Card:"), name);

    bool offer_repair = card->repair_supported && !write_protected;
    if (!offer_repair && !write_protected) {
        const char *why = jw__storage_ui_unavailable_text(card->repair_unavailable_reason);
        size_t used = strlen(message);
        snprintf(message + used, sizeof(message) - used, "\n\n%s%s%s", why,
                 why[0] ? " " : "",
                 T("To repair it, turn off your device and check the card on a computer."));
    }

    const char *next_mode = jw_storage_advice_next_mode(advice);
    if (!next_mode) {
        offer_repair = false;
    }
    bool check = next_mode && strcmp(next_mode, "check") == 0;
    bool repair = false;
    if (offer_repair) {
        repair = jw__storage_ui_confirm(message, T("Later"),
                                        check ? T("Restart and check") : T("Repair SD card"));
    } else {
        jw__storage_ui_message(message);
    }
    if (jw_ipc_storage_warning_ack(socket_path, card->source) != 0) {
        jw_log_warn("storage: could not acknowledge the warning for %s", card->source);
    }
    /* This warning already said what the check found; the separate result
       screen would only repeat it. */
    if (advice == JW_STORAGE_ADVICE_REPAIR_FOUND_ERRORS && card->last_repair_valid &&
        !card->last_repair_acknowledged &&
        jw_ipc_storage_repair_result_ack(socket_path, card->last_repair_request_id) != 0) {
        jw_log_warn("storage: could not acknowledge repair result %s",
                    card->last_repair_request_id);
    }
    return repair && jw_storage_ui_request_repair(socket_path, card, next_mode);
}

void jw_storage_ui_show_repair_result(
    const char *socket_path, const jw_ipc_storage_status_info *card,
    bool library_writable) {
    if (!socket_path || !card || !card->last_repair_valid) {
        return;
    }
    const char *outcome = card->last_repair_outcome;
    bool success = (strcmp(outcome, "repaired") == 0 || strcmp(outcome, "clean") == 0) &&
                   strcmp(card->last_repair_mount_state, "read-write") == 0 &&
                   card->mounted && strcmp(card->access, "read-write") == 0 &&
                   strcmp(card->repair, "none") == 0;
    char message[1024];
    if (jw_storage_repair_advice(card) == JW_STORAGE_ADVICE_REPAIR_FOUND_ERRORS) {
        char name[128];
        jw_storage_ui_card_name(card, name, sizeof(name));
        snprintf(message, sizeof(message), "%s\n\n%s\n\n%s %s",
                 T("Your SD card needs repair"),
                 T("Leaf checked this card and found file system errors, usually left by a shutdown that didn't finish saving. To keep your files safe, you can't save to this card until it's repaired."),
                 T("Card:"), name);
        bool repair = card->repair_supported &&
                      jw__storage_ui_confirm(message, T("Later"), T("Repair SD card"));
        if (!card->repair_supported) {
            jw__storage_ui_message(message);
        }
        if (jw_ipc_storage_repair_result_ack(socket_path, card->last_repair_request_id) != 0) {
            jw_log_warn("storage: could not acknowledge repair result %s",
                        card->last_repair_request_id);
        }
        if (repair) {
            (void)jw_storage_ui_request_repair(socket_path, card, "repair");
        }
        return;
    }
    if (success && strcmp(card->last_repair_trigger, "paused-shutdown") == 0) {
        /* A precautionary check after a paused shutdown found nothing wrong.
           The user never saw the card held, so there is nothing to report. */
        if (jw_ipc_storage_repair_result_ack(socket_path, card->last_repair_request_id) != 0) {
            jw_log_warn("storage: could not acknowledge repair result %s",
                        card->last_repair_request_id);
        }
        return;
    }
    if (success) {
        snprintf(message, sizeof(message), "%s %s%s",
                 T("Your card passed the file system check."),
                 T("You can save files again."),
                 strcmp(outcome, "repaired") == 0
                     ? T(" Some damaged files were changed.") : "");
        if (strcmp(outcome, "repaired") == 0) {
            size_t used = strlen(message);
            snprintf(message + used, sizeof(message) - used, "\n\n%s",
                     card->last_repair_reported_changes > 0
                         ? T("The repair log on your device lists the files that were changed. The list may not be complete.")
                         : T("The repair log on your device has the details."));
        }
        if (!library_writable) {
            size_t used = strlen(message);
            snprintf(message + used, sizeof(message) - used, "\n\n%s",
                     T("Your other SD card is still read-only, so your library can't update yet."));
        }
        jw__storage_ui_message(message);
    } else {
        const char *hint;
        if (strcmp(outcome, "timed-out") == 0) {
            hint = T("The check took too long. Your SD card is still protected. You can check it on a computer or restart to try again.");
        } else if (strcmp(outcome, "power-required") == 0) {
            hint = T("Connect to power or charge the battery to at least 30 percent and try again.");
        } else if (strcmp(outcome, "busy") == 0 || strcmp(outcome, "not-found") == 0 ||
                   strcmp(outcome, "ambiguous") == 0 || strcmp(outcome, "unsupported") == 0) {
            hint = T("Leaf couldn't safely check this card, so nothing was changed. Check the card on a computer.");
        } else if (strcmp(outcome, "remount-failed") == 0) {
            hint = T("The card passed the check but couldn't be made writable. Restart your device, then try Check SD card.");
        } else {
            hint = T("Your SD card is still protected. Turn off your device, repair the card on a computer, safely eject it, then insert it and turn your device on to check it again.");
        }
        snprintf(message, sizeof(message), "%s%s%s\n\n%s",
                 strcmp(card->last_repair_trigger, "paused-shutdown") == 0
                     ? T("Your last shutdown didn't finish saving.") : "",
                 strcmp(card->last_repair_trigger, "paused-shutdown") == 0 ? " " : "",
                 strcmp(card->last_repair_mode, "check") == 0
                     ? T("Your SD card check did not finish successfully.")
                     : T("The repair did not finish. The log may include changes that were attempted."),
                 hint);
        jw__storage_ui_message(message);
    }
    if (jw_ipc_storage_repair_result_ack(socket_path, card->last_repair_request_id) != 0) {
        jw_log_warn("storage: could not acknowledge repair result %s",
                    card->last_repair_request_id);
    }
}

void jw_storage_ui_manage_cards(const char *socket_path, char *status,
                                size_t status_size, bool *unmount_secondary) {
    if (unmount_secondary) {
        *unmount_secondary = false;
    }
    if (!socket_path) {
        return;
    }
    jw_ipc_storage_status_info cards[JW_STORAGE_UI_SOURCE_COUNT];
    char labels[JW_STORAGE_UI_SOURCE_COUNT][192];
    cat_list_item items[JW_STORAGE_UI_SOURCE_COUNT];
    int card_count = 0;
    int card_index[JW_STORAGE_UI_SOURCE_COUNT];
    for (int i = 0; i < JW_STORAGE_UI_SOURCE_COUNT; i++) {
        if (jw_ipc_get_storage_status(socket_path, jw_storage_ui_sources[i],
                                      &cards[i], NULL, 0) != 0) {
            continue;
        }
        char name[128];
        jw_storage_ui_card_name(&cards[i], name, sizeof(name));
        snprintf(labels[card_count], sizeof(labels[card_count]), "%s: %s", name,
                 jw_storage_ui_card_state(&cards[i]));
        items[card_count] = (cat_list_item)CAT_LIST_ITEM(labels[card_count], "card");
        card_index[card_count] = i;
        card_count++;
    }
    if (card_count == 0) {
        if (status && status_size) {
            snprintf(status, status_size, "%s", T("SD card status unavailable"));
        }
        return;
    }

    cat_footer_item footer[] = {
        { .button = CAT_BTN_B, .label = T("Back"), .is_confirm = false },
        { .button = CAT_BTN_A, .label = T("Choose"), .is_confirm = true },
    };
    cat_list_opts opts = cat_list_default_opts(T("SD Cards"), items, card_count);
    opts.footer = footer;
    opts.footer_count = 2;
    cat_list_result picked;
    if (cat_list(&opts, &picked) != CAT_OK || picked.selected_index < 0 ||
        picked.selected_index >= card_count) {
        return;
    }
    const jw_ipc_storage_status_info *card = &cards[card_index[picked.selected_index]];

    enum { ACTION_REPAIR, ACTION_CHECK, ACTION_UNMOUNT };
    cat_list_item actions[3];
    int action_ids[3];
    int action_count = 0;
    jw_storage_card_actions offered = jw_storage_advice_card_actions(card);
    if (offered.repair) {
        actions[action_count] = (cat_list_item)CAT_LIST_ITEM(T("Repair SD card"), "repair");
        action_ids[action_count++] = ACTION_REPAIR;
    }
    if (offered.check) {
        actions[action_count] = (cat_list_item)CAT_LIST_ITEM(T("Check SD card"), "check");
        action_ids[action_count++] = ACTION_CHECK;
    }
    if (offered.unmount) {
        actions[action_count] = (cat_list_item)CAT_LIST_ITEM(T("Unmount"), "unmount");
        action_ids[action_count++] = ACTION_UNMOUNT;
    }

    char name[128];
    char title[192];
    jw_storage_ui_card_name(card, name, sizeof(name));
    snprintf(title, sizeof(title), "%s: %s", name, jw_storage_ui_card_state(card));
    if (action_count == 0) {
        char message[512];
        snprintf(message, sizeof(message), "%s\n\n%s", title,
                 card->mounted ? T("This card is working normally.")
                               : T("This card isn't mounted."));
        jw__storage_ui_message(message);
        return;
    }
    cat_list_opts action_opts = cat_list_default_opts(title, actions, action_count);
    action_opts.footer = footer;
    action_opts.footer_count = 2;
    cat_list_result chosen;
    if (cat_list(&action_opts, &chosen) != CAT_OK || chosen.selected_index < 0 ||
        chosen.selected_index >= action_count) {
        return;
    }
    switch (action_ids[chosen.selected_index]) {
        case ACTION_REPAIR:
            if (jw_storage_ui_request_repair(socket_path, card, "repair") && status && status_size) {
                snprintf(status, status_size, "%s", T("Restarting to repair your SD card"));
            }
            break;
        case ACTION_CHECK:
            if (jw_storage_ui_request_repair(socket_path, card, "check") && status && status_size) {
                snprintf(status, status_size, "%s", T("Restarting to check your SD card"));
            }
            break;
        case ACTION_UNMOUNT:
            if (unmount_secondary) {
                *unmount_secondary = true;
            }
            break;
    }
}
