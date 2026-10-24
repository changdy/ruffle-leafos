#ifndef JW_PLATFORM_INPUT_SHORTCUTS_H
#define JW_PLATFORM_INPUT_SHORTCUTS_H

#include <stdbool.h>
#include <stddef.h>

/* Leaf's in-game shortcut bindings: which physical button, held with Menu,
 * performs each Leaf action.
 *
 * This exists because Settings writes the bindings and jawakad reads them, and
 * both have to interpret the same persisted strings the same way. It is a
 * value model and nothing more -- no remapping framework, no per-core
 * profiles, no dispatch. Keep it that way; the finite set below is what makes
 * one native test enough to prove the two sides agree.
 *
 * Symbolic names are persisted rather than numbers. evdev codes, SDL indices
 * and RetroArch joypad ids all disagree about which number a button is, so a
 * raw number in the database would let the UI and the daemon silently mean
 * different buttons. Nothing here includes <linux/input.h>: the MLP1 input
 * proxy alone maps these symbols to evdev codes, and Settings and the database
 * never see one.
 *
 * The modifier is always the physical Menu button and is not configurable.
 */

typedef enum {
    JW_INPUT_SHORTCUT_GAME_SWITCHER = 0,
    JW_INPUT_SHORTCUT_SCREENSHOT,
    JW_INPUT_SHORTCUT_RECORDING,
    JW_INPUT_SHORTCUT_ACTION_COUNT
} jw_input_shortcut_action;

/* Buttons offerable as the second half of a Menu chord.
 *
 * Deliberately excluded: Menu (it is the modifier), volume and power (system
 * owned), and the D-pad and analog sticks (they arrive as EV_ABS and would
 * need a second chord state machine). Add one only for a concrete request that
 * justifies the cost -- every entry here is a button a game can no longer use
 * while Menu is held.
 *
 * JW_INPUT_SHORTCUT_BUTTON_NONE is the "Disabled" choice and is a real,
 * persistable value, not an error. */
typedef enum {
    JW_INPUT_SHORTCUT_BUTTON_NONE = 0,
    JW_INPUT_SHORTCUT_BUTTON_A,
    JW_INPUT_SHORTCUT_BUTTON_B,
    JW_INPUT_SHORTCUT_BUTTON_X,
    JW_INPUT_SHORTCUT_BUTTON_Y,
    JW_INPUT_SHORTCUT_BUTTON_L1,
    JW_INPUT_SHORTCUT_BUTTON_R1,
    JW_INPUT_SHORTCUT_BUTTON_L2,
    JW_INPUT_SHORTCUT_BUTTON_R2,
    JW_INPUT_SHORTCUT_BUTTON_SELECT,
    JW_INPUT_SHORTCUT_BUTTON_START,
    JW_INPUT_SHORTCUT_BUTTON_L3,
    JW_INPUT_SHORTCUT_BUTTON_COUNT
} jw_input_shortcut_button;

/* One binding per action, indexed by jw_input_shortcut_action. Passed whole
 * between Settings, IPC and the daemon so a partial update cannot exist. */
typedef struct {
    jw_input_shortcut_button buttons[JW_INPUT_SHORTCUT_ACTION_COUNT];
} jw_input_shortcuts;

/* Settings-table key for an action, e.g. "input_shortcut_screenshot".
   NULL for an out-of-range action. */
const char *jw_input_shortcut_action_key(jw_input_shortcut_action action);

/* Untranslated action name for logs and conflict messages ("Game Switcher").
   Settings translates it at the point it is drawn. */
const char *jw_input_shortcut_action_label(jw_input_shortcut_action action);

/* Persisted form of a button, e.g. "l1" or "disabled". Never NULL for a valid
   button; NULL for an out-of-range one. */
const char *jw_input_shortcut_button_name(jw_input_shortcut_button button);

/* Display form, e.g. "L1" or "Disabled". Button names are the silkscreen
   labels on the device and are deliberately not translated; only "Disabled"
   is marked for extraction. */
const char *jw_input_shortcut_button_label(jw_input_shortcut_button button);

/* Parse a persisted name. Returns false and leaves *out untouched for NULL,
   empty, or unrecognized input, so a caller can tell "absent or corrupt" from
   an explicit "disabled". */
bool jw_input_shortcut_button_parse(const char *name,
                                    jw_input_shortcut_button *out);

/* The compiled-in bindings: Game Switcher on Select, Screenshot on L1,
   Recording on R1 -- the fixed chords these replace. */
void jw_input_shortcuts_defaults(jw_input_shortcuts *out);

/* Resolve persisted values into a usable snapshot.
 *
 * `values` is indexed by action. A NULL entry (the key is absent) keeps that
 * action's default. A present but unparseable entry -- including an empty
 * string -- **disables** that action rather than falling back, so a corrupt
 * row cannot silently arm a chord the stored configuration does not describe.
 * Two actions cannot share a button: on a collision the
 * lower-priority action is disabled, priority being declaration order (Game
 * Switcher, then Screenshot, then Recording). This is fail-closed on purpose
 * -- a hand-edited or half-written database must not make one button fire two
 * actions, and must never fire the wrong one.
 *
 * Returns the number of entries that were rejected. When non-NULL, `invalid`
 * receives a per-action flag for values that failed to parse, and `duplicate`
 * receives, for each disabled loser, the action that kept the button (or -1).
 * Reporting the winner rather than a bare flag is what lets a caller name both
 * sides of the clash, which is what makes the log actionable. A caller that
 * does not care passes NULL for both. */
int jw_input_shortcuts_resolve(const char *const *values,
                               jw_input_shortcuts *out,
                               bool *invalid, int *duplicate);

/* Which action owns `button`, or false if none does. Disabled never matches,
   so several disabled actions do not collide with each other. */
bool jw_input_shortcuts_action_for_button(const jw_input_shortcuts *shortcuts,
                                          jw_input_shortcut_button button,
                                          jw_input_shortcut_action *out);

/* True when the snapshot is internally consistent: every button in range, and
   no two enabled actions sharing one. jw_input_shortcuts_resolve() always
   produces a snapshot that satisfies this; the IPC handler uses it to reject
   one that arrived from anywhere else. */
bool jw_input_shortcuts_valid(const jw_input_shortcuts *shortcuts);

#endif /* JW_PLATFORM_INPUT_SHORTCUTS_H */
