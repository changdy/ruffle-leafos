/* Real launcher handlers and rendering; the daemon boundary is a disposable fake. */
static char delete_resume_path[4096];
#define JW_RESUME_PATH delete_resume_path
#define jw_ipc_delete_preview test_delete_preview
#define jw_ipc_delete_begin test_delete_begin
#define jw_ipc_delete_poll test_delete_poll
#define jw_ipc_delete_close test_delete_close
#define jw_ipc_get_storage_status test_storage_status
#define jw_storage_ui_show_warning test_storage_warning
#define main jw_launcher_main
#include "cmd/jawaka-launcher/main.c"
#undef main
#include <assert.h>

static int previews, commits, closes, warnings;
static jw_ipc_delete_phase preview_phase = JW_IPC_DELETE_READY;
static int preview_rc;
static jw_ipc_delete_phase commit_phase = JW_IPC_DELETE_COMMITTING;
static int commit_rc;
static char readonly_source[32];
static char warning_source[32];

static void fixture_status(jw_ipc_delete_status *status, jw_ipc_delete_phase phase) {
    jw_ipc_delete_status_free(status);
    status->phase = phase;
    if (phase == JW_IPC_DELETE_READY) snprintf(status->digest, sizeof(status->digest), "0123456789abcdef");
    snprintf(status->name, sizeof(status->name), "Synthetic Adventure");
    snprintf(status->source_id, sizeof(status->source_id), "primary");
    snprintf(status->readonly_source, sizeof(status->readonly_source), "%s", readonly_source);
    if (phase == JW_IPC_DELETE_ERROR) snprintf(status->error, sizeof(status->error), "Your game's files changed. Open Delete Game again.");
    status->bytes = 12288;
    status->files_count = 3;
    status->files = calloc(status->files_count, sizeof(*status->files));
    assert(status->files);
    for (size_t i = 0; i < status->files_count; ++i) {
        snprintf(status->files[i].source_id, sizeof(status->files[i].source_id), "primary");
        snprintf(status->files[i].rom_relpath, sizeof(status->files[i].rom_relpath),
                 i == 2 ? "PS/Adventure.m3u" : "PS/Adventure/Disc %zu.chd", i + 1);
        status->files[i].size = 4096;
    }
}

int test_delete_preview(const char *socket, const char *source, const char *relative,
                        jw_ipc_delete_status *status) {
    (void)socket;
    assert(!strcmp(source, "primary") && !strcmp(relative, "PS/Adventure.m3u"));
    ++previews;
    fixture_status(status, preview_phase);
    return preview_rc;
}

int test_delete_begin(const char *socket, const char *source, const char *relative,
                      const char *digest, jw_ipc_delete_session **session,
                      jw_ipc_delete_status *status) {
    (void)socket;
    assert(!strcmp(source, "primary") && !strcmp(relative, "PS/Adventure.m3u"));
    assert(!strcmp(digest, "0123456789abcdef")); /* the reviewed file set, never a path list */
    ++commits;
    *session = (jw_ipc_delete_session *)(uintptr_t)1;
    fixture_status(status, commit_phase);
    return commit_rc;
}

int test_delete_poll(jw_ipc_delete_session *session, jw_ipc_delete_status *status) {
    assert(session);
    fixture_status(status, JW_IPC_DELETE_DONE);
    status->removed_count = 2;
    status->files[0].removed = status->files[1].removed = true;
    return 0;
}

void test_delete_close(jw_ipc_delete_session *session) { assert(session); ++closes; }

int test_storage_status(const char *socket, const char *source,
                          jw_ipc_storage_status_info *card, char *message, int size) {
    (void)socket; (void)message; (void)size;
    memset(card, 0, sizeof(*card));
    snprintf(card->source, sizeof(card->source), "%s", source);
    snprintf(card->access, sizeof(card->access), "read-only");
    return 0;
}

