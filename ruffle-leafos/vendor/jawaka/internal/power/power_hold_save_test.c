#include "internal/power/power_hold_save.h"

#include <assert.h>
#include <string.h>

#define MIB (1024ull * 1024ull)
#define PRESS 100000LL
#define LONG_PRESS (PRESS + 2000)
/* Force Off Hold 10 s: cut 9700, release offset 9700 - 4100 - 500. */
#define CUT jw_power_hold_save_cut_ms_for_hold_s(10)
#define OFFSET jw_power_hold_save_release_offset_ms(CUT)
#define BOUNDARY (PRESS + OFFSET)

static const jw_power_hold_save_request ps_state = { 4456472ull };
static const jw_power_hold_save_request saturn_state = { 9068872ull };

static void armed(jw_power_hold_save *s) {
    jw_power_hold_save_init(s);
    assert(jw_power_hold_save_long_press(s, PRESS, LONG_PRESS, CUT, true, 0) == JW_POWER_HOLD_SAVE_WAIT);
    assert(s->phase == JW_POWER_HOLD_SAVE_AWAIT_RELEASE && s->key_down);
}

static void released(jw_power_hold_save *s, long long release_ms) {
    armed(s);
    jw_power_hold_save_key_edge(s, false, release_ms);
    assert(s->phase == JW_POWER_HOLD_SAVE_RELEASED);
    assert(jw_power_hold_save_effective_deadline_ms(s) ==
           release_ms + JW_POWER_HOLD_SAVE_WINDOW_MS);
}

static void test_constants(void) {
    /* Cuts land up to 0.3 s before the nominal hold; unknown means stock. */
    assert(jw_power_hold_save_cut_ms_for_hold_s(10) == 9700);
    assert(jw_power_hold_save_cut_ms_for_hold_s(12) == 11700);
    assert(jw_power_hold_save_cut_ms_for_hold_s(6) == 5700);
    assert(jw_power_hold_save_cut_ms_for_hold_s(0) == JW_POWER_HOLD_SAVE_STOCK_CUT_MS);
    assert(jw_power_hold_save_cut_ms_for_hold_s(-1) == JW_POWER_HOLD_SAVE_STOCK_CUT_MS);
    /* 9.7 s cut - 4.1 s teardown - 0.5 s margin; the stock cut leaves 1.1 s. */
    assert(OFFSET == 5100);
    assert(jw_power_hold_save_release_offset_ms(JW_POWER_HOLD_SAVE_STOCK_CUT_MS) == 1100);
    assert(strcmp(jw_power_hold_save_outcome_name(
                      JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT),
                  "budget-insufficient") == 0);
    assert(strcmp(jw_power_hold_save_outcome_name(
                      JW_POWER_HOLD_SAVE_OUTCOME_DURABILITY_UNCERTAIN),
                  "durability-uncertain") == 0);
    assert(strcmp(jw_power_hold_save_outcome_name(JW_POWER_HOLD_SAVE_OUTCOME_STILL_HELD),
                  "still-held") == 0);
}

