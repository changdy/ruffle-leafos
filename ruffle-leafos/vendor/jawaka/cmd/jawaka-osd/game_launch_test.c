#include "cmd/jawaka-osd/game_launch.h"
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

static cJSON *parse(const char *json) {
    cJSON *root = cJSON_Parse(json);
    assert(root);
    return root;
}

static void expect_text(jw_osd_game_stage stage, int pending, const char *want_title,
                        const char *want_action) {
    char title[128];
    char action[128];
    jw_osd_game_launch_text(stage, pending, title, sizeof(title), action, sizeof(action));
    if (strcmp(title, want_title) != 0 || strcmp(action, want_action) != 0) {
        fprintf(stderr, "stage=%s got=%s|%s want=%s|%s\n", jw_osd_game_stage_name(stage),
                title, action, want_title, want_action);
        assert(0);
    }
}

static void expect_valid(const char *json, jw_osd_game_stage want_stage,
                         int want_pending, uint64_t want_expires) {
    cJSON *root = parse(json);
    jw_osd_game_stage stage;
    int pending = -1;
    uint64_t expires = 99;
    assert(jw_osd_game_launch_parse(root, &stage, &pending, &expires));
    assert(stage == want_stage);
    assert(pending == want_pending);
    assert(expires == want_expires);
    assert(strcmp(jw_osd_game_stage_name(stage),
                  cJSON_GetObjectItemCaseSensitive(root, "stage")->valuestring) == 0);
    cJSON_Delete(root);
}

static void expect_invalid(const char *json) {
    cJSON *root = parse(json);
    jw_osd_game_stage stage;
    int pending = 0;
    uint64_t expires = 0;
    assert(!jw_osd_game_launch_parse(root, &stage, &pending, &expires));
    cJSON_Delete(root);
}

static bool valid_utf8(const char *s) {
    return jw_osd_utf8_boundary(s, strlen(s)) == strlen(s);
}

static void parser(void) {
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"pico8-import\"}",
                 JW_OSD_PICO8_IMPORT, 0, 0);
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"pico8-import-failed\"}",
                 JW_OSD_PICO8_IMPORT_FAILED, 0, 0);
    assert(!JW_OSD_GAME_STAGE_IS_TRANSIENT(JW_OSD_PICO8_IMPORT));
    assert(JW_OSD_GAME_STAGE_IS_TRANSIENT(JW_OSD_PICO8_IMPORT_FAILED));
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"pico8-exit\"}",
                 JW_OSD_PICO8_EXIT_CONFIRM, 0, 0);
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"pico8-exit\",\"expires_ms\":123456789}",
                 JW_OSD_PICO8_EXIT_CONFIRM, 0, 123456789u);
    assert(JW_OSD_GAME_STAGE_IS_TRANSIENT(JW_OSD_PICO8_EXIT_CONFIRM));
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"pico8-exit\",\"pending_items\":0}");
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"checking\"}",
                 JW_OSD_GAME_CHECKING, 0, 0);
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"syncing\",\"pending_items\":1}",
                 JW_OSD_GAME_SYNCING, 1, 0);
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"syncing\",\"pending_items\":12,\"expires_ms\":5}",
                 JW_OSD_GAME_SYNCING, 12, 5);
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"stopping\"}",
                 JW_OSD_GAME_STOPPING, 0, 0);
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"settings-not-saved\"}",
                 JW_OSD_GAME_SETTINGS_NOT_SAVED, 0, 0);
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"storage-read-only\"}",
                 JW_OSD_GAME_STORAGE_READ_ONLY, 0, 0);
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"power-save-release\"}",
                 JW_OSD_POWER_SAVE_RELEASE, 0, 0);
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"power-save-saving\"}",
                 JW_OSD_POWER_SAVE_SAVING, 0, 0);
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"power-save-saved\"}",
                 JW_OSD_POWER_SAVE_SAVED, 0, 0);
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"power-save-failed\"}",
                 JW_OSD_POWER_SAVE_FAILED, 0, 0);
    assert(!JW_OSD_GAME_STAGE_IS_TRANSIENT(JW_OSD_POWER_SAVE_RELEASE));
    assert(!JW_OSD_GAME_STAGE_IS_TRANSIENT(JW_OSD_POWER_SAVE_SAVING));
    assert(JW_OSD_GAME_STAGE_IS_TRANSIENT(JW_OSD_POWER_SAVE_SAVED));
    assert(JW_OSD_GAME_STAGE_IS_TRANSIENT(JW_OSD_POWER_SAVE_FAILED));
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"power-save-saving\",\"pending_items\":0}");
    expect_valid("{\"type\":\"show-game-launch\",\"stage\":\"boot-resume\"}",
                 JW_OSD_BOOT_RESUME, 0, 0);
    assert(JW_OSD_GAME_STAGE_IS_TRANSIENT(JW_OSD_BOOT_RESUME));
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"boot-resume\",\"pending_items\":0}");

    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"unknown\"}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"starting\"}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"syncing\"}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"syncing\",\"pending_items\":-1}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"syncing\",\"pending_items\":1.5}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"checking\",\"pending_items\":0}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"checking\",\"extra\":true}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"checking\",\"expires_ms\":0}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"checking\",\"expires_ms\":-4}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"checking\",\"expires_ms\":1.5}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"checking\",\"expires_ms\":\"9\"}");
    expect_invalid("{\"type\":\"show-game-launch\",\"stage\":\"checking\",\"expires_ms\":9,\"extra\":1}");
    expect_invalid("{\"type\":\"show-game-waiting\",\"stage\":\"syncing\",\"pending_items\":1}");
}

