#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/power/boot_resume.h"

#include "internal/retroarch/states.h"
#include "internal/storage/sources.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

/* A record is a dozen short fields; anything this big is not one. */
#define JW_BOOT_RESUME_FILE_MAX (16u * 1024u)
#define JW_BOOT_RESUME_FIELD_COUNT 14
/* Doubles are exact below 2^53; no state comes near it. */
#define JW_BOOT_RESUME_BYTES_MAX 9007199254740992ull

static void jw__reason(char *out, size_t out_size, const char *value) {
    if (out && out_size > 0) {
        snprintf(out, out_size, "%s", value ? value : "unknown");
    }
}

static bool jw__path(char *out, size_t out_size, const char *dir, const char *leaf) {
    if (!out || out_size == 0 || !dir || !dir[0] || !leaf || !leaf[0]) {
        return false;
    }
    int n = snprintf(out, out_size, "%s/%s", dir, leaf);
    return n >= 0 && (size_t)n < out_size;
}

static bool jw__fsync_dir(const char *dir) {
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    int rc;
    do {
        rc = fsync(fd);
    } while (rc != 0 && errno == EINTR);
    int saved = errno;
    close(fd);
    errno = saved;
    return rc == 0;
}

static bool jw__write_all(int fd, const char *data, size_t len) {
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, data + written, len - written);
        if (n > 0) {
            written += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

/* Non-empty, shorter than `max`, every byte from `extra` or alphanumeric. */
static bool jw__token_ok(const char *value, size_t max, const char *extra) {
    if (!value || !value[0]) {
        return false;
    }
    size_t len = strlen(value);
    if (len >= max) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)value[i];
        bool alnum = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
                     (c >= 'A' && c <= 'Z');
        if (!alnum && !(extra && c != '\0' && strchr(extra, (int)c))) {
            return false;
        }
    }
    return true;
}

/* Printable single-line text with no path separators. */
static bool jw__name_ok(const char *value, size_t max) {
    if (!value || !value[0] || strlen(value) >= max ||
        strcmp(value, ".") == 0 || strcmp(value, "..") == 0) {
        return false;
    }
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\') {
            return false;
        }
    }
    return true;
}

static bool jw__fingerprint_ok(const char *value) {
    const char *rest = NULL;
    if (value && strncmp(value, "uuid:", 5) == 0) {
        rest = value + 5;
    } else if (value && strncmp(value, "fat:", 4) == 0) {
        rest = value + 4;
    }
    return rest && jw__token_ok(rest, JW_BOOT_RESUME_FINGERPRINT_MAX - 5, "-_.");
}

bool jw_boot_resume_record_valid(const jw_boot_resume_record *record,
                                 const char *platform,
                                 char *reason, size_t reason_size) {
    const char *bad = NULL;
    if (!record) {
        bad = "invalid-arguments";
    } else if (!platform || !platform[0] || strcmp(record->platform, platform) != 0) {
        bad = "platform";
    } else if (!jw__token_ok(record->boot_id, sizeof(record->boot_id), "-")) {
        bad = "boot_id";
    } else if (!jw__token_ok(record->request_id, sizeof(record->request_id), "-_")) {
        bad = "request_id";
    } else if (!jw__fingerprint_ok(record->source_fingerprint)) {
        bad = "source_fingerprint";
    } else if (!jw__name_ok(record->system, sizeof(record->system))) {
        bad = "system";
    } else if (strlen(record->rom_path) >= sizeof(record->rom_path) ||
               !jw_storage_relative_path_valid(record->rom_path)) {
        bad = "rom_path";
    } else if (!jw__token_ok(record->core_id, sizeof(record->core_id), "-_.")) {
        bad = "core_id";
    } else if (!jw__name_ok(record->core_config_folder,
                            sizeof(record->core_config_folder))) {
        bad = "core_config_folder";
    } else if (strlen(record->provider) >= sizeof(record->provider) ||
               (record->provider[0] &&
                !jw_storage_relative_path_valid(record->provider))) {
        bad = "provider";
    } else if (record->slot != JW_BOOT_RESUME_SLOT) {
        bad = "slot";
    } else if (strlen(record->state_path) >= sizeof(record->state_path) ||
               !jw_storage_relative_path_valid(record->state_path)) {
        bad = "state_path";
    } else {
        /* The state lives directly in the core's own States folder. */
        size_t folder_len = strlen(record->core_config_folder);
        const char *name = record->state_path + folder_len + 1;
        if (strncmp(record->state_path, record->core_config_folder, folder_len) != 0 ||
            record->state_path[folder_len] != '/' || !name[0] || strchr(name, '/')) {
            bad = "state_path";
        } else if (record->state_bytes == 0 ||
                   record->state_bytes >= JW_BOOT_RESUME_BYTES_MAX) {
            bad = "state_bytes";
        }
    }
    jw__reason(reason, reason_size, bad ? bad : "ok");
    return bad == NULL;
}

