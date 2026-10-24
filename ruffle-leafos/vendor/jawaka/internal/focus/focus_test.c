#include "internal/focus/focus.h"
#include "internal/db/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

static int g_failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

static void test_lock_style_enums(void) {
    CHECK(jw_focus_lock_parse("pin") == JW_FOCUS_LOCK_PIN);
    CHECK(jw_focus_lock_parse("none") == JW_FOCUS_LOCK_NONE);
    CHECK(jw_focus_lock_parse("") == JW_FOCUS_LOCK_NONE);
    CHECK(jw_focus_lock_parse(NULL) == JW_FOCUS_LOCK_NONE);
    CHECK(jw_focus_lock_parse("garbage") == JW_FOCUS_LOCK_NONE);
    /* "combo" is not a lock type (the button chord is a universal failsafe, not a
       selectable lock) — it must fall back to NONE, never a bare free-exit lock. */
    CHECK(jw_focus_lock_parse("combo") == JW_FOCUS_LOCK_NONE);
    CHECK(strcmp(jw_focus_lock_name(JW_FOCUS_LOCK_PIN), "pin") == 0);
    CHECK(strcmp(jw_focus_lock_name(JW_FOCUS_LOCK_NONE), "none") == 0);

    CHECK(jw_focus_style_parse("bw") == JW_FOCUS_STYLE_BW);
    CHECK(jw_focus_style_parse("theme") == JW_FOCUS_STYLE_THEME);
    CHECK(jw_focus_style_parse(NULL) == JW_FOCUS_STYLE_THEME);
    CHECK(strcmp(jw_focus_style_name(JW_FOCUS_STYLE_BW), "bw") == 0);
    CHECK(strcmp(jw_focus_style_name(JW_FOCUS_STYLE_THEME), "theme") == 0);
}

static void test_ids_roundtrip(void) {
    int ids[JW_FOCUS_MAX_GAMES];
    int count = 0;

    jw_focus_ids_parse("12,3,45", ids, &count);
    CHECK(count == 3);
    CHECK(ids[0] == 12 && ids[1] == 3 && ids[2] == 45);

    char csv[128];
    jw_focus_ids_to_csv(ids, count, csv, sizeof(csv));
    CHECK(strcmp(csv, "12,3,45") == 0);

    /* dedupe + cap at 5 + drop non-positive / non-numeric, order preserved */
    jw_focus_ids_parse("1, 2 ,2,3,-4,0,foo,5,6,7,8", ids, &count);
    CHECK(count == 5);
    CHECK(ids[0] == 1 && ids[1] == 2 && ids[2] == 3 && ids[3] == 5 && ids[4] == 6);

    /* empty / NULL */
    jw_focus_ids_parse("", ids, &count);
    CHECK(count == 0);
    jw_focus_ids_parse(NULL, ids, &count);
    CHECK(count == 0);
    jw_focus_ids_to_csv(ids, count, csv, sizeof(csv));
    CHECK(csv[0] == '\0');
}

static void test_pin(void) {
    char h1[65], h2[65];
    jw_focus_pin_hash("1234", h1);
    jw_focus_pin_hash("1234", h2);
    CHECK(strlen(h1) == 64);
    CHECK(strcmp(h1, h2) == 0);            /* deterministic */

    char h3[65];
    jw_focus_pin_hash("1235", h3);
    CHECK(strcmp(h1, h3) != 0);            /* different PIN -> different hash */
    CHECK(strcmp(h1, "1234") != 0);        /* not plaintext */

    CHECK(jw_focus_pin_verify("1234", h1) == true);
    CHECK(jw_focus_pin_verify("0000", h1) == false);
    CHECK(jw_focus_pin_verify("1234", "") == false);
    CHECK(jw_focus_pin_verify("1234", NULL) == false);
}

static void test_lock_file(const char *sdroot) {
    char path[1024];
    CHECK(jw_focus_lock_path(sdroot, path, sizeof(path)) == 0);
    CHECK(strstr(path, ".leaf-focus-lock") != NULL);

    CHECK(jw_focus_lock_exists(sdroot) == false);

    jw_focus_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.active = true;
    cfg.ids[0] = 7; cfg.ids[1] = 8; cfg.id_count = 2;
    cfg.lock = JW_FOCUS_LOCK_PIN;
    cfg.style = JW_FOCUS_STYLE_BW;
    cfg.wifi_prev = 1;
    jw_focus_pin_hash("4242", cfg.pin_hash);

    CHECK(jw_focus_lock_write(sdroot, &cfg) == 0);
    CHECK(jw_focus_lock_exists(sdroot) == true);

    /* file echoes config but never the plaintext PIN */
    FILE *fp = fopen(path, "r");
    CHECK(fp != NULL);
    if (fp) {
        char buf[512] = "";
        size_t got = fread(buf, 1, sizeof(buf) - 1, fp);
        buf[got] = '\0';
        fclose(fp);
        CHECK(strstr(buf, "\"lock\":\"pin\"") != NULL);
        CHECK(strstr(buf, "\"style\":\"bw\"") != NULL);
        CHECK(strstr(buf, "\"ids\":\"7,8\"") != NULL);
        CHECK(strstr(buf, "4242") == NULL);
    }

    CHECK(jw_focus_lock_remove(sdroot) == 0);
    CHECK(jw_focus_lock_exists(sdroot) == false);
    CHECK(jw_focus_lock_remove(sdroot) == 0);   /* idempotent */
}

