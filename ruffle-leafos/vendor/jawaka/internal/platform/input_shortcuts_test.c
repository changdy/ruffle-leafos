#include "internal/platform/input_shortcuts.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Build the `values` array resolve() takes, so a case reads as the three
   stored strings it actually is. NULL means the key is absent. */
static int resolve3(const char *switcher, const char *shot, const char *rec,
                    jw_input_shortcuts *out, bool *invalid, int *duplicate) {
    const char *values[JW_INPUT_SHORTCUT_ACTION_COUNT];
    values[JW_INPUT_SHORTCUT_GAME_SWITCHER] = switcher;
    values[JW_INPUT_SHORTCUT_SCREENSHOT] = shot;
    values[JW_INPUT_SHORTCUT_RECORDING] = rec;
    return jw_input_shortcuts_resolve(values, out, invalid, duplicate);
}

static void expect_bindings(const jw_input_shortcuts *s,
                            jw_input_shortcut_button switcher,
                            jw_input_shortcut_button shot,
                            jw_input_shortcut_button rec) {
    assert(s->buttons[JW_INPUT_SHORTCUT_GAME_SWITCHER] == switcher);
    assert(s->buttons[JW_INPUT_SHORTCUT_SCREENSHOT] == shot);
    assert(s->buttons[JW_INPUT_SHORTCUT_RECORDING] == rec);
    /* Anything resolve() produces must also pass the IPC-side check, or the
       daemon would reject a snapshot Settings considers good. */
    assert(jw_input_shortcuts_valid(s));
}