bool jw_boot_resume_write(const char *dir, const jw_boot_resume_record *record,
                          char *reason, size_t reason_size) {
    char invalid[32];
    if (!dir || !dir[0] || !record ||
        !jw_boot_resume_record_valid(record, record->platform, invalid, sizeof(invalid))) {
        jw__reason(reason, reason_size, "invalid-record");
        return false;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root ||
        !cJSON_AddNumberToObject(root, "schema", JW_BOOT_RESUME_SCHEMA) ||
        !cJSON_AddStringToObject(root, "platform", record->platform) ||
        !cJSON_AddStringToObject(root, "boot_id", record->boot_id) ||
        !cJSON_AddStringToObject(root, "request_id", record->request_id) ||
        !cJSON_AddStringToObject(root, "source_fingerprint", record->source_fingerprint) ||
        !cJSON_AddStringToObject(root, "system", record->system) ||
        !cJSON_AddStringToObject(root, "rom_path", record->rom_path) ||
        !cJSON_AddStringToObject(root, "core_id", record->core_id) ||
        !cJSON_AddStringToObject(root, "core_config_folder", record->core_config_folder) ||
        !cJSON_AddStringToObject(root, "provider", record->provider) ||
        !cJSON_AddNumberToObject(root, "slot", record->slot) ||
        !cJSON_AddStringToObject(root, "state_path", record->state_path) ||
        !cJSON_AddNumberToObject(root, "state_bytes", (double)record->state_bytes) ||
        !cJSON_AddBoolToObject(root, "offline_proxy", record->offline_proxy)) {
        cJSON_Delete(root);
        jw__reason(reason, reason_size, "encode-failed");
        return false;
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        jw__reason(reason, reason_size, "encode-failed");
        return false;
    }

    char final_path[JW_BOOT_RESUME_PATH_MAX];
    char temp_leaf[96];
    char temp_path[JW_BOOT_RESUME_PATH_MAX];
    int leaf_len = snprintf(temp_leaf, sizeof(temp_leaf), ".%s.tmp.%ld",
                            JW_BOOT_RESUME_FILENAME, (long)getpid());
    if (leaf_len < 0 || (size_t)leaf_len >= sizeof(temp_leaf) ||
        !jw__path(final_path, sizeof(final_path), dir, JW_BOOT_RESUME_FILENAME) ||
        !jw__path(temp_path, sizeof(temp_path), dir, temp_leaf)) {
        cJSON_free(json);
        jw__reason(reason, reason_size, "path-too-long");
        return false;
    }

    /* A leftover from a daemon that died mid-write carries this pid only by
       coincidence; it is ours to replace either way. */
    (void)unlink(temp_path);
    int fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0) {
        cJSON_free(json);
        jw__reason(reason, reason_size, "temp-open-failed");
        return false;
    }
    bool ok = jw__write_all(fd, json, strlen(json));
    cJSON_free(json);
    if (ok) {
        int rc;
        do {
            rc = fsync(fd);
        } while (rc != 0 && errno == EINTR);
        ok = rc == 0;
    }
    int saved = errno;
    if (close(fd) != 0 && ok) {
        ok = false;
        saved = errno;
    }
    if (!ok) {
        unlink(temp_path);
        errno = saved;
        jw__reason(reason, reason_size, "write-failed");
        return false;
    }
    if (rename(temp_path, final_path) != 0) {
        saved = errno;
        unlink(temp_path);
        errno = saved;
        jw__reason(reason, reason_size, "rename-failed");
        return false;
    }
    if (!jw__fsync_dir(dir)) {
        /* Reported as not armed, so make that true rather than leave a record
           whose durability nobody knows. */
        (void)unlink(final_path);
        jw__reason(reason, reason_size, "directory-fsync-failed");
        return false;
    }
    jw__reason(reason, reason_size, "ok");
    return true;
}

