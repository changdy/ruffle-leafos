/* Exercise the actual resident-menu footer and input paths with a dummy
   renderer; only the footer sink and daemon Continue request are intercepted. */
#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"
#include <assert.h>

static cat_footer_item test_footer[3];
static int test_footer_count;
static int test_continue_count;
static int test_continue_result;

static void test_draw_footer(cat_footer_item *items, int count) {
    assert(count <= 3);
    memcpy(test_footer, items, (size_t)count * sizeof(*items));
    test_footer_count = count;
}

#define cat_draw_footer test_draw_footer
#define jw_ipc_retroarch_action test_retroarch_action
#define main jw_menu_main
#include "cmd/jawaka-menu/main.c"
#undef main
#undef jw_ipc_retroarch_action
#undef cat_draw_footer

int test_retroarch_action(const char *socket_path, const char *action,
                          int value, char *status, int status_len) {
    (void)socket_path;
    (void)status;
    (void)status_len;
    assert(strcmp(action, "continue") == 0 && value == 0);
    test_continue_count++;
    return test_continue_result;
}

int main(void) {
    char root[] = "/tmp/jw-game-switcher-XXXXXX";
    assert(mkdtemp(root));
    char db_path[PATH_MAX], absolute_rom[PATH_MAX];
    snprintf(db_path, sizeof(db_path), "%s/library.db", root);
    snprintf(absolute_rom, sizeof(absolute_rom), "%s/Roms/GB/First.gb", root);
    sqlite3 *db = NULL;
    assert(jw_db_open(db_path, &db) == 0);
    assert(jw_db_apply_schema(db) == 0);
    assert(jw_db_scan_begin(db) == 0);
    assert(jw_db_insert_game(db, "GB", "First", "Roms/GB/First.gb", NULL) == 0);
    assert(jw_db_insert_game_stable(db, "GB", "Second", "secondary", "GB/Second.gb",
                                  "/secondary/Roms/GB/Second.gb", "", "", "") == 0);
    jw_db_close(db);

    jw_game_entry first, second;
    assert(jw_db_get_game_by_rom_path(db_path, "Roms/GB/First.gb", &first) == 0);
    assert(jw_db_get_game_by_rom_path(db_path, "/secondary/Roms/GB/Second.gb", &second) == 0);
    jw_game_switcher sw;
    jw_game_switcher_reset(&sw, true, root, NULL);
    assert(jw_game_switcher_load(&sw, db_path) == 0);
    assert(sw.count == 0);
    jw_game_switcher_set_current(&sw, db_path, "GB", absolute_rom, "First", NULL);
    assert(sw.count == 1 && sw.current_index == 0 && sw.entries[0].id == -1);

    assert(jw_db_set_game_hidden(db_path, first.id, 1) == 0);
    assert(jw_game_switcher_load(&sw, db_path) == 0);
    jw_game_switcher_set_current(&sw, db_path, "GB", absolute_rom, "First", NULL);
    assert(jw_game_switcher_is_empty(&sw) && sw.current_index == -1);
    assert(jw_db_set_game_hidden(db_path, second.id, 1) == 0);
    jw_game_switcher_set_current(&sw, db_path, "GB", second.rom_path, "Second", NULL);
    assert(jw_game_switcher_is_empty(&sw));

    /* Files may disappear and be pruned while the process remains running. */
    assert(jw_db_open(db_path, &db) == 0);
    assert(sqlite3_exec(db, "DELETE FROM games WHERE source_id='secondary';",
                        NULL, NULL, NULL) == SQLITE_OK);
    jw_db_close(db);
    jw_game_switcher_set_current(&sw, db_path, "GB", second.rom_path, "Second", NULL);
    assert(jw_game_switcher_is_empty(&sw));
    jw_game_switcher_set_current(&sw, db_path, "GB", "Roms/GB/Unknown.gb", "Unknown", NULL);
    assert(jw_game_switcher_is_empty(&sw));
    jw_game_switcher_set_current(&sw, root, "GB", absolute_rom, "First", NULL);
    assert(jw_game_switcher_is_empty(&sw)); /* Database lookup failed. */

    /* An empty carousel still renders and accepts its independent Resume. */
    setenv("UMRK_INTERNAL_DATA_PATH", root, 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("CAT_THEMES_DIR", "res/themes", 1);
    setenv("CAT_THEME_NAME", "Jawaka-Tabs", 1);
    char font[PATH_MAX];
    snprintf(font, sizeof(font), "%s/fonts/Nunito/Nunito-Bold.ttf", getenv("CAT_FONTS_DIR"));
    cat_config config = { .start_hidden = true, .defer_input_init = true,
                         .disable_background = true, .font_path = font };
    assert(cat_init(&config) == CAT_OK);
    jw_ingame_state state = { .show_hints = true, .db_path = db_path };
    state.session.active = true;
    snprintf(state.session.rom_path, sizeof(state.session.rom_path), "%s", "Roms/GB/Unknown.gb");
    jw__render_ingame_switcher(&state, &sw);
    assert(test_footer_count == 1 && test_footer[0].button == CAT_BTN_B);
    assert(strcmp(test_footer[0].label, "Resume") == 0);
    bool running = true;
    jw__handle_ingame_switcher_input(NULL, db_path, &state, &sw, CAT_BTN_A, &running);
    assert(running && test_continue_count == 0);
    jw__handle_ingame_switcher_input(NULL, db_path, &state, &sw, CAT_BTN_B, &running);
    assert(!running && test_continue_count == 1);
    running = true;
    test_continue_result = -1;
    jw__handle_ingame_switcher_input(NULL, db_path, &state, &sw, CAT_BTN_B, &running);
    assert(running && test_continue_count == 2);
    cat_quit();

    /* Reload sees a direct Unhide without a scanner/generation update. */
    assert(jw_db_set_game_hidden(db_path, first.id, 0) == 0);
    assert(jw_db_record_play_by_id(db_path, first.id, 1) == 0);
    assert(jw_game_switcher_load(&sw, db_path) == 0 && sw.count == 1);
    jw_game_switcher_set_current(&sw, db_path, "GB", first.rom_path, "First", NULL);
    assert(sw.count == 1 && sw.current_index == 0 && sw.entries[0].id == first.id);
    assert(jw_db_set_game_hidden(db_path, first.id, 1) == 0);
    assert(jw_game_switcher_load(&sw, db_path) == 0 && sw.count == 0);
    jw_game_switcher_set_current(&sw, db_path, "GB", absolute_rom, "First", NULL);
    assert(jw_game_switcher_is_empty(&sw));

    unlink(db_path);
    rmdir(root);
    puts("game-switcher-test: hidden current games, reload, empty Resume hint/input passed");
    return 0;
}
