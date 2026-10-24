#include "internal/storage/health.h"

#include <fcntl.h>

#include "internal/core/log.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/klog.h>
#endif

#define JW__KERNEL_LOG_MAX (1024 * 1024)
#define JW__REPAIR_FILE_MAX 4096

static atomic_bool jw__refresh_requested;

void jw_storage_probe_env_default(jw_storage_probe_env *env) {
    if (!env) {
        return;
    }
    memset(env, 0, sizeof(*env));
    env->mountinfo_path = "/proc/self/mountinfo";
    env->by_uuid_dir = "/dev/disk/by-uuid";
    env->by_label_dir = "/dev/disk/by-label";
    env->sys_dev_block = "/sys/dev/block";
    env->dev_dir = "/dev";
#ifdef PLATFORM_MLP1
    /* Both MLP1 card roots are fixed mount points with empty rootfs stubs
       underneath, so an unmounted root must never pass as a writable card. */
    env->require_mounted_roots = true;
    env->repair_dir = JW_STORAGE_MLP1_REPAIR_DIR;
#endif
}

/* ── Mount table ─────────────────────────────────────────────────────── */

/* mountinfo escapes space, tab, newline and backslash as \ooo. */
static void jw__unescape_mount_field(const char *in, size_t in_len,
                                     char *out, size_t out_size) {
    size_t o = 0;
    for (size_t i = 0; i < in_len && o + 1 < out_size; i++) {
        if (in[i] == '\\' && i + 3 < in_len + 0 &&
            in[i + 1] >= '0' && in[i + 1] <= '3' &&
            in[i + 2] >= '0' && in[i + 2] <= '7' &&
            in[i + 3] >= '0' && in[i + 3] <= '7') {
            out[o++] = (char)(((in[i + 1] - '0') << 6) |
                              ((in[i + 2] - '0') << 3) |
                              (in[i + 3] - '0'));
            i += 3;
        } else {
            out[o++] = in[i];
        }
    }
    out[o] = '\0';
}

static bool jw__option_list_has(const char *options, const char *wanted) {
    size_t wanted_len = strlen(wanted);
    const char *cursor = options;
    while (cursor && *cursor) {
        const char *comma = strchr(cursor, ',');
        size_t len = comma ? (size_t)(comma - cursor) : strlen(cursor);
        if (len == wanted_len && strncmp(cursor, wanted, len) == 0) {
            return true;
        }
        cursor = comma ? comma + 1 : NULL;
    }
    return false;
}

/* Split the next space-separated field. Returns false at end of line. */
static bool jw__next_field(const char **cursor, const char *end,
                           const char **field, size_t *len) {
    const char *p = *cursor;
    while (p < end && *p == ' ') {
        p++;
    }
    if (p >= end) {
        return false;
    }
    const char *start = p;
    while (p < end && *p != ' ') {
        p++;
    }
    *field = start;
    *len = (size_t)(p - start);
    *cursor = p;
    return true;
}

static bool jw__parse_mountinfo_line(const char *line, const char *end,
                                     jw_storage_mount *out) {
    const char *cursor = line;
    const char *field = NULL;
    size_t len = 0;
    memset(out, 0, sizeof(*out));

    /* mount ID, parent ID */
    if (!jw__next_field(&cursor, end, &field, &len) ||
        !jw__next_field(&cursor, end, &field, &len)) {
        return false;
    }
    /* major:minor */
    if (!jw__next_field(&cursor, end, &field, &len)) {
        return false;
    }
    char devnum[32];
    if (len >= sizeof(devnum)) {
        return false;
    }
    memcpy(devnum, field, len);
    devnum[len] = '\0';
    if (sscanf(devnum, "%u:%u", &out->major, &out->minor) != 2) {
        return false;
    }
    /* root within the filesystem (unused), mount point, mount options */
    if (!jw__next_field(&cursor, end, &field, &len) ||
        !jw__next_field(&cursor, end, &field, &len)) {
        return false;
    }
    jw__unescape_mount_field(field, len, out->mount_point,
                             sizeof(out->mount_point));
    if (!jw__next_field(&cursor, end, &field, &len)) {
        return false;
    }
    jw__unescape_mount_field(field, len, out->mount_options,
                             sizeof(out->mount_options));
    /* optional fields up to the "-" separator */
    for (;;) {
        if (!jw__next_field(&cursor, end, &field, &len)) {
            return false;
        }
        if (len == 1 && field[0] == '-') {
            break;
        }
    }
    if (!jw__next_field(&cursor, end, &field, &len)) {
        return false;
    }
    jw__unescape_mount_field(field, len, out->fs_type, sizeof(out->fs_type));
    if (!jw__next_field(&cursor, end, &field, &len)) {
        return false;
    }
    jw__unescape_mount_field(field, len, out->device, sizeof(out->device));
    if (jw__next_field(&cursor, end, &field, &len)) {
        jw__unescape_mount_field(field, len, out->super_options,
                                 sizeof(out->super_options));
    }
    out->read_only = jw__option_list_has(out->mount_options, "ro") ||
                     jw__option_list_has(out->super_options, "ro");
    return true;
}

