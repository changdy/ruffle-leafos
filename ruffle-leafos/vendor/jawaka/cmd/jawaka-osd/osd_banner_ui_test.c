/* Banner text through the real font, layout and translation code, under SDL's
   dummy driver. Pixel placement on the device is Wayland's; this checks what
   both backends share: bounds, rows and that the action survives shortening. */
#include "cmd/jawaka-osd/osd_text.h"
#include "cmd/jawaka-osd/osd_utf8.h"
#include "internal/i18n/i18n.h"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OUTPUT_W 960
#define OUTPUT_H 720

static const jw_osd_game_stage kStages[] = {
    JW_OSD_GAME_CHECKING, JW_OSD_GAME_SYNCING, JW_OSD_GAME_STOPPING,
    JW_OSD_GAME_SETTINGS_NOT_SAVED, JW_OSD_GAME_STORAGE_READ_ONLY,
    JW_OSD_PICO8_EXIT_CONFIRM, JW_OSD_PICO8_IMPORT, JW_OSD_PICO8_IMPORT_FAILED,
    JW_OSD_POWER_SAVE_RELEASE, JW_OSD_POWER_SAVE_SAVING, JW_OSD_POWER_SAVE_SAVED,
    JW_OSD_POWER_SAVE_FAILED, JW_OSD_BOOT_RESUME,
};

static void assert_inside(const jw_osd_rect *box, int x, int y, const SDL_Surface *row) {
    assert(row);
    assert(x >= box->x && y >= box->y);
    assert(x + row->w <= box->x + box->w);
    assert(y + row->h <= box->y + box->h);
}

static void dump(const char *tag, jw_osd_game_stage stage, const jw_osd_banner *banner) {
    const char *dir = getenv("JAWAKA_OSD_BANNER_DUMP");
    if (!dir || !dir[0]) return;
    SDL_Surface *out = SDL_CreateRGBSurfaceWithFormat(0, OUTPUT_W, OUTPUT_H, 32,
                                                      SDL_PIXELFORMAT_ARGB8888);
    assert(out);
    SDL_FillRect(out, NULL, SDL_MapRGB(out->format, 0x60, 0x70, 0x80));
    const jw_osd_rect *b = &banner->layout.box;
    SDL_Rect outer = { b->x, b->y, b->w, b->h };
    SDL_FillRect(out, &outer, SDL_MapRGB(out->format, 0x7E, 0x95, 0x79));
    SDL_Rect inner = { b->x + 1, b->y + 1, b->w - 2, b->h - 2 };
    SDL_FillRect(out, &inner, SDL_MapRGB(out->format, 0x0F, 0x16, 0x0E));
    SDL_Rect title = { banner->layout.title_x, banner->layout.title_y, 0, 0 };
    SDL_BlitSurface(banner->title, NULL, out, &title);
    if (banner->action) {
        SDL_Rect action = { banner->layout.action_x, banner->layout.action_y, 0, 0 };
        SDL_BlitSurface(banner->action, NULL, out, &action);
    }
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s-%s.bmp", dir, tag, jw_osd_game_stage_name(stage));
    assert(SDL_SaveBMP(out, path) == 0);
    SDL_FreeSurface(out);
}

static void check_all(const char *tag) {
    for (size_t i = 0; i < sizeof(kStages) / sizeof(kStages[0]); i++) {
        jw_osd_banner banner;
        assert(jw_osd_banner_render(kStages[i], 3, OUTPUT_W, OUTPUT_H, &banner) == 0);
        const jw_osd_rect *box = &banner.layout.box;
        assert(box->x >= 0 && box->y >= 0);
        assert(box->x + box->w <= OUTPUT_W && box->y + box->h <= OUTPUT_H);
        assert(box->w <= banner.metrics.max_w);
        assert(box->y + box->h == OUTPUT_H - banner.metrics.inset);
        assert_inside(box, banner.layout.title_x, banner.layout.title_y, banner.title);

        char title[256];
        char action[128];
        jw_osd_game_launch_text(kStages[i], 3, title, sizeof(title), action, sizeof(action));
        assert(!action[0] == !banner.action);
        if (banner.action) {
            assert_inside(box, banner.layout.action_x, banner.layout.action_y, banner.action);
            assert(banner.layout.action_y >= banner.layout.title_y + banner.title->h);
        }
        dump(tag, kStages[i], &banner);
        jw_osd_banner_free(&banner);
    }
}

