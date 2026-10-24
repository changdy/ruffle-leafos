#include "cmd/jawaka-osd/osd_text.h"

#include "internal/core/log.h"
#include "internal/platform/paths.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static TTF_Font *s_font;
static jw_osd_banner_metrics s_metrics;

static bool jw__readable(const char *path) {
    return path && path[0] && access(path, R_OK) == 0;
}

static bool jw__join(char *out, size_t size, const char *root, const char *rel) {
    if (!root || !root[0] || !rel || !rel[0]) return false;
    int n = snprintf(out, size, "%s/%s", root, rel);
    return n > 0 && (size_t)n < size && jw__readable(out);
}

/* The same order Catastrophe uses for the launcher: an absolute CAT_FONT_PATH,
   then CAT_FONTS_DIR, then the launcher's res tree, then the bundled default. */
static bool jw__resolve_font(char *out, size_t size) {
    const char *rel = getenv("CAT_FONT_PATH");
    if (rel && rel[0] == '/') {
        if (jw__readable(rel)) {
            snprintf(out, size, "%s", rel);
            return true;
        }
        rel = NULL;
    }
    char *res = jw_launcher_res_dir();
    const char *roots[] = { getenv("CAT_FONTS_DIR"), res };
    bool found = false;
    for (size_t i = 0; !found && i < sizeof(roots) / sizeof(roots[0]); i++) {
        found = jw__join(out, size, roots[i], rel) ||
                jw__join(out, size, roots[i], "font.ttf");
    }
    free(res);
    return found;
}

int jw_osd_text_init(int output_w) {
    jw_osd_banner_metrics_for(output_w, jw_osd_font_bump(getenv("CAT_FONT_BUMP")),
                              &s_metrics);
    if (s_font) return 0;
    if (!TTF_WasInit() && TTF_Init() != 0) {
        jw_log_error("osd: SDL_ttf init failed: %s", TTF_GetError());
        return -1;
    }
    char path[PATH_MAX];
    if (!jw__resolve_font(path, sizeof(path))) {
        jw_log_error("osd: no readable font for CAT_FONT_PATH=%s",
                     getenv("CAT_FONT_PATH") ? getenv("CAT_FONT_PATH") : "");
        return -1;
    }
    s_font = TTF_OpenFont(path, s_metrics.font_px);
    if (!s_font) {
        jw_log_error("osd: could not open font %s: %s", path, TTF_GetError());
        return -1;
    }
    TTF_SetFontStyle(s_font, TTF_STYLE_BOLD);
    jw_log_info("osd: banner font %s size=%d", path, s_metrics.font_px);
    return 0;
}

void jw_osd_text_shutdown(void) {
    if (s_font) {
        TTF_CloseFont(s_font);
        s_font = NULL;
    }
    if (TTF_WasInit()) TTF_Quit();
}

bool jw_osd_text_ready(void) {
    return s_font != NULL;
}

static int jw__measure(void *ctx, const char *text) {
    int w = 0;
    int h = 0;
    if (!text || !text[0] || TTF_SizeUTF8((TTF_Font *)ctx, text, &w, &h) != 0) return 0;
    return w;
}

static SDL_Surface *jw__render_row(const char *text, Uint32 rgb) {
    if (!text || !text[0]) return NULL;
    SDL_Color color = { (rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF, 255 };
    SDL_Surface *rendered = TTF_RenderUTF8_Blended(s_font, text, color);
    if (!rendered) return NULL;
    if (rendered->format->format == SDL_PIXELFORMAT_ARGB8888) return rendered;
    SDL_Surface *converted = SDL_ConvertSurfaceFormat(rendered, SDL_PIXELFORMAT_ARGB8888, 0);
    SDL_FreeSurface(rendered);
    return converted;
}

int jw_osd_banner_render(jw_osd_game_stage stage, int pending_items,
                         int output_w, int output_h, jw_osd_banner *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!s_font) return -1;

    char title_text[256];
    char action_text[128];
    jw_osd_game_launch_text(stage, pending_items, title_text, sizeof(title_text),
                            action_text, sizeof(action_text));
    /* Shorten the main line before the action: "Press Menu again" must stay
       readable whatever length the translation of the question is. */
    int max_w = jw_osd_banner_text_max_w(&s_metrics);
    char title[256];
    char action[128] = "";
    int title_w = jw_osd_fit_text(title_text, max_w, jw__measure, s_font,
                                  title, sizeof(title));
    if (action_text[0]) {
        (void)jw_osd_fit_text(action_text, max_w, jw__measure, s_font,
                              action, sizeof(action));
    }
    bool has_action = action[0] != '\0';

    out->metrics = s_metrics;
    out->title = jw__render_row(title, JW_OSD_BANNER_TEXT);
    out->action = has_action ? jw__render_row(action, JW_OSD_BANNER_MUTED) : NULL;
    if (!out->title || (has_action && !out->action)) {
        jw_log_warn("osd: could not render banner text: %s", TTF_GetError());
        jw_osd_banner_free(out);
        return -1;
    }
    jw_osd_banner_layout_for(&s_metrics, output_w, output_h,
                             title_w ? out->title->w : 0,
                             has_action ? out->action->w : 0, has_action,
                             TTF_FontHeight(s_font), &out->layout);
    return 0;
}

void jw_osd_banner_free(jw_osd_banner *banner) {
    if (!banner) return;
    if (banner->title) SDL_FreeSurface(banner->title);
    if (banner->action) SDL_FreeSurface(banner->action);
    banner->title = NULL;
    banner->action = NULL;
}