int jw_storage_mount_table_parse(const char *text, jw_storage_mount_table *out) {
    if (!out) {
        return -1;
    }
    out->count = 0;
    if (!text) {
        return -1;
    }
    const char *line = text;
    while (*line) {
        const char *end = strchr(line, '\n');
        if (!end) {
            end = line + strlen(line);
        }
        if (out->count < JW_STORAGE_MOUNT_MAX &&
            jw__parse_mountinfo_line(line, end, &out->entries[out->count])) {
            out->count++;
        }
        line = *end ? end + 1 : end;
    }
    return 0;
}

static char *jw__read_small_file(const char *path, size_t max, size_t *out_len) {
    FILE *fp = path ? fopen(path, "r") : NULL;
    if (!fp) {
        return NULL;
    }
    size_t cap = 4096;
    size_t len = 0;
    char *buf = malloc(cap);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    for (;;) {
        if (len + 1 >= cap) {
            if (cap >= max) {
                break;
            }
            size_t next = cap * 2 > max ? max : cap * 2;
            char *grown = realloc(buf, next);
            if (!grown) {
                break;
            }
            buf = grown;
            cap = next;
        }
        size_t n = fread(buf + len, 1, cap - len - 1, fp);
        if (n == 0) {
            break;
        }
        len += n;
    }
    fclose(fp);
    buf[len] = '\0';
    if (out_len) {
        *out_len = len;
    }
    return buf;
}

int jw_storage_mount_table_read(const char *mountinfo_path,
                                jw_storage_mount_table *out) {
    char *text = jw__read_small_file(
        mountinfo_path ? mountinfo_path : "/proc/self/mountinfo",
        256 * 1024, NULL);
    if (!text) {
        if (out) {
            out->count = 0;
        }
        return -1;
    }
    int rc = jw_storage_mount_table_parse(text, out);
    free(text);
    return rc;
}

const jw_storage_mount *jw_storage_mount_at(const jw_storage_mount_table *table,
                                            const char *mount_point) {
    if (!table || !mount_point) {
        return NULL;
    }
    /* Later entries shadow earlier ones mounted at the same point. */
    for (int i = table->count - 1; i >= 0; i--) {
        if (strcmp(table->entries[i].mount_point, mount_point) == 0) {
            return &table->entries[i];
        }
    }
    return NULL;
}

const jw_storage_mount *jw_storage_mount_for_path(const jw_storage_mount_table *table,
                                                  const char *abs_path) {
    if (!table || !abs_path || abs_path[0] != '/') {
        return NULL;
    }
    const jw_storage_mount *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < table->count; i++) {
        const jw_storage_mount *m = &table->entries[i];
        size_t len = strlen(m->mount_point);
        bool contains;
        if (len == 1 && m->mount_point[0] == '/') {
            contains = true;
        } else {
            contains = strncmp(abs_path, m->mount_point, len) == 0 &&
                       (abs_path[len] == '\0' || abs_path[len] == '/');
        }
        /* >= so a later mount over the same point wins. */
        if (contains && len >= best_len) {
            best = m;
            best_len = len;
        }
    }
    return best;
}

/* ── Kernel evidence ─────────────────────────────────────────────────── */

/* Does this log line name device_name as its filesystem device? Returns the
   message after the device tag, or NULL. Accepts "FAT-fs (mmcblk1): msg",
   "EXT4-fs (mmcblk1p1): msg" and "EXT4-fs error (device mmcblk1p1): msg". */
static const char *jw__kernel_fs_message(const char *line, size_t line_len,
                                         const char *device_name, bool *is_error_form) {
    size_t dev_len = strlen(device_name);
    *is_error_form = false;
    for (size_t i = 0; i + 5 < line_len; i++) {
        if (strncmp(line + i, "-fs ", 4) != 0) {
            continue;
        }
        const char *p = line + i + 4;
        const char *end = line + line_len;
        if (end - p > 6 && strncmp(p, "error ", 6) == 0) {
            *is_error_form = true;
            p += 6;
        }
        if (p >= end || *p != '(') {
            continue;
        }
        p++;
        if (end - p > 7 && strncmp(p, "device ", 7) == 0) {
            p += 7;
        }
        if ((size_t)(end - p) < dev_len + 2 ||
            strncmp(p, device_name, dev_len) != 0 || p[dev_len] != ')') {
            continue;
        }
        p += dev_len + 1;
        if (p < end && *p == ':') {
            p++;
        }
        while (p < end && *p == ' ') {
            p++;
        }
        return p;
    }
    return NULL;
}

/* "mmcblk1: mmc1:b36a SDABC 58.2 GiB" -- a card was (re)detected. */
static bool jw__kernel_card_detected(const char *line, size_t line_len,
                                     const char *device_name) {
    size_t dev_len = strlen(device_name);
    for (size_t i = 0; i + dev_len + 5 < line_len; i++) {
        if ((i == 0 || line[i - 1] == ' ' || line[i - 1] == ']') &&
            strncmp(line + i, device_name, dev_len) == 0 &&
            strncmp(line + i + dev_len, ": mmc", 5) == 0) {
            return true;
        }
    }
    return false;
}