static int action_width(jw_osd_game_stage stage) {
    jw_osd_banner banner;
    assert(jw_osd_banner_render(stage, 0, OUTPUT_W, OUTPUT_H, &banner) == 0);
    int w = banner.action ? banner.action->w : 0;
    jw_osd_banner_free(&banner);
    return w;
}

int main(void) {
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    const char *fonts = getenv("CAT_FONTS_DIR");
    assert(fonts && fonts[0]);
    char root[] = "/tmp/jw-osd-banner-XXXXXX";
    assert(mkdtemp(root));
    setenv("UMRK_INTERNAL_DATA_PATH", root, 1);
    setenv("UMRK_PLATFORM_PATH", root, 1);
    setenv("CAT_FONT_PATH", "fonts/Nunito/Nunito-Bold.ttf", 1);
    setenv("CAT_FONT_BUMP", "5", 1);   /* the largest setting */
    assert(SDL_Init(0) == 0);
    assert(jw_osd_text_init(OUTPUT_W) == 0 && jw_osd_text_ready());
    check_all("en");
    int english_action = action_width(JW_OSD_PICO8_EXIT_CONFIRM);
    assert(english_action > 0);

    /* A translation far wider than the screen shortens the question and keeps
       the action intact. */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/i18n", root);
    assert(mkdir(dir, 0755) == 0);
    snprintf(dir, sizeof(dir), "%s/i18n/zz.tsv", root);
    FILE *fp = fopen(dir, "w");
    assert(fp);
    fputs("Return to Leaf?\tReturn to the Leaf launcher now and leave this game "
          "behind, closing PICO-8 and everything it is doing right now?\n", fp);
    fclose(fp);
    assert(jw_i18n_load("zz"));
    jw_osd_banner banner;
    assert(jw_osd_banner_render(JW_OSD_PICO8_EXIT_CONFIRM, 0, OUTPUT_W, OUTPUT_H, &banner) == 0);
    /* Shortened by whole characters, so within one character of the limit. */
    assert(banner.layout.box.w <= banner.metrics.max_w);
    assert(banner.title->w <= jw_osd_banner_text_max_w(&banner.metrics));
    assert(banner.title->w > jw_osd_banner_text_max_w(&banner.metrics) - banner.metrics.font_px * 2);
    assert(banner.action && banner.action->w == english_action);
    dump("long", JW_OSD_PICO8_EXIT_CONFIRM, &banner);
    jw_osd_banner_free(&banner);
    jw_i18n_shutdown();

    /* Chinese through the CJK face when the checkout has it. */
    char cjk[1024];
    snprintf(cjk, sizeof(cjk), "%s/fonts/SourceHanSansCN/SourceHanSansCN-Regular.otf", fonts);
    struct stat st;
    if (stat(cjk, &st) == 0) {
        jw_osd_text_shutdown();
        setenv("CAT_FONT_PATH", "fonts/SourceHanSansCN/SourceHanSansCN-Regular.otf", 1);
        snprintf(dir, sizeof(dir), "%s/i18n/zh_CN.tsv", root);
        fp = fopen(dir, "w");
        assert(fp);
        fputs("Return to Leaf?\t返回 Leaf？\nPress Menu again\t再次按菜单键\n"
              "Syncthing: Syncing %d items\tSyncthing：正在同步 %d 项\n"
              "Menu: Start now\t菜单键：立即开始\n", fp);
        fclose(fp);
        assert(jw_i18n_load("zh_CN"));
        assert(jw_osd_text_init(OUTPUT_W) == 0);
        check_all("zh");
        jw_i18n_shutdown();
    } else {
        puts("SKIP zh banners: no SourceHanSansCN under CAT_FONTS_DIR");
    }

    jw_osd_text_shutdown();
    SDL_Quit();
    puts("PASS osd-banner-ui-test");
    return 0;
}
