/* Exercise the actual launcher resolver and async decoder with SDL's dummy
   renderer. Keeping the test in the same TU avoids a second artwork loader. */
#define main jw_launcher_main
#include "cmd/jawaka-launcher/main.c"
#undef main
#include <assert.h>
#include <utime.h>

static void wm_dirs(const char *path) {
    char copy[PATH_MAX]; snprintf(copy, sizeof(copy), "%s", path);
    for (char *p = copy + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(copy, 0700); *p = '/'; }
    mkdir(copy, 0700);
}
static void wm_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb"); assert(f); fputs(text, f); fclose(f);
}
static void wm_png(const char *path, int width) {
    SDL_Surface *surface = SDL_CreateRGBSurfaceWithFormat(0, width, 16, 32, SDL_PIXELFORMAT_RGBA32);
    assert(surface); SDL_FillRect(surface, NULL, 0xffffffff);
    assert(!IMG_SavePNG(surface, path)); SDL_FreeSurface(surface);
}
static void wm_expect(jw_launcher_state *state, int width, bool color) {
    int w = 0, h = 0; SDL_Texture *texture = NULL; bool got_color = false;
    for (int i = 0; i < 150; i++) {
        texture = jw__gg_wordmark(state, &w, &h, &got_color);
        if (texture) break;
        SDL_Delay(10);
    }
    if ((width && (!texture || w != width || got_color != color)) || (!width && texture)) {
        fprintf(stderr, "wordmark: wanted width %d color %d, got %d color %d (texture %p)\n",
                width, color, w, got_color, (void *)texture);
        abort();
    }
}
/* A new generation is the only thing that re-resolves artwork memos. */
static void art_refresh(jw_launcher_state *state, jw_ra_catalog *catalog, const char *info_dir) {
    catalog->info_dir = (char *)info_dir;
    jw__adopt_art_catalog(state, catalog);
}
static void grid_expect(jw_launcher_state *state, int width) {
    int w = 0, h = 0; SDL_Texture *texture = NULL;
    for (int i = 0; i < 200; i++) {
        texture = jw__grid_icon(state, 0, &w, &h);
        if (texture) break;
        SDL_Delay(10);
    }
    if ((width && (!texture || w != width)) || (!width && texture)) {
        fprintf(stderr, "grid: wanted %d got %d\n", width, w); abort();
    }
}
static void grid_corrupt(const char *path) {
    unsigned char header[24]; FILE *f = fopen(path, "rb"); assert(f);
    assert(fread(header, 1, 24, f) == 24); fclose(f);
    f = fopen(path, "wb"); assert(f); assert(fwrite(header, 1, 24, f) == 24); fclose(f);
}
static void grid_test(jw_launcher_state *state, jw_ra_system *system,
                       jw_ra_catalog *catalog, const char *root) {
    cat_stylesheet *style = (cat_stylesheet *)cat_get_stylesheet();
    style->launcher.layout = CAT_LAUNCHER_GRID;
    state->system_count = 1; state->flat_count = 1;
    snprintf(state->systems[0].name, sizeof(state->systems[0].name), "TEST");
    state->flat_items[0] = (jw_flat_item){JW_FLAT_SYSTEM, 0};
    state->settings.system_icon_pack_index = JW_SYSTEM_ICON_PACK_PHOTOGRAPHIC;
    snprintf(state->settings.user_themes.items[0].dir, 128, "A");
    snprintf(state->settings.user_theme_dir, 128, "A");
    char theme[PATH_MAX], rom[PATH_MAX], grid[PATH_MAX], flat[PATH_MAX], photo[PATH_MAX], bundle[PATH_MAX], pack[PATH_MAX], dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/Themes/A/grid/icons", root); wm_dirs(dir);
    snprintf(dir, sizeof(dir), "%s/res/system_icons", root); wm_dirs(dir);
    snprintf(dir, sizeof(dir), "%s/res/themes/Jawaka-Coverflow/system_icons", root); wm_dirs(dir);
    snprintf(pack, sizeof(pack), "%s/TEST.png", dir);
    snprintf(theme, sizeof(theme), "%s/Themes/A/grid/icons/TEST.png", root);
    snprintf(rom, sizeof(rom), "%s/Roms/TEST/icon.png", root);
    snprintf(grid, sizeof(grid), "%s/Apps/mlp1/Test.pak/art/grid.png", root);
    snprintf(flat, sizeof(flat), "%s/Apps/mlp1/Test.pak/art/flat.png", root);
    snprintf(photo, sizeof(photo), "%s/Apps/mlp1/Test.pak/art/photo.png", root);
    snprintf(bundle, sizeof(bundle), "%s/res/system_icons/TEST.png", root);
    wm_png(theme,64); wm_png(rom,32); wm_png(grid,128); wm_png(flat,160); wm_png(photo,192); wm_png(bundle,256); wm_png(pack,288);
    system->grid_icon = "art/grid.png"; system->grid_icon_provider = "mlp1/Test.pak";
    system->provider = "mlp1/Test.pak"; system->icon_flat = "art/flat.png"; system->icon_photographic = "art/photo.png";
    int w,h; assert(jw__load_cached_image(bundle,&w,&h));
    assert(!jw__grid_icon(state,0,&w,&h)); /* cold theme must stay pending */
    jw__grid_prewarm_icons(state); grid_expect(state,64); /* theme > ROM > built-in pack > pak */
    unlink(theme); art_refresh(state,catalog,"grid-no-theme/info"); grid_expect(state,32);
    wm_file(rom,"bad PNG"); art_refresh(state,catalog,"grid-corrupt-rom/info"); grid_expect(state,288);
    grid_corrupt(pack); art_refresh(state,catalog,"grid-corrupt-pack/info"); grid_expect(state,128);
    grid_corrupt(grid); art_refresh(state,catalog,"grid-corrupt-pak/info"); grid_expect(state,192);
    grid_corrupt(photo); art_refresh(state,catalog,"grid-corrupt-photo/info"); grid_expect(state,160);
    grid_corrupt(flat); art_refresh(state,catalog,"grid-corrupt-flat/info"); grid_expect(state,256);
    wm_png(grid,128); art_refresh(state,catalog,"grid-repaired/info"); grid_expect(state,128);
    struct stat st; assert(!stat(grid,&st)); wm_png(grid,224);
    struct utimbuf same = {.actime=st.st_atime,.modtime=st.st_mtime}; assert(!utime(grid,&same));
    art_refresh(state,catalog,"grid-two/info"); grid_expect(state,224); /* generation alone invalidates */
    system->grid_icon_provider = NULL; art_refresh(state,catalog,"grid-three/info"); grid_expect(state,256);
    system->provider = NULL; system->grid_icon_provider = "mlp1/Test.pak";
    wm_png(grid,1025); art_refresh(state,catalog,"grid-four/info"); grid_expect(state,256); /* over cap */
    wm_png(grid,1024); art_refresh(state,catalog,"grid-five/info"); grid_expect(state,320); /* 320px decode */
    /* Reloading the same generation (an ordinary settings write) keeps the
       warm texture and memo; only a new identity or a lost catalog drops them. */
    unsigned epoch = state->art_epoch;
    jw__adopt_art_catalog(state, catalog);
    assert(state->art_epoch == epoch && cat_cache_get(grid, NULL, NULL));
    jw__adopt_art_catalog(state, NULL);
    assert(state->art_epoch == epoch + 1 && !cat_cache_get(grid, NULL, NULL));
    art_refresh(state,catalog,"grid-five/info"); grid_expect(state,320);
    assert(system->provider == NULL); /* base-owned extension decorates independently */
    snprintf(dir,sizeof(dir),"%s/Themes/B/grid/icons",root); wm_dirs(dir);
    snprintf(theme,sizeof(theme),"%s/Themes/B/grid/icons/TEST.png",root); wm_png(theme,96);
    snprintf(state->settings.user_themes.items[0].dir,128,"B");
    snprintf(state->settings.user_theme_dir,128,"B"); grid_expect(state,96); /* same index, new theme */
    /* Candidate order by consumer: pak grid art and the built-in pack ahead of
       pak art only for Grid tiles; Grid Search and Cover Flow as before. */
    system->provider = "mlp1/Test.pak";
    jw_system_icon_candidates candidates;
    int at_grid, at_photo, at_pack;
    #define ICON_AT(path, out) do { out = -1; for (int i=0;i<candidates.count;i++) if (!strcmp(candidates.paths[i],path)) out = i; } while (0)
    jw__build_system_icon_candidates(state,"TEST",true,&candidates);
    ICON_AT(grid,at_grid); ICON_AT(photo,at_photo); ICON_AT(pack,at_pack);
    assert(at_pack >= 0 && at_pack < at_grid && at_grid < at_photo);
    int at_flat_pack; /* an explicit Flat pack precedes pak art on a Grid tile too */
    state->settings.system_icon_pack_index = JW_SYSTEM_ICON_PACK_FLAT;
    char flat_pack[PATH_MAX];
    snprintf(flat_pack, sizeof(flat_pack), "%s/../system_icons/TEST.png", cat_get_active_theme_dir());
    jw__build_system_icon_candidates(state,"TEST",true,&candidates);
    ICON_AT(grid,at_grid); ICON_AT(flat_pack,at_flat_pack);
    assert(at_flat_pack >= 0 && at_flat_pack < at_grid);
    state->settings.system_icon_pack_index = JW_SYSTEM_ICON_PACK_PHOTOGRAPHIC;
    jw__build_system_icon_candidates(state,"TEST",false,&candidates); /* Grid Search */
    ICON_AT(grid,at_grid); ICON_AT(photo,at_photo); ICON_AT(pack,at_pack);
    assert(at_grid < 0 && at_photo >= 0 && at_photo < at_pack);
    style->launcher.layout = CAT_LAUNCHER_COVERFLOW;
    jw__build_system_icon_candidates(state,"TEST",true,&candidates);
    ICON_AT(grid,at_grid); ICON_AT(photo,at_photo); ICON_AT(pack,at_pack);
    assert(at_grid < 0 && at_photo >= 0 && at_photo < at_pack);
    #undef ICON_AT
    style->launcher.layout = CAT_LAUNCHER_GRID;
    system->provider = NULL;
    unlink(theme); unlink(rom); unlink(grid); unlink(bundle); unlink(pack);
    art_refresh(state,catalog,"grid-six/info"); grid_expect(state,0);
    puts("PASS grid-icon UI: priorities, pending/failure, independent provider, generation replacement/removal, dimensions, theme switch, same-generation reload, unchanged Cover Flow");
}
int main(void) {
    char root[] = "/tmp/jw-wordmark-XXXXXX"; assert(mkdtemp(root));
    char path[PATH_MAX], font[PATH_MAX];
    snprintf(path, sizeof(path), "%s/res/themes/Test", root); wm_dirs(path);
    snprintf(path, sizeof(path), "%s/res/themes/Test/stylesheet.json", root); wm_file(path, "{}");
    snprintf(path, sizeof(path), "%s/res/themes", root); setenv("CAT_THEMES_DIR", path, 1);
    setenv("CAT_THEME_NAME", "Test", 1);
    snprintf(path, sizeof(path), "%s/state", root); wm_dirs(path); setenv("UMRK_INTERNAL_DATA_PATH", path, 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    snprintf(font, sizeof(font), "%s/fonts/Nunito/Nunito-Bold.ttf", getenv("CAT_FONTS_DIR"));
    cat_config config = {.start_hidden = true, .defer_input_init = true,
                         .disable_background = true, .font_path = font};
    assert(cat_init(&config) == CAT_OK);
    jw_launcher_state *state = calloc(1, sizeof(*state)); assert(state);
    snprintf(state->sdcard_root, sizeof(state->sdcard_root), "%s", root);
    snprintf(state->game_system, sizeof(state->game_system), "TEST");
    state->settings.user_theme_index = 0;
    state->settings.user_themes.count = 2;
    snprintf(state->settings.user_theme_dir, sizeof(state->settings.user_theme_dir), "A");
    snprintf(state->settings.user_themes.root, PATH_MAX, "%s/Themes", root);
    snprintf(state->settings.user_themes.items[0].dir, 128, "A");
    snprintf(state->settings.user_themes.items[1].dir, 128, "B");
    const char *dirs[] = {"Roms/TEST", "Themes/A/grid/wordmarks", "Themes/B/grid/wordmarks", "Apps/mlp1/Test.pak/art", "res/grid_wordmarks"};
    for (size_t i = 0; i < sizeof(dirs)/sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", root, dirs[i]); wm_dirs(path);
    }
    char rom[PATH_MAX], theme[PATH_MAX], provider[PATH_MAX], bundled[PATH_MAX];
    char rom_color[PATH_MAX], theme_color[PATH_MAX], provider_color[PATH_MAX], bundled_color[PATH_MAX];
    snprintf(rom, sizeof(rom), "%s/Roms/TEST/wordmark.png", root);
    snprintf(rom_color, sizeof(rom_color), "%s/Roms/TEST/wordmark.color.png", root);
    snprintf(theme, sizeof(theme), "%s/Themes/A/grid/wordmarks/TEST.png", root);
    snprintf(theme_color, sizeof(theme_color), "%s/Themes/A/grid/wordmarks/TEST.color.png", root);
    snprintf(provider, sizeof(provider), "%s/Apps/mlp1/Test.pak/art/mark.png", root);
    snprintf(provider_color, sizeof(provider_color), "%s/Apps/mlp1/Test.pak/art/mark.color.png", root);
    snprintf(bundled, sizeof(bundled), "%s/res/grid_wordmarks/TEST.png", root);
    snprintf(bundled_color, sizeof(bundled_color), "%s/res/grid_wordmarks/TEST.color.png", root);
    wm_png(rom_color, 40); wm_png(rom, 32); wm_png(theme_color, 72); wm_png(theme, 64);
    wm_png(provider_color, 136); wm_png(provider, 128); wm_png(bundled_color, 264); wm_png(bundled, 256);
    snprintf(path, sizeof(path), "%s/Apps", root); setenv("APPS_PATH", path, 1);
    jw_ra_system system = {.id = "TEST", .name = "Test system", .wordmark = "art/mark.png", .wordmark_provider = "mlp1/Test.pak",
                           .wordmark_color = "art/mark.color.png", .wordmark_color_provider = "mlp1/Test.pak"};
    jw_ra_catalog catalog = {.sdcard_root = root, .systems = &system, .system_count = 1};
    state->system_catalog = &catalog;
    art_refresh(state, &catalog, "gen-one/info");
    /* A cold higher-priority candidate stays pending even with lower art ready. */
    int w, h; bool color; assert(!jw__gg_wordmark(state, &w, &h, &color));
    /* Each source offers its full-color mark first, then the tinted one. */
    wm_expect(state, 40, true);
    /* Over-cap art is refused before decode, from the ROM folder and the pak alike. */
    wm_png(rom_color, 1025); art_refresh(state, &catalog, "gen-cap/info"); wm_expect(state, 32, false);
    unlink(rom_color); unlink(rom); art_refresh(state, &catalog, "gen-no-rom/info"); wm_expect(state, 72, true);
    unlink(theme_color); art_refresh(state, &catalog, "gen-no-theme-color/info"); wm_expect(state, 64, false);
    /* Same index, different folder: theme identity must invalidate the memo. */
    snprintf(path, sizeof(path), "%s/Themes/B/grid/wordmarks/TEST.png", root); wm_png(path, 96);
    snprintf(state->settings.user_themes.items[0].dir, 128, "B");
    snprintf(state->settings.user_theme_dir, 128, "B"); wm_expect(state, 96, false);
    /* A highest-priority PNG with a sound header but a bad body advances after
       its async decode failure. */
    snprintf(state->settings.user_themes.items[0].dir, 128, "A");
    snprintf(state->settings.user_theme_dir, 128, "A");
    grid_corrupt(theme); wm_png(provider_color, 1025);
    art_refresh(state, &catalog, "gen-two/info"); wm_expect(state, 128, false);
    wm_png(provider_color, 136); art_refresh(state, &catalog, "gen-color/info"); wm_expect(state, 136, true);
    system.wordmark_color_provider = NULL; art_refresh(state, &catalog, "gen-no-color/info"); wm_expect(state, 128, false);
    assert(system.provider == NULL); /* artwork doesn't assign system ownership */
    /* Same-path, same-mtime provider replacement changes only catalog identity. */
    struct stat st; assert(!stat(provider, &st));
    wm_png(provider, 192); struct utimbuf times = {.actime = st.st_atime, .modtime = st.st_mtime};
    assert(!utime(provider, &times)); art_refresh(state, &catalog, "gen-three/info"); wm_expect(state, 192, false);
    /* An ordinary reload of the same generation keeps the memo and texture. */
    unsigned epoch = state->art_epoch; jw__adopt_art_catalog(state, &catalog);
    assert(state->art_epoch == epoch && cat_cache_get(provider, NULL, NULL)); wm_expect(state, 192, false);
    /* Removed provider, then undecodable bundle: the text branch receives NULL. */
    system.wordmark_provider = NULL; art_refresh(state, &catalog, "gen-no-provider/info"); wm_expect(state, 264, true);
    unlink(bundled_color); art_refresh(state, &catalog, "gen-no-bundled-color/info"); wm_expect(state, 256, false);
    wm_file(bundled, "bad PNG"); art_refresh(state, &catalog, "gen-four/info"); wm_expect(state, 0, false);
    /* Reinstall after failures and a 1024px mark: decode cap remains 512px. */
    system.wordmark_provider = "mlp1/Test.pak"; wm_png(provider, 1024);
    art_refresh(state, &catalog, "gen-five/info"); wm_expect(state, 512, false);
    jw_cover_loader_shutdown(jw__covers());
    cat_cache_clear();
    /* A full/unwritable thumbnail directory still returns the decoded surface. */
    snprintf(path, sizeof(path), "%s/missing/thumb.png", root);
    SDL_Texture *texture = NULL; bool failed = false;
    for (int i = 0; i < 150 && !texture && !failed; i++) {
        texture = jw__load_page_image_status(provider, JW_WORDMARK_MAX, &w, &h, path, &failed);
        SDL_Delay(10);
    }
    assert(texture && !failed && w == 512);
    jw_cover_loader_shutdown(jw__covers());
    grid_test(state, &system, &catalog, root);
    jw_cover_loader_shutdown(jw__covers());
    state->system_catalog = NULL; free(state); cat_quit();
    puts("PASS wordmark-test: color/tinted precedence, dimension cap, pending/failure, theme switch, replacement, same-generation reload, removal, decode cap");
    return 0;
}
