#include "cmd/jawaka-osd/osd_view.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>

static void expect_stage(const jw_osd_view *view, jw_osd_game_stage stage, int pending) {
    assert(view->kind == JW_OSD_VIEW_STAGE);
    assert(view->stage == stage);
    assert(view->pending_items == pending);
}

/* Syncthing progress survives a volume adjustment and comes back when the
   toast ends, with the latest count reported while the toast was up. */
static void progress_returns_after_level(void) {
    jw_osd_view view;
    jw_osd_view_reset(&view);
    assert(jw_osd_view_stage(&view, JW_OSD_GAME_SYNCING, 3, 0) == JW_OSD_VIEW_DRAW);
    assert(view.hide_at == UINT64_MAX);
    assert(jw_osd_view_tick(&view, 60000) == JW_OSD_VIEW_KEEP);

    assert(jw_osd_view_level(&view, JW_OSD_VIEW_VOLUME, 40, 1000) == JW_OSD_VIEW_DRAW);
    assert(view.kind == JW_OSD_VIEW_VOLUME && view.percent == 40);
    assert(view.has_retained && view.retained_pending_items == 3);

    /* Updates during the toast only move what comes back. */
    assert(jw_osd_view_stage(&view, JW_OSD_GAME_SYNCING, 2, 1100) == JW_OSD_VIEW_KEEP);
    assert(view.kind == JW_OSD_VIEW_VOLUME && view.hide_at == 1000 + JW_OSD_LEVEL_MS);

    /* Repeated adjustments extend the toast and keep the retained stage. */
    assert(jw_osd_view_level(&view, JW_OSD_VIEW_VOLUME, 45, 1500) == JW_OSD_VIEW_DRAW);
    assert(jw_osd_view_level(&view, JW_OSD_VIEW_BRIGHTNESS, 70, 1900) == JW_OSD_VIEW_DRAW);
    assert(jw_osd_view_stage(&view, JW_OSD_GAME_SYNCING, 1, 2000) == JW_OSD_VIEW_KEEP);
    assert(jw_osd_view_tick(&view, 1900 + JW_OSD_LEVEL_MS - 1) == JW_OSD_VIEW_KEEP);

    assert(jw_osd_view_tick(&view, 1900 + JW_OSD_LEVEL_MS) == JW_OSD_VIEW_DRAW);
    expect_stage(&view, JW_OSD_GAME_SYNCING, 1);
    assert(!view.has_retained && view.hide_at == UINT64_MAX);
    assert(jw_osd_view_tick(&view, 999999) == JW_OSD_VIEW_KEEP);
}

/* A stage that starts while a toast is up waits for the toast too. */
static void progress_started_under_level(void) {
    jw_osd_view view;
    jw_osd_view_reset(&view);
    assert(jw_osd_view_level(&view, JW_OSD_VIEW_BRIGHTNESS, 10, 0) == JW_OSD_VIEW_DRAW);
    assert(jw_osd_view_stage(&view, JW_OSD_PICO8_IMPORT, 0, 100) == JW_OSD_VIEW_KEEP);
    assert(jw_osd_view_tick(&view, JW_OSD_LEVEL_MS) == JW_OSD_VIEW_DRAW);
    expect_stage(&view, JW_OSD_PICO8_IMPORT, 0);
}

/* Completion or cancellation while the toast is visible: nothing returns. */
static void hide_under_level_prevents_restore(void) {
    jw_osd_view view;
    jw_osd_view_reset(&view);
    jw_osd_view_stage(&view, JW_OSD_GAME_CHECKING, 0, 0);
    jw_osd_view_level(&view, JW_OSD_VIEW_VOLUME, 50, 10);
    assert(jw_osd_view_hide_stage(&view) == JW_OSD_VIEW_KEEP);
    assert(view.kind == JW_OSD_VIEW_VOLUME && !view.has_retained);
    /* A late progress update after the hide is a new producer post, not a
       restoration; the level toast still wins until it ends. */
    assert(jw_osd_view_tick(&view, 10 + JW_OSD_LEVEL_MS) == JW_OSD_VIEW_HIDE);
    assert(view.kind == JW_OSD_VIEW_NONE);

    jw_osd_view_stage(&view, JW_OSD_GAME_STOPPING, 0, 0);
    assert(jw_osd_view_hide_stage(&view) == JW_OSD_VIEW_HIDE);
    assert(view.kind == JW_OSD_VIEW_NONE);
    assert(jw_osd_view_hide_stage(&view) == JW_OSD_VIEW_KEEP);
}

/* Warnings and the exit prompt are never retained or restored. */
static void transient_never_restored(void) {
    jw_osd_game_stage transient[] = {
        JW_OSD_GAME_SETTINGS_NOT_SAVED, JW_OSD_GAME_STORAGE_READ_ONLY,
        JW_OSD_PICO8_EXIT_CONFIRM, JW_OSD_PICO8_IMPORT_FAILED,
        JW_OSD_POWER_SAVE_SAVED, JW_OSD_POWER_SAVE_FAILED, JW_OSD_BOOT_RESUME,
    };
    for (size_t i = 0; i < sizeof(transient) / sizeof(transient[0]); i++) {
        jw_osd_view view;
        jw_osd_view_reset(&view);
        assert(!JW_OSD_GAME_STAGE_IS_PROGRESS(transient[i]));
        assert(jw_osd_view_stage(&view, transient[i], 0, 0) == JW_OSD_VIEW_DRAW);
        assert(view.hide_at == JW_OSD_GAME_TRANSIENT_MS);
        assert(jw_osd_view_level(&view, JW_OSD_VIEW_VOLUME, 20, 100) == JW_OSD_VIEW_DRAW);
        assert(!view.has_retained);
        assert(jw_osd_view_tick(&view, 100 + JW_OSD_LEVEL_MS) == JW_OSD_VIEW_HIDE);
        assert(view.kind == JW_OSD_VIEW_NONE);
    }
}

