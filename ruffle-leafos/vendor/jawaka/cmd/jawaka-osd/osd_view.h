#ifndef JW_OSD_VIEW_H
#define JW_OSD_VIEW_H

#include "cmd/jawaka-osd/game_launch.h"

#include <stdbool.h>
#include <stdint.h>

/* What the OSD shows, independent of how a backend draws it. Both backends
   route every request through here so a volume or brightness toast treats an
   active progress banner the same way on the host and on the device. */

#define JW_OSD_LEVEL_MS 1200u

/* Stages that stay up until their producer replaces or hides them. Only these
   are worth restoring after a level toast: a warning has already had its
   moment, and a hidden PICO-8 exit prompt must never come back. */
#define JW_OSD_GAME_STAGE_IS_PROGRESS(stage) \
    ((stage) == JW_OSD_GAME_CHECKING || (stage) == JW_OSD_GAME_SYNCING || \
     (stage) == JW_OSD_GAME_STOPPING || (stage) == JW_OSD_PICO8_IMPORT || \
     (stage) == JW_OSD_POWER_SAVE_RELEASE || (stage) == JW_OSD_POWER_SAVE_SAVING)

typedef enum {
    JW_OSD_VIEW_NONE = 0,
    JW_OSD_VIEW_BRIGHTNESS,
    JW_OSD_VIEW_VOLUME,
    JW_OSD_VIEW_STAGE,
} jw_osd_view_kind;

typedef struct {
    jw_osd_view_kind kind;
    int percent;
    jw_osd_game_stage stage;
    int pending_items;
    uint64_t hide_at;             /* UINT64_MAX: until the producer hides it */
    /* The latest progress stage a level toast is covering. One slot for the
       current operation, not a queue. */
    bool has_retained;
    jw_osd_game_stage retained_stage;
    int retained_pending_items;
} jw_osd_view;

typedef enum {
    JW_OSD_VIEW_KEEP = 0,   /* nothing on screen changes */
    JW_OSD_VIEW_DRAW,       /* draw the current view */
    JW_OSD_VIEW_HIDE,       /* remove the surface */
} jw_osd_view_effect;

void jw_osd_view_reset(jw_osd_view *view);
jw_osd_view_effect jw_osd_view_level(jw_osd_view *view, jw_osd_view_kind kind,
                                     int percent, uint64_t now_ms);
jw_osd_view_effect jw_osd_view_stage(jw_osd_view *view, jw_osd_game_stage stage,
                                     int pending_items, uint64_t now_ms);
jw_osd_view_effect jw_osd_view_hide_stage(jw_osd_view *view);
jw_osd_view_effect jw_osd_view_tick(jw_osd_view *view, uint64_t now_ms);
/* How long until jw_osd_view_tick has something to do: milliseconds until a
   timed view ends (0 when it is already due), or -1 when nothing on screen
   ends by itself. */
int jw_osd_view_timeout_ms(const jw_osd_view *view, uint64_t now_ms);

#endif /* JW_OSD_VIEW_H */
