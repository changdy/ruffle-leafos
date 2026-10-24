#ifndef JW_POWER_HOLD_SAVE_H
#define JW_POWER_HOLD_SAVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Save-before-power-off policy for a physical power-key long press.

   Saves run only after the player releases the key: releasing resets the
   RK817 long-press timer, and while held there is too little time left before
   the PMIC hard cut to save and still finish the clean shutdown. All times are
   CLOCK_MONOTONIC milliseconds; key times are the input events' own kernel
   timestamps, never the time an edge was processed.

   Pure logic: no I/O, clocks, or IPC. jawakad feeds it edges and ticks and
   acts on the returned decisions. Constants come from the MLP1 Step 0
   measurements (umrk-workspace plans/Jawaka/save-state-on-power-hold.md). */

/* PMIC hard cut when nothing has applied a Force Off Hold: U-Boot's 6 s,
   measured 5.7-6.0 s after key-down. */
#define JW_POWER_HOLD_SAVE_STOCK_CUT_MS         5700
/* The measured cut came up to 0.3 s before the nominal hold (6 s: 5.7-6.0,
   10 s: 9.92-9.99, 12 s: 11.89-11.96; 2026-09-16 and 2026-10-03). */
#define JW_POWER_HOLD_SAVE_CUT_EARLY_MS         300
/* Shutdown request to kernel power-off entry on the supervisor path, worst of
   four runs on 2026-09-29 (3.67-4.08 s). */
#define JW_POWER_HOLD_SAVE_TEARDOWN_RESERVE_MS  4100
/* Kernel power-off entry to the physical cut is unmeasured. */
#define JW_POWER_HOLD_SAVE_MARGIN_MS            500
/* Software cap on the whole save once released. */
#define JW_POWER_HOLD_SAVE_WINDOW_MS            8000
/* One serialization allowance for every core RetroArch can sync-save. The
   synchronous path (serialize + write + fsync) measured 386-432 ms for a
   4.3 MiB PS state and 915-966 ms for an 8.6 MiB Saturn state in full, so
   1 s on top of the per-MiB flush covers the slowest measured core with room.
   The release window is 8 s and RetroArch refuses a save past its start-by
   deadline, so a core that is slower than this fails safe: the old quicksave
   stays. */
#define JW_POWER_HOLD_SAVE_SERIALIZE_MS         1000
/* Conservative flush rate for both MLP1 cards (measured worst 51 and 67). */
#define JW_POWER_HOLD_SAVE_FLUSH_MS_PER_MIB     70
/* rename (<= 70 ms), directory fsync (<= 25 ms), thumbnail removal, slack. */
#define JW_POWER_HOLD_SAVE_PUBLISH_MS           150
/* After a saved outcome RetroArch gets QUIT so its own exit path flushes SRAM
   and config. Measured clean exits on the MLP1 (2026-10-03): PCSX-ReARMed and
   SwanStation 315-571 ms, yabasanshiro ~3.2 s. The key is released here, so
   the PMIC imposes no deadline; a re-press ends the grace at once. */
#define JW_POWER_HOLD_SAVE_QUIT_GRACE_MS        4000

typedef enum {
    JW_POWER_HOLD_SAVE_IDLE = 0,
    JW_POWER_HOLD_SAVE_AWAIT_RELEASE,  /* prompt shown; key still down */
    JW_POWER_HOLD_SAVE_RELEASED,       /* window open; admission pending */
    JW_POWER_HOLD_SAVE_SAVING,         /* request admitted and in flight */
    JW_POWER_HOLD_SAVE_DONE            /* outcome latched; shut down */
} jw_power_hold_save_phase;

typedef enum {
    JW_POWER_HOLD_SAVE_OUTCOME_NONE = 0,
    JW_POWER_HOLD_SAVE_OUTCOME_SAVED,
    JW_POWER_HOLD_SAVE_OUTCOME_UNSUPPORTED,
    JW_POWER_HOLD_SAVE_OUTCOME_STILL_HELD,
    JW_POWER_HOLD_SAVE_OUTCOME_BUDGET_INSUFFICIENT,
    JW_POWER_HOLD_SAVE_OUTCOME_STORAGE_ERROR,
    JW_POWER_HOLD_SAVE_OUTCOME_TIMEOUT,
    JW_POWER_HOLD_SAVE_OUTCOME_CHILD_EXITED,
    JW_POWER_HOLD_SAVE_OUTCOME_DURABILITY_UNCERTAIN,
    JW_POWER_HOLD_SAVE_OUTCOME_INTERRUPTED   /* shutdown signal overrode the save */
} jw_power_hold_save_outcome;

