#ifndef JW_OSD_GAME_LAUNCH_H
#define JW_OSD_GAME_LAUNCH_H

#include "cJSON.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    JW_OSD_GAME_CHECKING = 0,
    JW_OSD_GAME_SYNCING,
    JW_OSD_GAME_STOPPING,
    /* Not a launch stage: the RetroArch app runner could not write the
       session's settings back to the durable shared config. It reuses this
       banner because it is the one text surface the OSD has, and it must be
       seen -- a silent loss is exactly the failure Leaf#48 reported. Unlike
       the Syncthing stages it hides itself after a few seconds; nothing is
       waiting on it. */
    JW_OSD_GAME_SETTINGS_NOT_SAVED,
    JW_OSD_GAME_STORAGE_READ_ONLY,
    JW_OSD_PICO8_EXIT_CONFIRM,
    JW_OSD_PICO8_IMPORT,
    JW_OSD_PICO8_IMPORT_FAILED,
    /* Save before power-button shutdown. Release and saving stay up until the
       daemon replaces them; the outcome dismisses itself (the device is
       powering off anyway). */
    JW_OSD_POWER_SAVE_RELEASE,
    JW_OSD_POWER_SAVE_SAVING,
    JW_OSD_POWER_SAVE_SAVED,
    JW_OSD_POWER_SAVE_FAILED,
    /* Resume Game on Boot: the saved state loaded and play continues. Over
       the game, so it dismisses itself. */
    JW_OSD_BOOT_RESUME,
} jw_osd_game_stage;

/* Stages that dismiss themselves instead of waiting for hide-game-launch. */
#define JW_OSD_GAME_STAGE_IS_TRANSIENT(stage) \
    ((stage) == JW_OSD_GAME_SETTINGS_NOT_SAVED || (stage) == JW_OSD_GAME_STORAGE_READ_ONLY || (stage) == JW_OSD_PICO8_EXIT_CONFIRM || (stage) == JW_OSD_PICO8_IMPORT_FAILED || \
     (stage) == JW_OSD_POWER_SAVE_SAVED || (stage) == JW_OSD_POWER_SAVE_FAILED || \
     (stage) == JW_OSD_BOOT_RESUME)
#define JW_OSD_GAME_TRANSIENT_MS 4000u

/* `expires_ms` is optional: a CLOCK_MONOTONIC millisecond deadline after
   which the banner must not be submitted, because the daemon has already
   given up on the request. 0 when absent. */
bool jw_osd_game_launch_parse(const cJSON *root,
                              jw_osd_game_stage *stage,
                              int *pending_items,
                              uint64_t *expires_ms);
/* Translated through the loaded Leaf table. Both outputs stay valid UTF-8
   when a buffer is too small. */
void jw_osd_game_launch_text(jw_osd_game_stage stage, int pending_items,
                             char *title, size_t title_size,
                             char *action, size_t action_size);
const char *jw_osd_game_stage_name(jw_osd_game_stage stage);

#endif