static void english_wording(void) {
    expect_text(JW_OSD_GAME_CHECKING, 0, "Syncthing: Checking saves", "");
    expect_text(JW_OSD_GAME_SYNCING, 0, "Syncthing: Syncing 0 items", "Menu: Start now");
    expect_text(JW_OSD_GAME_SYNCING, 1, "Syncthing: Syncing 1 item", "Menu: Start now");
    expect_text(JW_OSD_GAME_SYNCING, 12, "Syncthing: Syncing 12 items", "Menu: Start now");
    expect_text(JW_OSD_GAME_SYNCING, -3, "Syncthing: Syncing 0 items", "Menu: Start now");
    expect_text(JW_OSD_GAME_STOPPING, 0, "Syncthing: Stopping", "");
    expect_text(JW_OSD_GAME_SETTINGS_NOT_SAVED, 0, "RetroArch settings not saved", "");
    expect_text(JW_OSD_GAME_STORAGE_READ_ONLY, 0, "Your SD card is read-only", "New saves may fail");
    expect_text(JW_OSD_PICO8_EXIT_CONFIRM, 0, "Return to Leaf?", "Press Menu again");
    expect_text(JW_OSD_PICO8_IMPORT, 0, "Adding Splore favorites", "Please wait");
    expect_text(JW_OSD_PICO8_IMPORT_FAILED, 0, "Splore import incomplete", "Try Splore again");
    expect_text(JW_OSD_POWER_SAVE_RELEASE, 0, "Preparing to save", "Release the power button");
    expect_text(JW_OSD_POWER_SAVE_SAVING, 0, "Saving game", "");
    expect_text(JW_OSD_POWER_SAVE_SAVED, 0, "Game saved", "Powering off");
    expect_text(JW_OSD_POWER_SAVE_FAILED, 0, "Game not saved", "Powering off");
    expect_text(JW_OSD_BOOT_RESUME, 0, "Resuming your game", "");
}

static void write_table(const char *root, const char *lang, const char *body) {
    char path[512];
    snprintf(path, sizeof(path), "%s/i18n", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/i18n/%s.tsv", root, lang);
    FILE *fp = fopen(path, "w");
    assert(fp);
    fputs(body, fp);
    fclose(fp);
}

static void translated_wording(const char *root) {
    write_table(root, "zh_CN",
        "Syncthing: Syncing %d item\tSyncthing：正在同步 %d 项\n"
        "Syncthing: Syncing %d items\tSyncthing：正在同步 %d 项（多项）\n"
        "Return to Leaf?\t返回 Leaf？\n"
        "Press Menu again\t再次按菜单键\n"
        "Please wait\t请稍候请稍候请稍候请稍候请稍候请稍候请稍候请稍候请稍候请稍候\n");
    assert(jw_i18n_load("zh_CN"));
    expect_text(JW_OSD_GAME_SYNCING, 0, "Syncthing：正在同步 0 项（多项）", "Menu: Start now");
    expect_text(JW_OSD_GAME_SYNCING, 1, "Syncthing：正在同步 1 项", "Menu: Start now");
    expect_text(JW_OSD_GAME_SYNCING, 7, "Syncthing：正在同步 7 项（多项）", "Menu: Start now");
    expect_text(JW_OSD_PICO8_EXIT_CONFIRM, 0, "返回 Leaf？", "再次按菜单键");
    /* Untranslated keys fall back to English. */
    expect_text(JW_OSD_GAME_STOPPING, 0, "Syncthing: Stopping", "");

    /* Buffers that end inside a character keep valid UTF-8. */
    for (size_t size = 1; size <= 48; size++) {
        char title[48];
        char action[48];
        jw_osd_game_launch_text(JW_OSD_GAME_SYNCING, 123, title, size, action, size);
        assert(valid_utf8(title) && valid_utf8(action));
        assert(strncmp("Syncthing：正在同步 123 项（多项）", title, strlen(title)) == 0);
        jw_osd_game_launch_text(JW_OSD_PICO8_IMPORT, 0, title, size, action, size);
        assert(valid_utf8(title) && valid_utf8(action));
        assert(strlen(action) < size);
    }

    /* A live table cannot change the count conversion. */
    write_table(root, "zz",
        "Syncthing: Syncing %d items\tbroken %s\n"
        "Syncthing: Syncing %d item\ttwo %d %d\n");
    assert(jw_i18n_load("zz"));
    expect_text(JW_OSD_GAME_SYNCING, 3, "Syncthing: Syncing 3 items", "Menu: Start now");
    expect_text(JW_OSD_GAME_SYNCING, 1, "Syncthing: Syncing 1 item", "Menu: Start now");
    jw_i18n_shutdown();
}

int main(void) {
    parser();
    english_wording();
    char root[] = "/tmp/jw-osd-game-launch-XXXXXX";
    assert(mkdtemp(root));
    setenv("UMRK_INTERNAL_DATA_PATH", root, 1);
    setenv("UMRK_PLATFORM_PATH", root, 1);
    translated_wording(root);
    puts("PASS osd-game-launch-test");
    return 0;
}
