/* The RetroArch half of the account handoff, end to end short of fork():
   stored account rows -> the one launch resolve jawakad uses ->
   JAWAKA_CHEEVOS_* -> cheevos_username/cheevos_password in the per-launch
   RetroArch config.

   A configured account reaches the config byte for byte. Every other verdict
   (signed out, never configured, invalid, an oversized legacy value, a
   revision that cannot be initialized, an unreadable store) writes no
   credential keys at all; cheevos_* are protected keys, so none arrive from
   the shared config either. An oversized legacy value is cleared, not
   truncated: the pre-contract bridge copied 63/127 bytes and could sign
   RetroArch in with a name the user never typed. All accounts here are
   synthetic. */

#include "internal/db/db.h"
#include "internal/launcher/ra_account.h"
#include "internal/platform/paths.h"

#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int failures;

static void expect(int ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "ra-account-retroarch-test: %s\n", what);
        failures++;
    }
}

static char root[PATH_MAX];
static char runtime[PATH_MAX];
static char core[PATH_MAX];

static void mkdir_p(const char *path) {
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "mkdir -p '%s'", path);
    if (system(cmd) != 0) exit(1);
}

static void write_text(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f || fputs(text, f) < 0 || fclose(f) != 0) exit(1);
}

static char *read_text(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = calloc((size_t)size + 1u, 1u);
    if (text && fread(text, 1, (size_t)size, f) != (size_t)size) {
        free(text);
        text = NULL;
    }
    fclose(f);
    return text;
}

/* The number of `key = "..."` lines, and the value of the last one with the
   writer's backslash escapes for '\\' and '"' undone. */
static int cfg_lookup(const char *cfg, const char *key, char *value,
                      size_t value_size) {
    int count = 0;
    size_t key_len = strlen(key);
    value[0] = '\0';
    for (const char *line = cfg; line && *line;) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len > key_len + 4 && strncmp(line, key, key_len) == 0 &&
            strncmp(line + key_len, " = \"", 4) == 0 && line[len - 1] == '"') {
            const char *v = line + key_len + 4;
            const char *v_end = line + len - 1;
            size_t n = 0;
            while (v < v_end && n + 1 < value_size) {
                if (*v == '\\' && v + 1 < v_end) v++;
                value[n++] = *v++;
            }
            value[n] = '\0';
            count++;
        }
        line = end ? end + 1 : NULL;
    }
    return count;
}

static void seed_db(const char *db, const char *user, const char *pass,
                    const char *revision) {
    unlink(db);
    if ((user && jw_db_set_setting(db, "retroachievements_user", user) != 0) ||
        (pass && jw_db_set_setting(db, "retroachievements_pass", pass) != 0) ||
        (revision &&
         jw_db_set_setting(db, "retroachievements_revision", revision) != 0) ||
        (!user && !pass && !revision &&
         jw_db_set_setting(db, "theme_name", "default") != 0)) {
        fprintf(stderr, "ra-account-retroarch-test: could not seed the store\n");
        exit(1);
    }
}

/* What jawakad does for a RetroArch launch: resolve once, export the
   RetroArch channel in the parent around the config write, clear it after. */
static char *launch_config(const char *db) {
    jw_ra_account account;
    jw_db_resolve_ra_account_handoff(db, &account);
    /* A value inherited from an earlier launch must never survive. */
    setenv("JAWAKA_CHEEVOS_USERNAME", "stale-inherited", 1);
    setenv("JAWAKA_CHEEVOS_PASSWORD", "stale-inherited", 1);
    jw_ra_account_apply_retroarch_env(&account);
    char error[256] = "";
    char *cfg_path = jw_prepare_retroarch_config(runtime, root, core, NULL,
                                                 false, false, error,
                                                 sizeof(error));
    jw_ra_account_clear_retroarch_env();
    if (!cfg_path) {
        fprintf(stderr, "ra-account-retroarch-test: config write failed: %s\n",
                error);
        exit(1);
    }
    char *cfg = read_text(cfg_path);
    free(cfg_path);
    if (!cfg) exit(1);
    return cfg;
}

static void expect_signed_in(const char *db, const char *name,
                             const char *user, const char *pass) {
    char *cfg = launch_config(db);
    char value[256];
    char what[160];
    snprintf(what, sizeof(what), "%s: cheevos_username written once, exactly", name);
    expect(cfg_lookup(cfg, "cheevos_username", value, sizeof(value)) == 1 &&
               strcmp(value, user) == 0,
           what);
    snprintf(what, sizeof(what), "%s: cheevos_password written once, exactly", name);
    expect(cfg_lookup(cfg, "cheevos_password", value, sizeof(value)) == 1 &&
               strcmp(value, pass) == 0,
           what);
    snprintf(what, sizeof(what), "%s: cheevos_enable true", name);
    expect(cfg_lookup(cfg, "cheevos_enable", value, sizeof(value)) == 1 &&
               strcmp(value, "true") == 0,
           what);
    expect(!strstr(cfg, "stale-inherited"), "inherited credentials survived");
    free(cfg);
}

static void expect_cleared(const char *db, const char *name) {
    char *cfg = launch_config(db);
    char value[256];
    char what[160];
    snprintf(what, sizeof(what), "%s: no cheevos_username in the config", name);
    expect(cfg_lookup(cfg, "cheevos_username", value, sizeof(value)) == 0, what);
    snprintf(what, sizeof(what), "%s: no cheevos_password in the config", name);
    expect(cfg_lookup(cfg, "cheevos_password", value, sizeof(value)) == 0, what);
    snprintf(what, sizeof(what), "%s: inherited credentials did not survive", name);
    expect(!strstr(cfg, "stale-inherited"), what);
    free(cfg);
}