static bool jw__contains_n(const char *hay, size_t hay_len, const char *needle) {
    size_t n = strlen(needle);
    if (n == 0 || hay_len < n) {
        return false;
    }
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (memcmp(hay + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

void jw_storage_kernel_scan(const char *log_text, const char *device_name,
                            jw_storage_kernel_evidence *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!log_text || !device_name || !device_name[0]) {
        return;
    }
    const char *line = log_text;
    while (*line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        bool error_form = false;
        const char *msg = jw__kernel_fs_message(line, len, device_name, &error_form);
        if (!msg && jw__kernel_card_detected(line, len, device_name)) {
            /* A new card lifetime: nothing earlier belongs to it. */
            memset(out, 0, sizeof(*out));
        } else if (msg) {
            size_t msg_len = len - (size_t)(msg - line);
            bool mount_marker =
                jw__contains_n(msg, msg_len, "is not a recommended IO charset") ||
                jw__contains_n(msg, msg_len, "Volume was not properly unmounted") ||
                jw__contains_n(msg, msg_len, "mounted filesystem");
            bool dirty = jw__contains_n(msg, msg_len, "Volume was not properly unmounted");
            if (mount_marker) {
                /* Two markers belong to one FAT mount (charset warning, then
                   the dirty bit); only a second charset warning or an ext4
                   mount line starts a new lifetime. */
                bool continues_mount = dirty && out->mount_seen &&
                                       !out->set_read_only && !out->message[0];
                if (!continues_mount) {
                    memset(out, 0, sizeof(*out));
                }
                out->mount_seen = true;
                if (dirty) {
                    out->dirty_at_boot = true;
                }
            } else if (out->mount_seen) {
                if (jw__contains_n(msg, msg_len, "set read-only") ||
                    jw__contains_n(msg, msg_len, "Remounting filesystem read-only")) {
                    out->set_read_only = true;
                } else if ((error_form ||
                            (msg_len >= 6 && strncmp(msg, "error,", 6) == 0)) &&
                           !out->message[0]) {
                    size_t copy = msg_len < sizeof(out->message) - 1
                                      ? msg_len : sizeof(out->message) - 1;
                    memcpy(out->message, msg, copy);
                    out->message[copy] = '\0';
                }
            }
        }
        if (!end) {
            break;
        }
        line = end + 1;
    }
}

char *jw_storage_kernel_log_read(void) {
#ifdef __linux__
    int size = klogctl(10 /* SYSLOG_ACTION_SIZE_BUFFER */, NULL, 0);
    if (size <= 0) {
        return NULL;
    }
    if (size > JW__KERNEL_LOG_MAX) {
        size = JW__KERNEL_LOG_MAX;
    }
    char *buf = malloc((size_t)size + 1);
    if (!buf) {
        return NULL;
    }
    int n = klogctl(3 /* SYSLOG_ACTION_READ_ALL */, buf, size);
    if (n < 0) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    return buf;
#else
    return NULL;
#endif
}

/* ── Identity ────────────────────────────────────────────────────────── */

bool jw_storage_uuid_valid(const char *uuid) {
    if (!uuid || !uuid[0] || strlen(uuid) >= JW_STORAGE_UUID_MAX) {
        return false;
    }
    for (const char *p = uuid; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '-') {
            return false;
        }
    }
    return true;
}

static const char *jw__basename(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash ? slash + 1 : path;
}

static bool jw__device_link_lookup(const char *dir_path, const char *device,
                                   char *out, size_t out_size) {
    if (out && out_size) {
        out[0] = '\0';
    }
    if (!dir_path || !device || !device[0] || !out || out_size == 0) {
        return false;
    }
    const char *want = jw__basename(device);
    DIR *dir = opendir(dir_path);
    if (!dir) {
        return false;
    }
    bool found = false;
    int matches = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        char link_path[JW_STORAGE_PATH_MAX];
        char target[JW_STORAGE_PATH_MAX];
        if (snprintf(link_path, sizeof(link_path), "%s/%s", dir_path,
                     entry->d_name) >= (int)sizeof(link_path)) {
            continue;
        }
        ssize_t n = readlink(link_path, target, sizeof(target) - 1);
        if (n <= 0) {
            continue;
        }
        target[n] = '\0';
        if (strcmp(jw__basename(target), want) == 0) {
            matches++;
            if (!found) {
                snprintf(out, out_size, "%s", entry->d_name);
                found = true;
            }
        }
    }
    closedir(dir);
    /* Two names for one device is not an identity. */
    if (matches != 1) {
        out[0] = '\0';
        return false;
    }
    return found;
}

bool jw_storage_device_uuid(const jw_storage_probe_env *env, const char *device,
                            char *uuid, size_t uuid_size) {
    return env && jw__device_link_lookup(env->by_uuid_dir, device, uuid, uuid_size) &&
           jw_storage_uuid_valid(uuid);
}

bool jw_storage_device_label(const jw_storage_probe_env *env, const char *device,
                             char *label, size_t label_size) {
    return env && jw__device_link_lookup(env->by_label_dir, device, label, label_size);
}