static void test_ineligible_and_suspend(void) {
    jw_power_hold_save s;
    jw_power_hold_save_init(&s);
    assert(jw_power_hold_save_tick(&s, PRESS) == JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(jw_power_hold_save_long_press(&s, PRESS, LONG_PRESS, CUT, false, 0) ==
           JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED);
    /* A later release cannot revive a latched attempt. */
    jw_power_hold_save_key_edge(&s, false, PRESS + 2500);
    assert(s.phase == JW_POWER_HOLD_SAVE_DONE);

    /* Key-down before the last resume spans suspend: not timing evidence. */
    jw_power_hold_save_init(&s);
    assert(jw_power_hold_save_long_press(&s, PRESS, LONG_PRESS, CUT, true, PRESS + 1) ==
           JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED);
    jw_power_hold_save_init(&s);
    assert(jw_power_hold_save_long_press(&s, PRESS, LONG_PRESS, CUT, true, PRESS - 1) ==
           JW_POWER_HOLD_SAVE_WAIT);
}

static void test_cut_decides_the_wait(void) {
    jw_power_hold_save s;
    /* Stock 6 s cut: the boundary (press + 1.1 s) is already behind the 2.0 s
       long press, so the attempt declines at once and no prompt is shown. */
    jw_power_hold_save_init(&s);
    assert(jw_power_hold_save_long_press(&s, PRESS, LONG_PRESS,
                                         JW_POWER_HOLD_SAVE_STOCK_CUT_MS, true, 0) ==
           JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);
    /* An unknown cut (0) is treated as stock. */
    jw_power_hold_save_init(&s);
    assert(jw_power_hold_save_long_press(&s, PRESS, LONG_PRESS, 0, true, 0) ==
           JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.cut_ms == JW_POWER_HOLD_SAVE_STOCK_CUT_MS);
    /* 8 s hold: 7.7 s cut, boundary at press + 3.1 s, 1.1 s of wait. */
    jw_power_hold_save_init(&s);
    long long cut8 = jw_power_hold_save_cut_ms_for_hold_s(8);
    assert(jw_power_hold_save_long_press(&s, PRESS, LONG_PRESS, cut8, true, 0) ==
           JW_POWER_HOLD_SAVE_WAIT);
    assert(s.release_boundary_ms == PRESS + 3100);
    assert(jw_power_hold_save_tick(&s, PRESS + 3099) == JW_POWER_HOLD_SAVE_WAIT);
    assert(jw_power_hold_save_tick(&s, PRESS + 3100) == JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_STILL_HELD);
    /* Recognized late, after the boundary: no prompt either. */
    jw_power_hold_save_init(&s);
    assert(jw_power_hold_save_long_press(&s, PRESS, PRESS + 3100, cut8, true, 0) ==
           JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);
}

static void test_never_saves_while_held(void) {
    jw_power_hold_save s;
    armed(&s);
    /* Duplicate hold ticks keep the first attempt. */
    assert(jw_power_hold_save_long_press(&s, PRESS + 50, LONG_PRESS + 50, CUT, true, 0) ==
           JW_POWER_HOLD_SAVE_WAIT);
    assert(s.press_ms == PRESS);
    assert(jw_power_hold_save_tick(&s, LONG_PRESS) == JW_POWER_HOLD_SAVE_WAIT);
    assert(!jw_power_hold_save_admit(&s, &ps_state, LONG_PRESS, NULL));
    assert(s.phase == JW_POWER_HOLD_SAVE_AWAIT_RELEASE);
    assert(jw_power_hold_save_tick(&s, BOUNDARY - 1) == JW_POWER_HOLD_SAVE_WAIT);
    assert(jw_power_hold_save_tick(&s, BOUNDARY) == JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_STILL_HELD);
}

static void test_release_timestamps(void) {
    jw_power_hold_save s;
    /* Release exactly at the boundary still qualifies. */
    released(&s, BOUNDARY);

    /* A release queued behind a stalled tick uses its own timestamp: the edge
       says 2.6 s even though it is processed after the boundary. */
    armed(&s);
    jw_power_hold_save_key_edge(&s, false, PRESS + 2600);
    assert(s.phase == JW_POWER_HOLD_SAVE_RELEASED);
    assert(jw_power_hold_save_tick(&s, BOUNDARY + 400) == JW_POWER_HOLD_SAVE_ADMIT_NOW);
    assert(jw_power_hold_save_effective_deadline_ms(&s) ==
           PRESS + 2600 + JW_POWER_HOLD_SAVE_WINDOW_MS);

    /* A release stamped after the boundary is too late. */
    armed(&s);
    jw_power_hold_save_key_edge(&s, false, BOUNDARY + 1);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_STILL_HELD);

    /* Out-of-order stamps are not trusted. */
    armed(&s);
    jw_power_hold_save_key_edge(&s, false, PRESS - 1);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED);
}

