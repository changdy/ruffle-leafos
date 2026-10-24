#include "cmd/jawaka-osd/game_launch.h"
#include "cmd/jawaka-osd/osd_utf8.h"
#include "internal/i18n/i18n.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

const char *jw_osd_game_stage_name(jw_osd_game_stage stage) {
    switch (stage) {
        case JW_OSD_GAME_CHECKING: return "checking";
        case JW_OSD_GAME_SYNCING:  return "syncing";
        case JW_OSD_GAME_STOPPING: return "stopping";
        case JW_OSD_GAME_SETTINGS_NOT_SAVED: return "settings-not-saved";
        case JW_OSD_GAME_STORAGE_READ_ONLY: return "storage-read-only";
        case JW_OSD_PICO8_EXIT_CONFIRM: return "pico8-exit";
        case JW_OSD_PICO8_IMPORT: return "pico8-import";
        case JW_OSD_PICO8_IMPORT_FAILED: return "pico8-import-failed";
        case JW_OSD_POWER_SAVE_RELEASE: return "power-save-release";
        case JW_OSD_POWER_SAVE_SAVING: return "power-save-saving";
        case JW_OSD_POWER_SAVE_SAVED: return "power-save-saved";
        case JW_OSD_POWER_SAVE_FAILED: return "power-save-failed";
        case JW_OSD_BOOT_RESUME: return "boot-resume";
    }
    return "unknown";
}

bool jw_osd_game_launch_parse(const cJSON *root,
                              jw_osd_game_stage *stage,
                              int *pending_items,
                              uint64_t *expires_ms) {
    if (!cJSON_IsObject(root) || !stage || !pending_items || !expires_ms) {
        return false;
    }
    const cJSON *expires = cJSON_GetObjectItemCaseSensitive(root, "expires_ms");
    int fields = 2;
    *expires_ms = 0;
    if (expires) {
        /* Monotonic milliseconds stay far below 2^53, where doubles are exact. */
        if (!cJSON_IsNumber(expires) || expires->valuedouble < 1.0 ||
            expires->valuedouble > 9007199254740992.0 ||
            expires->valuedouble != (double)(uint64_t)expires->valuedouble) {
            return false;
        }
        *expires_ms = (uint64_t)expires->valuedouble;
        fields++;
    }
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const cJSON *stage_item = cJSON_GetObjectItemCaseSensitive(root, "stage");
    const cJSON *pending = cJSON_GetObjectItemCaseSensitive(root, "pending_items");
    if (!cJSON_IsString(type) || !type->valuestring ||
        strcmp(type->valuestring, "show-game-launch") != 0 ||
        !cJSON_IsString(stage_item) || !stage_item->valuestring) {
        return false;
    }

    if (strcmp(stage_item->valuestring, "checking") == 0) {
        *stage = JW_OSD_GAME_CHECKING;
    } else if (strcmp(stage_item->valuestring, "syncing") == 0) {
        *stage = JW_OSD_GAME_SYNCING;
    } else if (strcmp(stage_item->valuestring, "stopping") == 0) {
        *stage = JW_OSD_GAME_STOPPING;
    } else if (strcmp(stage_item->valuestring, "settings-not-saved") == 0) {
        *stage = JW_OSD_GAME_SETTINGS_NOT_SAVED;
    } else if (strcmp(stage_item->valuestring, "storage-read-only") == 0) {
        *stage = JW_OSD_GAME_STORAGE_READ_ONLY;
    } else if (strcmp(stage_item->valuestring, "pico8-exit") == 0) {
        *stage = JW_OSD_PICO8_EXIT_CONFIRM;
    } else if (strcmp(stage_item->valuestring, "pico8-import") == 0) {
        *stage = JW_OSD_PICO8_IMPORT;
    } else if (strcmp(stage_item->valuestring, "pico8-import-failed") == 0) {
        *stage = JW_OSD_PICO8_IMPORT_FAILED;
    } else if (strcmp(stage_item->valuestring, "power-save-release") == 0) {
        *stage = JW_OSD_POWER_SAVE_RELEASE;
    } else if (strcmp(stage_item->valuestring, "power-save-saving") == 0) {
        *stage = JW_OSD_POWER_SAVE_SAVING;
    } else if (strcmp(stage_item->valuestring, "power-save-saved") == 0) {
        *stage = JW_OSD_POWER_SAVE_SAVED;
    } else if (strcmp(stage_item->valuestring, "power-save-failed") == 0) {
        *stage = JW_OSD_POWER_SAVE_FAILED;
    } else if (strcmp(stage_item->valuestring, "boot-resume") == 0) {
        *stage = JW_OSD_BOOT_RESUME;
    } else {
        return false;
    }

    if (*stage == JW_OSD_GAME_SYNCING) {
        if (!cJSON_IsNumber(pending) || pending->valuedouble < 0.0 ||
            pending->valuedouble > (double)INT_MAX ||
            pending->valuedouble != (double)pending->valueint ||
            cJSON_GetArraySize(root) != fields + 1) {
            return false;
        }
        *pending_items = pending->valueint;
        return true;
    }
    if (pending || cJSON_GetArraySize(root) != fields) {
        return false;
    }
    *pending_items = 0;
    return true;
}