int main(void) {
    snprintf(root, sizeof(root), "/tmp/jawaka-ra-retroarch.XXXXXX");
    if (!mkdtemp(root)) {
        perror("mkdtemp");
        return 1;
    }
    char platform[PATH_MAX], defaults[PATH_MAX], internal[PATH_MAX];
    char cores[PATH_MAX], shaders[PATH_MAX], user_shaders[PATH_MAX];
    char db[PATH_MAX], path[PATH_MAX];
    snprintf(platform, sizeof(platform), "%s/platform", root);
    snprintf(defaults, sizeof(defaults), "%s/defaults", platform);
    snprintf(internal, sizeof(internal), "%s/internal", root);
    snprintf(runtime, sizeof(runtime), "%s/runtime", root);
    snprintf(cores, sizeof(cores), "%s/cores", platform);
    snprintf(shaders, sizeof(shaders), "%s/shaders", platform);
    snprintf(user_shaders, sizeof(user_shaders),
             "%s/retroarch/.config/retroarch/shaders", internal);
    mkdir_p(defaults);
    mkdir_p(runtime);
    mkdir_p(cores);
    mkdir_p(shaders);
    snprintf(path, sizeof(path), "%s/retroarch", internal);
    mkdir_p(path);
    snprintf(path, sizeof(path), "%s/retroarch.cfg", defaults);
    write_text(path, "video_vsync = \"true\"\n");
    /* A cheevos_username left in the shared config is a protected key: it
       never reaches the per-launch config, so a cleared handoff really
       carries no account. */
    snprintf(path, sizeof(path), "%s/retroarch/retroarch.cfg", internal);
    write_text(path, "menu_driver = \"rgui\"\n"
                     "cheevos_username = \"users-own-retroarch-login\"\n");
    snprintf(core, sizeof(core), "%s/mgba_libretro.so", cores);
    snprintf(db, sizeof(db), "%s/library.db", root);

    setenv("SDCARD_PATH", root, 1);
    setenv("UMRK_PLATFORM_PATH", platform, 1);
    setenv("UMRK_INTERNAL_DATA_PATH", internal, 1);
    setenv("UMRK_RETROARCH_SHADERS_DIR", shaders, 1);
    setenv("UMRK_RETROARCH_USER_SHADERS_DIR", user_shaders, 1);

    /* Configured: saved through the checked write, and a legacy pair. A
       double quote plus a space has no retroarch.cfg spelling; that case is
       covered by retroarch-config-test against RetroArch's own parser. */
    long long revision = 0;
    unlink(db);
    if (jw_db_save_ra_account(db, "ra-player", "p@$$ w0rd; 'q' |&,",
                              &revision) != 0) {
        fprintf(stderr, "ra-account-retroarch-test: save failed\n");
        return 1;
    }
    expect_signed_in(db, "configured", "ra-player", "p@$$ w0rd; 'q' |&,");
    seed_db(db, "legacy-player", "legacy pass", NULL);
    expect_signed_in(db, "legacy pair", "legacy-player", "legacy pass");

    /* The 63/127-byte ceilings are inclusive. */
    char user63[64], pass127[128];
    memset(user63, 'u', 63);
    user63[63] = '\0';
    memset(pass127, 'p', 127);
    pass127[127] = '\0';
    seed_db(db, user63, pass127, "4");
    expect_signed_in(db, "63/127-byte boundary", user63, pass127);

    /* Signed out: credentials cleared, revision retained. */
    if (jw_db_clear_ra_account(db, &revision) != 0) return 1;
    expect_cleared(db, "signed out");

    /* Never configured. */
    seed_db(db, NULL, NULL, NULL);
    expect_cleared(db, "never configured");

    /* Invalid: incomplete pair, malformed revision, CR/LF. */
    seed_db(db, "ra-player", NULL, "3");
    expect_cleared(db, "incomplete pair");
    seed_db(db, "ra-player", "correct horse", "+7");
    expect_cleared(db, "malformed revision");
    seed_db(db, "ra\nplayer", "correct horse", "3");
    expect_cleared(db, "control character");

    /* Oversized legacy values: cleared, never truncated to 63/127 bytes. */
    char user64[65], pass128[129];
    memset(user64, 'u', 64);
    user64[64] = '\0';
    memset(pass128, 'p', 128);
    pass128[128] = '\0';
    seed_db(db, user64, "legacy pass", NULL);
    expect_cleared(db, "oversized legacy username");
    seed_db(db, "legacy-player", pass128, NULL);
    expect_cleared(db, "oversized legacy password");

    /* Unreadable: a legacy pair whose revision cannot be written, and a store
       that is not a database. */
    seed_db(db, "legacy-player", "legacy pass", NULL);
    sqlite3 *handle = NULL;
    if (sqlite3_open(db, &handle) != SQLITE_OK ||
        sqlite3_exec(handle,
                     "CREATE TRIGGER ra_revision_write_fails BEFORE INSERT ON "
                     "settings WHEN NEW.key = 'retroachievements_revision' "
                     "BEGIN SELECT RAISE(ABORT, 'injected'); END;",
                     NULL, NULL, NULL) != SQLITE_OK) {
        return 1;
    }
    sqlite3_close(handle);
    expect_cleared(db, "revision cannot be initialized");
    unlink(db);
    write_text(db, "not an SQLite database\n");
    expect_cleared(db, "unreadable store");

    if (failures) {
        fprintf(stderr, "ra-account-retroarch-test: %d FAILURE(S)\n", failures);
        return 1;
    }
    printf("ra-account-retroarch-test: ok\n");
    return 0;
}