/* A transient stage replaces progress and clears it rather than stacking. */
static void transient_clears_progress(void) {
    jw_osd_view view;
    jw_osd_view_reset(&view);
    jw_osd_view_stage(&view, JW_OSD_GAME_SYNCING, 4, 0);
    jw_osd_view_level(&view, JW_OSD_VIEW_VOLUME, 20, 10);
    assert(view.has_retained);
    assert(jw_osd_view_stage(&view, JW_OSD_GAME_STORAGE_READ_ONLY, 0, 20) == JW_OSD_VIEW_DRAW);
    expect_stage(&view, JW_OSD_GAME_STORAGE_READ_ONLY, 0);
    assert(!view.has_retained);
    assert(jw_osd_view_tick(&view, 20 + JW_OSD_GAME_TRANSIENT_MS) == JW_OSD_VIEW_HIDE);

    jw_osd_view_stage(&view, JW_OSD_GAME_SYNCING, 4, 0);
    assert(jw_osd_view_stage(&view, JW_OSD_PICO8_EXIT_CONFIRM, 0, 5) == JW_OSD_VIEW_DRAW);
    assert(jw_osd_view_tick(&view, 5 + JW_OSD_GAME_TRANSIENT_MS) == JW_OSD_VIEW_HIDE);
    assert(view.kind == JW_OSD_VIEW_NONE);

    /* Progress to progress simply replaces. */
    jw_osd_view_stage(&view, JW_OSD_GAME_CHECKING, 0, 0);
    assert(jw_osd_view_stage(&view, JW_OSD_GAME_SYNCING, 7, 1) == JW_OSD_VIEW_DRAW);
    expect_stage(&view, JW_OSD_GAME_SYNCING, 7);
}

static void reset_clears_everything(void) {
    jw_osd_view view;
    jw_osd_view_reset(&view);
    jw_osd_view_stage(&view, JW_OSD_GAME_SYNCING, 2, 0);
    jw_osd_view_level(&view, JW_OSD_VIEW_VOLUME, 20, 0);
    jw_osd_view_reset(&view);
    assert(view.kind == JW_OSD_VIEW_NONE && !view.has_retained);
    assert(jw_osd_view_tick(&view, 999999) == JW_OSD_VIEW_KEEP);
    /* Percent clamps as the old backends did. */
    jw_osd_view_level(&view, JW_OSD_VIEW_BRIGHTNESS, 140, 0);
    assert(view.percent == 100);
    jw_osd_view_level(&view, JW_OSD_VIEW_BRIGHTNESS, -3, 0);
    assert(view.percent == 0);
    assert(jw_osd_view_level(&view, JW_OSD_VIEW_STAGE, 5, 0) == JW_OSD_VIEW_KEEP);
}

/* The OSD sleeps without a timeout unless something on screen ends by
   itself, and wakes exactly when it does. */
static void timeout_follows_the_timed_view(void) {
    jw_osd_view view;
    jw_osd_view_reset(&view);
    assert(jw_osd_view_timeout_ms(&view, 0) == -1);
    jw_osd_view_level(&view, JW_OSD_VIEW_VOLUME, 30, 1000);
    assert(jw_osd_view_timeout_ms(&view, 1000) == JW_OSD_LEVEL_MS);
    assert(jw_osd_view_timeout_ms(&view, 1000 + JW_OSD_LEVEL_MS - 1) == 1);
    assert(jw_osd_view_timeout_ms(&view, 1000 + JW_OSD_LEVEL_MS + 50) == 0);
    assert(jw_osd_view_tick(&view, 1000 + JW_OSD_LEVEL_MS) == JW_OSD_VIEW_HIDE);
    assert(jw_osd_view_timeout_ms(&view, 999999) == -1);

    /* Progress stays until the producer hides it: no timer. Under a level
       toast, the toast's end is the timer, and afterwards there is none. */
    jw_osd_view_stage(&view, JW_OSD_GAME_SYNCING, 1, 0);
    assert(jw_osd_view_timeout_ms(&view, 5) == -1);
    jw_osd_view_level(&view, JW_OSD_VIEW_BRIGHTNESS, 10, 100);
    assert(jw_osd_view_timeout_ms(&view, 100) == JW_OSD_LEVEL_MS);
    assert(jw_osd_view_tick(&view, 100 + JW_OSD_LEVEL_MS) == JW_OSD_VIEW_DRAW);
    assert(jw_osd_view_timeout_ms(&view, 100 + JW_OSD_LEVEL_MS) == -1);

    jw_osd_view_stage(&view, JW_OSD_PICO8_EXIT_CONFIRM, 0, 0);
    assert(jw_osd_view_timeout_ms(&view, 0) == JW_OSD_GAME_TRANSIENT_MS);
    assert(jw_osd_view_timeout_ms(NULL, 0) == -1);
}

int main(void) {
    progress_returns_after_level();
    progress_started_under_level();
    hide_under_level_prevents_restore();
    transient_never_restored();
    transient_clears_progress();
    reset_clears_everything();
    timeout_follows_the_timed_view();
    puts("PASS osd-view-test");
    return 0;
}