static void test_estimates(void) {
    /* 1000 + 5 MiB * 70 + 150: one allowance for every core that can save. */
    assert(jw_power_hold_save_estimate_ms(&ps_state) == 1500);
    /* 1000 + 9 MiB * 70 + 150 */
    assert(jw_power_hold_save_estimate_ms(&saturn_state) == 1780);
    jw_power_hold_save_request exact = { 2 * MIB };
    assert(jw_power_hold_save_estimate_ms(&exact) == 1000 + 2 * 70 + 150);

    jw_power_hold_save_request unknown_size = { 0 };
    jw_power_hold_save_request huge = { 2048ull * MIB };
    assert(jw_power_hold_save_estimate_ms(&unknown_size) == -1);
    assert(jw_power_hold_save_estimate_ms(&huge) == -1);
    assert(jw_power_hold_save_estimate_ms(NULL) == -1);
}

static void test_admission(void) {
    jw_power_hold_save s;
    long long deadline = -1;
    long long release = PRESS + 2700;

    released(&s, release);
    assert(jw_power_hold_save_admit(&s, &saturn_state, release + 100, &deadline));
    assert(s.phase == JW_POWER_HOLD_SAVE_SAVING);
    assert(deadline == release + JW_POWER_HOLD_SAVE_WINDOW_MS);
    assert(s.admitted_deadline_ms == deadline);
    /* Admission happens once. */
    assert(!jw_power_hold_save_admit(&s, &saturn_state, release + 100, NULL));
    assert(s.phase == JW_POWER_HOLD_SAVE_SAVING);
    assert(jw_power_hold_save_tick(&s, deadline - 1) == JW_POWER_HOLD_SAVE_WAIT);
    jw_power_hold_save_finish(&s, JW_POWER_HOLD_SAVE_OUTCOME_SAVED);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_SAVED);
    assert(jw_power_hold_save_tick(&s, deadline - 1) == JW_POWER_HOLD_SAVE_SHUT_DOWN);
    /* A late finish cannot rewrite the latched outcome. */
    jw_power_hold_save_finish(&s, JW_POWER_HOLD_SAVE_OUTCOME_STORAGE_ERROR);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_SAVED);

    /* Fits exactly at the bound. */
    released(&s, release);
    deadline = release + JW_POWER_HOLD_SAVE_WINDOW_MS;
    assert(jw_power_hold_save_admit(&s, &ps_state, deadline - 1500, NULL));

    /* One millisecond short fails closed without sending anything. */
    released(&s, release);
    assert(!jw_power_hold_save_admit(&s, &ps_state, deadline - 1499, &deadline));
    assert(deadline == 0);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);
    assert(jw_power_hold_save_tick(&s, release + 1) == JW_POWER_HOLD_SAVE_SHUT_DOWN);

    /* Unknown size (no probe reply) fails closed. */
    jw_power_hold_save_request unknown = { 0 };
    released(&s, release);
    assert(!jw_power_hold_save_admit(&s, &unknown, release, NULL));
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);

    /* Never admitted before the window closed. */
    released(&s, release);
    assert(jw_power_hold_save_tick(&s, release + JW_POWER_HOLD_SAVE_WINDOW_MS) ==
           JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);

    /* finish() without an outcome is a storage error, not a success. */
    released(&s, release);
    assert(jw_power_hold_save_admit(&s, &ps_state, release, NULL));
    jw_power_hold_save_finish(&s, JW_POWER_HOLD_SAVE_OUTCOME_NONE);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_STORAGE_ERROR);
}

static void test_timeout(void) {
    jw_power_hold_save s;
    long long release = PRESS + 2500;
    released(&s, release);
    assert(jw_power_hold_save_admit(&s, &ps_state, release, NULL));
    assert(jw_power_hold_save_tick(&s, release + JW_POWER_HOLD_SAVE_WINDOW_MS) ==
           JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_TIMEOUT);
    /* A reply that lands after the timeout cannot turn into "saved". */
    jw_power_hold_save_finish(&s, JW_POWER_HOLD_SAVE_OUTCOME_SAVED);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_TIMEOUT);
}