static int jw__object_size(const cJSON *object) {
    int count = 0;
    const cJSON *item = NULL;
    cJSON_ArrayForEach(item, object) {
        count++;
    }
    return count;
}

static bool jw__copy_string(const cJSON *object, const char *name,
                            char *out, size_t out_size, bool allow_empty) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsString(item) || !item->valuestring ||
        (!allow_empty && !item->valuestring[0])) {
        return false;
    }
    size_t len = strlen(item->valuestring);
    if (len >= out_size) {
        return false;
    }
    memcpy(out, item->valuestring, len + 1u);
    return true;
}

/* A non-negative integer that a double carries exactly. */
static bool jw__copy_count(const cJSON *object, const char *name,
                           unsigned long long *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    double value = item->valuedouble;
    if (!(value >= 0.0) || value >= (double)JW_BOOT_RESUME_BYTES_MAX ||
        floor(value) != value) {
        return false;
    }
    *out = (unsigned long long)value;
    return true;
}

jw_boot_resume_load_result jw_boot_resume_load(const char *dir, const char *platform,
                                               jw_boot_resume_record *out,
                                               char *reason, size_t reason_size) {
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    jw__reason(reason, reason_size, "invalid-arguments");
    char path[JW_BOOT_RESUME_PATH_MAX];
    if (!out || !dir || !dir[0] || !jw__path(path, sizeof(path), dir,
                                             JW_BOOT_RESUME_FILENAME)) {
        return JW_BOOT_RESUME_LOAD_ERROR;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        if (errno == ENOENT) {
            jw__reason(reason, reason_size, "absent");
            return JW_BOOT_RESUME_LOAD_ABSENT;
        }
        /* ELOOP: a symlink in the record's place is not a record. */
        jw__reason(reason, reason_size, errno == ELOOP ? "not-a-file" : "open-failed");
        return errno == ELOOP ? JW_BOOT_RESUME_LOAD_INVALID : JW_BOOT_RESUME_LOAD_ERROR;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        jw__reason(reason, reason_size, "open-failed");
        return JW_BOOT_RESUME_LOAD_ERROR;
    }
    if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
        (uintmax_t)st.st_size > JW_BOOT_RESUME_FILE_MAX) {
        close(fd);
        jw__reason(reason, reason_size, !S_ISREG(st.st_mode) ? "not-a-file" : "size");
        return JW_BOOT_RESUME_LOAD_INVALID;
    }
    size_t len = (size_t)st.st_size;
    char *json = malloc(len + 1u);
    if (!json) {
        close(fd);
        jw__reason(reason, reason_size, "out-of-memory");
        return JW_BOOT_RESUME_LOAD_ERROR;
    }
    size_t read_len = 0;
    while (read_len < len) {
        ssize_t n = read(fd, json + read_len, len - read_len);
        if (n > 0) {
            read_len += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
    close(fd);
    if (read_len != len) {
        free(json);
        jw__reason(reason, reason_size, "read-failed");
        return JW_BOOT_RESUME_LOAD_ERROR;
    }
    if (memchr(json, '\0', len) != NULL) {
        free(json);
        jw__reason(reason, reason_size, "malformed");
        return JW_BOOT_RESUME_LOAD_INVALID;
    }
    json[len] = '\0';

    const char *end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, len, &end, false);
    /* The object must be the whole file. */
    bool whole = end == json + len;
    free(json);
    const char *bad = NULL;
    unsigned long long schema = 0;
    unsigned long long slot = 0;
    if (!root || !cJSON_IsObject(root) || !whole) {
        bad = "malformed";
    } else if (!jw__copy_count(root, "schema", &schema) ||
               schema != JW_BOOT_RESUME_SCHEMA) {
        /* Checked before the shape: a future schema may add or drop keys. */
        bad = "schema";
    } else if (jw__object_size(root) != JW_BOOT_RESUME_FIELD_COUNT) {
        bad = "fields";
    } else if (!jw__copy_string(root, "platform", out->platform,
                                sizeof(out->platform), false) ||
               !jw__copy_string(root, "boot_id", out->boot_id,
                                sizeof(out->boot_id), false) ||
               !jw__copy_string(root, "request_id", out->request_id,
                                sizeof(out->request_id), false) ||
               !jw__copy_string(root, "source_fingerprint", out->source_fingerprint,
                                sizeof(out->source_fingerprint), false) ||
               !jw__copy_string(root, "system", out->system,
                                sizeof(out->system), false) ||
               !jw__copy_string(root, "rom_path", out->rom_path,
                                sizeof(out->rom_path), false) ||
               !jw__copy_string(root, "core_id", out->core_id,
                                sizeof(out->core_id), false) ||
               !jw__copy_string(root, "core_config_folder", out->core_config_folder,
                                sizeof(out->core_config_folder), false) ||
               !jw__copy_string(root, "provider", out->provider,
                                sizeof(out->provider), true) ||
               !jw__copy_string(root, "state_path", out->state_path,
                                sizeof(out->state_path), false) ||
               !jw__copy_count(root, "slot", &slot) || slot > 999 ||
               !jw__copy_count(root, "state_bytes", &out->state_bytes) ||
               !cJSON_IsBool(cJSON_GetObjectItemCaseSensitive(root, "offline_proxy"))) {
        bad = "fields";
    } else {
        out->offline_proxy =
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "offline_proxy"));
    }
    cJSON_Delete(root);
    if (!bad) {
        out->slot = (int)slot;
        char field[32];
        if (!jw_boot_resume_record_valid(out, platform, field, sizeof(field))) {
            jw__reason(reason, reason_size, field);
            memset(out, 0, sizeof(*out));
            return JW_BOOT_RESUME_LOAD_INVALID;
        }
        jw__reason(reason, reason_size, "ok");
        return JW_BOOT_RESUME_LOAD_VALID;
    }
    memset(out, 0, sizeof(*out));
    jw__reason(reason, reason_size, bad);
    return JW_BOOT_RESUME_LOAD_INVALID;
}