typedef enum {
    JW_POWER_HOLD_SAVE_WAIT = 0,      /* keep ticking; defer child teardown */
    JW_POWER_HOLD_SAVE_ADMIT_NOW,     /* released: estimate and call admit() */
    JW_POWER_HOLD_SAVE_SHUT_DOWN      /* outcome latched; continue shutdown */
} jw_power_hold_save_decision;

typedef struct {
    jw_power_hold_save_phase phase;
    jw_power_hold_save_outcome outcome;
    long long press_ms;          /* key-down that armed the attempt */
    long long cut_ms;            /* PMIC cut planned against, from key-down */
    long long release_offset_ms; /* cut - teardown reserve - margin */
    long long release_boundary_ms; /* press_ms + release_offset_ms */
    long long release_ms;        /* first qualifying release */
    long long window_deadline_ms;
    bool key_down;               /* physical key state from edges */
    long long repress_ms;        /* latest key-down after release, if down */
    long long admitted_deadline_ms;
} jw_power_hold_save;

typedef struct {
    uint64_t state_bytes; /* serialize size plus format overhead; 0 = unknown */
} jw_power_hold_save_request;

void jw_power_hold_save_init(jw_power_hold_save *s);

/* PMIC cut to plan against for a Force Off Hold of `hold_s` seconds (the
   daemon's applied value); 0 or less means unknown or not applied and yields
   the stock cut. */
long long jw_power_hold_save_cut_ms_for_hold_s(int hold_s);

/* How long after key-down a release must land for a save to open, given the
   cut: cut - teardown reserve - margin. At or below the long press (2.0 s) no
   release wait fits. */
long long jw_power_hold_save_release_offset_ms(long long cut_ms);

/* The long press was recognized at `now_ms`. `cut_ms` is the PMIC cut planned
   against (see jw_power_hold_save_cut_ms_for_hold_s). `eligible` covers the
   setting, a Leaf-managed RetroArch game with state support, a safe namespace,
   writable storage, and the synchronous-save capability. `last_resume_ms` is
   the monotonic time of the latest resume from suspend (0 if none): a key-down
   before it spans suspend and is not timing evidence. Returns WAIT to show the
   release prompt, or SHUT_DOWN when no attempt starts, including when the
   release boundary has already passed at `now_ms` (budget-insufficient).
   Duplicate calls keep the first attempt. */
jw_power_hold_save_decision jw_power_hold_save_long_press(jw_power_hold_save *s,
                                                         long long press_ms,
                                                         long long now_ms,
                                                         long long cut_ms,
                                                         bool eligible,
                                                         long long last_resume_ms);

/* Feed every power-key edge with its event timestamp, in queue order. For a
   released hold recognized late, call long_press() first, then feed the
   queued release. */
void jw_power_hold_save_key_edge(jw_power_hold_save *s, bool down, long long edge_ms);

/* Advance on each daemon tick. */
jw_power_hold_save_decision jw_power_hold_save_tick(jw_power_hold_save *s,
                                                   long long now_ms);

/* Conservative duration for the request, or -1 when it cannot be estimated. */
long long jw_power_hold_save_estimate_ms(const jw_power_hold_save_request *req);

/* Admit or reject the save once released. On admission the phase becomes
   SAVING and `*deadline_ms` receives the absolute bound RetroArch must recheck
   before opening its temporary output. Returns true when admitted; otherwise
   the outcome is BUDGET_INSUFFICIENT and the next tick shuts down. */
bool jw_power_hold_save_admit(jw_power_hold_save *s,
                              const jw_power_hold_save_request *req,
                              long long now_ms, long long *deadline_ms);

/* Current bound on waiting, honoring a re-press; 0 when no window is open. */
long long jw_power_hold_save_effective_deadline_ms(const jw_power_hold_save *s);

/* True while an attempt defers shutdown teardown. */
bool jw_power_hold_save_active(const jw_power_hold_save *s);

/* End any active attempt with `outcome` (child exit, shutdown signal).
   Ignored when nothing is active. */
void jw_power_hold_save_abort(jw_power_hold_save *s,
                              jw_power_hold_save_outcome outcome);

/* Report how the admitted save ended. Ignored unless SAVING. */
void jw_power_hold_save_finish(jw_power_hold_save *s,
                               jw_power_hold_save_outcome outcome);

/* Log token for an outcome, e.g. "budget-insufficient". */
const char *jw_power_hold_save_outcome_name(jw_power_hold_save_outcome outcome);

#endif
