#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/launcher/active_game.h"

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void write_corrupt(const char *dir) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", dir, JW_ACTIVE_GAME_FILENAME);
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    const char *body = "{\"launch_id\":\"truncated\"";
    assert(write(fd, body, strlen(body)) == (ssize_t)strlen(body));
    assert(close(fd) == 0);
}

int main(void) {
    char root[] = "/tmp/jw-active-game-test.XXXXXX";
    assert(mkdtemp(root));
    char reason[64];
    jw_active_game loaded;
    assert(jw_active_game_load(root, &loaded, reason, sizeof(reason)) ==
           JW_ACTIVE_GAME_LOAD_ABSENT);
    assert(!loaded.active);

    jw_active_game first;
    memset(&first, 0, sizeof(first));
    first.active = true;
    snprintf(first.launch_id, sizeof(first.launch_id), "%s", "launch-1");
    snprintf(first.source_id, sizeof(first.source_id), "%s", "primary_sd");
    snprintf(first.saves_path, sizeof(first.saves_path), "%s", "/card/Saves");
    snprintf(first.states_path, sizeof(first.states_path), "%s", "/card/States");
    assert(jw_active_game_persist(root, &first, reason, sizeof(reason)));
    assert(jw_active_game_load(root, &loaded, reason, sizeof(reason)) ==
           JW_ACTIVE_GAME_LOAD_VALID);
    assert(loaded.active && loaded.recovered && !loaded.uncertain);
    assert(strcmp(loaded.launch_id, first.launch_id) == 0);
    assert(strcmp(loaded.source_id, first.source_id) == 0);
    assert(strcmp(loaded.saves_path, first.saves_path) == 0);
    assert(strcmp(loaded.states_path, first.states_path) == 0);

    jw_active_game second = first;
    snprintf(second.launch_id, sizeof(second.launch_id), "%s", "launch-2");
    snprintf(second.source_id, sizeof(second.source_id), "%s", "secondary_sd");
    assert(jw_active_game_persist(root, &second, reason, sizeof(reason)));
    assert(jw_active_game_load(root, &loaded, reason, sizeof(reason)) ==
           JW_ACTIVE_GAME_LOAD_VALID);
    assert(strcmp(loaded.launch_id, "launch-2") == 0);
    assert(strcmp(loaded.source_id, "secondary_sd") == 0);

    assert(jw_active_game_clear(root, reason, sizeof(reason)));
    assert(jw_active_game_clear(root, reason, sizeof(reason)));
    assert(jw_active_game_load(root, &loaded, reason, sizeof(reason)) ==
           JW_ACTIVE_GAME_LOAD_ABSENT);

    write_corrupt(root);
    assert(jw_active_game_load(root, &loaded, reason, sizeof(reason)) ==
           JW_ACTIVE_GAME_LOAD_UNCERTAIN);
    assert(loaded.active && loaded.recovered && loaded.uncertain);
    assert(jw_active_game_clear(root, reason, sizeof(reason)));

    char launch_id[JW_ACTIVE_GAME_LAUNCH_ID_MAX + 1];
    assert(jw_active_game_generate_id(launch_id, sizeof(launch_id)));
    assert(launch_id[0]);

    assert(rmdir(root) == 0);
    puts("active_game_test: ok");
    return 0;
}
