#include "internal/launcher/pico8.h"
#include "internal/db/db.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

bool jw_pico8_core_matches(const char *id, const char *provider, const char *entry) {
    return id && provider && entry && strcmp(id, JW_PICO8_CORE) == 0 &&
           strcmp(provider, JW_PICO8_PROVIDER) == 0 && strcmp(entry, JW_PICO8_ENTRY) == 0;
}

bool jw_pico8_exit_confirmed(long long *deadline, long long now) {
    if (*deadline > now) {
        *deadline = 0;
        return true;
    }
    *deadline = now + JW_PICO8_EXIT_CONFIRM_MS;
    return false;
}

/* Preserve the first request time so repeated confirmations cannot postpone
 * the controller escape from a hung SDL quit handler. */
int jw_pico8_exit_signal(long long *requested, long long now) {
    if (!*requested) { *requested = now; return SIGTERM; }
    return now - *requested >= 2000 ? SIGKILL : 0;
}

bool jw_pico8_app_matches(const jw_storage_source *primary, const char *pak_abs) {
    char expected[JW_STORAGE_PATH_MAX];
    if (!primary || !primary->primary || !primary->available || !pak_abs) return false;
    int n = snprintf(expected, sizeof(expected), "%s/%s", primary->apps_path, JW_PICO8_PROVIDER);
    char resolved[JW_STORAGE_PATH_MAX];
    return n > 0 && n < (int)sizeof(expected) && realpath(expected, resolved) &&
           strcmp(resolved, pak_abs) == 0;
}

static bool join(char *out, size_t size, const char *base, const char *leaf) {
    if (!base || base[0] != '/') return false;
    int n = snprintf(out, size, "%s/%s", base, leaf);
    return n > 0 && n < (int)size;
}

bool jw_pico8_resolve_paths(const jw_storage_source_list *sources,
                            const jw_storage_source *rom_source, jw_pico8_paths *out) {
    if (!sources || !out) return false;
    const jw_storage_source *primary = jw_storage_sources_primary(sources);
    if (!primary || !primary->available || (rom_source && !rom_source->available)) return false;
    const char *recordings = getenv("RECORDINGS_PATH");
    char default_recordings[JW_STORAGE_PATH_MAX];
    if (!recordings || !recordings[0]) {
        if (!join(default_recordings, sizeof(default_recordings), primary->root, "Recordings")) return false;
        recordings = default_recordings;
    }
    return join(out->runtime, sizeof(out->runtime), primary->bios_path, "PICO8") &&
           join(out->home, sizeof(out->home), primary->userdata_path, "pico8") &&
           join(out->root, sizeof(out->root), rom_source ? rom_source->roms_path : primary->roms_path, "PICO8") &&
           join(out->desktop, sizeof(out->desktop), recordings, "PICO8");
}

void jw_pico8_export(const jw_pico8_paths *paths, bool protected_session) {
    const char *names[] = {"UMRK_PICO8_RUNTIME_PATH", "UMRK_PICO8_HOME_PATH",
                          "UMRK_PICO8_ROOT_PATH", "UMRK_PICO8_DESKTOP_PATH"};
    const char *values[] = {paths ? paths->runtime : NULL, paths ? paths->home : NULL,
                           paths ? paths->root : NULL, paths ? paths->desktop : NULL};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        if (values[i]) setenv(names[i], values[i], 1);
        else unsetenv(names[i]);
    }
    if (paths && protected_session) setenv("UMRK_PICO8_SESSION", "1", 1);
    else unsetenv("UMRK_PICO8_SESSION");
}