bool test_storage_warning(const char *socket, const jw_ipc_storage_status_info *card) {
    (void)socket;
    ++warnings;
    snprintf(warning_source, sizeof(warning_source), "%s", card->source);
    return false;
}

static void screenshot(const char *variable) {
    const char *path = getenv(variable);
    if (!path || !path[0]) return;
    SDL_Surface *pixels = SDL_CreateRGBSurfaceWithFormat(0, cat_get_screen_width(),
        cat_get_screen_height(), 32, SDL_PIXELFORMAT_RGBA32);
    assert(pixels && SDL_RenderReadPixels(cat_get_renderer(), NULL, pixels->format->format,
                                         pixels->pixels, pixels->pitch) == 0);
    assert(IMG_SavePNG(pixels, path) == 0);
    SDL_FreeSurface(pixels);
}

/* Opens Delete Game and lets the next tick fetch the preview. */
static void open_preview(jw_launcher_state *state, const jw_game_entry *game, bool *running) {
    int before = previews;
    jw__delete_begin(state, game);
    assert(state->delete_open && !state->delete_confirm && previews == before);
    assert(state->delete_status.phase == JW_IPC_DELETE_PREPARING);
    jw__render_launcher(state); /* "Checking your game's files..." while jawakad works */
    state->delete_next_poll = SDL_GetTicks();
    jw__delete_tick(state, running);
    assert(previews == before + 1);
}

static void attach_catalog(jw_launcher_state *state) {
    jw_ra_catalog_free(state->system_catalog);
    state->system_catalog = calloc(1, sizeof(*state->system_catalog));
    assert(state->system_catalog);
    state->system_catalog->systems = calloc(1, sizeof(*state->system_catalog->systems));
    assert(state->system_catalog->systems);
    state->system_catalog->system_count = 1;
    state->system_catalog->systems[0].id = strdup("PS");
    state->system_catalog->systems[0].name = strdup("PlayStation");
}

/* Back can arrive before the regular frame tick after a quick result; it must
   still close Options and refresh the library. */
static void check_quick_result_back(jw_launcher_state *state, const jw_game_entry *game) {
    attach_catalog(state);
    state->actions_open = true;
    state->action_scope = JW_ACTION_GAME;
    state->action_game = *game;
    commit_phase = JW_IPC_DELETE_COMMITTING;
    bool running = true;
    open_preview(state, game, &running);
    int before = commits;
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == before + 1 && state->delete_status.phase == JW_IPC_DELETE_COMMITTING);
    assert(test_delete_poll(state->delete_session, &state->delete_status) == 0);
    assert(state->delete_status.phase == JW_IPC_DELETE_DONE && !state->delete_reconciled);
    jw__handle_delete_input(state, CAT_BTN_B);
    assert(state->delete_reconciled && !state->delete_open && !state->actions_open);
    jw_ra_catalog_free(state->system_catalog);
    state->system_catalog = NULL;
}

