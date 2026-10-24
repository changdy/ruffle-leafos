#include "internal/db/db.h"

#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static void expect(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "setting-nowait-test: %s\n", what);
        exit(1);
    }
}

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(void) {
    char path[] = "/tmp/jawaka-setting-nowait.XXXXXX";
    int fd = mkstemp(path);
    expect(fd >= 0, "mkstemp");
    close(fd);
    unlink(path);
    char value[32];

    expect(jw_db_read_setting_nowait(path, "save_state_on_power_hold", value,
                                     sizeof(value)) == -1,
           "missing database is an error, not a default");
    expect(access(path, F_OK) != 0, "a read-only probe must not create the database");

    expect(jw_db_set_setting(path, "other", "x") == 0, "seed database");
    expect(jw_db_read_setting_nowait(path, "save_state_on_power_hold", value,
                                     sizeof(value)) == 1 && value[0] == '\0',
           "absent key");
    expect(jw_db_set_setting(path, "save_state_on_power_hold", "1") == 0, "set key");
    expect(jw_db_read_setting_nowait(path, "save_state_on_power_hold", value,
                                     sizeof(value)) == 0 && strcmp(value, "1") == 0,
           "present key");

    /* Another connection holds an exclusive lock: fail at once, never wait. */
    sqlite3 *holder = NULL;
    expect(sqlite3_open(path, &holder) == SQLITE_OK, "open holder");
    expect(sqlite3_exec(holder, "PRAGMA locking_mode=EXCLUSIVE; BEGIN EXCLUSIVE;"
                                "UPDATE settings SET value='0' WHERE key='other';",
                        NULL, NULL, NULL) == SQLITE_OK, "take exclusive lock");
    long long started = now_ms();
    expect(jw_db_read_setting_nowait(path, "save_state_on_power_hold", value,
                                     sizeof(value)) == -1, "locked database is an error");
    expect(now_ms() - started < 200, "locked read returned without a busy wait");
    sqlite3_exec(holder, "ROLLBACK;", NULL, NULL, NULL);
    sqlite3_close(holder);

    char empty[] = "/tmp/jawaka-setting-nowait-empty.XXXXXX";
    fd = mkstemp(empty);
    expect(fd >= 0, "mkstemp empty");
    close(fd);
    sqlite3 *bare = NULL;
    expect(sqlite3_open(empty, &bare) == SQLITE_OK &&
           sqlite3_exec(bare, "CREATE TABLE unrelated(x);", NULL, NULL, NULL) == SQLITE_OK,
           "create database without settings table");
    sqlite3_close(bare);
    expect(jw_db_read_setting_nowait(empty, "save_state_on_power_hold", value,
                                     sizeof(value)) == 1, "no settings table reads as absent");

    unlink(path);
    unlink(empty);
    puts("PASS setting-nowait-test");
    return 0;
}
