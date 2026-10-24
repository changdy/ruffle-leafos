#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/store/pakrat_recovery.h"

#include "internal/storage/sources.h"
#include "internal/store/pakrat_kind.h"
#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define JW_PAKRAT_STAGE_PREFIX ".pakrat-stage-"
#define JW_PAKRAT_ROLLBACK_PREFIX ".pakrat-rollback-"
#define JW_PAKRAT_ORIGIN_PREFIX ".pakrat-origin-"
#define JW_PAKRAT_ENTRY_POINT "launch.sh"
#define JW_PAKRAT_COMMIT_MARKER ".pakrat-commit"
#define JW_PAKRAT_MANIFEST_MAX_BYTES (1024L * 1024L)
#define JW_PAKRAT_COMMIT_MAX_BYTES 4096L
/* The launcher reads at most 64 KiB of theme.json (THEME-1). */
#define JW_PAKRAT_THEME_MANIFEST_MAX_BYTES (64L * 1024L)

/* TXN-1 pending uninstall is an irreversible, confirmed forward operation.
   P1 recovery must never restore or adopt that target while the uninstall
   record exists. Keep this query local so the generic P1 module does not
   depend on the higher-level transaction implementation. */
static int jw__pakrat_pending_uninstall_exists(const char *db_path,
                                               const char *store_id) {
    if (!db_path || !store_id || !store_id[0]) {
        return -1;
    }
    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) != 0 || jw_db_apply_schema(db) != 0) {
        jw_db_close(db);
        return -1;
    }
    sqlite3_stmt *stmt = NULL;
    int rc = -1;
    if (sqlite3_prepare_v2(
            db,
            "SELECT 1 FROM pakrat_pending_uninstalls WHERE store_id=?1;",
            -1, &stmt, NULL) == SQLITE_OK &&
        sqlite3_bind_text(stmt, 1, store_id, -1, SQLITE_TRANSIENT) ==
            SQLITE_OK) {
        int step = sqlite3_step(stmt);
        rc = step == SQLITE_ROW ? 1 : (step == SQLITE_DONE ? 0 : -1);
    }
    sqlite3_finalize(stmt);
    jw_db_close(db);
    return rc;
}

int jw__pakrat_copy(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0 || !value) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s", value);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

int jw__pakrat_join2(char *out, size_t out_size, const char *a, const char *b) {
    if (!out || out_size == 0 || !a || !b) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s/%s", a, b);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

int jw__pakrat_join3(char *out, size_t out_size, const char *a, const char *b,
                     const char *c) {
    if (!out || out_size == 0 || !a || !b || !c) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s/%s/%s", a, b, c);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

bool jw__pakrat_path_exists(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0;
}

bool jw__pakrat_is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool jw__pakrat_is_regular_file(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int jw__pakrat_mkdir_p(const char *path, mode_t mode) {
    if (!path || !path[0]) {
        return -1;
    }
    char tmp[PATH_MAX];
    if (jw__pakrat_copy(tmp, sizeof(tmp), path) != 0) {
        return -1;
    }
    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, mode) != 0 &&
                (errno != EEXIST || !jw__pakrat_is_dir(tmp))) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) != 0 &&
        (errno != EEXIST || !jw__pakrat_is_dir(tmp))) {
        return -1;
    }
    return 0;
}

int jw__pakrat_mkdir_parent(const char *path) {
    char parent[PATH_MAX];
    if (jw__pakrat_copy(parent, sizeof(parent), path) != 0) {
        return -1;
    }
    char *slash = strrchr(parent, '/');
    if (!slash) {
        return 0;
    }
    if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return jw__pakrat_mkdir_p(parent, 0755);
}

int jw__pakrat_remove_tree(const char *path) {
    struct stat st;
    if (!path || !path[0]) {
        return -1;
    }
    if (lstat(path, &st) != 0) {
        return errno == ENOENT ? 0 : -1;
    }
    if (!S_ISDIR(st.st_mode) || S_ISLNK(st.st_mode)) {
        return unlink(path) == 0 || errno == ENOENT ? 0 : -1;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return -1;
    }
    struct dirent *entry = NULL;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        if (jw__pakrat_join2(child, sizeof(child), path, entry->d_name) != 0 ||
            jw__pakrat_remove_tree(child) != 0) {
            closedir(dir);
            return -1;
        }
    }
    closedir(dir);
    return rmdir(path) == 0 || errno == ENOENT ? 0 : -1;
}

bool jw__pakrat_safe_name(const char *name) {
    return name && name[0] && strcmp(name, ".") != 0 && strcmp(name, "..") != 0 &&
           !strchr(name, '/') && !strchr(name, '\\');
}

bool jw__pakrat_safe_rel_path(const char *path) {
    if (!path || !path[0] || path[0] == '/' || strchr(path, '\\')) {
        return false;
    }
    const char *p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (len == 0 || (len == 1 && p[0] == '.') ||
            (len == 2 && p[0] == '.' && p[1] == '.')) {
            return false;
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
    }
    return true;
}

void jw__pakrat_log(const char *state_dir, const char *fmt, ...) {
    if (!state_dir || !state_dir[0] || !fmt) {
        return;
    }
    char log_dir[PATH_MAX];
    char log_path[PATH_MAX];
    if (jw__pakrat_join3(log_dir, sizeof(log_dir), state_dir, "store", "logs") != 0 ||
        jw__pakrat_join2(log_path, sizeof(log_path), log_dir, "pakrat.log") != 0 ||
        jw__pakrat_mkdir_p(log_dir, 0755) != 0) {
        return;
    }
    FILE *fp = fopen(log_path, "a");
    if (!fp) {
        return;
    }

    char timestamp[32];
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S%z", &tm_now);
    fprintf(fp, "%s ", timestamp);

    va_list args;
    va_start(args, fmt);
    vfprintf(fp, fmt, args);
    va_end(args);

    fputc('\n', fp);
    fclose(fp);
}

