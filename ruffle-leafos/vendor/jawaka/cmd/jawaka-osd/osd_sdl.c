#include "cmd/jawaka-osd/osd_backend.h"
#include "cmd/jawaka-osd/osd_text.h"
#include "cmd/jawaka-osd/osd_view.h"

#include <SDL.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Host preview of the OSD. Level toasts keep their fixed 480 x 96 window.
   Banners are laid out against the full output (CAT_WINDOW_WIDTH x
   CAT_WINDOW_HEIGHT, 960 x 720 by default) and the window is sized to the
   banner box, so text, shortening and actions match the device; placement
   and damage only show on the Wayland backend. */

#define JW_OSD_LEVEL_W 480
#define JW_OSD_LEVEL_H 96

static SDL_Window *s_window;
static SDL_Renderer *s_renderer;
static jw_osd_view s_view;
static int s_output_w = 960;
static int s_output_h = 720;

static int jw__env_dimension(const char *name, int fallback) {
    const char *value = getenv(name);
    char *end = NULL;
    long parsed = value && value[0] ? strtol(value, &end, 10) : 0;
    return (end && *end == '\0' && parsed > 0 && parsed <= 4096) ? (int)parsed : fallback;
}

static void jw__draw_rect(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b, Uint8 a) {
    SDL_Rect rect = { x, y, w, h };
    SDL_SetRenderDrawColor(s_renderer, r, g, b, a);
    SDL_RenderFillRect(s_renderer, &rect);
}

static void jw__fill_round_rect(int x, int y, int w, int h, int radius, Uint32 rgb) {
    if (w <= 0 || h <= 0) return;
    if (radius * 2 > w) radius = w / 2;
    if (radius * 2 > h) radius = h / 2;
    if (radius < 0) radius = 0;
    SDL_SetRenderDrawColor(s_renderer, (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
    for (int row = 0; row < h; row++) {
        double dy = row < radius ? radius - row - 0.5
                  : row >= h - radius ? row - (h - radius) + 0.5 : -1.0;
        int inset = dy < 0.0 ? 0
                  : radius - (int)(sqrt((double)radius * radius - dy * dy) + 0.5);
        SDL_RenderDrawLine(s_renderer, x + inset, y + row, x + w - 1 - inset, y + row);
    }
}

static void jw__copy_surface(SDL_Surface *surface, int x, int y) {
    if (!surface) return;
    SDL_Texture *texture = SDL_CreateTextureFromSurface(s_renderer, surface);
    if (!texture) return;
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
    SDL_Rect dst = { x, y, surface->w, surface->h };
    SDL_RenderCopy(s_renderer, texture, NULL, &dst);
    SDL_DestroyTexture(texture);
}

static int jw__draw_banner(void) {
    jw_osd_banner banner;
    if (jw_osd_banner_render(s_view.stage, s_view.pending_items,
                             s_output_w, s_output_h, &banner) != 0) {
        return -1;
    }
    const jw_osd_rect box = banner.layout.box;
    SDL_SetWindowSize(s_window, box.w, box.h);
    SDL_RenderSetViewport(s_renderer, NULL);
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);
    int border = banner.metrics.border;
    jw__fill_round_rect(0, 0, box.w, box.h, banner.metrics.radius, JW_OSD_BANNER_MUTED);
    jw__fill_round_rect(border, border, box.w - 2 * border, box.h - 2 * border,
                        banner.metrics.inner_radius, JW_OSD_BANNER_BACKGROUND);
    jw__copy_surface(banner.title, banner.layout.title_x - box.x,
                     banner.layout.title_y - box.y);
    jw__copy_surface(banner.action, banner.layout.action_x - box.x,
                     banner.layout.action_y - box.y);
    SDL_RenderPresent(s_renderer);
    jw_osd_banner_free(&banner);
    return 0;
}

static int jw__draw(void) {
    if (s_view.kind == JW_OSD_VIEW_STAGE) {
        return jw__draw_banner();
    }
    SDL_SetWindowSize(s_window, JW_OSD_LEVEL_W, JW_OSD_LEVEL_H);
    SDL_RenderSetViewport(s_renderer, NULL);
    SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 0);
    SDL_RenderClear(s_renderer);

    jw__draw_rect(0, 0, JW_OSD_LEVEL_W, JW_OSD_LEVEL_H, 18, 20, 24, 230);
    jw__draw_rect(24, 43, 432, 10, 72, 76, 84, 255);

    int fill = (432 * s_view.percent) / 100;
    jw__draw_rect(24, 43, fill, 10, 250, 210, 92, 255);
    jw__draw_rect(20 + fill, 35, 18, 26, 255, 240, 150, 255);

    SDL_RenderPresent(s_renderer);
    return 0;
}

