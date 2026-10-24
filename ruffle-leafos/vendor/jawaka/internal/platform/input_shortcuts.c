#include "internal/platform/input_shortcuts.h"

#include "internal/i18n/i18n.h"

#include <string.h>

/* One row per button, in enum order. Keeping the persisted name and the
   display label together is what stops the two drifting apart: a new button is
   one row, and a row missing either half will not compile. */
static const struct {
    const char *name;  /* persisted in the settings table */
    const char *label; /* shown in Settings */
} kButtons[JW_INPUT_SHORTCUT_BUTTON_COUNT] = {
    /* "Disabled" is the only UI word here. The rest are the labels silkscreened
       on the device, which stay as they are in every language -- see the proper
       nouns note in i18n.h. */
    [JW_INPUT_SHORTCUT_BUTTON_NONE]   = {"disabled", JW_UI("Disabled")},
    [JW_INPUT_SHORTCUT_BUTTON_A]      = {"a", "A"},
    [JW_INPUT_SHORTCUT_BUTTON_B]      = {"b", "B"},
    [JW_INPUT_SHORTCUT_BUTTON_X]      = {"x", "X"},
    [JW_INPUT_SHORTCUT_BUTTON_Y]      = {"y", "Y"},
    [JW_INPUT_SHORTCUT_BUTTON_L1]     = {"l1", "L1"},
    [JW_INPUT_SHORTCUT_BUTTON_R1]     = {"r1", "R1"},
    [JW_INPUT_SHORTCUT_BUTTON_L2]     = {"l2", "L2"},
    [JW_INPUT_SHORTCUT_BUTTON_R2]     = {"r2", "R2"},
    [JW_INPUT_SHORTCUT_BUTTON_SELECT] = {"select", "Select"},
    [JW_INPUT_SHORTCUT_BUTTON_START]  = {"start", "Start"},
    [JW_INPUT_SHORTCUT_BUTTON_L3]     = {"l3", "L3"},
};

/* Declaration order is also the duplicate-resolution priority, so the Game
   Switcher keeps its button when two actions collide. It is the one action
   with no feature toggle to turn off and no other way to reach it in a
   running game. */
static const struct {
    const char *key;   /* settings-table key */
    const char *label; /* action name for the UI and for logs */
    jw_input_shortcut_button fallback;
} kActions[JW_INPUT_SHORTCUT_ACTION_COUNT] = {
    [JW_INPUT_SHORTCUT_GAME_SWITCHER] = {"input_shortcut_game_switcher",
                                         JW_UI("Game Switcher"),
                                         JW_INPUT_SHORTCUT_BUTTON_SELECT},
    [JW_INPUT_SHORTCUT_SCREENSHOT]    = {"input_shortcut_screenshot",
                                         JW_UI("Screenshot"),
                                         JW_INPUT_SHORTCUT_BUTTON_L1},
    [JW_INPUT_SHORTCUT_RECORDING]     = {"input_shortcut_recording",
                                         JW_UI("Recording"),
                                         JW_INPUT_SHORTCUT_BUTTON_R1},
};

static bool jw__action_in_range(jw_input_shortcut_action action) {
    return action >= 0 && action < JW_INPUT_SHORTCUT_ACTION_COUNT;
}

static bool jw__button_in_range(jw_input_shortcut_button button) {
    return button >= 0 && button < JW_INPUT_SHORTCUT_BUTTON_COUNT;
}

const char *jw_input_shortcut_action_key(jw_input_shortcut_action action) {
    return jw__action_in_range(action) ? kActions[action].key : NULL;
}

const char *jw_input_shortcut_action_label(jw_input_shortcut_action action) {
    return jw__action_in_range(action) ? kActions[action].label : NULL;
}

const char *jw_input_shortcut_button_name(jw_input_shortcut_button button) {
    return jw__button_in_range(button) ? kButtons[button].name : NULL;
}

