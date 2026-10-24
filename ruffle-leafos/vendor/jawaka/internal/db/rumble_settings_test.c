#include "internal/db/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

static void expect(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "rumble-settings-test: %s\n", what);
        failures++;
    }
}

/* A fresh database file per case, so no case sees another's keys. */
static void fresh_db(char *path, size_t size) {
    snprintf(path, size, "/tmp/jawaka-rumble-settings.XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        exit(1);
    }
    close(fd);
    unlink(path);
}

static void set(const char *db, const char *key, const char *value) {
    if (jw_db_set_setting(db, key, value) != 0) {
        fprintf(stderr, "rumble-settings-test: could not set %s\n", key);
        exit(1);
    }
}

static int stored_is(const char *db, const char *key, const char *want) {
    char v[16] = "";
    return jw_db_get_setting(db, key, v, sizeof(v)) == 0 && strcmp(v, want) == 0;
}

static void check(const char *db, int ui, int game, int strength, const char *name) {
    jw_rumble_settings rs;
    char what[160];
    snprintf(what, sizeof(what), "%s: load failed", name);
    expect(jw_db_load_rumble_settings(db, &rs) == 0, what);
    snprintf(what, sizeof(what), "%s: ui %d, want %d", name, rs.ui, ui);
    expect(rs.ui == ui, what);
    snprintf(what, sizeof(what), "%s: game %d, want %d", name, rs.game, game);
    expect(rs.game == game, what);
    snprintf(what, sizeof(what), "%s: strength %d, want %d", name, rs.strength, strength);
    expect(rs.strength == strength, what);
}

int main(void) {
    char db[64];

    /* A new card: UI rumble off, game rumble on, medium strength. */
    fresh_db(db, sizeof(db));
    check(db, 0, 1, JW_RUMBLE_DEFAULT_STRENGTH, "fresh");
    expect(stored_is(db, "rumble_ui", "0") && stored_is(db, "rumble_game", "1"),
           "fresh: migration did not record both switches");
    unlink(db);

    /* Old master switch off: nothing may start rumbling after the update. */
    fresh_db(db, sizeof(db));
    set(db, "rumble_enabled", "0");
    set(db, "rumble_nav", "1");
    set(db, "rumble_game", "1");
    check(db, 0, 0, JW_RUMBLE_DEFAULT_STRENGTH, "master off");
    /* The old key is not consulted again: turning game rumble back on sticks. */
    set(db, "rumble_game", "1");
    check(db, 0, 1, JW_RUMBLE_DEFAULT_STRENGTH, "master off, then game on");
    unlink(db);

    /* Master on: UI rumble takes Cursor Movement, game rumble keeps its value. */
    fresh_db(db, sizeof(db));
    set(db, "rumble_enabled", "1");
    set(db, "rumble_nav", "1");
    set(db, "rumble_game", "0");
    set(db, "rumble_strength", "40");
    check(db, 1, 0, 40, "master on, cursor on, game off");
    unlink(db);

    /* Master left at its default with Cursor Movement off. */
    fresh_db(db, sizeof(db));
    set(db, "rumble_nav", "0");
    check(db, 0, 1, JW_RUMBLE_DEFAULT_STRENGTH, "master default, cursor off");
    unlink(db);

    /* Already on the new keys: a stale master switch is ignored. */
    fresh_db(db, sizeof(db));
    set(db, "rumble_ui", "1");
    set(db, "rumble_enabled", "0");
    check(db, 1, 1, JW_RUMBLE_DEFAULT_STRENGTH, "new keys win over stale master");
    unlink(db);

    /* Strength is clamped to 0-100. */
    fresh_db(db, sizeof(db));
    set(db, "rumble_strength", "150");
    check(db, 0, 1, 100, "strength above range");
    set(db, "rumble_strength", "-5");
    check(db, 0, 1, 0, "strength below range");
    unlink(db);

    if (failures) return 1;
    puts("PASS rumble-settings-test: defaults, master-off and master-on migration, "
         "one-time rewrite, stale master ignored, strength clamp");
    return 0;
}