bool jw_boot_resume_consume(const char *dir, char *reason, size_t reason_size) {
    char path[JW_BOOT_RESUME_PATH_MAX];
    if (!dir || !dir[0] || !jw__path(path, sizeof(path), dir, JW_BOOT_RESUME_FILENAME)) {
        jw__reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    if (unlink(path) != 0 && errno != ENOENT) {
        jw__reason(reason, reason_size, "unlink-failed");
        return false;
    }
    if (!jw__fsync_dir(dir)) {
        jw__reason(reason, reason_size, "directory-fsync-failed");
        return false;
    }
    jw__reason(reason, reason_size, "ok");
    return true;
}

jw_boot_resume_decision jw_boot_resume_decide(const jw_boot_resume_record *record,
                                              const jw_boot_resume_boot_facts *facts) {
    if (!record || !facts || facts->storage_recovery) {
        return JW_BOOT_RESUME_DISCARD_STORAGE_RECOVERY;
    }
    if (!facts->save_setting_on || !facts->resume_setting_on) {
        return JW_BOOT_RESUME_DISCARD_SETTING_OFF;
    }
    if (!facts->current_boot_id || !facts->current_boot_id[0]) {
        return JW_BOOT_RESUME_DISCARD_BOOT_UNKNOWN;
    }
    if (strcmp(record->boot_id, facts->current_boot_id) == 0) {
        return JW_BOOT_RESUME_DISCARD_SAME_BOOT;
    }
    if (facts->bypass_held) {
        return JW_BOOT_RESUME_DISCARD_BYPASS;
    }
    return JW_BOOT_RESUME_PROCEED;
}

const char *jw_boot_resume_decision_name(jw_boot_resume_decision decision) {
    switch (decision) {
    case JW_BOOT_RESUME_PROCEED:                  return "proceed";
    case JW_BOOT_RESUME_DISCARD_STORAGE_RECOVERY: return "storage-recovery";
    case JW_BOOT_RESUME_DISCARD_SETTING_OFF:      return "setting-off";
    case JW_BOOT_RESUME_DISCARD_BOOT_UNKNOWN:     return "boot-id-unknown";
    case JW_BOOT_RESUME_DISCARD_SAME_BOOT:        return "same-boot";
    case JW_BOOT_RESUME_DISCARD_BYPASS:           return "bypass";
    }
    return "unknown";
}

bool jw_boot_resume_relative_to(const char *root, const char *abs,
                                char *out, size_t out_size) {
    if (!root || !root[0] || !abs || !out || out_size == 0) {
        return false;
    }
    size_t root_len = strlen(root);
    while (root_len > 1 && root[root_len - 1] == '/') {
        root_len--;
    }
    if (strncmp(abs, root, root_len) != 0 || abs[root_len] != '/') {
        return false;
    }
    const char *rest = abs + root_len + 1;
    if (!jw_storage_relative_path_valid(rest)) {
        return false;
    }
    int n = snprintf(out, out_size, "%s", rest);
    return n >= 0 && (size_t)n < out_size;
}

bool jw_boot_resume_state_file_ok(const jw_boot_resume_record *record,
                                  const char *states_root, const char *rom_abs,
                                  char *out, size_t out_size,
                                  char *reason, size_t reason_size) {
    char expected[JW_BOOT_RESUME_PATH_MAX];
    char recorded[JW_BOOT_RESUME_PATH_MAX];
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    if (!record || !states_root || !states_root[0] || !rom_abs || !rom_abs[0] ||
        !out || out_size == 0 ||
        !jw_ra_slot_state_path_for_core(states_root, record->core_config_folder,
                                        rom_abs, record->slot, false,
                                        expected, sizeof(expected)) ||
        !jw__path(recorded, sizeof(recorded), states_root, record->state_path)) {
        jw__reason(reason, reason_size, "state-path");
        return false;
    }
    /* Never a different slot or another stem: exactly the file the save
       published for this game in this core's namespace. */
    if (strcmp(expected, recorded) != 0) {
        jw__reason(reason, reason_size, "state-path-mismatch");
        return false;
    }
    struct stat st;
    if (lstat(expected, &st) != 0) {
        jw__reason(reason, reason_size, errno == ENOENT ? "state-missing" : "state-unreadable");
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        jw__reason(reason, reason_size, "state-not-a-file");
        return false;
    }
    if (st.st_size <= 0 || (unsigned long long)st.st_size != record->state_bytes) {
        jw__reason(reason, reason_size, "state-size");
        return false;
    }
    int n = snprintf(out, out_size, "%s", expected);
    if (n < 0 || (size_t)n >= out_size) {
        out[0] = '\0';
        jw__reason(reason, reason_size, "state-path");
        return false;
    }
    jw__reason(reason, reason_size, "ok");
    return true;
}