/* A failed draw leaves nothing to restore later: the view is dropped with it. */
static int jw__apply(jw_osd_view_effect effect) {
    switch (effect) {
        case JW_OSD_VIEW_KEEP:
            return 0;
        case JW_OSD_VIEW_HIDE:
            SDL_HideWindow(s_window);
            return 0;
        case JW_OSD_VIEW_DRAW:
            if (!s_window || !s_renderer) break;
            SDL_ShowWindow(s_window);
            SDL_RaiseWindow(s_window);
            if (jw__draw() == 0) return 0;
            break;
    }
    jw_osd_view_reset(&s_view);
    if (s_window) SDL_HideWindow(s_window);
    return -1;
}

int jw_osd_backend_init(void) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        return -1;
    }

    Uint32 flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_HIDDEN;
#ifdef SDL_WINDOW_ALWAYS_ON_TOP
    flags |= SDL_WINDOW_ALWAYS_ON_TOP;
#endif
    s_window = SDL_CreateWindow("Jawaka OSD",
                                SDL_WINDOWPOS_CENTERED,
                                SDL_WINDOWPOS_CENTERED,
                                JW_OSD_LEVEL_W, JW_OSD_LEVEL_H, flags);
    if (!s_window) {
        SDL_Quit();
        return -1;
    }

    s_renderer = SDL_CreateRenderer(s_window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
        SDL_Quit();
        return -1;
    }
    SDL_SetRenderDrawBlendMode(s_renderer, SDL_BLENDMODE_BLEND);
    jw_osd_view_reset(&s_view);
    s_output_w = jw__env_dimension("CAT_WINDOW_WIDTH", 960);
    s_output_h = jw__env_dimension("CAT_WINDOW_HEIGHT", 720);
    (void)jw_osd_text_init(s_output_w);
    return 0;
}

int jw_osd_backend_show_brightness(int percent, uint64_t now_ms) {
    return jw__apply(jw_osd_view_level(&s_view, JW_OSD_VIEW_BRIGHTNESS, percent, now_ms));
}

int jw_osd_backend_show_volume(int percent, uint64_t now_ms) {
    return jw__apply(jw_osd_view_level(&s_view, JW_OSD_VIEW_VOLUME, percent, now_ms));
}

int jw_osd_backend_show_game_launch(jw_osd_game_stage stage,
                                    int pending_items, uint64_t now_ms) {
    return jw__apply(jw_osd_view_stage(&s_view, stage, pending_items, now_ms));
}

void jw_osd_backend_hide_game_launch(void) {
    (void)jw__apply(jw_osd_view_hide_stage(&s_view));
}

/* SDL wants its events pumped even with nothing shown, so the host preview
   keeps the old 50 ms cadence. */
#define JW_OSD_SDL_PUMP_MS 50

int jw_osd_backend_event_fd(void) {
    return -1;
}

int jw_osd_backend_timeout_ms(uint64_t now_ms) {
    int timeout = jw_osd_view_timeout_ms(&s_view, now_ms);
    return (timeout < 0 || timeout > JW_OSD_SDL_PUMP_MS) ? JW_OSD_SDL_PUMP_MS : timeout;
}

void jw_osd_backend_tick(uint64_t now_ms) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) { }
    (void)jw__apply(jw_osd_view_tick(&s_view, now_ms));
}

void jw_osd_backend_shutdown(void) {
    jw_osd_view_reset(&s_view);
    jw_osd_text_shutdown();
    if (s_renderer) {
        SDL_DestroyRenderer(s_renderer);
        s_renderer = NULL;
    }
    if (s_window) {
        SDL_DestroyWindow(s_window);
        s_window = NULL;
    }
    SDL_Quit();
}
