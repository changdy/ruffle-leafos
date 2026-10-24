#include "internal/focus/focus.h"

#include "internal/db/db.h"
#include "internal/update/sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Fixed application salt for the PIN hash. A 4-digit PIN has only 10k values,
   so no salt makes it "secure"; this is a child deterrent and the salt just
   avoids storing a bare, trivially-recognizable SHA of the digits. The real
   recovery boundary is deleting the SD lock file. */
#define JW_FOCUS_PIN_SALT "leaf.five-game.v1:"

/* ---------- enum <-> string ---------- */

jw_focus_lock jw_focus_lock_parse(const char *s) {
    if (s && strcmp(s, "pin") == 0) return JW_FOCUS_LOCK_PIN;
    return JW_FOCUS_LOCK_NONE;
}

const char *jw_focus_lock_name(jw_focus_lock v) {
    switch (v) {
        case JW_FOCUS_LOCK_PIN:  return "pin";
        case JW_FOCUS_LOCK_NONE: break;
    }
    return "none";
}

jw_focus_style jw_focus_style_parse(const char *s) {
    if (s && strcmp(s, "bw") == 0) return JW_FOCUS_STYLE_BW;
    return JW_FOCUS_STYLE_THEME;
}

const char *jw_focus_style_name(jw_focus_style v) {
    return v == JW_FOCUS_STYLE_BW ? "bw" : "theme";
}

/* ---------- chosen-set (de)serialization ---------- */

/* ---------- PIN ---------- */

void jw_focus_pin_hash(const char *pin, char out_hex[65]) {
    if (!out_hex) return;
    if (!pin) pin = "";
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s%s", JW_FOCUS_PIN_SALT, pin);
    if (n < 0) n = 0;
    if ((size_t)n >= sizeof(buf)) n = (int)sizeof(buf) - 1;
    jw_sha256_buf_hex(buf, (size_t)n, out_hex);
}

bool jw_focus_pin_verify(const char *pin, const char *stored_hash) {
    if (!stored_hash || !stored_hash[0]) return false;
    char got[65];
    jw_focus_pin_hash(pin, got);
    return strcmp(got, stored_hash) == 0;
}

/* ---------- SD recovery lock file ---------- */

int jw_focus_lock_path(const char *sdcard_root, char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    if (!sdcard_root || !sdcard_root[0]) return -1;
    int n = snprintf(out, out_size, "%s/%s", sdcard_root, JW_FOCUS_LOCK_FILENAME);
    if (n < 0 || (size_t)n >= out_size) { out[0] = '\0'; return -1; }
    return 0;
}

bool jw_focus_lock_exists(const char *sdcard_root) {
    char path[1024];
    if (jw_focus_lock_path(sdcard_root, path, sizeof(path)) != 0) return false;
    return access(path, F_OK) == 0;
}

int jw_focus_lock_write(const char *sdcard_root, const jw_focus_config *cfg) {
    char path[1024];
    if (jw_focus_lock_path(sdcard_root, path, sizeof(path)) != 0) return -1;

    char ids_csv[128] = "";
    jw_focus_lock lock = JW_FOCUS_LOCK_NONE;
    jw_focus_style style = JW_FOCUS_STYLE_THEME;
    int wifi_prev = -1;
    if (cfg) {
        jw_focus_ids_to_csv(cfg->ids, cfg->id_count, ids_csv, sizeof(ids_csv));
        lock = cfg->lock;
        style = cfg->style;
        wifi_prev = cfg->wifi_prev;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    /* Human-readable marker + config echo; never the plaintext PIN. */
    fprintf(fp,
            "{\"active\":true,\"lock\":\"%s\",\"style\":\"%s\","
            "\"ids\":\"%s\",\"wifi_prev\":%d}\n",
            jw_focus_lock_name(lock), jw_focus_style_name(style),
            ids_csv, wifi_prev);
    int rc = (fflush(fp) == 0) ? 0 : -1;
    if (fclose(fp) != 0) rc = -1;
    return rc;
}

int jw_focus_lock_remove(const char *sdcard_root) {
    char path[1024];
    if (jw_focus_lock_path(sdcard_root, path, sizeof(path)) != 0) return -1;
    if (unlink(path) != 0 && access(path, F_OK) == 0) return -1;
    return 0;
}

/* ---------- DB config load/store ---------- */

int jw_focus_config_load(const char *db_path, jw_focus_config *out) {
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->lock = JW_FOCUS_LOCK_NONE;
    out->style = JW_FOCUS_STYLE_THEME;
    out->wifi_prev = -1;
    if (!db_path || !db_path[0]) return 0;

    char val[512];

    if (jw_db_get_setting(db_path, JW_FOCUS_KEY_ACTIVE, val, sizeof(val)) == 0)
        out->active = (val[0] == '1');

    if (jw_db_get_setting(db_path, JW_FOCUS_KEY_IDS, val, sizeof(val)) == 0)
        jw_focus_ids_parse(val, out->ids, &out->id_count);

    if (jw_db_get_setting(db_path, JW_FOCUS_KEY_LOCK, val, sizeof(val)) == 0)
        out->lock = jw_focus_lock_parse(val);

    if (jw_db_get_setting(db_path, JW_FOCUS_KEY_PIN_HASH, val, sizeof(val)) == 0) {
        strncpy(out->pin_hash, val, sizeof(out->pin_hash) - 1);
        out->pin_hash[sizeof(out->pin_hash) - 1] = '\0';
    }

    if (jw_db_get_setting(db_path, JW_FOCUS_KEY_STYLE, val, sizeof(val)) == 0)
        out->style = jw_focus_style_parse(val);

    if (jw_db_get_setting(db_path, JW_FOCUS_KEY_WIFI_PREV, val, sizeof(val)) == 0) {
        if (val[0] == '0') out->wifi_prev = 0;
        else if (val[0] == '1') out->wifi_prev = 1;
    }

    return 0;
}

jw_focus_boot_decision jw_focus_resolve_boot(const char *db_path,
                                             const char *sdcard_root,
                                             jw_focus_config *out_cfg) {
    jw_focus_config cfg;
    jw_focus_config_load(db_path, &cfg);
    if (out_cfg) *out_cfg = cfg;

    if (!cfg.active) return JW_FOCUS_BOOT_NORMAL;

    if (jw_focus_lock_exists(sdcard_root)) return JW_FOCUS_BOOT_ENTER;

    /* Active but the recovery lock file was deleted (from a computer): treat the
       device as unlocked. Clear the active flag so the next boot is normal; keep
       the remembered set / lock config for easy re-entry. */
    if (db_path && db_path[0])
        jw_db_set_setting(db_path, JW_FOCUS_KEY_ACTIVE, "0");
    if (out_cfg) out_cfg->active = false;
    return JW_FOCUS_BOOT_NORMAL;
}