static int jw__read_block_ro(const jw_storage_probe_env *env,
                             unsigned major, unsigned minor) {
    if (!env || !env->sys_dev_block || (major == 0 && minor == 0)) {
        return -1;
    }
    char path[JW_STORAGE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/%u:%u/ro", env->sys_dev_block, major, minor);
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    int value = -1;
    if (fscanf(fp, "%d", &value) != 1) {
        value = -1;
    }
    fclose(fp);
    return value;
}

/* ── Repair state files ──────────────────────────────────────────────── */

static bool jw__repair_value_valid(const char *value) {
    if (!value[0]) {
        return false;
    }
    for (const char *p = value; *p; p++) {
        if (!isalnum((unsigned char)*p) && strchr("._:/-", *p) == NULL) {
            return false;
        }
    }
    return true;
}

/* Strict key=value reader: unknown keys and malformed values are ignored,
   and nothing in the file is ever interpreted as a command or a path to act on. */
static bool jw__repair_kv_get(const char *text, const char *key,
                              char *out, size_t out_size) {
    size_t key_len = strlen(key);
    const char *line = text;
    out[0] = '\0';
    while (line && *line) {
        const char *end = strchr(line, '\n');
        size_t len = end ? (size_t)(end - line) : strlen(line);
        if (len > key_len && strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            size_t value_len = len - key_len - 1;
            if (value_len > 0 && line[key_len + 1 + value_len - 1] == '\r') {
                value_len--;
            }
            if (value_len < out_size) {
                memcpy(out, line + key_len + 1, value_len);
                out[value_len] = '\0';
                if (jw__repair_value_valid(out)) {
                    return true;
                }
            }
            out[0] = '\0';
            return false;
        }
        line = end ? end + 1 : NULL;
    }
    return false;
}

jw_storage_repair jw_storage_repair_hold_for_uuid(const jw_storage_probe_env *env,
                                                  const char *uuid,
                                                  char *request_id,
                                                  size_t request_id_size) {
    if (request_id && request_id_size) {
        request_id[0] = '\0';
    }
    if (!env || !env->repair_dir || !jw_storage_uuid_valid(uuid)) {
        return JW_STORAGE_REPAIR_NONE;
    }
    char path[JW_STORAGE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/holds/%s", env->repair_dir, uuid);
    struct stat st;
    if (stat(path, &st) != 0) {
        return JW_STORAGE_REPAIR_NONE;
    }
    char *text = jw__read_small_file(path, JW__REPAIR_FILE_MAX, NULL);
    if (!text) {
        /* A hold that exists but cannot be read still holds. */
        return JW_STORAGE_REPAIR_FAILED;
    }
    char value[64];
    jw_storage_repair repair = JW_STORAGE_REPAIR_FAILED;
    if (jw__repair_kv_get(text, "state", value, sizeof(value))) {
        if (strcmp(value, "pending") == 0) {
            repair = JW_STORAGE_REPAIR_PENDING;
        } else if (strcmp(value, "running") == 0) {
            repair = JW_STORAGE_REPAIR_RUNNING;
        }
    }
    if (request_id && request_id_size) {
        (void)jw__repair_kv_get(text, "request_id", request_id, request_id_size);
    }
    free(text);
    return repair;
}

/* Triggers name a reason, never a path or free text. */
static bool jw__repair_trigger_valid(const char *value) {
    if (!value || !value[0]) return false;
    for (const char *p = value; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || *p == '-')) return false;
    }
    return true;
}

bool jw_storage_repair_hold_trigger(const jw_storage_probe_env *env, const char *uuid,
                                    char *out, size_t out_size) {
    if (!out || out_size == 0) return false;
    out[0] = '\0';
    if (!env || !env->repair_dir || !jw_storage_uuid_valid(uuid)) return false;
    char path[JW_STORAGE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/holds/%s", env->repair_dir, uuid);
    char *text = jw__read_small_file(path, JW__REPAIR_FILE_MAX, NULL);
    if (!text) return false;
    bool found = jw__repair_kv_get(text, "trigger", out, out_size) &&
                 jw__repair_trigger_valid(out);
    free(text);
    if (!found) out[0] = '\0';
    return found;
}

bool jw_storage_repair_last_result(const jw_storage_probe_env *env, const char *uuid,
                                   jw_storage_repair_result *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!env || !env->repair_dir || !jw_storage_uuid_valid(uuid)) {
        return false;
    }
    char path[JW_STORAGE_PATH_MAX];
    snprintf(path, sizeof(path), "%s/last-results/%s", env->repair_dir, uuid);
    char *pointer = jw__read_small_file(path, 256, NULL);
    if (!pointer) {
        snprintf(path, sizeof(path), "%s/last-result", env->repair_dir);
        pointer = jw__read_small_file(path, 256, NULL);
    }
    if (!pointer) {
        return false;
    }
    char id[JW_STORAGE_REQUEST_ID_MAX];
    size_t id_len = strcspn(pointer, "\r\n");
    if (id_len == 0 || id_len >= sizeof(id)) {
        free(pointer);
        return false;
    }
    memcpy(id, pointer, id_len);
    id[id_len] = '\0';
    free(pointer);
    if (!jw__repair_value_valid(id) || strchr(id, '/')) {
        return false;
    }
    snprintf(path, sizeof(path), "%s/results/%s.summary", env->repair_dir, id);
    char *text = jw__read_small_file(path, JW__REPAIR_FILE_MAX, NULL);
    if (!text) {
        return false;
    }
    char value[64];
    if (!jw__repair_kv_get(text, "uuid", value, sizeof(value)) ||
        strcmp(value, uuid) != 0 ||
        !jw__repair_kv_get(text, "request_id", out->request_id,
                           sizeof(out->request_id)) ||
        strcmp(out->request_id, id) != 0 ||
        !jw__repair_kv_get(text, "outcome", out->outcome, sizeof(out->outcome))) {
        free(text);
        memset(out, 0, sizeof(*out));
        return false;
    }
    (void)jw__repair_kv_get(text, "mount_state", out->mount_state,
                            sizeof(out->mount_state));
    (void)jw__repair_kv_get(text, "mode", out->mode, sizeof(out->mode));
    if (!jw__repair_kv_get(text, "origin", out->origin, sizeof(out->origin))) {
        snprintf(out->origin, sizeof(out->origin), "user-request");
    }
    if (!jw__repair_kv_get(text, "trigger", out->trigger, sizeof(out->trigger)) ||
        !jw__repair_trigger_valid(out->trigger)) {
        out->trigger[0] = '\0';
    }
    if (jw__repair_kv_get(text, "changes_complete", value, sizeof(value))) {
        out->changes_complete = strcmp(value, "true") == 0;
    }
    if (jw__repair_kv_get(text, "reported_changes", value, sizeof(value))) {
        out->reported_change_count = atoi(value);
    }
    free(text);
    snprintf(path, sizeof(path), "%s/acks/%s", env->repair_dir, id);
    struct stat st;
    out->acknowledged = stat(path, &st) == 0;
    out->valid = true;
    return true;
}