int jw__pakrat_target_path(const char *sdcard_root, const char *install_path,
                           char *out, size_t out_size) {
    if (!sdcard_root || !sdcard_root[0] || !install_path || !out ||
        out_size == 0) {
        return -1;
    }
    if (jw_pakrat_install_path_kind(install_path) == JW_PAKRAT_KIND_THEME) {
        /* Themes/<id>: exactly one folder directly under the themes root. */
        const char *name =
            install_path + sizeof(JW_PAKRAT_THEME_PATH_PREFIX) - 1;
        if (!jw__pakrat_safe_name(name)) {
            return -1;
        }
        return jw__pakrat_join3(out, out_size, sdcard_root,
                                JW_PAKRAT_THEMES_DIR, name);
    }
    const char *path = install_path;
    if (strncmp(path, "Apps/", 5) == 0) {
        path += 5;
    }
    if (!jw__pakrat_safe_rel_path(path)) {
        return -1;
    }
    return jw__pakrat_join3(out, out_size, sdcard_root, JW_PAKRAT_APPS_DIR,
                            path);
}

int jw__pakrat_target_sibling_path(const char *target, const char *store_id,
                                   const char *kind, char *out,
                                   size_t out_size) {
    if (!target || !target[0] || !store_id || !store_id[0] ||
        !kind || !kind[0] || !out || out_size == 0 ||
        !jw__pakrat_safe_name(store_id) || !jw__pakrat_safe_name(kind)) {
        return -1;
    }
    char parent[PATH_MAX];
    if (jw__pakrat_copy(parent, sizeof(parent), target) != 0) {
        return -1;
    }
    char *slash = strrchr(parent, '/');
    if (!slash) {
        return -1;
    }
    if (slash == parent) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    int n = snprintf(out, out_size, "%s/.pakrat-%s-%s", parent, kind, store_id);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

/* ── Rollback origin marker ───────────────────────────────────────────────
   A rollback sibling is named for its store id, not its install name, so the
   target path cannot be reconstructed from the sibling alone. Recovery reaches
   a recorded install through jw_db_pakrat_list_installs, but an adopted install
   that crashes before its first record is written has no row — and deleting its
   rollback would destroy the only copy of the app that was moved aside. The
   marker records install_path before move-aside so that case can be restored
   instead of swept without leaving an unmarked crash window. */

int jw__pakrat_write_origin_marker(const char *target, const char *store_id,
                                   const char *install_path) {
    char marker[PATH_MAX];
    if (!jw__pakrat_safe_rel_path(install_path) ||
        jw__pakrat_target_sibling_path(target, store_id, "origin",
                                       marker, sizeof(marker)) != 0) {
        return -1;
    }
    FILE *fp = fopen(marker, "wb");
    if (!fp) {
        return -1;
    }
    int ok = fprintf(fp, "%s\n", install_path) > 0;
    if (fflush(fp) != 0) {
        ok = 0;
    }
    if (fclose(fp) != 0) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

int jw__pakrat_read_origin_marker(const char *apps_dir, const char *store_id,
                                  char *out, size_t out_size) {
    char marker[PATH_MAX];
    if (!apps_dir || !out || out_size == 0 || !jw__pakrat_safe_name(store_id)) {
        return -1;
    }
    int marker_n = snprintf(marker, sizeof(marker), "%s/%s%s", apps_dir,
                            JW_PAKRAT_ORIGIN_PREFIX, store_id);
    if (marker_n < 0 || (size_t)marker_n >= sizeof(marker)) {
        return -1;
    }
    FILE *fp = fopen(marker, "rb");
    if (!fp) {
        return -1;
    }
    char line[PATH_MAX];
    char *got = fgets(line, (int)sizeof(line), fp);
    int complete = got && (strchr(line, '\n') != NULL || feof(fp));
    int read_failed = ferror(fp);
    int close_failed = fclose(fp) != 0;
    if (!got || !complete || read_failed || close_failed) {
        return -1;
    }
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[--len] = '\0';
    }
    if (!len || !jw__pakrat_safe_rel_path(line)) {
        return -1;
    }
    return jw__pakrat_copy(out, out_size, line);
}

void jw__pakrat_clear_origin_marker(const char *target, const char *store_id) {
    char marker[PATH_MAX];
    if (jw__pakrat_target_sibling_path(target, store_id, "origin",
                                       marker, sizeof(marker)) == 0) {
        (void)unlink(marker);
    }
}

static bool jw__pakrat_hex_exact(const char *value, size_t length) {
    if (!value || strlen(value) != length) {
        return false;
    }
    for (size_t i = 0; i < length; i++) {
        char c = value[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    return true;
}

static bool jw__pakrat_commit_marker_valid(
    const jw_pakrat_commit_marker *marker) {
    return marker && jw__pakrat_safe_name(marker->store_id) &&
           marker->version[0] &&
           jw__pakrat_hex_exact(marker->artifact_sha256, 64u) &&
           jw__pakrat_hex_exact(marker->token,
                                JW_PAKRAT_COMMIT_TOKEN_HEX_LEN);
}

int jw__pakrat_write_commit_marker(const char *pak_dir,
                                   const jw_pakrat_commit_marker *marker) {
    if (!pak_dir || !jw__pakrat_commit_marker_valid(marker)) {
        return -1;
    }
    char path[PATH_MAX];
    if (jw__pakrat_join2(path, sizeof(path), pak_dir,
                         JW_PAKRAT_COMMIT_MARKER) != 0) {
        return -1;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                  0600);
    if (fd < 0) {
        return -1;
    }
    cJSON *root = cJSON_CreateObject();
    char *json = NULL;
    if (!root || !cJSON_AddNumberToObject(root, "schema", 1) ||
        !cJSON_AddStringToObject(root, "store_id", marker->store_id) ||
        !cJSON_AddStringToObject(root, "version", marker->version) ||
        !cJSON_AddStringToObject(root, "artifact_sha256",
                                marker->artifact_sha256) ||
        !cJSON_AddStringToObject(root, "token", marker->token) ||
        !(json = cJSON_PrintUnformatted(root))) {
        cJSON_Delete(root);
        close(fd);
        unlink(path);
        return -1;
    }
    cJSON_Delete(root);
    size_t length = strlen(json);
    size_t offset = 0;
    int ok = 1;
    while (offset < length) {
        ssize_t written = write(fd, json + offset, length - offset);
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            ok = 0;
            break;
        }
        offset += (size_t)written;
    }
    free(json);
    if (ok && fsync(fd) != 0) {
        ok = 0;
    }
    if (close(fd) != 0) {
        ok = 0;
    }
    if (!ok) {
        unlink(path);
        return -1;
    }
    return 0;
}

static bool jw__pakrat_closed_object(const cJSON *root,
                                     const char *const *allowed,
                                     size_t allowed_count) {
    bool seen[16] = {false};
    if (!cJSON_IsObject(root) || !allowed || allowed_count == 0 ||
        allowed_count > sizeof(seen) / sizeof(seen[0])) {
        return false;
    }
    for (const cJSON *item = root->child; item; item = item->next) {
        size_t matched = allowed_count;
        for (size_t i = 0; i < allowed_count; i++) {
            if (item->string && strcmp(item->string, allowed[i]) == 0) {
                matched = i;
                break;
            }
        }
        if (matched == allowed_count || seen[matched]) {
            return false;
        }
        seen[matched] = true;
    }
    for (size_t i = 0; i < allowed_count; i++) {
        if (!seen[i]) {
            return false;
        }
    }
    return true;
}

int jw__pakrat_read_commit_marker(const char *pak_dir,
                                  jw_pakrat_commit_marker *out) {
    static const char *const keys[] = {
        "schema", "store_id", "version", "artifact_sha256", "token",
    };
    if (!pak_dir || !out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char path[PATH_MAX];
    if (jw__pakrat_join2(path, sizeof(path), pak_dir,
                         JW_PAKRAT_COMMIT_MARKER) != 0) {
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || st.st_size > JW_PAKRAT_COMMIT_MAX_BYTES) {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    size_t size = (size_t)st.st_size;
    char *json = malloc(size + 1u);
    if (!json) {
        close(fd);
        return -1;
    }
    size_t offset = 0;
    while (offset < size) {
        ssize_t got = read(fd, json + offset, size - offset);
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got <= 0) {
            free(json);
            close(fd);
            return -1;
        }
        offset += (size_t)got;
    }
    int close_failed = close(fd) != 0;
    json[size] = '\0';
    cJSON *root = close_failed ? NULL : cJSON_ParseWithOpts(json, NULL, true);
    free(json);
    if (!root || !jw__pakrat_closed_object(
                     root, keys, sizeof(keys) / sizeof(keys[0]))) {
        cJSON_Delete(root);
        return -1;
    }
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(root, "schema");
    const cJSON *store = cJSON_GetObjectItemCaseSensitive(root, "store_id");
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "version");
    const cJSON *sha =
        cJSON_GetObjectItemCaseSensitive(root, "artifact_sha256");
    const cJSON *token = cJSON_GetObjectItemCaseSensitive(root, "token");
    bool valid = cJSON_IsNumber(schema) && schema->valuedouble == 1.0 &&
                 cJSON_IsString(store) && store->valuestring &&
                 cJSON_IsString(version) && version->valuestring &&
                 cJSON_IsString(sha) && sha->valuestring &&
                 cJSON_IsString(token) && token->valuestring &&
                 jw__pakrat_copy(out->store_id, sizeof(out->store_id),
                                 store->valuestring) == 0 &&
                 jw__pakrat_copy(out->version, sizeof(out->version),
                                 version->valuestring) == 0 &&
                 jw__pakrat_copy(out->artifact_sha256,
                                 sizeof(out->artifact_sha256),
                                 sha->valuestring) == 0 &&
                 jw__pakrat_copy(out->token, sizeof(out->token),
                                 token->valuestring) == 0 &&
                 jw__pakrat_commit_marker_valid(out);
    cJSON_Delete(root);
    if (!valid) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

int jw__pakrat_sync_filesystem(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
#if defined(__linux__)
    int rc = syncfs(fd);
#else
    int rc = fsync(fd);
    /* MLP1 uses syncfs above. Directory fsync is best-effort on hosts that
     * reject it, such as macOS, so native installs do not fail on EINVAL. */
    if (rc != 0 && errno == EINVAL) {
        rc = 0;
    }
#endif
    int saved = errno;
    if (close(fd) != 0 && rc == 0) {
        return -1;
    }
    errno = saved;
    return rc == 0 ? 0 : -1;
}

static int jw__pakrat_manifest_string(const cJSON *obj, const char *key,
                                      char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    if (cJSON_IsString(item) && item->valuestring) {
        int n = snprintf(out, out_size, "%s", item->valuestring);
        if (n < 0 || (size_t)n >= out_size) {
            return -1;
        }
    }
    return 0;
}

int jw__pakrat_read_manifest(const char *pak_dir, const char *manifest_rel,
                             jw__pakrat_manifest *out) {
    if (!pak_dir || !out || !jw__pakrat_safe_rel_path(manifest_rel)) {
        return -1;
    }
    memset(out, 0, sizeof(*out));
    char path[PATH_MAX];
    if (jw__pakrat_join2(path, sizeof(path), pak_dir, manifest_rel) != 0) {
        return -1;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long size = ftell(fp);
    if (size <= 0 || size > JW_PAKRAT_MANIFEST_MAX_BYTES ||
        fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return -1;
    }
    char *json = (char *)malloc((size_t)size + 1u);
    if (!json) {
        fclose(fp);
        return -1;
    }
    size_t got = fread(json, 1, (size_t)size, fp);
    int read_failed = ferror(fp);
    int close_failed = fclose(fp) != 0;
    if (got != (size_t)size || read_failed || close_failed) {
        free(json);
        return -1;
    }
    json[got] = '\0';

    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    free(json);
    if (!root || !cJSON_IsObject(root)) {
        if (root) {
            cJSON_Delete(root);
        }
        return -1;
    }
    int fields_ok =
        jw__pakrat_manifest_string(root, "platform", out->platform,
                                   sizeof(out->platform)) == 0 &&
        jw__pakrat_manifest_string(root, "pak_version", out->pak_version,
                                   sizeof(out->pak_version)) == 0 &&
        jw__pakrat_manifest_string(root, "min_leaf_version",
                                   out->min_leaf_version,
                                   sizeof(out->min_leaf_version)) == 0;
    /* Presence only. Whether the block is VALID is jw_content_manifest_validate's
       question, asked later by the catalog compiler; a malformed `provides`
       must still keep the pak out of the launch.sh requirement, or a typo
       would turn a content pak into an uninstallable one. */
    out->has_provides =
        cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(root, "provides")) ? 1 : 0;
    cJSON_Delete(root);
    return fields_ok ? 0 : -1;
}

int jw__pakrat_read_theme_identity(const char *theme_dir, char *id,
                                   size_t id_size, char *version,
                                   size_t version_size) {
    if (!theme_dir || !id || id_size == 0 || !version || version_size == 0) {
        return -1;
    }
    id[0] = '\0';
    version[0] = '\0';
    char path[PATH_MAX];
    if (jw__pakrat_join2(path, sizeof(path), theme_dir,
                         JW_PAKRAT_THEME_MANIFEST) != 0) {
        return -1;
    }
    struct stat st;
    if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 ||
        st.st_size > JW_PAKRAT_THEME_MANIFEST_MAX_BYTES) {
        return -1;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }
    size_t size = (size_t)st.st_size;
    char *json = malloc(size + 1u);
    size_t got = json ? fread(json, 1, size, fp) : 0;
    int close_failed = fclose(fp) != 0;
    if (!json || got != size || close_failed) {
        free(json);
        return -1;
    }
    json[size] = '\0';
    cJSON *root = cJSON_ParseWithOpts(json, NULL, true);
    free(json);
    int rc = cJSON_IsObject(root) &&
                     jw__pakrat_manifest_string(root, "id", id, id_size) == 0 &&
                     jw__pakrat_manifest_string(root, "version", version,
                                                version_size) == 0 &&
                     id[0] && version[0]
                 ? 0
                 : -1;
    cJSON_Delete(root);
    return rc;
}

/* The promoted tree at |target| counts as committed only when its identity and
   commit marker match the durable install record exactly. Returns a static
   reason string for the rollback log when they do not. A theme's identity is
   its theme.json, and it has no platform or entry point. */
static const char *jw__uncommitted_theme_reason(const char *target,
                                                const jw_pakrat_install *install) {
    char id[128];
    char version[64];
    if (jw__pakrat_read_theme_identity(target, id, sizeof(id), version,
                                       sizeof(version)) != 0) {
        return "manifest-unreadable";
    }
    jw_pakrat_commit_marker marker;
    if (jw__pakrat_read_commit_marker(target, &marker) != 0) {
        return "commit-marker-unreadable";
    }
    if (!install) {
        return "no-record";
    }
    if (strcmp(marker.store_id, install->store_id) != 0 ||
        strcmp(id, install->store_id) != 0) {
        return "store-id-mismatch";
    }
    if (strcmp(marker.version, version) != 0 ||
        strcmp(marker.version, install->version) != 0) {
        return "version-mismatch";
    }
    if (strcmp(marker.artifact_sha256, install->artifact_sha256) != 0) {
        return "artifact-mismatch";
    }
    if (!install->commit_token[0]) {
        return "record-token-missing";
    }
    if (strcmp(marker.token, install->commit_token) != 0) {
        return "token-mismatch";
    }
    return NULL;
}

static const char *jw__uncommitted_promote_reason(const char *target,
                                                  const char *install_path,
                                                  const jw_pakrat_install *install) {
    if (jw_pakrat_install_path_kind(install_path) == JW_PAKRAT_KIND_THEME) {
        return jw__uncommitted_theme_reason(target, install);
    }
    jw__pakrat_manifest manifest;
    if (jw__pakrat_read_manifest(target, "pak.json", &manifest) != 0 ||
        !manifest.pak_version[0]) {
        return "manifest-unreadable";
    }
    jw_pakrat_commit_marker marker;
    if (jw__pakrat_read_commit_marker(target, &marker) != 0) {
        return "commit-marker-unreadable";
    }
    if (!install) {
        return "no-record";
    }
    if (strcmp(marker.store_id, install->store_id) != 0) {
        return "store-id-mismatch";
    }
    if (strcmp(marker.version, manifest.pak_version) != 0 ||
        strcmp(marker.version, install->version) != 0) {
        return "version-mismatch";
    }
    if (!manifest.platform[0] || !install->platform[0] ||
        strcmp(manifest.platform, install->platform) != 0) {
        return "platform-mismatch";
    }
    if (strcmp(marker.artifact_sha256, install->artifact_sha256) != 0) {
        return "artifact-mismatch";
    }
    char entry_point[PATH_MAX];
    if (!manifest.has_provides &&
        (jw__pakrat_join2(entry_point, sizeof(entry_point), target,
                          JW_PAKRAT_ENTRY_POINT) != 0 ||
         !jw__pakrat_is_regular_file(entry_point))) {
        return "entry-point-missing";
    }
    if (!install->commit_token[0]) {
        return "record-token-missing";
    }
    if (strcmp(marker.token, install->commit_token) != 0) {
        return "token-mismatch";
    }
    return NULL;
}

/* A restored rollback must already match the pre-transaction record. Never
   manufacture a new commit decision from package bytes: the record is the
   commit point. Legacy rows (NULL token) may identify a marker-free tree only
   when no transaction sibling remains. */
static int jw__validate_restored_tree(const jw_pakrat_recovery_context *ctx,
                                      const jw_pakrat_install *install,
                                      const char *target) {
    if (!install) {
        return 0;
    }
    if (jw_pakrat_install_path_kind(install->install_path) ==
        JW_PAKRAT_KIND_THEME) {
        char id[128];
        char version[64];
        if (jw__pakrat_read_theme_identity(target, id, sizeof(id), version,
                                           sizeof(version)) != 0 ||
            strcmp(id, install->store_id) != 0 ||
            strcmp(version, install->version) != 0) {
            fprintf(stderr, "restored Pak Rat theme does not match install record: %s\n",
                    install->store_id);
            return -1;
        }
    } else {
    jw__pakrat_manifest manifest;
    if (jw__pakrat_read_manifest(target, "pak.json", &manifest) != 0 ||
        !manifest.pak_version[0] || !manifest.platform[0] ||
        strcmp(manifest.pak_version, install->version) != 0 ||
        strcmp(manifest.platform, install->platform) != 0) {
        fprintf(stderr, "restored Pak Rat tree does not match install record: %s\n",
                install->store_id);
        return -1;
    }
    char entry_point[PATH_MAX];
    if (!manifest.has_provides &&
        (jw__pakrat_join2(entry_point, sizeof(entry_point), target,
                          JW_PAKRAT_ENTRY_POINT) != 0 ||
         !jw__pakrat_is_regular_file(entry_point))) {
        fprintf(stderr, "restored Pak Rat tree has no entry point or content: %s\n",
                install->store_id);
        return -1;
    }
    }
    if (install->commit_token[0]) {
        jw_pakrat_commit_marker marker;
        if (jw__pakrat_read_commit_marker(target, &marker) != 0 ||
            strcmp(marker.store_id, install->store_id) != 0 ||
            strcmp(marker.version, install->version) != 0 ||
            strcmp(marker.artifact_sha256, install->artifact_sha256) != 0 ||
            strcmp(marker.token, install->commit_token) != 0) {
            fprintf(stderr,
                    "restored Pak Rat commit marker does not match record: %s\n",
                    install->store_id);
            return -1;
        }
    }
    (void)ctx;
    return 0;
}

/* Returns 0 when the selected source and install parent are positively mounted,
   1 when recovery must defer without mutation, and -1 for invalid arguments.
   jw_storage_sources_resolve supplies the same decoded mountinfo membership
   used by normal storage discovery; on MLP1 `available` requires an exact
   mountpoint rather than a rootfs stub. */
static int jw__pakrat_recovery_source_ready(
    const jw_pakrat_recovery_context *ctx, const char *apps_path) {
    if (!ctx || !apps_path || !apps_path[0]) {
        return -1;
    }
    jw_storage_source_list sources;
    if (jw_storage_sources_resolve(ctx->sdcard_root, &sources) != 0) {
        return 1;
    }
    const jw_storage_source *primary = jw_storage_sources_primary(&sources);
    struct stat root_st;
    struct stat apps_st;
    char root_abs[PATH_MAX];
    if (!primary || !primary->available ||
        !realpath(ctx->sdcard_root, root_abs) ||
        strcmp(root_abs, primary->root_abs) != 0 ||
        stat(ctx->sdcard_root, &root_st) != 0 || !S_ISDIR(root_st.st_mode) ||
        stat(apps_path, &apps_st) != 0 || !S_ISDIR(apps_st.st_mode) ||
        (unsigned long long)root_st.st_dev != primary->device_id ||
        apps_st.st_dev != root_st.st_dev) {
        return 1;
    }
    return 0;
}

int jw__pakrat_reconcile_transition(const jw_pakrat_recovery_context *ctx,
                                    const char *store_id,
                                    const char *install_path,
                                    const jw_pakrat_install *install) {
    if (!ctx) {
        return -1;
    }
    char target[PATH_MAX];
    char target_stage[PATH_MAX];
    char target_rollback[PATH_MAX];
    char legacy_partial[PATH_MAX];
    if (jw__pakrat_target_path(ctx->sdcard_root, install_path,
                               target, sizeof(target)) != 0 ||
        jw__pakrat_target_sibling_path(target, store_id, "stage",
                                       target_stage, sizeof(target_stage)) != 0 ||
        jw__pakrat_target_sibling_path(target, store_id, "rollback",
                                       target_rollback, sizeof(target_rollback)) != 0) {
        return -1;
    }
    int legacy_n = snprintf(legacy_partial, sizeof(legacy_partial), "%s.partial",
                            target);
    if (legacy_n < 0 || (size_t)legacy_n >= sizeof(legacy_partial)) {
        return -1;
    }

    char apps_parent[PATH_MAX];
    if (jw__pakrat_copy(apps_parent, sizeof(apps_parent), target) != 0) {
        return -1;
    }
    char *target_leaf = strrchr(apps_parent, '/');
    if (!target_leaf || target_leaf == apps_parent) {
        return -1;
    }
    *target_leaf = '\0';
    int source_ready = jw__pakrat_recovery_source_ready(ctx, apps_parent);
    if (source_ready != 0) {
        if (source_ready > 0) {
            fprintf(stderr,
                    "Pak Rat recovery deferred: install source is not mounted for %s\n",
                    install_path);
        }
        return source_ready;
    }

    if (jw__pakrat_path_exists(target_stage) &&
        jw__pakrat_remove_tree(target_stage) != 0) {
        fprintf(stderr, "could not remove stale Pak Rat stage path: %s\n", target_stage);
        return -1;
    }
    if (jw__pakrat_path_exists(legacy_partial) &&
        jw__pakrat_remove_tree(legacy_partial) != 0) {
        fprintf(stderr, "could not remove stale Pak Rat legacy stage path: %s\n",
                legacy_partial);
        return -1;
    }

    int target_exists = jw__pakrat_path_exists(target);
    int rollback_exists = jw__pakrat_path_exists(target_rollback);
    if (!target_exists && rollback_exists) {
        if (rename(target_rollback, target) != 0) {
            fprintf(stderr, "could not restore Pak Rat rollback path: %s\n", target);
            return -1;
        }
        jw__pakrat_clear_origin_marker(target, store_id);
        jw__pakrat_log(ctx->state_dir,
                       "install-recover restored store_id=%s target=%s%s",
                       store_id, jw_pakrat_install_path_display_prefix(install_path),
                       install_path);
        if (jw__validate_restored_tree(ctx, install, target) != 0) {
            return -1;
        }
    } else if (target_exists && rollback_exists) {
        const char *rollback_reason =
            jw__uncommitted_promote_reason(target, install_path, install);
        if (!rollback_reason) {
            if (jw__pakrat_remove_tree(target_rollback) != 0) {
                fprintf(stderr, "could not remove stale Pak Rat rollback path: %s\n",
                        target_rollback);
                return -1;
            }
            jw__pakrat_clear_origin_marker(target, store_id);
            jw__pakrat_log(ctx->state_dir,
                           "install-recover cleaned rollback store_id=%s target=%s%s",
                           store_id,
                           jw_pakrat_install_path_display_prefix(install_path),
                           install_path);
        } else {
            /* The install-record update is the commit point: this promote
               never committed, so prefer the tree that was already running. */
            if (jw__pakrat_remove_tree(target) != 0 ||
                rename(target_rollback, target) != 0) {
                fprintf(stderr,
                        "could not roll back uncommitted Pak Rat promote: %s\n",
                        target);
                return -1;
            }
            jw__pakrat_clear_origin_marker(target, store_id);
            jw__pakrat_log(ctx->state_dir,
                           "install-recover rolled back uncommitted promote store_id=%s target=%s%s reason=%s",
                           store_id,
                           jw_pakrat_install_path_display_prefix(install_path),
                           install_path, rollback_reason);
            if (jw__validate_restored_tree(ctx, install, target) != 0) {
                return -1;
            }
        }
    } else if (target_exists && !rollback_exists && install) {
        if (install->commit_token[0]) {
            const char *reason =
                jw__uncommitted_promote_reason(target, install_path, install);
            if (reason) {
                jw__pakrat_log(
                    ctx->state_dir,
                    "install-recover inconsistent committed tree store_id=%s target=%s%s reason=%s",
                    store_id, jw_pakrat_install_path_display_prefix(install_path),
                    install_path, reason);
                return JW_PAKRAT_RECOVERY_REPAIR_REQUIRED;
            }
        } else {
            if (jw__validate_restored_tree(ctx, install, target) != 0) {
                jw__pakrat_log(
                    ctx->state_dir,
                    "install-recover inconsistent legacy tree store_id=%s target=%s%s",
                    store_id, jw_pakrat_install_path_display_prefix(install_path),
                    install_path);
                return JW_PAKRAT_RECOVERY_REPAIR_REQUIRED;
            }
        }
        jw__pakrat_clear_origin_marker(target, store_id);
    } else if (!rollback_exists) {
        /* A write-ahead origin marker can survive a crash just before the
           move-aside rename. With no rollback tree it has no recovery role. */
        jw__pakrat_clear_origin_marker(target, store_id);
    }
    return 0;
}

/* Sweep one install dir -- Apps/<platform> or Themes -- for transition
   siblings whose store id has no install row. Recovery iterates install rows,
   so an interrupted first install (no row yet) is only reachable here.
   Conservative: exact prefixes only. */
static int jw__sweep_orphan_transition_dirs(const jw_pakrat_recovery_context *ctx,
                                            const jw_pakrat_install *installs,
                                            int install_count,
                                            const char *source_id,
                                            jw_pakrat_kind kind,
                                            const char *platform_dir) {
    char apps_dir[PATH_MAX];
    /* Card-relative name of the swept dir, for the log. */
    char shown[128];
    bool themes = kind == JW_PAKRAT_KIND_THEME;
    if (themes) {
        if (jw__pakrat_join2(apps_dir, sizeof(apps_dir), ctx->sdcard_root,
                             JW_PAKRAT_THEMES_DIR) != 0 ||
            jw__pakrat_copy(shown, sizeof(shown), JW_PAKRAT_THEMES_DIR) != 0) {
            return -1;
        }
    } else if (!jw__pakrat_safe_name(platform_dir) ||
               jw__pakrat_join3(apps_dir, sizeof(apps_dir), ctx->sdcard_root,
                                JW_PAKRAT_APPS_DIR, platform_dir) != 0 ||
               snprintf(shown, sizeof(shown), JW_PAKRAT_APPS_DIR "/%s",
                        platform_dir) >= (int)sizeof(shown)) {
        return -1;
    }
    int source_ready = jw__pakrat_recovery_source_ready(ctx, apps_dir);
    if (source_ready != 0) {
        return source_ready > 0 ? 0 : -1;
    }
    DIR *dir = opendir(apps_dir);
    if (!dir) {
        return errno == ENOENT ? 0 : -1;
    }
    int rc = 0;
    struct dirent *entry = NULL;
    for (;;) {
        errno = 0;
        entry = readdir(dir);
        if (!entry) {
            if (errno != 0) {
                rc = -1;
            }
            break;
        }
        if (entry->d_name[0] != '.') {
            size_t name_len = strlen(entry->d_name);
            /* A store theme's folder is its id; every other folder in Themes/
               is the user's, even one carrying a copied commit marker. */
            if (themes || (name_len >= 4u &&
                           strcmp(entry->d_name + name_len - 4u, ".pak") == 0)) {
                char package[PATH_MAX];
                jw_pakrat_commit_marker marker;
                if (jw__pakrat_join2(package, sizeof(package), apps_dir,
                                     entry->d_name) != 0) {
                    rc = -1;
                    break;
                }
                if (jw__pakrat_read_commit_marker(package, &marker) == 0 &&
                    (!themes || strcmp(marker.store_id, entry->d_name) == 0)) {
                    int pending = jw__pakrat_pending_uninstall_exists(
                        ctx->db_path, marker.store_id);
                    if (pending < 0) {
                        rc = -1;
                        break;
                    }
                    if (pending > 0) {
                        continue;
                    }
                    jw_pakrat_install record;
                    int record_rc = jw_db_pakrat_get_install(
                        ctx->db_path, marker.store_id, &record);
                    if (record_rc < 0) {
                        rc = -1;
                        break;
                    }
                    if (record_rc == 0 && source_id && source_id[0] &&
                        strcmp(record.source_id[0] ? record.source_id : "primary",
                               source_id) != 0) {
                        jw__pakrat_log(
                            ctx->state_dir,
                            "install-recover duplicate target store_id=%s source=%s recorded_source=%s",
                            marker.store_id, source_id,
                            record.source_id[0] ? record.source_id : "primary");
                        rc = -1;
                        break;
                    }
                    if (record_rc > 0) {
                        /* An adoption transaction writes this mapping before
                           moving the pre-existing target aside. If power is
                           lost at that boundary, the target may itself carry
                           an old Pak Rat marker but is still the user's live
                           tree. Honor the write-ahead mapping before treating
                           a marker-without-record as an interrupted first
                           install. */
                        char origin_install_path[PATH_MAX];
                        char origin_target[PATH_MAX];
                        bool origin_keeps_target =
                            jw__pakrat_read_origin_marker(
                                apps_dir, marker.store_id,
                                origin_install_path,
                                sizeof(origin_install_path)) == 0 &&
                            jw__pakrat_target_path(
                                ctx->sdcard_root, origin_install_path,
                                origin_target, sizeof(origin_target)) == 0 &&
                            strcmp(origin_target, package) == 0;
                        if (origin_keeps_target) {
                            continue;
                        }
                        if (jw__pakrat_remove_tree(package) != 0) {
                            rc = -1;
                            break;
                        }
                        jw__pakrat_log(
                            ctx->state_dir,
                            "install-recover removed uncommitted first install store_id=%s target=%s/%s",
                            marker.store_id, shown, entry->d_name);
                    }
                }
            }
            continue;
        }
        const char *store_id = NULL;
        bool is_rollback = false;
        bool is_origin = false;
        if (strncmp(entry->d_name, JW_PAKRAT_STAGE_PREFIX,
                    sizeof(JW_PAKRAT_STAGE_PREFIX) - 1) == 0) {
            store_id = entry->d_name + sizeof(JW_PAKRAT_STAGE_PREFIX) - 1;
        } else if (strncmp(entry->d_name, JW_PAKRAT_ROLLBACK_PREFIX,
                           sizeof(JW_PAKRAT_ROLLBACK_PREFIX) - 1) == 0) {
            store_id = entry->d_name + sizeof(JW_PAKRAT_ROLLBACK_PREFIX) - 1;
            is_rollback = true;
        } else if (strncmp(entry->d_name, JW_PAKRAT_ORIGIN_PREFIX,
                           sizeof(JW_PAKRAT_ORIGIN_PREFIX) - 1) == 0) {
            store_id = entry->d_name + sizeof(JW_PAKRAT_ORIGIN_PREFIX) - 1;
            is_origin = true;
        }
        if (!store_id || !jw__pakrat_safe_name(store_id)) {
            continue;
        }
        int pending = jw__pakrat_pending_uninstall_exists(ctx->db_path,
                                                          store_id);
        if (pending < 0) {
            rc = -1;
            break;
        }
        if (pending > 0) {
            continue;
        }
        char orphan[PATH_MAX];
        if (jw__pakrat_join2(orphan, sizeof(orphan), apps_dir, entry->d_name) != 0) {
            rc = -1;
            break;
        }

        bool owned = false;
        for (int i = 0; i < install_count; i++) {
            if (strcmp(installs[i].store_id, store_id) != 0) {
                continue;
            }
            if (source_id && source_id[0] &&
                strcmp(installs[i].source_id[0]
                           ? installs[i].source_id : "primary",
                       source_id) != 0) {
                continue;
            }
            const char *kind =
                is_rollback ? "rollback" : (is_origin ? "origin" : "stage");
            char expected_target[PATH_MAX];
            char expected_sibling[PATH_MAX];
            if (jw__pakrat_target_path(ctx->sdcard_root,
                                       installs[i].install_path,
                                       expected_target,
                                       sizeof(expected_target)) != 0 ||
                jw__pakrat_target_sibling_path(expected_target, store_id, kind,
                                               expected_sibling,
                                               sizeof(expected_sibling)) != 0) {
                /* A malformed ownership row must never make recovery delete a
                   possibly-related tree. The row-level reconcile reports it. */
                owned = true;
                break;
            }
            if (strcmp(expected_sibling, orphan) == 0) {
                owned = true;
                break;
            }
        }
        if (owned) {
            continue;
        }

        if (is_origin) {
            char rollback[PATH_MAX];
            int rollback_n =
                snprintf(rollback, sizeof(rollback), "%s/%s%s", apps_dir,
                         JW_PAKRAT_ROLLBACK_PREFIX, store_id);
            if (rollback_n < 0 || (size_t)rollback_n >= sizeof(rollback)) {
                rc = -1;
                break;
            }
            if (jw__pakrat_path_exists(rollback)) {
                continue;
            }
            if (unlink(orphan) != 0 && errno != ENOENT) {
                fprintf(stderr,
                        "could not remove orphaned Pak Rat origin marker: %s\n",
                        orphan);
                rc = -1;
                break;
            }
            jw__pakrat_log(ctx->state_dir,
                           "install-recover swept orphan origin store_id=%s path=%s/%s",
                           store_id, shown, entry->d_name);
            continue;
        }

        /* A rollback tree may be the only surviving copy of an app: an adopted
           install that crashed between the move-aside and the promote has no
           install row, so it reaches this sweep rather than reconcile.
           Restore it when its target is missing; never delete it blind. */
        if (is_rollback) {
            char install_path[PATH_MAX];
            char target[PATH_MAX];
            char target_rollback[PATH_MAX];
            /* The mapping must lead back into the dir being swept: a theme's
               rollback is never restored into Apps/, nor an app's into Themes/. */
            if (jw__pakrat_read_origin_marker(apps_dir, store_id, install_path,
                                              sizeof(install_path)) == 0 &&
                jw__pakrat_target_path(ctx->sdcard_root, install_path,
                                       target, sizeof(target)) == 0 &&
                jw__pakrat_target_sibling_path(target, store_id, "rollback",
                                               target_rollback,
                                               sizeof(target_rollback)) == 0 &&
                strcmp(target_rollback, orphan) == 0) {
                if (!jw__pakrat_path_exists(target)) {
                    if (rename(orphan, target) != 0) {
                        fprintf(stderr,
                                "could not restore orphaned Pak Rat rollback: %s\n",
                                target);
                        rc = -1;
                        break;
                    }
                    jw__pakrat_clear_origin_marker(target, store_id);
                    jw__pakrat_log(ctx->state_dir,
                                   "install-recover restored unrecorded rollback store_id=%s target=%s%s",
                                   store_id,
                                   jw_pakrat_install_path_display_prefix(install_path),
                                   install_path);
                    continue;
                }
                jw__pakrat_clear_origin_marker(target, store_id);
            } else {
                /* No marker: the target name is not reconstructable from the
                   sibling, so deleting it could destroy an app. Leave it and
                   surface it instead. */
                jw__pakrat_log(ctx->state_dir,
                               "install-recover retained unidentifiable rollback store_id=%s path=%s/%s",
                               store_id, shown, entry->d_name);
                continue;
            }
        }

        if (jw__pakrat_remove_tree(orphan) != 0) {
            fprintf(stderr, "could not remove orphaned Pak Rat path: %s/%s\n",
                    apps_dir, entry->d_name);
            rc = -1;
            break;
        }
        jw__pakrat_log(ctx->state_dir,
                       "install-recover swept orphan store_id=%s path=%s/%s",
                       store_id, shown, entry->d_name);
    }
    closedir(dir);
    return rc;
}

int jw_pakrat_recover_installs(const jw_pakrat_recovery_context *ctx) {
    enum { JW_PAKRAT_MAX_RECOVERY_INSTALLS = 1024 };
    int install_count = 0;
    if (!ctx || !ctx->sdcard_root[0] || !ctx->db_path[0]) {
        return -1;
    }
    /* The card root, not Apps/: a card holding only store themes has no Apps
       folder, and each install root is checked again where it is used. The
       message is matched by the device recovery matrix; keep its wording. */
    int root_ready = jw__pakrat_recovery_source_ready(ctx, ctx->sdcard_root);
    if (root_ready > 0) {
        fprintf(stderr,
                "Pak Rat recovery deferred: owning Apps source is not mounted\n");
        return 0;
    }
    if (root_ready < 0 || jw__pakrat_mkdir_parent(ctx->db_path) != 0) {
        return -1;
    }
    jw_pakrat_install *installs =
        (jw_pakrat_install *)calloc((size_t)JW_PAKRAT_MAX_RECOVERY_INSTALLS,
                                    sizeof(*installs));
    if (!installs) {
        return -1;
    }
    int rc = -1;
    jw_storage_source_list sources;
    if (jw_storage_sources_resolve(ctx->sdcard_root, &sources) != 0) {
        goto done;
    }
    if (jw_db_pakrat_list_installs(ctx->db_path, installs,
                                   JW_PAKRAT_MAX_RECOVERY_INSTALLS,
                                   &install_count) != 0) {
        goto done;
    }
    for (int i = 0; i < install_count; i++) {
        int pending = jw__pakrat_pending_uninstall_exists(
            ctx->db_path, installs[i].store_id);
        if (pending < 0) {
            goto done;
        }
        if (pending > 0) {
            continue;
        }
        const jw_storage_source *source = jw_storage_sources_find_by_id(
            &sources,
            installs[i].source_id[0] ? installs[i].source_id : "primary");
        if (!source || !source->available) {
            jw__pakrat_log(
                ctx->state_dir,
                "install-recover deferred absent source store_id=%s source=%s",
                installs[i].store_id,
                installs[i].source_id[0] ? installs[i].source_id : "primary");
            continue;
        }
        jw_pakrat_recovery_context source_ctx = *ctx;
        if (jw__pakrat_copy(source_ctx.sdcard_root,
                            sizeof(source_ctx.sdcard_root),
                            source->root) != 0) {
            goto done;
        }
        int reconcile_rc = jw__pakrat_reconcile_transition(
            &source_ctx, installs[i].store_id, installs[i].install_path,
            &installs[i]);
        if (reconcile_rc < 0) {
            rc = reconcile_rc;
            goto done;
        }
    }
    for (int s = 0; s < sources.count; s++) {
        const jw_storage_source *source = &sources.sources[s];
        if (!source->available) {
            continue;
        }
        jw_pakrat_recovery_context source_ctx = *ctx;
        if (jw__pakrat_copy(source_ctx.sdcard_root,
                            sizeof(source_ctx.sdcard_root),
                            source->root) != 0 ||
            (ctx->platform[0] &&
             jw__sweep_orphan_transition_dirs(
                 &source_ctx, installs, install_count, source->id,
                 JW_PAKRAT_KIND_APP, ctx->platform) != 0) ||
            (ctx->platform[0] && strcmp(ctx->platform, "shared") != 0 &&
             jw__sweep_orphan_transition_dirs(
                 &source_ctx, installs, install_count, source->id,
                 JW_PAKRAT_KIND_APP, "shared") != 0) ||
            /* Themes live on the primary card only. */
            (source->primary &&
             jw__sweep_orphan_transition_dirs(
                 &source_ctx, installs, install_count, source->id,
                 JW_PAKRAT_KIND_THEME, NULL) != 0)) {
            goto done;
        }
    }
    rc = 0;
done:
    free(installs);
    return rc;
}