static void test_repress_caps_the_window(void) {
    jw_power_hold_save s;
    long long release = PRESS + 2500;
    long long window = release + JW_POWER_HOLD_SAVE_WINDOW_MS;
    released(&s, release);
    assert(jw_power_hold_save_admit(&s, &saturn_state, release + 50, NULL));

    /* Re-press 1 s after release: fresh PMIC timer, cap at repress + offset. */
    long long repress = release + 1000;
    jw_power_hold_save_key_edge(&s, true, repress);
    assert(jw_power_hold_save_effective_deadline_ms(&s) ==
           repress + OFFSET);

    /* Releasing again removes that cap but never extends past the window. */
    jw_power_hold_save_key_edge(&s, false, repress + 400);
    assert(jw_power_hold_save_effective_deadline_ms(&s) == window);

    /* Repeated presses cannot push waiting past the window either. */
    for (int i = 0; i < 20; i++) {
        long long t = repress + 1000 + i * 400;
        jw_power_hold_save_key_edge(&s, true, t);
        assert(jw_power_hold_save_effective_deadline_ms(&s) <= window);
        jw_power_hold_save_key_edge(&s, false, t + 100);
        assert(jw_power_hold_save_effective_deadline_ms(&s) == window);
    }

    /* A re-press late in the window keeps the window as the tighter bound. */
    jw_power_hold_save_key_edge(&s, true, window - 1000);
    assert(jw_power_hold_save_effective_deadline_ms(&s) == window);

    /* Held re-press that runs out its cap times out the save. */
    released(&s, release);
    assert(jw_power_hold_save_admit(&s, &ps_state, release, NULL));
    jw_power_hold_save_key_edge(&s, true, release + 200);
    long long cap = release + 200 + OFFSET;
    assert(jw_power_hold_save_tick(&s, cap - 1) == JW_POWER_HOLD_SAVE_WAIT);
    assert(jw_power_hold_save_tick(&s, cap) == JW_POWER_HOLD_SAVE_SHUT_DOWN);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_TIMEOUT);

    /* A re-press before admission shrinks the budget admission sees. */
    released(&s, release);
    jw_power_hold_save_key_edge(&s, true, release + 100);
    long long held_cap = release + 100 + OFFSET;
    assert(!jw_power_hold_save_admit(&s, &saturn_state, held_cap - 1779, NULL));
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);
}

static void test_abort(void) {
    jw_power_hold_save s;
    jw_power_hold_save_init(&s);
    assert(!jw_power_hold_save_active(&s));
    jw_power_hold_save_abort(&s, JW_POWER_HOLD_SAVE_OUTCOME_CHILD_EXITED);
    assert(s.phase == JW_POWER_HOLD_SAVE_IDLE);

    armed(&s);
    assert(jw_power_hold_save_active(&s));
    jw_power_hold_save_abort(&s, JW_POWER_HOLD_SAVE_OUTCOME_CHILD_EXITED);
    assert(!jw_power_hold_save_active(&s));
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_CHILD_EXITED);
    /* A second abort does not rewrite the first outcome. */
    jw_power_hold_save_abort(&s, JW_POWER_HOLD_SAVE_OUTCOME_INTERRUPTED);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_CHILD_EXITED);

    released(&s, PRESS + 2500);
    assert(jw_power_hold_save_active(&s));
    /* Abort can never claim success. */
    jw_power_hold_save_abort(&s, JW_POWER_HOLD_SAVE_OUTCOME_SAVED);
    assert(s.outcome == JW_POWER_HOLD_SAVE_OUTCOME_INTERRUPTED);
    assert(strcmp(jw_power_hold_save_outcome_name(s.outcome), "interrupted") == 0);

    released(&s, PRESS + 2500);
    assert(jw_power_hold_save_admit(&s, &ps_state, PRESS + 2500, NULL));
    assert(jw_power_hold_save_active(&s));
    jw_power_hold_save_abort(&s, JW_POWER_HOLD_SAVE_OUTCOME_INTERRUPTED);
    assert(jw_power_hold_save_tick(&s, PRESS + 2600) == JW_POWER_HOLD_SAVE_SHUT_DOWN);
}

int main(void) {
    test_constants();
    test_ineligible_and_suspend();
    test_cut_decides_the_wait();
    test_never_saves_while_held();
    test_release_timestamps();
    test_estimates();
    test_admission();
    test_timeout();
    test_repress_caps_the_window();
    test_abort();
    return 0;
}