/* ── Probe ───────────────────────────────────────────────────────────── */

void jw_storage_health_probe(const jw_storage_probe_env *env,
                             const char *source_id, const char *root,
                             const char *kernel_log, jw_storage_health *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->source_id, sizeof(out->source_id), "%s", source_id ? source_id : "");
    snprintf(out->root, sizeof(out->root), "%s", root ? root : "");
    if (!env || !root || !root[0]) {
        return;
    }

    char resolved[JW_STORAGE_PATH_MAX];
    const char *lookup = root;
    if (realpath(root, resolved)) {
        lookup = resolved;
    }

    jw_storage_mount_table *table = malloc(sizeof(*table));
    if (!table) {
        return;
    }
    if (jw_storage_mount_table_read(env->mountinfo_path, table) != 0) {
        free(table);
        /* No mount table (macOS, a restricted /proc): where roots need not be
           mounts, the filesystem's own read-only flag is the only evidence. */
        struct statvfs vfs;
        if (!env->require_mounted_roots && !env->skip_statvfs &&
            statvfs(lookup, &vfs) == 0) {
            out->mounted = true;
            out->access = (vfs.f_flag & ST_RDONLY) ? JW_STORAGE_ACCESS_READ_ONLY
                                                   : JW_STORAGE_ACCESS_READ_WRITE;
        }
        return; /* otherwise access stays unknown */
    }
    const jw_storage_mount *mount = env->require_mounted_roots
        ? jw_storage_mount_at(table, lookup)
        : jw_storage_mount_for_path(table, lookup);
    if (!mount) {
        free(table);
        return; /* not mounted: never writable */
    }
    out->mounted = true;
    snprintf(out->device, sizeof(out->device), "%s", mount->device);
    snprintf(out->fs_type, sizeof(out->fs_type), "%s", mount->fs_type);
    out->major = mount->major;
    out->minor = mount->minor;
    bool read_only = mount->read_only;
    free(table);

    out->access = read_only ? JW_STORAGE_ACCESS_READ_ONLY : JW_STORAGE_ACCESS_READ_WRITE;
    if (!env->skip_statvfs) {
        struct statvfs vfs;
        if (statvfs(lookup, &vfs) != 0) {
            out->access = JW_STORAGE_ACCESS_UNKNOWN;
        } else if (vfs.f_flag & ST_RDONLY) {
            out->access = JW_STORAGE_ACCESS_READ_ONLY;
        }
    }

    if (out->device[0] == '/') {
        (void)jw_storage_device_uuid(env, out->device, out->uuid, sizeof(out->uuid));
        (void)jw_storage_device_label(env, out->device, out->label, sizeof(out->label));
    }
    out->block_write_protected = jw__read_block_ro(env, out->major, out->minor) == 1;
    out->repair = jw_storage_repair_hold_for_uuid(env, out->uuid,
                                                  out->repair_request_id,
                                                  sizeof(out->repair_request_id));

    if (out->block_write_protected && out->access == JW_STORAGE_ACCESS_READ_ONLY) {
        out->cause = JW_STORAGE_CAUSE_WRITE_PROTECTED;
    }
    if (kernel_log && out->device[0]) {
        jw_storage_kernel_evidence evidence;
        jw_storage_kernel_scan(kernel_log, jw__basename(out->device), &evidence);
        if (evidence.mount_seen) {
            out->dirty_at_boot = evidence.dirty_at_boot;
            if (out->access == JW_STORAGE_ACCESS_READ_ONLY &&
                out->cause == JW_STORAGE_CAUSE_UNKNOWN && evidence.set_read_only) {
                out->cause = JW_STORAGE_CAUSE_FILESYSTEM_ERROR;
                snprintf(out->kernel_message, sizeof(out->kernel_message), "%s",
                         evidence.message);
            }
        }
    }
}