static void test_db_and_boot(const char *db_path, const char *sdroot) {
    sqlite3 *db = NULL;
    CHECK(jw_db_open(db_path, &db) == 0);
    CHECK(jw_db_apply_schema(db) == 0);
    jw_db_close(db);

    /* Fresh device: no keys -> inactive, NORMAL boot. */
    jw_focus_config cfg;
    CHECK(jw_focus_config_load(db_path, &cfg) == 0);
    CHECK(cfg.active == false);
    CHECK(jw_focus_resolve_boot(db_path, sdroot, &cfg) == JW_FOCUS_BOOT_NORMAL);

    /* Write an active config. */
    char pin[65];
    jw_focus_pin_hash("1234", pin);
    jw_db_set_setting(db_path, JW_FOCUS_KEY_ACTIVE, "1");
    jw_db_set_setting(db_path, JW_FOCUS_KEY_IDS, "5,6,7");
    jw_db_set_setting(db_path, JW_FOCUS_KEY_LOCK, "pin");
    jw_db_set_setting(db_path, JW_FOCUS_KEY_PIN_HASH, pin);
    jw_db_set_setting(db_path, JW_FOCUS_KEY_STYLE, "bw");
    jw_db_set_setting(db_path, JW_FOCUS_KEY_WIFI_PREV, "1");

    CHECK(jw_focus_config_load(db_path, &cfg) == 0);
    CHECK(cfg.active == true);
    CHECK(cfg.id_count == 3);
    CHECK(cfg.ids[0] == 5 && cfg.ids[2] == 7);
    CHECK(cfg.lock == JW_FOCUS_LOCK_PIN);
    CHECK(cfg.style == JW_FOCUS_STYLE_BW);
    CHECK(cfg.wifi_prev == 1);
    CHECK(jw_focus_pin_verify("1234", cfg.pin_hash) == true);

    /* Active but NO lock file -> recovery: NORMAL + active cleared in DB. */
    (void)jw_focus_lock_remove(sdroot);
    CHECK(jw_focus_resolve_boot(db_path, sdroot, &cfg) == JW_FOCUS_BOOT_NORMAL);
    CHECK(cfg.active == false);
    jw_focus_config after;
    CHECK(jw_focus_config_load(db_path, &after) == 0);
    CHECK(after.active == false);                 /* persisted */
    CHECK(after.id_count == 3);                   /* remembered set kept */
    CHECK(after.lock == JW_FOCUS_LOCK_PIN);       /* lock config kept */

    /* Re-arm active + write the lock file -> ENTER. */
    jw_db_set_setting(db_path, JW_FOCUS_KEY_ACTIVE, "1");
    CHECK(jw_focus_config_load(db_path, &cfg) == 0);
    CHECK(jw_focus_lock_write(sdroot, &cfg) == 0);
    CHECK(jw_focus_resolve_boot(db_path, sdroot, &cfg) == JW_FOCUS_BOOT_ENTER);
    CHECK(cfg.active == true);
    (void)jw_focus_lock_remove(sdroot);
}

int main(void) {
    char tmpl[] = "/tmp/jw_focus_test_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) {
        fprintf(stderr, "mkdtemp failed\n");
        return 1;
    }
    char db_path[1024];
    snprintf(db_path, sizeof(db_path), "%s/library.db", dir);

    test_lock_style_enums();
    test_ids_roundtrip();
    test_pin();
    test_lock_file(dir);
    test_db_and_boot(db_path, dir);

    /* cleanup */
    unlink(db_path);
    char lock[1024];
    if (jw_focus_lock_path(dir, lock, sizeof(lock)) == 0) unlink(lock);
    rmdir(dir);

    if (g_failures == 0) {
        printf("focus-test: all checks passed\n");
        return 0;
    }
    fprintf(stderr, "focus-test: %d check(s) failed\n", g_failures);
    return 1;
}
