#include "cmd/jawaka-osd/osd_view.h"

#include <limits.h>
#include <string.h>

void jw_osd_view_reset(jw_osd_view *view) {
    if (!view) return;
    memset(view, 0, sizeof(*view));
    view->kind = JW_OSD_VIEW_NONE;
}

static void jw__retain(jw_osd_view *view, jw_osd_game_stage stage, int pending_items) {
    view->has_retained = true;
    view->retained_stage = stage;
    view->retained_pending_items = pending_items;
}

jw_osd_view_effect jw_osd_view_level(jw_osd_view *view, jw_osd_view_kind kind,
                                     int percent, uint64_t now_ms) {
    if (!view || (kind != JW_OSD_VIEW_BRIGHTNESS && kind != JW_OSD_VIEW_VOLUME)) {
        return JW_OSD_VIEW_KEEP;
    }
    /* Covering a progress banner keeps it for later. Covering a warning or the
       exit prompt simply ends it. */
    if (view->kind == JW_OSD_VIEW_STAGE &&
        JW_OSD_GAME_STAGE_IS_PROGRESS(view->stage)) {
        jw__retain(view, view->stage, view->pending_items);
    }
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    view->kind = kind;
    view->percent = percent;
    view->hide_at = now_ms + JW_OSD_LEVEL_MS;
    return JW_OSD_VIEW_DRAW;
}

jw_osd_view_effect jw_osd_view_stage(jw_osd_view *view, jw_osd_game_stage stage,
                                     int pending_items, uint64_t now_ms) {
    if (!view) return JW_OSD_VIEW_KEEP;
    if (pending_items < 0) pending_items = 0;
    /* A progress update while the user adjusts a level must not cut the toast
       short; it only moves what comes back when the toast ends. */
    if (JW_OSD_GAME_STAGE_IS_PROGRESS(stage) &&
        (view->kind == JW_OSD_VIEW_BRIGHTNESS || view->kind == JW_OSD_VIEW_VOLUME)) {
        jw__retain(view, stage, pending_items);
        return JW_OSD_VIEW_KEEP;
    }
    /* Any text stage replaces the old progress, and a transient warning or
       exit prompt clears it rather than stacking above it. */
    view->has_retained = false;
    view->kind = JW_OSD_VIEW_STAGE;
    view->stage = stage;
    view->pending_items = pending_items;
    view->hide_at = JW_OSD_GAME_STAGE_IS_TRANSIENT(stage)
                        ? now_ms + JW_OSD_GAME_TRANSIENT_MS
                        : UINT64_MAX;
    return JW_OSD_VIEW_DRAW;
}

jw_osd_view_effect jw_osd_view_hide_stage(jw_osd_view *view) {
    if (!view) return JW_OSD_VIEW_KEEP;
    /* Completion, cancellation or Start now: nothing may reappear afterwards,
       even when a level toast is covering the banner right now. */
    view->has_retained = false;
    if (view->kind != JW_OSD_VIEW_STAGE) return JW_OSD_VIEW_KEEP;
    view->kind = JW_OSD_VIEW_NONE;
    return JW_OSD_VIEW_HIDE;
}

jw_osd_view_effect jw_osd_view_tick(jw_osd_view *view, uint64_t now_ms) {
    if (!view || view->kind == JW_OSD_VIEW_NONE ||
        view->hide_at == UINT64_MAX || now_ms < view->hide_at) {
        return JW_OSD_VIEW_KEEP;
    }
    if (view->kind != JW_OSD_VIEW_STAGE && view->has_retained) {
        view->kind = JW_OSD_VIEW_STAGE;
        view->stage = view->retained_stage;
        view->pending_items = view->retained_pending_items;
        view->hide_at = UINT64_MAX;
        view->has_retained = false;
        return JW_OSD_VIEW_DRAW;
    }
    view->kind = JW_OSD_VIEW_NONE;
    view->has_retained = false;
    return JW_OSD_VIEW_HIDE;
}

int jw_osd_view_timeout_ms(const jw_osd_view *view, uint64_t now_ms) {
    if (!view || view->kind == JW_OSD_VIEW_NONE || view->hide_at == UINT64_MAX) {
        return -1;
    }
    if (view->hide_at <= now_ms) return 0;
    uint64_t wait = view->hide_at - now_ms;
    return wait > (uint64_t)INT_MAX ? INT_MAX : (int)wait;
}
