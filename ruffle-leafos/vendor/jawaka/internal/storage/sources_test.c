#include "internal/storage/sources.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void set_path2(const char *card1, const char *card2) {
    char value[JW_STORAGE_PATH_MAX * 2];
    setenv("UMRK_ENV_VERSION", "2", 1);
    setenv("SDCARD_PATH", card1, 1);
    snprintf(value, sizeof(value), "%s:%s", card1, card2);
    setenv("SDCARD_PATHS", value, 1);
    snprintf(value, sizeof(value), "%s/.userdata/mac:%s/.userdata/mac",
             card1, card2);
    char *separator = strchr(value, ':');
    assert(separator);
    *separator = '\0';
    setenv("USERDATA_PATH", value, 1);
    *separator = ':';
    setenv("USERDATA_PATHS", value, 1);
    snprintf(value, sizeof(value), "%s/.userdata/shared:%s/.userdata/shared",
             card1, card2);
    separator = strchr(value, ':');
    assert(separator);
    *separator = '\0';
    setenv("SHARED_USERDATA_PATH", value, 1);
    *separator = ':';
    setenv("SHARED_USERDATA_PATHS", value, 1);
    snprintf(value, sizeof(value), "%s/Saves:%s/Saves", card1, card2);
    separator = strchr(value, ':');
    assert(separator);
    *separator = '\0';
    setenv("SAVES_PATH", value, 1);
    *separator = ':';
    setenv("SAVES_PATHS", value, 1);
    snprintf(value, sizeof(value), "%s/States:%s/States", card1, card2);
    separator = strchr(value, ':');
    assert(separator);
    *separator = '\0';
    setenv("STATES_PATH", value, 1);
    *separator = ':';
    setenv("STATES_PATHS", value, 1);
}

static void set_path2_one_card(const char *card) {
    char value[JW_STORAGE_PATH_MAX];
    setenv("UMRK_ENV_VERSION", "2", 1);
    setenv("SDCARD_PATH", card, 1);
    setenv("SDCARD_PATHS", card, 1);
    snprintf(value, sizeof(value), "%s/.userdata/mac", card);
    setenv("USERDATA_PATH", value, 1);
    setenv("USERDATA_PATHS", value, 1);
    snprintf(value, sizeof(value), "%s/.userdata/shared", card);
    setenv("SHARED_USERDATA_PATH", value, 1);
    setenv("SHARED_USERDATA_PATHS", value, 1);
    snprintf(value, sizeof(value), "%s/Saves", card);
    setenv("SAVES_PATH", value, 1);
    setenv("SAVES_PATHS", value, 1);
    snprintf(value, sizeof(value), "%s/States", card);
    setenv("STATES_PATH", value, 1);
    setenv("STATES_PATHS", value, 1);
}