int main(void) {
    char root[] = "/tmp/jw-delete-ui-XXXXXX";
    assert(mkdtemp(root));
    snprintf(delete_resume_path, sizeof(delete_resume_path), "%s/resume", root);
    setenv("SDCARD_PATH", root, 1);
    setenv("UMRK_INTERNAL_DATA_PATH", root, 1);
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("CAT_THEMES_DIR", "res/themes", 1);
    setenv("CAT_THEME_NAME", "Jawaka-Tabs", 1);
    char font[PATH_MAX];
    snprintf(font, sizeof(font), "%s/fonts/Nunito/Nunito-Bold.ttf", getenv("CAT_FONTS_DIR"));
    cat_config config = {.start_hidden = true, .defer_input_init = true,
                         .disable_background = true, .font_path = font};
    assert(cat_init(&config) == CAT_OK);
    jw_launcher_state *state = calloc(1, sizeof(*state));
    assert(state);
    g_present_state = state;
    state->settings.show_hints = true;
    state->haptics_muted = true;
    state->scan_ready = true;
    snprintf(state->db_path, sizeof(state->db_path), "%s/library.db", root);
    snprintf(state->settings.db_path, sizeof(state->settings.db_path), "%s", state->db_path);
    snprintf(state->sdcard_root, sizeof(state->sdcard_root), "%s", root);
    sqlite3 *db = NULL;
    assert(jw_db_open(state->db_path, &db) == 0 && jw_db_apply_schema(db) == 0);
    jw_db_close(db);
    jw_ra_system system = {.id = "PS"};
    jw_ra_catalog catalog = {.systems = &system, .system_count = 1};
    state->system_catalog = &catalog;
    jw_game_entry game = {.id = 42};
    snprintf(game.name, sizeof(game.name), "Synthetic Adventure");
    snprintf(game.source_id, sizeof(game.source_id), "primary");
    snprintf(game.rom_relpath, sizeof(game.rom_relpath), "PS/Adventure.m3u");
    state->action_scope = JW_ACTION_GAME;
    state->action_game = game;
    const struct { const char *id; bool eligible; } cases[] = {
        {"PS", true}, {"SEGACD", true}, {"GBA", true}, {"DOS", true}, {"AMIGA", true},
        {"DC", true}, {"PORTS", false}, {"ARCADE", false}, {"NEOGEO", false}, {"NAOMI", false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        system.id = (char *)cases[i].id;
        snprintf(state->action_game.system, sizeof(state->action_game.system), "%s", system.id);
        jw__action_refresh_rows(state);
        assert(state->action_rows[state->action_row_count - 1] ==
            (cases[i].eligible ? JW_ACTION_ROW_DELETE : JW_ACTION_ROW_HIDE));
    }
    system.id = "PS";
    system.provider = "mlp1/Custom.pak";
    snprintf(state->action_game.system, sizeof(state->action_game.system), "PS");
    jw__action_refresh_rows(state);
    assert(state->action_rows[state->action_row_count - 1] == JW_ACTION_ROW_HIDE);
    system.provider = NULL;
    snprintf(game.system, sizeof(game.system), "PS");

    state->action_core_count = 2;
    state->action_bios_supported = true;
    jw__action_refresh_rows(state);
    assert(state->action_row_count == 8);
    assert(state->action_rows[6] == JW_ACTION_ROW_HIDE &&
           state->action_rows[7] == JW_ACTION_ROW_DELETE);
    cat_list_state_jump(&state->action_list, 7, 8);
    jw__render_actions(state);
    assert(state->action_list.scroll_offset > 0 &&
           state->action_list.cursor < state->action_list.scroll_offset + state->action_list.visible_rows);

    /* Cancel is selected first. */
    bool running = true;
    open_preview(state, &game, &running);
    assert(state->delete_status.phase == JW_IPC_DELETE_READY && state->delete_status.files_count == 3);
    jw__render_launcher(state);
    screenshot("JW_DELETE_SCREENSHOT");
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(!state->delete_open && !commits);

    /* Files lists every file; A does nothing there. */
    open_preview(state, &game, &running);
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_X);
    assert(state->delete_files_open && !state->delete_confirm);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(!commits);
    assert(cat_set_font_bump(5) == CAT_OK);
    jw__render_launcher(state);
    screenshot("JW_DELETE_FILES_SCREENSHOT");
    assert(!SDL_RenderIsClipEnabled(cat_get_renderer()));
    assert(cat_set_font_bump(0) == CAT_OK);
    jw__handle_delete_input(state, CAT_BTN_B);
    assert(state->delete_open && !state->delete_files_open && !state->delete_confirm);
    int plain = jw__delete_body(state, 0, 0, cat_get_screen_width() - CAT_S(36), false);
    state->delete_status.writable_progress = true;
    int warned = jw__delete_body(state, 0, 0, cat_get_screen_width() - CAT_S(36), false);
    assert(warned > plain);

    /* Confirm sends the digest; nothing repeats while removal runs. */
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 1 && state->delete_status.phase == JW_IPC_DELETE_COMMITTING);
    jw__render_launcher(state);
    jw__handle_delete_input(state, CAT_BTN_A);
    jw__handle_delete_input(state, CAT_BTN_B);
    assert(commits == 1 && state->delete_open);
    state->delete_reconciled = true; /* cache reconciliation has its own disposable DB test */
    state->delete_next_poll = 0;
    jw__delete_tick(state, &running);
    assert(state->delete_status.phase == JW_IPC_DELETE_DONE && state->delete_status.removed_count == 2);
    jw__render_launcher(state);
    screenshot("JW_DELETE_DONE_SCREENSHOT");
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 1);
    jw__handle_delete_input(state, CAT_BTN_B);
    assert(!state->delete_open && closes == 1);

    /* A refused delete (changed files) is never retried from the same screen. */
    commit_phase = JW_IPC_DELETE_ERROR;
    open_preview(state, &game, &running);
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 2 && state->delete_status.phase == JW_IPC_DELETE_ERROR && state->delete_result_known);
    jw__render_launcher(state);
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 2);
    state->delete_reconciled = true;
    jw__handle_delete_input(state, CAT_BTN_B);

    /* A lost result can't be replayed either. */
    commit_rc = -1;
    open_preview(state, &game, &running);
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 3 && !state->delete_result_known);
    jw__render_launcher(state);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 3);
    state->delete_reconciled = true;
    jw__handle_delete_input(state, CAT_BTN_B);
    commit_rc = 0;
    commit_phase = JW_IPC_DELETE_COMMITTING;

    /* Back before the preview arrives asks jawakad nothing. */
    jw__delete_begin(state, &game);
    jw__handle_delete_input(state, CAT_BTN_B);
    assert(!state->delete_open && previews == 4);

    /* A read-only card opens the storage warning instead of a preview. */
    preview_phase = JW_IPC_DELETE_ERROR;
    snprintf(readonly_source, sizeof(readonly_source), "secondary_sd");
    open_preview(state, &game, &running);
    assert(!state->delete_open && warnings == 1 && !strcmp(warning_source, "secondary_sd"));
    readonly_source[0] = '\0';
    /* An unreachable daemon shows the error and offers only Back. */
    preview_rc = -1;
    open_preview(state, &game, &running);
    assert(state->delete_open && state->delete_status.phase == JW_IPC_DELETE_ERROR);
    jw__handle_delete_input(state, CAT_BTN_RIGHT);
    jw__handle_delete_input(state, CAT_BTN_A);
    assert(commits == 3);
    jw__handle_delete_input(state, CAT_BTN_B);
    assert(!state->delete_open);
    preview_rc = 0;
    preview_phase = JW_IPC_DELETE_READY;

    /* Hidden Games only unhides; X does not open Delete Game. */
    state->settings.open = true;
    state->settings.screen = JW_SETTINGS_HIDDEN_GAMES;
    state->settings.hidden_games_count = 1;
    state->settings.hidden_games = calloc(1, sizeof(*state->settings.hidden_games));
    assert(state->settings.hidden_games);
    state->settings.hidden_games[0].rom.game = game;
    bool theme_changed = false;
    int before = previews;
    assert(jw__system_settings_input(state, &state->settings, CAT_BTN_X, &theme_changed));
    assert(previews == before && !state->delete_open && state->settings.hidden_games_count == 1);
    jw_settings_ui_close(&state->settings);
    state->system_catalog = NULL;
    check_quick_result_back(state, &game);
    g_present_state = NULL;
    unlink(state->db_path);
    free(state);
    cat_quit();
    unlink(delete_resume_path); /* the refresh path saves a resume breadcrumb */
    rmdir(root);
    puts("delete-ui-test: eligibility, preview on the next tick, Cancel default, file inspection, no replay, quick-result Back, read-only and Hidden Games passed");
    return 0;
}