/* Shortening into a fixed buffer must not split a character. */
static void jw__copy(char *out, size_t size, const char *text) {
    size_t len = strlen(text);
    if (len >= size) len = jw_osd_utf8_boundary(text, size - 1);
    memcpy(out, text, len);
    out[len] = '\0';
}

/* A shipped table cannot change a conversion (i18n-compile.py refuses it), but
   a live .tsv override is not compiled. Anything other than exactly one %d
   would make snprintf read an argument that is not there. */
static bool jw__count_format_ok(const char *format) {
    int conversions = 0;
    for (const char *p = format; p && *p; p++) {
        if (*p != '%') continue;
        if (p[1] == '%') {
            p++;
        } else if (p[1] == 'd') {
            conversions++;
            p++;
        } else {
            return false;
        }
    }
    return conversions == 1;
}

void jw_osd_game_launch_text(jw_osd_game_stage stage, int pending_items,
                             char *title, size_t title_size,
                             char *action, size_t action_size) {
    if (title && title_size > 0) title[0] = '\0';
    if (action && action_size > 0) action[0] = '\0';
    if (!title || title_size == 0 || !action || action_size == 0) {
        return;
    }
    switch (stage) {
        case JW_OSD_PICO8_IMPORT:
            jw__copy(title, title_size, T("Adding Splore favorites"));
            jw__copy(action, action_size, T("Please wait"));
            break;
        case JW_OSD_PICO8_IMPORT_FAILED:
            jw__copy(title, title_size, T("Splore import incomplete"));
            jw__copy(action, action_size, T("Try Splore again"));
            break;
        case JW_OSD_PICO8_EXIT_CONFIRM:
            jw__copy(title, title_size, T("Return to Leaf?"));
            jw__copy(action, action_size, T("Press Menu again"));
            break;
        case JW_OSD_GAME_CHECKING:
            jw__copy(title, title_size, T("Syncthing: Checking saves"));
            break;
        case JW_OSD_GAME_SYNCING: {
            if (pending_items < 0) pending_items = 0;
            /* Singular and plural are separate keys, translated whole. */
            const char *english = pending_items == 1
                ? "Syncthing: Syncing %d item" : "Syncthing: Syncing %d items";
            const char *format = pending_items == 1
                ? T("Syncthing: Syncing %d item") : T("Syncthing: Syncing %d items");
            if (!jw__count_format_ok(format)) format = english;
            int n = snprintf(title, title_size, format, pending_items);
            if (n > 0 && (size_t)n >= title_size) {
                title[jw_osd_utf8_boundary(title, title_size - 1)] = '\0';
            }
            jw__copy(action, action_size, T("Menu: Start now"));
            break;
        }
        case JW_OSD_GAME_STOPPING:
            jw__copy(title, title_size, T("Syncthing: Stopping"));
            break;
        case JW_OSD_GAME_SETTINGS_NOT_SAVED:
            jw__copy(title, title_size, T("RetroArch settings not saved"));
            break;
        case JW_OSD_GAME_STORAGE_READ_ONLY:
            /* The card flipped read-only during play. */
            jw__copy(title, title_size, T("Your SD card is read-only"));
            jw__copy(action, action_size, T("New saves may fail"));
            break;
        case JW_OSD_POWER_SAVE_RELEASE:
            jw__copy(title, title_size, T("Preparing to save"));
            jw__copy(action, action_size, T("Release the power button"));
            break;
        case JW_OSD_POWER_SAVE_SAVING:
            jw__copy(title, title_size, T("Saving game"));
            break;
        case JW_OSD_POWER_SAVE_SAVED:
            jw__copy(title, title_size, T("Game saved"));
            jw__copy(action, action_size, T("Powering off"));
            break;
        case JW_OSD_POWER_SAVE_FAILED:
            jw__copy(title, title_size, T("Game not saved"));
            jw__copy(action, action_size, T("Powering off"));
            break;
        case JW_OSD_BOOT_RESUME:
            jw__copy(title, title_size, T("Resuming your game"));
            break;
    }
}
