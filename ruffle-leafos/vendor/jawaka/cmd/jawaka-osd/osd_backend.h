#ifndef JW_OSD_BACKEND_H
#define JW_OSD_BACKEND_H

#include "cmd/jawaka-osd/game_launch.h"

#include <stdint.h>

/* Show calls return 0 once the backend has submitted what the view asks for,
   and -1 when it could not; the OSD then replies with an error instead of ok.
   Success means submitted, not seen. */
int  jw_osd_backend_init(void);
int  jw_osd_backend_show_brightness(int percent, uint64_t now_ms);
int  jw_osd_backend_show_volume(int percent, uint64_t now_ms);
int  jw_osd_backend_show_game_launch(jw_osd_game_stage stage,
                                     int pending_items, uint64_t now_ms);
void jw_osd_backend_hide_game_launch(void);
/* The main loop sleeps until a client connects, the backend's event fd is
   readable (-1: it has none), or jw_osd_backend_timeout_ms runs out (-1: no
   limit), then calls tick. Tick reads what the backend's fd has without
   blocking and ends whatever view is due. */
int  jw_osd_backend_event_fd(void);
int  jw_osd_backend_timeout_ms(uint64_t now_ms);
void jw_osd_backend_tick(uint64_t now_ms);
void jw_osd_backend_shutdown(void);

#endif /* JW_OSD_BACKEND_H */