const char *jw_input_shortcut_button_label(jw_input_shortcut_button button) {
    return jw__button_in_range(button) ? kButtons[button].label : NULL;
}

bool jw_input_shortcut_button_parse(const char *name,
                                    jw_input_shortcut_button *out) {
    if (!name || !name[0]) {
        return false;
    }
    for (int i = 0; i < JW_INPUT_SHORTCUT_BUTTON_COUNT; i++) {
        if (strcmp(name, kButtons[i].name) == 0) {
            if (out) {
                *out = (jw_input_shortcut_button)i;
            }
            return true;
        }
    }
    return false;
}

void jw_input_shortcuts_defaults(jw_input_shortcuts *out) {
    if (!out) {
        return;
    }
    for (int i = 0; i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        out->buttons[i] = kActions[i].fallback;
    }
}

int jw_input_shortcuts_resolve(const char *const *values,
                               jw_input_shortcuts *out,
                               bool *invalid, int *duplicate) {
    if (!out) {
        return 0;
    }
    for (int i = 0; i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        if (invalid) invalid[i] = false;
        if (duplicate) duplicate[i] = -1;
    }

    jw_input_shortcuts_defaults(out);
    int rejected = 0;

    /* Pass one. An absent key is the ordinary case on a device that has never
       opened this screen, and keeps the default.

       A key that is *present* but unparseable -- including an empty string --
       disables that action instead. Falling back to the default there would
       silently arm a chord the stored configuration does not describe: a
       corrupted or hand-edited row would quietly behave as though it said
       "select", and the user would have no way to tell a working default from
       a value that failed to load. Fail closed and say so. */
    for (int i = 0; i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        const char *value = values ? values[i] : NULL;
        if (!value) {
            continue;   /* absent: keep the default */
        }
        jw_input_shortcut_button button;
        if (value[0] && jw_input_shortcut_button_parse(value, &button)) {
            out->buttons[i] = button;
        } else {
            out->buttons[i] = JW_INPUT_SHORTCUT_BUTTON_NONE;
            if (invalid) invalid[i] = true;
            rejected++;
        }
    }

    /* Pass two: resolve collisions in priority order. Done after parsing
       rather than during it because a later action must be able to lose to an
       earlier one regardless of which of the two was stored. Disabling the
       loser rather than reassigning it keeps the outcome predictable: the user
       sees the binding they made fail, instead of finding it silently moved. */
    for (int i = 0; i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        if (out->buttons[i] == JW_INPUT_SHORTCUT_BUTTON_NONE) {
            continue;
        }
        for (int j = 0; j < i; j++) {
            if (out->buttons[j] == out->buttons[i]) {
                out->buttons[i] = JW_INPUT_SHORTCUT_BUTTON_NONE;
                if (duplicate) duplicate[i] = j;   /* the action that kept it */
                rejected++;
                break;
            }
        }
    }

    return rejected;
}

bool jw_input_shortcuts_action_for_button(const jw_input_shortcuts *shortcuts,
                                          jw_input_shortcut_button button,
                                          jw_input_shortcut_action *out) {
    if (!shortcuts || button == JW_INPUT_SHORTCUT_BUTTON_NONE ||
        !jw__button_in_range(button)) {
        return false;
    }
    for (int i = 0; i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        if (shortcuts->buttons[i] == button) {
            if (out) {
                *out = (jw_input_shortcut_action)i;
            }
            return true;
        }
    }
    return false;
}

bool jw_input_shortcuts_valid(const jw_input_shortcuts *shortcuts) {
    if (!shortcuts) {
        return false;
    }
    for (int i = 0; i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        if (!jw__button_in_range(shortcuts->buttons[i])) {
            return false;
        }
        if (shortcuts->buttons[i] == JW_INPUT_SHORTCUT_BUTTON_NONE) {
            continue;
        }
        for (int j = 0; j < i; j++) {
            if (shortcuts->buttons[j] == shortcuts->buttons[i]) {
                return false;
            }
        }
    }
    return true;
}
