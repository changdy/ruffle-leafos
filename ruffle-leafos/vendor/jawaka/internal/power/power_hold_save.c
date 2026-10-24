#include "internal/power/power_hold_save.h"

#include <string.h>

#define JW_PHS_MIB (1024ull * 1024ull)

void jw_power_hold_save_init(jw_power_hold_save *s) {
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

static jw_power_hold_save_decision jw__phs_latch(jw_power_hold_save *s,
                                                 jw_power_hold_save_outcome outcome) {
    s->phase = JW_POWER_HOLD_SAVE_DONE;
    s->outcome = outcome;
    return JW_POWER_HOLD_SAVE_SHUT_DOWN;
}

long long jw_power_hold_save_cut_ms_for_hold_s(int hold_s) {
    if (hold_s <= 0) {
        return JW_POWER_HOLD_SAVE_STOCK_CUT_MS;
    }
    return (long long)hold_s * 1000 - JW_POWER_HOLD_SAVE_CUT_EARLY_MS;
}

long long jw_power_hold_save_release_offset_ms(long long cut_ms) {
    return cut_ms - JW_POWER_HOLD_SAVE_TEARDOWN_RESERVE_MS - JW_POWER_HOLD_SAVE_MARGIN_MS;
}

jw_power_hold_save_decision jw_power_hold_save_long_press(jw_power_hold_save *s,
                                                         long long press_ms,
                                                         long long now_ms,
                                                         long long cut_ms,
                                                         bool eligible,
                                                         long long last_resume_ms) {
    if (!s) {
        return JW_POWER_HOLD_SAVE_SHUT_DOWN;
    }
    if (s->phase == JW_POWER_HOLD_SAVE_DONE) {
        return JW_POWER_HOLD_SAVE_SHUT_DOWN;
    }
    if (s->phase != JW_POWER_HOLD_SAVE_IDLE) {
        return JW_POWER_HOLD_SAVE_WAIT;  /* duplicate hold tick */
    }
    s->press_ms = press_ms;
    if (!eligible) {
        return jw__phs_latch(s, JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED);
    }
    if (press_ms <= 0 || (last_resume_ms > 0 && press_ms < last_resume_ms)) {
        return jw__phs_latch(s, JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED);
    }
    s->cut_ms = cut_ms > 0 ? cut_ms : JW_POWER_HOLD_SAVE_STOCK_CUT_MS;
    s->release_offset_ms = jw_power_hold_save_release_offset_ms(s->cut_ms);
    s->release_boundary_ms = press_ms + s->release_offset_ms;
    if (s->release_offset_ms <= 0 || now_ms >= s->release_boundary_ms) {
        /* With this cut the teardown reserve already eats the time a release
           could be waited for (the stock 6 s cut leaves 1.1 s, inside the
           2.0 s long press): decline before showing a prompt. */
        return jw__phs_latch(s, JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);
    }
    /* The key is down since press_ms. When the release edge was already
       queued (a released hold recognized late), the caller feeds it through
       key_edge() right after this call, with its own timestamp. */
    s->key_down = true;
    s->phase = JW_POWER_HOLD_SAVE_AWAIT_RELEASE;
    return JW_POWER_HOLD_SAVE_WAIT;
}

void jw_power_hold_save_key_edge(jw_power_hold_save *s, bool down, long long edge_ms) {
    if (!s) {
        return;
    }
    if (down) {
        s->key_down = true;
        if (s->phase == JW_POWER_HOLD_SAVE_RELEASED ||
            s->phase == JW_POWER_HOLD_SAVE_SAVING) {
            s->repress_ms = edge_ms;
        }
        return;
    }
    s->key_down = false;
    if (s->phase == JW_POWER_HOLD_SAVE_AWAIT_RELEASE) {
        if (edge_ms < s->press_ms) {
            /* Out-of-order timestamps are not timing evidence. */
            (void)jw__phs_latch(s, JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED);
            return;
        }
        if (edge_ms > s->release_boundary_ms) {
            (void)jw__phs_latch(s, JW_POWER_HOLD_SAVE_OUTCOME_STILL_HELD);
            return;
        }
        s->release_ms = edge_ms;
        s->window_deadline_ms = edge_ms + JW_POWER_HOLD_SAVE_WINDOW_MS;
        s->phase = JW_POWER_HOLD_SAVE_RELEASED;
        return;
    }
    /* A release after a re-press removes that hold's cap; the window stays. */
    s->repress_ms = 0;
}

long long jw_power_hold_save_effective_deadline_ms(const jw_power_hold_save *s) {
    if (!s || (s->phase != JW_POWER_HOLD_SAVE_RELEASED &&
               s->phase != JW_POWER_HOLD_SAVE_SAVING)) {
        return 0;
    }
    long long deadline = s->window_deadline_ms;
    if (s->key_down && s->repress_ms > 0) {
        long long held_cap = s->repress_ms + s->release_offset_ms;
        if (held_cap < deadline) {
            deadline = held_cap;
        }
    }
    return deadline;
}

jw_power_hold_save_decision jw_power_hold_save_tick(jw_power_hold_save *s,
                                                   long long now_ms) {
    if (!s) {
        return JW_POWER_HOLD_SAVE_SHUT_DOWN;
    }
    switch (s->phase) {
    case JW_POWER_HOLD_SAVE_IDLE:
    case JW_POWER_HOLD_SAVE_DONE:
        return JW_POWER_HOLD_SAVE_SHUT_DOWN;
    case JW_POWER_HOLD_SAVE_AWAIT_RELEASE:
        if (now_ms >= s->release_boundary_ms) {
            return jw__phs_latch(s, JW_POWER_HOLD_SAVE_OUTCOME_STILL_HELD);
        }
        return JW_POWER_HOLD_SAVE_WAIT;
    case JW_POWER_HOLD_SAVE_RELEASED:
        if (now_ms >= jw_power_hold_save_effective_deadline_ms(s)) {
            return jw__phs_latch(s, JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);
        }
        return JW_POWER_HOLD_SAVE_ADMIT_NOW;
    case JW_POWER_HOLD_SAVE_SAVING:
        if (now_ms >= jw_power_hold_save_effective_deadline_ms(s)) {
            return jw__phs_latch(s, JW_POWER_HOLD_SAVE_OUTCOME_TIMEOUT);
        }
        return JW_POWER_HOLD_SAVE_WAIT;
    }
    return JW_POWER_HOLD_SAVE_SHUT_DOWN;
}

long long jw_power_hold_save_estimate_ms(const jw_power_hold_save_request *req) {
    if (!req || req->state_bytes == 0) {
        return -1;
    }
    /* Cap well above any libretro state so the multiply cannot overflow. */
    if (req->state_bytes > 1024ull * JW_PHS_MIB) {
        return -1;
    }
    long long mib = (long long)((req->state_bytes + JW_PHS_MIB - 1) / JW_PHS_MIB);
    return JW_POWER_HOLD_SAVE_SERIALIZE_MS + mib * JW_POWER_HOLD_SAVE_FLUSH_MS_PER_MIB +
           JW_POWER_HOLD_SAVE_PUBLISH_MS;
}

bool jw_power_hold_save_admit(jw_power_hold_save *s,
                              const jw_power_hold_save_request *req,
                              long long now_ms, long long *deadline_ms) {
    if (deadline_ms) {
        *deadline_ms = 0;
    }
    if (!s || s->phase != JW_POWER_HOLD_SAVE_RELEASED) {
        return false;
    }
    long long deadline = jw_power_hold_save_effective_deadline_ms(s);
    long long estimate = jw_power_hold_save_estimate_ms(req);
    if (estimate < 0 || now_ms + estimate > deadline) {
        (void)jw__phs_latch(s, JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT);
        return false;
    }
    s->phase = JW_POWER_HOLD_SAVE_SAVING;
    s->admitted_deadline_ms = deadline;
    if (deadline_ms) {
        *deadline_ms = deadline;
    }
    return true;
}

bool jw_power_hold_save_active(const jw_power_hold_save *s) {
    return s && (s->phase == JW_POWER_HOLD_SAVE_AWAIT_RELEASE ||
                 s->phase == JW_POWER_HOLD_SAVE_RELEASED ||
                 s->phase == JW_POWER_HOLD_SAVE_SAVING);
}

void jw_power_hold_save_abort(jw_power_hold_save *s,
                              jw_power_hold_save_outcome outcome) {
    if (!jw_power_hold_save_active(s)) {
        return;
    }
    if (outcome == JW_POWER_HOLD_SAVE_OUTCOME_NONE ||
        outcome == JW_POWER_HOLD_SAVE_OUTCOME_SAVED) {
        outcome = JW_POWER_HOLD_SAVE_OUTCOME_INTERRUPTED;
    }
    (void)jw__phs_latch(s, outcome);
}

void jw_power_hold_save_finish(jw_power_hold_save *s,
                               jw_power_hold_save_outcome outcome) {
    if (!s || s->phase != JW_POWER_HOLD_SAVE_SAVING) {
        return;
    }
    if (outcome == JW_POWER_HOLD_SAVE_OUTCOME_NONE) {
        outcome = JW_POWER_HOLD_SAVE_OUTCOME_STORAGE_ERROR;
    }
    (void)jw__phs_latch(s, outcome);
}

const char *jw_power_hold_save_outcome_name(jw_power_hold_save_outcome outcome) {
    switch (outcome) {
    case JW_POWER_HOLD_SAVE_OUTCOME_NONE:                 return "none";
    case JW_POWER_HOLD_SAVE_OUTCOME_SAVED:                return "saved";
    case JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED:          return "unsupported";
    case JW_POWER_HOLD_SAVE_OUTCOME_STILL_HELD:           return "still-held";
    case JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT:  return "budget-insufficient";
    case JW_POWER_HOLD_SAVE_OUTCOME_STORAGE_ERROR:        return "storage-error";
    case JW_POWER_HOLD_SAVE_OUTCOME_TIMEOUT:              return "timeout";
    case JW_POWER_HOLD_SAVE_OUTCOME_CHILD_EXITED:         return "child-exited";
    case JW_POWER_HOLD_SAVE_OUTCOME_DURABILITY_UNCERTAIN: return "durability-uncertain";
    case JW_POWER_HOLD_SAVE_OUTCOME_INTERRUPTED:          return "interrupted";
    }
    return "none";
}
