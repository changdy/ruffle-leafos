/* Exercise the actual presentation wrapper and tab snapshots with SDL's dummy
   renderer, following wordmark_test.c's single-TU launcher test pattern. */
#define main jw_launcher_main
#include "cmd/jawaka-launcher/main.c"
#undef main
#include <assert.h>

static SDL_Surface *notice_pixels(SDL_Texture *target) {
    SDL_Renderer *renderer = cat_get_renderer();
    SDL_Texture *previous = SDL_GetRenderTarget(renderer);
    assert(SDL_SetRenderTarget(renderer, target) == 0);
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0,
        cat_get_screen_width(), cat_get_screen_height(), 32, SDL_PIXELFORMAT_RGBA32);
    assert(surface);
    assert(SDL_RenderReadPixels(renderer, NULL, surface->format->format,
                               surface->pixels, surface->pitch) == 0);
    assert(SDL_SetRenderTarget(renderer, previous) == 0);
    return surface;
}

static bool notice_pixels_equal(const SDL_Surface *a, const SDL_Surface *b) {
    assert(a->w == b->w && a->h == b->h);
    for (int y = 0; y < a->h; ++y) {
        if (memcmp((const char *)a->pixels + y * a->pitch,
                   (const char *)b->pixels + y * b->pitch, a->w * 4) != 0)
            return false;
    }
    return true;
}

/* Bounding box of the pixels the pill changed, in the lower half so the
   status bar never counts. */
static SDL_Rect notice_bounds(const SDL_Surface *empty, const SDL_Surface *live) {
    int x0 = live->w, y0 = live->h, x1 = -1, y1 = -1;
    for (int y = live->h / 2; y < live->h; ++y) {
        const uint32_t *a = (const uint32_t *)((const char *)empty->pixels + y * empty->pitch);
        const uint32_t *b = (const uint32_t *)((const char *)live->pixels + y * live->pitch);
        for (int x = 0; x < live->w; ++x) {
            if (a[x] == b[x]) continue;
            if (x < x0) x0 = x;
            if (x > x1) x1 = x;
            if (y < y0) y0 = y;
            if (y > y1) y1 = y;
        }
    }
    assert(x1 >= 0);
    return (SDL_Rect){ x0, y0, x1 - x0 + 1, y1 - y0 + 1 };
}

static SDL_Rect notice_drawn(jw_launcher_state *state, const SDL_Surface *empty,
                             const char *text, const char *screenshot) {
    jw__launch_notice(state, text);
    jw__render_launcher(state);
    SDL_Surface *live = notice_pixels(NULL);
    SDL_Rect pill = notice_bounds(empty, live);
    if (screenshot && screenshot[0]) assert(IMG_SavePNG(live, screenshot) == 0);
    SDL_FreeSurface(live);
    return pill;
}

/* Whole text on two lines, each within the pill: nothing is ellipsized. */
static void expect_two_whole_lines(const char *text, int max_text_w) {
    jw_launch_notice_lines lines;
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    assert(cat_measure_text(small, text) > max_text_w);
    assert(jw_launch_notice_split(text, max_text_w, jw__launch_notice_measure,
                                  small, &lines) == 2);
    for (int i = 0; i < 2; ++i)
        assert(cat_measure_text(small, lines.line[i]) <= max_text_w);
    char joined[512];
    snprintf(joined, sizeof(joined), "%s %s", lines.line[0], lines.line[1]);
    assert(strcmp(joined, text) == 0);
}

/* A notice too wide for one line wraps instead of hiding its reason, growing
   the pill upward from the same bottom edge; a short one keeps its old box.
   Checked at the smallest and largest font sizes. */
static void check_wrapped_notice(jw_launcher_state *state) {
    static const char *hardcore =
        "Couldn't resume: Hardcore mode doesn't allow loading a save.";
    static const char *russian =
        "Не удалось возобновить игру. Откройте её, чтобы попытаться загрузить сохранение.";
    int sw = cat_get_screen_width();
    int max_pill_w = sw * 90 / 100;
    int max_text_w = max_pill_w - 2 * CAT_S(12);
    int font_bump = cat_get_font_bump();
    assert(cat_set_font_bump(0) == CAT_OK);
    expect_two_whole_lines(russian, max_text_w);

    assert(cat_set_font_bump(5) == CAT_OK);
    TTF_Font *small = cat_get_font(CAT_FONT_SMALL);
    expect_two_whole_lines(hardcore, max_text_w);
    expect_two_whole_lines(russian, max_text_w);

    jw_launch_notice_begin(&state->launch_notice);
    jw__render_launcher(state);
    SDL_Surface *empty = notice_pixels(NULL);
    int bottom = cat_get_screen_height() - jw__footer_height(state) -
                 jw__system_activity_height(state) - CAT_S(12);

    /* The box the single-line pill has always had. Its antialiased corners
       reach one pixel past it on every side. */
    const char *brief = "Launch canceled";
    SDL_Rect one = notice_drawn(state, empty, brief, NULL);
    int one_w = cat_measure_text(small, brief) + 2 * CAT_S(12);
    int one_h = TTF_FontHeight(small) + 2 * CAT_S(4);
    assert(one.x == (sw - one_w) / 2 - 1 && one.w == one_w + 2);
    assert(one.y == bottom - one_h - 1 && one.h == one_h + 2);

    /* Past two lines, the second ellipsizes rather than adding a third. */
    char endless[256] = "";
    while (strlen(endless) + sizeof("Launch canceled. ") <= sizeof(endless))
        strcat(endless, "Launch canceled. ");
    const char *wide[] = { hardcore, russian, endless };
    for (int i = 0; i < 3; ++i) {
        SDL_Rect two = notice_drawn(state, empty, wide[i],
                                    i == 0 ? getenv("JW_LAUNCH_NOTICE_SCREENSHOT") : NULL);
        assert(two.y + two.h == one.y + one.h);
        assert(two.h == one.h + TTF_FontLineSkip(small));
        assert(two.w <= max_pill_w + 2);
        assert(abs(2 * two.x + two.w - sw) <= 1);
    }

    SDL_FreeSurface(empty);
    jw_launch_notice_begin(&state->launch_notice);
    assert(cat_set_font_bump(font_bump) == CAT_OK);
}

