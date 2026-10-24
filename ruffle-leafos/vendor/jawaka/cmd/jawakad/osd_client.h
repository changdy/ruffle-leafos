#ifndef JW_OSD_CLIENT_H
#define JW_OSD_CLIENT_H

#include <stdbool.h>

/* The daemon's side of the OSD banners. It owns one rule the OSD cannot:
   the PICO-8 exit confirmation is armed only while its prompt is the thing on
   screen. Every request that replaces or removes the prompt disarms it first,
   so a timeout with an uncertain display outcome still leaves it disarmed.

   The transport is injected so tests drive these exact transitions. */

/* A visual hint must never extend a service's LIFE-1 acknowledgement budget. */
#define JW_OSD_CLIENT_BANNER_TIMEOUT_MS 100

typedef struct {
    void *ctx;
    /* One exchange. Returns 0 only for an {"type":"ok"} reply within the
       timeout. `show` requests may start the OSD; a request without it
       succeeds trivially when no OSD is running, since nothing is shown. */
    int (*request)(void *ctx, const char *json, int timeout_ms, bool show);
    long long (*now_ms)(void *ctx);
    void (*sleep_ms)(void *ctx, int ms);   /* optional */
    /* Last resort when a prompt may be on screen and could not be removed:
       end the OSD process so its surface goes with it. Optional. */
    void (*discard_osd)(void *ctx);
} jw_osd_client;

typedef enum {
    JW_OSD_PICO8_MENU_ARMED = 0,     /* prompt shown, waiting for a second press */
    JW_OSD_PICO8_MENU_CONFIRMED,     /* second press inside the prompt's window */
    JW_OSD_PICO8_MENU_UNAVAILABLE,   /* prompt could not be shown; stays disarmed */
} jw_osd_pico8_menu;

/* The OSD process exited or restarted: whatever it showed is gone. */
void jw_osd_client_prompt_lost(long long *exit_deadline_ms);

/* `type` is "show-volume" or "show-brightness". Two attempts, as before. */
int jw_osd_client_show_level(const jw_osd_client *client, long long *exit_deadline_ms,
                             const char *type, int percent, int timeout_ms);
/* Any stage other than "pico8-exit", which only jw_osd_client_pico8_menu shows. */
int jw_osd_client_show_stage(const jw_osd_client *client, long long *exit_deadline_ms,
                             const char *stage, int pending_items);
int jw_osd_client_hide(const jw_osd_client *client, long long *exit_deadline_ms);
jw_osd_pico8_menu jw_osd_client_pico8_menu(const jw_osd_client *client,
                                           long long *exit_deadline_ms);

#endif /* JW_OSD_CLIENT_H */