/* ── Unmounted held card ─────────────────────────────────────────────── */

/* Boot-sector signature only: enough to offer a check, which the repair
   runner revalidates with blkid before touching the card. */
static bool jw__block_fs_type(const char *path, char *out, size_t out_size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    unsigned char sector[512];
    ssize_t n = pread(fd, sector, sizeof(sector), 0);
    close(fd);
    if (n != (ssize_t)sizeof(sector) || sector[510] != 0x55 || sector[511] != 0xAA) {
        return false;
    }
    if (memcmp(sector + 3, "EXFAT   ", 8) == 0) {
        snprintf(out, out_size, "%s", "exfat");
        return true;
    }
    if (memcmp(sector + 82, "FAT32   ", 8) == 0 || memcmp(sector + 54, "FAT16   ", 8) == 0 ||
        memcmp(sector + 54, "FAT12   ", 8) == 0) {
        snprintf(out, out_size, "%s", "vfat");
        return true;
    }
    return false;
}

bool jw_storage_health_probe_unmounted_hold(const jw_storage_probe_env *env,
                                            jw_storage_health *out) {
    if (!env || !out || out->mounted || !env->repair_dir || !env->by_uuid_dir) {
        return false;
    }
    char holds[JW_STORAGE_PATH_MAX];
    if (snprintf(holds, sizeof(holds), "%s/holds", env->repair_dir) >= (int)sizeof(holds)) {
        return false;
    }
    DIR *dir = opendir(holds);
    if (!dir) {
        return false;
    }
    jw_storage_mount_table *table = malloc(sizeof(*table));
    if (!table || jw_storage_mount_table_read(env->mountinfo_path, table) != 0) {
        free(table);
        closedir(dir);
        return false;
    }
    bool found = false;
    struct dirent *entry;
    while (!found && (entry = readdir(dir)) != NULL) {
        if (!jw_storage_uuid_valid(entry->d_name)) {
            continue;
        }
        char link_path[JW_STORAGE_PATH_MAX];
        char target[JW_STORAGE_PATH_MAX];
        if (snprintf(link_path, sizeof(link_path), "%s/%s", env->by_uuid_dir,
                     entry->d_name) >= (int)sizeof(link_path)) {
            continue;
        }
        ssize_t n = readlink(link_path, target, sizeof(target) - 1);
        if (n <= 0) {
            continue;
        }
        target[n] = '\0';
        const char *name = jw__basename(target);
        char device[JW_STORAGE_DEVICE_MAX];
        char open_path[JW_STORAGE_PATH_MAX];
        if (!name[0] ||
            snprintf(device, sizeof(device), "/dev/%s", name) >= (int)sizeof(device) ||
            snprintf(open_path, sizeof(open_path), "%s/%s",
                     env->dev_dir ? env->dev_dir : "/dev", name) >= (int)sizeof(open_path)) {
            continue;
        }
        bool mounted = false;
        for (int i = 0; i < table->count && !mounted; i++) {
            mounted = strcmp(table->entries[i].device, device) == 0;
        }
        char fs_type[JW_STORAGE_FS_TYPE_MAX];
        if (mounted || !jw__block_fs_type(open_path, fs_type, sizeof(fs_type))) {
            continue;
        }
        if (snprintf(out->uuid, sizeof(out->uuid), "%s", entry->d_name) >=
            (int)sizeof(out->uuid)) {
            out->uuid[0] = '\0';
            continue;
        }
        snprintf(out->device, sizeof(out->device), "%s", device);
        snprintf(out->fs_type, sizeof(out->fs_type), "%s", fs_type);
        (void)jw_storage_device_label(env, device, out->label, sizeof(out->label));
        out->repair = jw_storage_repair_hold_for_uuid(env, out->uuid,
                                                      out->repair_request_id,
                                                      sizeof(out->repair_request_id));
        found = out->repair != JW_STORAGE_REPAIR_NONE;
        if (!found) {
            out->device[0] = out->uuid[0] = out->fs_type[0] = out->label[0] = '\0';
        }
    }
    free(table);
    closedir(dir);
    return found;
}

const char *jw_storage_access_name(jw_storage_access access) {
    switch (access) {
        case JW_STORAGE_ACCESS_READ_WRITE: return "read-write";
        case JW_STORAGE_ACCESS_READ_ONLY: return "read-only";
        default: return "unknown";
    }
}

const char *jw_storage_cause_name(jw_storage_cause cause) {
    switch (cause) {
        case JW_STORAGE_CAUSE_FILESYSTEM_ERROR: return "filesystem-error";
        case JW_STORAGE_CAUSE_WRITE_PROTECTED: return "write-protected";
        default: return "unknown";
    }
}