int main(void) {
    char root[] = "/tmp/jw-launch-notice-XXXXXX";
    assert(mkdtemp(root));
    setenv("UMRK_INTERNAL_DATA_PATH", root, 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    /* The MLP1's screen, so the small font is the device's 24-34 px. */
    setenv("CAT_WINDOW_WIDTH", "960", 1);
    setenv("CAT_WINDOW_HEIGHT", "720", 1);
    setenv("CAT_THEMES_DIR", "res/themes", 1);
    setenv("CAT_THEME_NAME", "Jawaka-Tabs", 1);
    char font[PATH_MAX];
    snprintf(font, sizeof(font), "%s/fonts/Nunito/Nunito-Bold.ttf", getenv("CAT_FONTS_DIR"));
    cat_config config = { .start_hidden = true, .defer_input_init = true,
                         .disable_background = true, .font_path = font };
    assert(cat_init(&config) == CAT_OK);
    jw_launcher_state *state = calloc(1, sizeof(*state));
    assert(state);
    state->current_tab = JW_TAB_APPS;
    state->visible_tab_count = JW_TAB_COUNT;
    for (int i = 0; i < JW_TAB_COUNT; ++i) state->visible_tabs[i] = (jw_tab)i;
    state->settings.show_hints = true;
    state->scan_ready = true;
    cat_list_state_init(&state->list, 10);
    g_present_state = state;

    SDL_Texture *baseline = jw__capture_view(state);
    assert(baseline);
    SDL_Surface *empty = notice_pixels(baseline);
    g_defer_present = true;
    jw__render_launcher(state);
    SDL_Surface *live_empty = notice_pixels(NULL);
    jw__launch_notice(state, "launch failed: standalone emulator missing");
    cat__g.next_redraw_ms = 0;
    SDL_Texture *snapshot = jw__capture_view(state);
    assert(snapshot);
    SDL_Surface *captured = notice_pixels(snapshot);
    assert(notice_pixels_equal(empty, captured)); /* no pill baked into the slide */
    assert(cat__g.next_redraw_ms == 0);           /* snapshots do not pace */

    /* The screenshot-flash path holds presentation on the live backbuffer:
       it must still include the pill and schedule its expiry. */
    jw__render_launcher(state);
    SDL_Surface *live = notice_pixels(NULL);
    assert(!notice_pixels_equal(live_empty, live));
    assert(cat__g.next_redraw_ms != 0);

    /* A hosted page never reaches main's tick. Once an earlier wake (e.g. the
       status poll) is consumed, its next presentation must re-arm expiry. */
    cat__g.next_redraw_ms = 0;
    jw__render_launcher(state);
    uint32_t deadline = state->launch_notice.started_ms + JW_SYSTEM_NOTICE_MS;
    assert(cat__g.next_redraw_ms >= deadline && cat__g.next_redraw_ms - deadline < 100);

    state->launch_notice.started_ms = SDL_GetTicks() - JW_SYSTEM_NOTICE_MS;
    cat__g.next_redraw_ms = 0;
    jw__render_launcher(state);
    SDL_Surface *expired = notice_pixels(NULL);
    assert(state->launch_notice.text[0] == '\0');
    assert(notice_pixels_equal(live_empty, expired));

    check_wrapped_notice(state);

    SDL_FreeSurface(empty);
    SDL_FreeSurface(live_empty);
    SDL_FreeSurface(captured);
    SDL_FreeSurface(live);
    SDL_FreeSurface(expired);
    SDL_DestroyTexture(baseline);
    SDL_DestroyTexture(snapshot);
    g_present_state = NULL;
    free(state);
    cat_quit();
    rmdir(root);
    puts("launch-notice-ui-test: snapshots, deferred live draw, expiry wake, removal and wrapping passed");
    return 0;
}