int main(void) {
    jw_input_shortcuts s;
    bool invalid[JW_INPUT_SHORTCUT_ACTION_COUNT];
    int duplicate[JW_INPUT_SHORTCUT_ACTION_COUNT];

    /* Every button round-trips through its persisted name. This is the
       property the whole module exists for: Settings writes a name and the
       daemon reads back the same button. */
    for (int i = 0; i < JW_INPUT_SHORTCUT_BUTTON_COUNT; i++) {
        jw_input_shortcut_button button = (jw_input_shortcut_button)i;
        const char *name = jw_input_shortcut_button_name(button);
        const char *label = jw_input_shortcut_button_label(button);
        assert(name && name[0]);
        assert(label && label[0]);
        jw_input_shortcut_button parsed;
        assert(jw_input_shortcut_button_parse(name, &parsed));
        assert(parsed == button);
    }

    /* Names are unique, so no two buttons can be stored identically. */
    for (int i = 0; i < JW_INPUT_SHORTCUT_BUTTON_COUNT; i++) {
        for (int j = i + 1; j < JW_INPUT_SHORTCUT_BUTTON_COUNT; j++) {
            assert(strcmp(jw_input_shortcut_button_name((jw_input_shortcut_button)i),
                          jw_input_shortcut_button_name((jw_input_shortcut_button)j)) != 0);
        }
    }

    /* Each action has a distinct settings key and a label. */
    for (int i = 0; i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        const char *key = jw_input_shortcut_action_key((jw_input_shortcut_action)i);
        assert(key && strncmp(key, "input_shortcut_", 15) == 0);
        assert(jw_input_shortcut_action_label((jw_input_shortcut_action)i));
        for (int j = i + 1; j < JW_INPUT_SHORTCUT_ACTION_COUNT; j++) {
            assert(strcmp(key,
                          jw_input_shortcut_action_key((jw_input_shortcut_action)j)) != 0);
        }
    }

    /* Out-of-range lookups report rather than index off the end. */
    assert(jw_input_shortcut_button_name(JW_INPUT_SHORTCUT_BUTTON_COUNT) == NULL);
    assert(jw_input_shortcut_button_label((jw_input_shortcut_button)-1) == NULL);
    assert(jw_input_shortcut_action_key(JW_INPUT_SHORTCUT_ACTION_COUNT) == NULL);
    assert(jw_input_shortcut_action_label((jw_input_shortcut_action)-1) == NULL);

    /* Junk never parses, and a rejected parse leaves the caller's value alone
       so "absent or corrupt" stays distinguishable from an explicit choice. */
    jw_input_shortcut_button untouched = JW_INPUT_SHORTCUT_BUTTON_X;
    assert(!jw_input_shortcut_button_parse(NULL, &untouched));
    assert(!jw_input_shortcut_button_parse("", &untouched));
    assert(!jw_input_shortcut_button_parse("L1", &untouched));   /* case matters */
    assert(!jw_input_shortcut_button_parse("l", &untouched));
    assert(!jw_input_shortcut_button_parse("l1 ", &untouched));
    assert(!jw_input_shortcut_button_parse("menu", &untouched)); /* the modifier */
    assert(!jw_input_shortcut_button_parse("up", &untouched));   /* d-pad */
    assert(!jw_input_shortcut_button_parse("r3", &untouched));   /* no such button */
    assert(!jw_input_shortcut_button_parse("9", &untouched));    /* an SDL index */
    assert(untouched == JW_INPUT_SHORTCUT_BUTTON_X);

    /* Disabled is a real persistable value, not a parse failure. */
    jw_input_shortcut_button none = JW_INPUT_SHORTCUT_BUTTON_A;
    assert(jw_input_shortcut_button_parse("disabled", &none));
    assert(none == JW_INPUT_SHORTCUT_BUTTON_NONE);

    /* The defaults are the fixed chords this feature replaces. */
    jw_input_shortcuts defaults;
    jw_input_shortcuts_defaults(&defaults);
    expect_bindings(&defaults, JW_INPUT_SHORTCUT_BUTTON_SELECT,
                    JW_INPUT_SHORTCUT_BUTTON_L1, JW_INPUT_SHORTCUT_BUTTON_R1);

    /* A device that has never opened the screen has no keys at all. Absent is
       the ordinary case and is not counted as rejected. */
    assert(resolve3(NULL, NULL, NULL, &s, invalid, duplicate) == 0);
    expect_bindings(&s, JW_INPUT_SHORTCUT_BUTTON_SELECT,
                    JW_INPUT_SHORTCUT_BUTTON_L1, JW_INPUT_SHORTCUT_BUTTON_R1);
    assert(!invalid[0] && !invalid[1] && !invalid[2]);
    assert(jw_input_shortcuts_resolve(NULL, &s, NULL, NULL) == 0);

    /* Fully remapped, including a disabled action. */
    assert(resolve3("start", "l2", "disabled", &s, invalid, duplicate) == 0);
    expect_bindings(&s, JW_INPUT_SHORTCUT_BUTTON_START,
                    JW_INPUT_SHORTCUT_BUTTON_L2, JW_INPUT_SHORTCUT_BUTTON_NONE);

    /* All three disabled: several NONEs must not collide with each other. */
    assert(resolve3("disabled", "disabled", "disabled", &s, invalid, duplicate) == 0);
    expect_bindings(&s, JW_INPUT_SHORTCUT_BUTTON_NONE,
                    JW_INPUT_SHORTCUT_BUTTON_NONE, JW_INPUT_SHORTCUT_BUTTON_NONE);

    /* A present-but-corrupt value DISABLES its action rather than falling back
       to the default. Falling back would silently arm a chord the stored
       configuration does not describe, and the user could not tell a working
       default from a value that failed to load. The others are untouched, and
       an absent key still keeps its default. */
    assert(resolve3("select", "nonsense", NULL, &s, invalid, duplicate) == 1);
    expect_bindings(&s, JW_INPUT_SHORTCUT_BUTTON_SELECT,
                    JW_INPUT_SHORTCUT_BUTTON_NONE, JW_INPUT_SHORTCUT_BUTTON_R1);
    assert(!invalid[JW_INPUT_SHORTCUT_GAME_SWITCHER]);
    assert(invalid[JW_INPUT_SHORTCUT_SCREENSHOT]);
    assert(!invalid[JW_INPUT_SHORTCUT_RECORDING]);

    /* An empty string is a present value, not an absent key: a row written as
       "" is corruption, not "never configured". */
    assert(resolve3("", NULL, NULL, &s, invalid, duplicate) == 1);
    expect_bindings(&s, JW_INPUT_SHORTCUT_BUTTON_NONE,
                    JW_INPUT_SHORTCUT_BUTTON_L1, JW_INPUT_SHORTCUT_BUTTON_R1);
    assert(invalid[JW_INPUT_SHORTCUT_GAME_SWITCHER]);

    /* All three unreadable: everything off, nothing guessed. */
    assert(resolve3("x!", "??", "  ", &s, invalid, duplicate) == 3);
    expect_bindings(&s, JW_INPUT_SHORTCUT_BUTTON_NONE,
                    JW_INPUT_SHORTCUT_BUTTON_NONE, JW_INPUT_SHORTCUT_BUTTON_NONE);
    assert(invalid[0] && invalid[1] && invalid[2]);

    /* Duplicate: the lower-priority action loses and is disabled, never
       silently moved to some other button. */
    assert(resolve3("l1", "l1", "r1", &s, invalid, duplicate) == 1);
    expect_bindings(&s, JW_INPUT_SHORTCUT_BUTTON_L1,
                    JW_INPUT_SHORTCUT_BUTTON_NONE, JW_INPUT_SHORTCUT_BUTTON_R1);
    /* The loser records WHICH action kept the button, so a log can name both
       sides of the clash. */
    assert(duplicate[JW_INPUT_SHORTCUT_SCREENSHOT] == JW_INPUT_SHORTCUT_GAME_SWITCHER);
    assert(duplicate[JW_INPUT_SHORTCUT_GAME_SWITCHER] == -1);

    /* Priority is declaration order, not storage order: Game Switcher keeps
       the button even when Screenshot is the one that "already had" L1 by
       default and the switcher is the value that changed. */
    assert(resolve3("l1", NULL, NULL, &s, invalid, duplicate) == 1);
    expect_bindings(&s, JW_INPUT_SHORTCUT_BUTTON_L1,
                    JW_INPUT_SHORTCUT_BUTTON_NONE, JW_INPUT_SHORTCUT_BUTTON_R1);

    /* Recording loses to both of its seniors. */
    assert(resolve3("y", "y", "y", &s, invalid, duplicate) == 2);
    expect_bindings(&s, JW_INPUT_SHORTCUT_BUTTON_Y,
                    JW_INPUT_SHORTCUT_BUTTON_NONE, JW_INPUT_SHORTCUT_BUTTON_NONE);
    /* Both losers point at the Game Switcher, which declared first. */
    assert(duplicate[JW_INPUT_SHORTCUT_SCREENSHOT] == JW_INPUT_SHORTCUT_GAME_SWITCHER);
    assert(duplicate[JW_INPUT_SHORTCUT_RECORDING] == JW_INPUT_SHORTCUT_GAME_SWITCHER);

    /* Invalid and duplicate in one load: both counted, each reported through
       its own channel, and an invalid entry cannot also read as a duplicate. */
    assert(resolve3("r1", "bogus", "r1", &s, invalid, duplicate) == 2);
    expect_bindings(&s, JW_INPUT_SHORTCUT_BUTTON_R1,
                    JW_INPUT_SHORTCUT_BUTTON_NONE, JW_INPUT_SHORTCUT_BUTTON_NONE);
    assert(invalid[JW_INPUT_SHORTCUT_SCREENSHOT]);
    assert(duplicate[JW_INPUT_SHORTCUT_SCREENSHOT] == -1);
    assert(!invalid[JW_INPUT_SHORTCUT_RECORDING]);
    assert(duplicate[JW_INPUT_SHORTCUT_RECORDING] == JW_INPUT_SHORTCUT_GAME_SWITCHER);

    /* A disabled winner is not a winner: three explicit "disabled" values are
       all legal and none of them collide. */
    assert(resolve3("disabled", "disabled", "disabled", &s, invalid, duplicate) == 0);
    for (int i = 0; i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        assert(!invalid[i] && duplicate[i] == -1);
    }

    /* Flags are cleared on entry, so a caller reusing its arrays cannot read
       a stale report from the previous load. */
    assert(resolve3("select", "l1", "r1", &s, invalid, duplicate) == 0);
    for (int i = 0; i < JW_INPUT_SHORTCUT_ACTION_COUNT; i++) {
        assert(!invalid[i] && duplicate[i] == -1);
    }
    /* NULL report arrays are allowed. */
    assert(resolve3("l1", "l1", NULL, &s, NULL, NULL) == 1);

    /* Button lookup: the dispatch path's only question. */
    jw_input_shortcuts_defaults(&s);
    jw_input_shortcut_action action;
    assert(jw_input_shortcuts_action_for_button(&s, JW_INPUT_SHORTCUT_BUTTON_SELECT,
                                                &action));
    assert(action == JW_INPUT_SHORTCUT_GAME_SWITCHER);
    assert(jw_input_shortcuts_action_for_button(&s, JW_INPUT_SHORTCUT_BUTTON_R1,
                                                &action));
    assert(action == JW_INPUT_SHORTCUT_RECORDING);
    assert(!jw_input_shortcuts_action_for_button(&s, JW_INPUT_SHORTCUT_BUTTON_START,
                                                 &action));
    assert(!jw_input_shortcuts_action_for_button(NULL, JW_INPUT_SHORTCUT_BUTTON_A,
                                                 &action));
    /* Disabled must never match, or a chord with no binding would dispatch. */
    assert(!jw_input_shortcuts_action_for_button(&s, JW_INPUT_SHORTCUT_BUTTON_NONE,
                                                 &action));
    s.buttons[JW_INPUT_SHORTCUT_SCREENSHOT] = JW_INPUT_SHORTCUT_BUTTON_NONE;
    assert(!jw_input_shortcuts_action_for_button(&s, JW_INPUT_SHORTCUT_BUTTON_NONE,
                                                 &action));

    /* Validation rejects what only an out-of-band sender could produce: the
       IPC handler's guard against a snapshot that never went through
       resolve(). */
    assert(!jw_input_shortcuts_valid(NULL));
    jw_input_shortcuts bad;
    jw_input_shortcuts_defaults(&bad);
    bad.buttons[JW_INPUT_SHORTCUT_RECORDING] =
        bad.buttons[JW_INPUT_SHORTCUT_GAME_SWITCHER];
    assert(!jw_input_shortcuts_valid(&bad));
    jw_input_shortcuts_defaults(&bad);
    bad.buttons[JW_INPUT_SHORTCUT_SCREENSHOT] = JW_INPUT_SHORTCUT_BUTTON_COUNT;
    assert(!jw_input_shortcuts_valid(&bad));
    jw_input_shortcuts_defaults(&bad);
    bad.buttons[JW_INPUT_SHORTCUT_SCREENSHOT] = (jw_input_shortcut_button)-1;
    assert(!jw_input_shortcuts_valid(&bad));
    /* Several disabled actions are valid, not duplicates. */
    jw_input_shortcuts_defaults(&bad);
    bad.buttons[JW_INPUT_SHORTCUT_SCREENSHOT] = JW_INPUT_SHORTCUT_BUTTON_NONE;
    bad.buttons[JW_INPUT_SHORTCUT_RECORDING] = JW_INPUT_SHORTCUT_BUTTON_NONE;
    assert(jw_input_shortcuts_valid(&bad));

    puts("input shortcut tests passed");
    return 0;
}