const char *jw_storage_repair_name(jw_storage_repair repair) {
    switch (repair) {
        case JW_STORAGE_REPAIR_PENDING: return "pending";
        case JW_STORAGE_REPAIR_RUNNING: return "running";
        case JW_STORAGE_REPAIR_FAILED: return "failed";
        default: return "none";
    }
}

const char *jw_storage_write_verdict_reason(jw_storage_write_verdict verdict) {
    switch (verdict) {
        case JW_STORAGE_WRITE_OK: return "";
        case JW_STORAGE_WRITE_MISSING: return "storage-missing";
        case JW_STORAGE_WRITE_READ_ONLY: return "storage-read-only";
        case JW_STORAGE_WRITE_REPAIR_HOLD: return "storage-repair-hold";
        default: return "storage-unknown";
    }
}

/* ── Writer gate ─────────────────────────────────────────────────────── */

static jw_storage_write_verdict jw__verdict(jw_storage_write_verdict verdict,
                                           char *reason, size_t reason_size) {
    if (reason && reason_size) {
        snprintf(reason, reason_size, "%s", jw_storage_write_verdict_reason(verdict));
    }
    return verdict;
}

/* Nearest existing ancestor of path, resolved. */
static bool jw__resolve_existing(const char *path, char *out, size_t out_size) {
    char work[JW_STORAGE_PATH_MAX];
    if (snprintf(work, sizeof(work), "%s", path) >= (int)sizeof(work)) {
        return false;
    }
    for (;;) {
        if (realpath(work, out)) {
            return strlen(out) < out_size;
        }
        if (errno != ENOENT && errno != ENOTDIR) {
            return false;
        }
        char *slash = strrchr(work, '/');
        if (!slash) {
            return false;
        }
        if (slash == work) {
            work[1] = '\0';
        } else {
            *slash = '\0';
        }
    }
}

jw_storage_write_verdict jw_storage_path_check_env(const jw_storage_probe_env *env,
                                                   const char *const *roots,
                                                   int root_count,
                                                   const char *path,
                                                   char *reason, size_t reason_size) {
    if (!env || !path || !path[0]) {
        return jw__verdict(JW_STORAGE_WRITE_UNKNOWN, reason, reason_size);
    }
    char abs[JW_STORAGE_PATH_MAX];
    if (!jw__resolve_existing(path, abs, sizeof(abs))) {
        return jw__verdict(JW_STORAGE_WRITE_UNKNOWN, reason, reason_size);
    }

    const char *card_root = NULL;
    char root_abs[JW_STORAGE_PATH_MAX];
    size_t card_root_len = 0;
    for (int i = 0; i < root_count; i++) {
        if (!roots[i] || !roots[i][0] || !realpath(roots[i], root_abs)) {
            continue;
        }
        size_t len = strlen(root_abs);
        if (strncmp(abs, root_abs, len) == 0 &&
            (abs[len] == '\0' || abs[len] == '/') && len > card_root_len) {
            card_root = roots[i];
            card_root_len = len;
        }
    }

    jw_storage_mount_table *table = malloc(sizeof(*table));
    if (!table) {
        return jw__verdict(JW_STORAGE_WRITE_UNKNOWN, reason, reason_size);
    }
    if (jw_storage_mount_table_read(env->mountinfo_path, table) != 0) {
        free(table);
        struct statvfs vfs;
        if (env->require_mounted_roots || env->skip_statvfs || statvfs(abs, &vfs) != 0) {
            return jw__verdict(JW_STORAGE_WRITE_UNKNOWN, reason, reason_size);
        }
        return jw__verdict((vfs.f_flag & ST_RDONLY) ? JW_STORAGE_WRITE_READ_ONLY
                                                    : JW_STORAGE_WRITE_OK,
                           reason, reason_size);
    }
    if (card_root && env->require_mounted_roots) {
        char mount_point[JW_STORAGE_PATH_MAX];
        memcpy(mount_point, abs, card_root_len);
        mount_point[card_root_len] = '\0';
        if (!jw_storage_mount_at(table, mount_point)) {
            free(table);
            return jw__verdict(JW_STORAGE_WRITE_MISSING, reason, reason_size);
        }
    }
    const jw_storage_mount *mount = jw_storage_mount_for_path(table, abs);
    if (!mount) {
        free(table);
        return jw__verdict(JW_STORAGE_WRITE_UNKNOWN, reason, reason_size);
    }
    bool read_only = mount->read_only;
    char device[JW_STORAGE_DEVICE_MAX];
    snprintf(device, sizeof(device), "%s", mount->device);
    free(table);
    if (read_only) {
        return jw__verdict(JW_STORAGE_WRITE_READ_ONLY, reason, reason_size);
    }
    if (!env->skip_statvfs) {
        struct statvfs vfs;
        if (statvfs(abs, &vfs) != 0) {
            return jw__verdict(JW_STORAGE_WRITE_UNKNOWN, reason, reason_size);
        }
        if (vfs.f_flag & ST_RDONLY) {
            return jw__verdict(JW_STORAGE_WRITE_READ_ONLY, reason, reason_size);
        }
    }
    if (card_root && env->repair_dir && device[0] == '/') {
        char uuid[JW_STORAGE_UUID_MAX];
        if (jw_storage_device_uuid(env, device, uuid, sizeof(uuid)) &&
            jw_storage_repair_hold_for_uuid(env, uuid, NULL, 0) != JW_STORAGE_REPAIR_NONE) {
            return jw__verdict(JW_STORAGE_WRITE_REPAIR_HOLD, reason, reason_size);
        }
    }
    return jw__verdict(JW_STORAGE_WRITE_OK, reason, reason_size);
}