int main(void) {
    char temp[] = "/tmp/jw-sources-XXXXXX";
    assert(mkdtemp(temp));
    char card1[JW_STORAGE_PATH_MAX];
    char card2[JW_STORAGE_PATH_MAX];
    char roms[JW_STORAGE_PATH_MAX];
    char game[JW_STORAGE_PATH_MAX];
    snprintf(card1, sizeof(card1), "%s/card1", temp);
    snprintf(card2, sizeof(card2), "%s/card2", temp);
    assert(mkdir(card1, 0700) == 0);
    assert(mkdir(card2, 0700) == 0);
    snprintf(roms, sizeof(roms), "%s/Roms", card2);
    assert(mkdir(roms, 0700) == 0);
    snprintf(game, sizeof(game), "%s/game.zip", roms);
    FILE *file = fopen(game, "wb");
    assert(file);
    assert(fputs("rom", file) >= 0);
    assert(fclose(file) == 0);

    char roots[JW_STORAGE_PATH_MAX * 2];
    char music[JW_STORAGE_PATH_MAX * 2];
    char videos[JW_STORAGE_PATH_MAX * 2];
    snprintf(roots, sizeof(roots), "%s:%s", card1, card2);
    snprintf(music, sizeof(music), "%s/music1:%s/music2", temp, temp);
    snprintf(videos, sizeof(videos), "%s/videos1:%s/videos2", temp, temp);
    setenv("SDCARD_PATHS", roots, 1);
    setenv("SDCARD_PATH", card1, 1);
    setenv("MUSIC_PATHS", music, 1);
    setenv("VIDEO_PATHS", videos, 1);

    jw_storage_source_list sources;
    assert(jw_storage_sources_resolve(card1, &sources) == 0);
    assert(sources.count == 2);
    assert(strcmp(sources.sources[0].id, "primary") == 0);
    assert(sources.sources[0].available);
    assert(strcmp(sources.sources[1].id, "secondary_sd") == 0);
    char expected_userdata[JW_STORAGE_PATH_MAX];
    snprintf(expected_userdata, sizeof(expected_userdata),
             "%s/.userdata/mac", card2);
    assert(strcmp(sources.sources[1].userdata_path, expected_userdata) == 0);

    setenv("PLATFORM", "mac", 1);
    set_path2_one_card(card1);
    assert(jw_storage_source_paths_v2_valid());
    set_path2(card1, card2);
    assert(jw_storage_source_paths_v2_valid());

    setenv("UMRK_ENV_VERSION", "1", 1);
    assert(!jw_storage_source_paths_v2_valid());
    set_path2(card1, card2);
    setenv("USERDATA_PATHS", "/tmp/primary:", 1);
    assert(!jw_storage_source_paths_v2_valid());
    set_path2(card1, card2);
    setenv("USERDATA_PATHS", card1, 1);
    assert(!jw_storage_source_paths_v2_valid());
    set_path2(card1, card2);
    setenv("SDCARD_PATHS", "/mnt/sdcard:/media/sd:card1", 1);
    assert(!jw_storage_source_paths_v2_valid());
    set_path2(card1, card2);
    char invalid[JW_STORAGE_PATH_MAX * 2];
    snprintf(invalid, sizeof(invalid), "%s/.userdata/mac:%s/.userdata/mac",
             card2, card1);
    setenv("USERDATA_PATHS", invalid, 1);
    assert(!jw_storage_source_paths_v2_valid());
    set_path2(card1, card2);
    snprintf(invalid, sizeof(invalid), "%s/.userdata/mac:%s/wrong",
             card1, card1);
    setenv("USERDATA_PATHS", invalid, 1);
    assert(!jw_storage_source_paths_v2_valid());
    set_path2(card1, card2);
    snprintf(invalid, sizeof(invalid), "%s:%s", card1, card1);
    setenv("SDCARD_PATHS", invalid, 1);
    assert(!jw_storage_source_paths_v2_valid());
    set_path2(card1, card2);
    snprintf(invalid, sizeof(invalid), "%s/", card1);
    setenv("SDCARD_PATH", invalid, 1);
    assert(!jw_storage_source_paths_v2_valid());
    set_path2(card1, card2);
    unsetenv("SHARED_USERDATA_PATHS");
    assert(!jw_storage_source_paths_v2_valid());
    assert(sources.sources[1].available);
    char expected[JW_STORAGE_PATH_MAX];
    snprintf(expected, sizeof(expected), "%s/music2", temp);
    assert(strcmp(sources.sources[1].music_path, expected) == 0);
    snprintf(expected, sizeof(expected), "%s/videos2", temp);
    assert(strcmp(sources.sources[1].video_path, expected) == 0);

    char resolved[JW_STORAGE_PATH_MAX];
    assert(jw_storage_resolve_rom(&sources.sources[1], "game.zip", true,
                                  resolved, sizeof(resolved)) == 0);
    char expected_game[JW_STORAGE_PATH_MAX];
    assert(realpath(game, expected_game));
    assert(strcmp(resolved, expected_game) == 0);
    assert(!jw_storage_relative_path_valid("../game.zip"));
    assert(!jw_storage_relative_path_valid("folder//game.zip"));
    assert(jw_storage_relative_path_valid("SNES/game.zip"));

    unsetenv("MUSIC_PATHS");
    setenv("MUSIC_PATH", "/primary-music", 1);
    unsetenv("VIDEO_PATHS");
    setenv("VIDEO_PATH", "/primary-videos", 1);
    assert(jw_storage_sources_resolve(card1, &sources) == 0);
    assert(strcmp(sources.sources[0].music_path, "/primary-music") == 0);
    assert(strcmp(sources.sources[0].video_path, "/primary-videos") == 0);
    snprintf(expected, sizeof(expected), "%s/Music", card2);
    assert(strcmp(sources.sources[1].music_path, expected) == 0);
    snprintf(expected, sizeof(expected), "%s/Videos", card2);
    assert(strcmp(sources.sources[1].video_path, expected) == 0);

    setenv("MUSIC_PATHS", "/only-one", 1);
    assert(jw_storage_sources_resolve(card1, &sources) != 0);

    setenv("MUSIC_PATHS", music, 1);
    setenv("VIDEO_PATHS", "/only-one", 1);
    assert(jw_storage_sources_resolve(card1, &sources) != 0);

    setenv("MUSIC_PATHS", music, 1);
    setenv("VIDEO_PATHS", videos, 1);
    setenv("SDCARD_PATH", card2, 1);
    assert(jw_storage_sources_resolve(card1, &sources) != 0);

    snprintf(roots, sizeof(roots), "%s:%s", card1, card1);
    setenv("SDCARD_PATHS", roots, 1);
    setenv("SDCARD_PATH", card1, 1);
    assert(jw_storage_sources_resolve(card1, &sources) != 0);

    unsetenv("SDCARD_PATHS");
    unsetenv("MUSIC_PATHS");
    unsetenv("VIDEO_PATHS");
    setenv("SDCARD_PATH", card1, 1);
    setenv("UMRK_SECONDARY_SDCARD_PATH", card2, 1);
    assert(jw_storage_sources_resolve(card1, &sources) == 0);
    assert(sources.count == 2);
    assert(strcmp(sources.sources[1].id, "secondary_sd") == 0);

    unlink(game);
    rmdir(roms);
    rmdir(card2);
    rmdir(card1);
    rmdir(temp);
    return 0;
}
