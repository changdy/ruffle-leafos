#ifndef JW_OSD_TEXT_H
#define JW_OSD_TEXT_H

#include "cmd/jawaka-osd/game_launch.h"
#include "cmd/jawaka-osd/osd_layout.h"

#include <SDL.h>
#include <SDL_ttf.h>
#include <stdbool.h>

/* Banner text for both OSD backends. The resolved Leaf font (the CJK face for
   a CJK language) opens once, before the OSD announces readiness, and is
   reused for every show. */

typedef struct {
    jw_osd_banner_metrics metrics;
    jw_osd_banner_layout layout;
    SDL_Surface *title;    /* ARGB8888, straight alpha; draw with its own colour */
    SDL_Surface *action;   /* NULL without a secondary line */
} jw_osd_banner;

/* Returns 0 when the font opened. On failure the level toasts still work and
   every banner show fails, so the daemon never arms a prompt it cannot show. */
int  jw_osd_text_init(int output_w);
void jw_osd_text_shutdown(void);
bool jw_osd_text_ready(void);

int  jw_osd_banner_render(jw_osd_game_stage stage, int pending_items,
                          int output_w, int output_h, jw_osd_banner *out);
void jw_osd_banner_free(jw_osd_banner *banner);

#endif /* JW_OSD_TEXT_H */