static long long milliseconds(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

bool jw_pico8_preflight(const char *wrapper, const jw_pico8_paths *paths) {
    if (!wrapper || !paths) return false;
    long long deadline = milliseconds() + 2000;
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        if (setpgid(0, 0) != 0) _exit(126);
        int fd = open("/dev/null", O_RDONLY);
        if (fd < 0 || dup2(fd, STDIN_FILENO) < 0) _exit(126);
        if (fd != STDIN_FILENO) close(fd);
        jw_pico8_export(paths, false);
        execl(wrapper, wrapper, "--check", (char *)NULL);
        _exit(127);
    }
    (void)setpgid(pid, pid);
    int status = 0;
    bool ready = false;
    bool reaped = false;
    while (milliseconds() < deadline) {
        pid_t result = waitpid(pid, &status, WNOHANG);
        if (result == pid) {
            reaped = true;
            ready = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            break;
        }
        if (result < 0 && errno != EINTR) break;
        usleep(10000);
    }
    /* A broken --check must not leave a background downloader or loader. */
    (void)kill(-pid, SIGKILL);
    if (!reaped) {
        (void)kill(pid, SIGKILL);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    return ready;
}

/* The importer publishes only complete, owned carts. Apply its titles through
 * the same path as other imported titles; a separate marker makes the native
 * preference a one-time seed, even when the user later chooses Default. */
int jw_pico8_apply_library(sqlite3 *db, const char *report) {
    FILE *fp = fopen(report, "r");
    if (!fp) return errno == ENOENT ? 0 : -1;
    typedef struct { char path[160], title[256]; const char *path_ref; } item;
    item *items = NULL;
    jw_db_imported_title_group *groups = NULL;
    int count = 0, rc = -1;
    sqlite3_stmt *seed = NULL, *mark = NULL;
    bool transaction = false;
    char line[512];
    while (fgets(line, sizeof(line), fp)) {
        char *tab = strchr(line, '\t'), *nl = strchr(line, '\n');
        if (!tab || !nl || strchr(tab+1, '\t')) goto done;
        *tab = 0; *nl = 0;
        size_t n = strlen(line);
        if (!n || n > 96 || !tab[1] || strlen(tab+1) > 255) goto done;
        for (const char *p = line; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) goto done;
        for (const unsigned char *p = (unsigned char *)tab+1; *p; p++)
            if (*p < 32 || *p == 127) goto done;
        item *grown = count < 10000 ? realloc(items, (size_t)(count+1)*sizeof(*items)) : NULL;
        if (!grown) goto done;
        items = grown;
        snprintf(items[count].path, sizeof(items[count].path), "Roms/PICO8/Splore/%s.p8.png", line);
        snprintf(items[count].title, sizeof(items[count].title), "%s", tab+1);
        count++;
    }
    if (ferror(fp)) goto done;
    if (!count) { rc = 0; goto done; }
    groups = calloc((size_t)count, sizeof(*groups));
    if (!groups) goto done;
    for (int i = 0; i < count; i++) {
        items[i].path_ref = items[i].path;
        groups[i] = (jw_db_imported_title_group){.provider=JW_PICO8_PROVIDER,
            .title=items[i].title, .rom_paths=&items[i].path_ref, .rom_path_count=1};
    }
    if (jw_db_apply_imported_title_groups(db, groups, count, NULL) != 0) goto done;
    if (sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO game_settings(game_id,key,value,updated_at) "
        "SELECT id,'core_id','pico8_native',strftime('%s','now') FROM games "
        "WHERE rom_path=? AND system='PICO8' AND source_id='primary' "
        "AND NOT EXISTS(SELECT 1 FROM game_settings WHERE game_id=games.id AND key='pico8_imported')",
        -1,&seed,NULL)!=SQLITE_OK || sqlite3_prepare_v2(db,
        "INSERT OR IGNORE INTO game_settings(game_id,key,value,updated_at) "
        "SELECT id,'pico8_imported','1',strftime('%s','now') FROM games "
        "WHERE rom_path=? AND system='PICO8' AND source_id='primary'",-1,&mark,NULL)!=SQLITE_OK) goto done;
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) goto done;
    transaction = true;
    for (int i = 0; i < count; i++) {
        sqlite3_bind_text(seed, 1, items[i].path, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(mark, 1, items[i].path, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(seed) != SQLITE_DONE || sqlite3_step(mark) != SQLITE_DONE) goto done;
        sqlite3_reset(seed); sqlite3_reset(mark);
    }
    if (sqlite3_exec(db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) goto done;
    transaction = false;
    rc = 0;
done:
    if (transaction) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    sqlite3_finalize(seed); sqlite3_finalize(mark);
    free(groups); free(items); fclose(fp); return rc;
}