jw_storage_write_verdict jw_storage_path_check(const char *path,
                                               char *reason, size_t reason_size) {
    jw_storage_probe_env env;
    jw_storage_probe_env_default(&env);

    const char *paths = getenv("SDCARD_PATHS");
    if (!paths || !paths[0]) {
        paths = getenv("SDCARD_PATH");
    }
    if (!paths || !paths[0]) {
        paths = getenv("JAWAKA_SDCARD_ROOT");
    }
    char copy[JW_STORAGE_PATH_MAX * 2];
    const char *roots[JW_STORAGE_MAX_SOURCES];
    int root_count = 0;
    if (paths && snprintf(copy, sizeof(copy), "%s", paths) < (int)sizeof(copy)) {
        char *save = NULL;
        for (char *token = strtok_r(copy, ":", &save);
             token && root_count < JW_STORAGE_MAX_SOURCES;
             token = strtok_r(NULL, ":", &save)) {
            if (token[0]) {
                roots[root_count++] = token;
            }
        }
    }
    return jw_storage_path_check_env(&env, roots, root_count, path, reason, reason_size);
}

bool jw_storage_path_writable(const char *path, char *reason, size_t reason_size) {
    return jw_storage_path_check(path, reason, reason_size) == JW_STORAGE_WRITE_OK;
}

bool jw_storage_errno_is_storage_failure(int err) {
    return err == EROFS;
}

void jw_storage_report_write_error(const char *path, int err) {
    (void)path;
    /* EIO alone does not prove read-only storage, but it is exactly what a
       card that is about to flip reports first; a refresh is cheap. */
    if (err == EROFS || err == EIO) {
        atomic_store(&jw__refresh_requested, true);
    }
}

bool jw_storage_take_refresh_request(void) {
    return atomic_exchange(&jw__refresh_requested, false);
}

/* ── Monitor ─────────────────────────────────────────────────────────── */

static bool jw__same_identity(const jw_storage_health *a, const jw_storage_health *b) {
    return a->mounted == b->mounted && a->major == b->major && a->minor == b->minor &&
           strcmp(a->uuid, b->uuid) == 0 && strcmp(a->root, b->root) == 0;
}

bool jw_storage_health_monitor_update(jw_storage_health_monitor *monitor,
                                      int index, const jw_storage_health *probe,
                                      bool *newly_read_only) {
    if (newly_read_only) {
        *newly_read_only = false;
    }
    if (!monitor || !probe || index < 0 || index >= JW_STORAGE_MAX_SOURCES) {
        return false;
    }
    if (index >= monitor->count) {
        monitor->count = index + 1;
    }
    jw_storage_health_slot *slot = &monitor->slots[index];
    jw_storage_health next = *probe;
    bool same = slot->valid && jw__same_identity(&slot->health, &next);

    /* A probe without kernel evidence must not erase an explanation already
       established for this same card and mount lifetime. */
    if (same && next.cause == JW_STORAGE_CAUSE_UNKNOWN &&
        next.access == JW_STORAGE_ACCESS_READ_ONLY &&
        slot->health.access == JW_STORAGE_ACCESS_READ_ONLY) {
        next.cause = slot->health.cause;
        snprintf(next.kernel_message, sizeof(next.kernel_message), "%s",
                 slot->health.kernel_message);
    }
    if (same && !next.dirty_at_boot) {
        next.dirty_at_boot = slot->health.dirty_at_boot;
    }

    bool changed = !slot->valid || !same ||
                   slot->health.access != next.access ||
                   slot->health.cause != next.cause ||
                   slot->health.repair != next.repair ||
                   strcmp(slot->health.fs_type, next.fs_type) != 0;
    if (next.access == JW_STORAGE_ACCESS_READ_ONLY &&
        (!same || slot->health.access != JW_STORAGE_ACCESS_READ_ONLY)) {
        if (newly_read_only) {
            *newly_read_only = true;
        }
    }
    if (!same) {
        slot->kernel_checked = false;
    }
    if (changed) {
        monitor->generation++;
        jw_log_info("storage: %s root=%s mounted=%d access=%s cause=%s repair=%s "
                    "device=%s fs=%s uuid=%s dirty_at_boot=%d%s%s",
                    next.source_id, next.root, next.mounted ? 1 : 0,
                    jw_storage_access_name(next.access),
                    jw_storage_cause_name(next.cause),
                    jw_storage_repair_name(next.repair),
                    next.device[0] ? next.device : "-",
                    next.fs_type[0] ? next.fs_type : "-",
                    next.uuid[0] ? next.uuid : "-",
                    next.dirty_at_boot ? 1 : 0,
                    next.kernel_message[0] ? " kernel=" : "",
                    next.kernel_message);
    }
    slot->health = next;
    slot->valid = true;
    return changed;
}
