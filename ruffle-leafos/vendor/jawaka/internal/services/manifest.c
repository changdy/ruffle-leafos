/* PATH_MAX, realpath(), strtok_r() are hidden by glibc under a bare -std=c11
 * without a broader feature-test macro, so this file did not compile on Linux
 * at all -- `make service-manifest-test` (the target that checks all 37 A0
 * manifest fixtures) failed there with PATH_MAX undeclared and an implicit
 * strtok_r. Matches the convention every other module in this directory
 * already uses. Must precede every #include, including the paired header. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/manifest.h"

#include "cJSON.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define JW_SVC_RESOLVE_BUF (PATH_MAX + 1)

static void jw__svc_set_reason(char *reason, size_t reason_size, const char *slug) {
    if (reason && reason_size > 0) {
        snprintf(reason, reason_size, "%s", slug);
    }
}

void jw_service_manifest_destroy(jw_service_manifest *manifest) {
    if (!manifest) {
        return;
    }
    free(manifest->state_root);
    for (int i = 0; i < JW_SVC_MAX_STATE_LIST; i++) {
        free(manifest->state_revoke_on_uninstall[i]);
        free(manifest->state_retained_roots[i]);
    }
    memset(manifest, 0, sizeof(*manifest));
}

static bool jw__svc_dup_string(char **out, const char *value) {
    size_t len = strlen(value);
    if (len == SIZE_MAX) {
        return false;
    }
    char *copy = (char *)malloc(len + 1);
    if (!copy) {
        return false;
    }
    memcpy(copy, value, len + 1);
    *out = copy;
    return true;
}

static cJSON *jw__svc_object_item(const cJSON *object, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

static bool jw__svc_object_has_no_duplicate_keys(const cJSON *object) {
    if (!cJSON_IsObject(object)) {
        return false;
    }
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, object) {
        if (!child->string) {
            return false;
        }
        for (cJSON *other = child->next; other; other = other->next) {
            if (other->string && strcmp(child->string, other->string) == 0) {
                return false;
            }
        }
    }
    return true;
}

static bool jw__svc_object_has_only_keys(const cJSON *object,
                                         const char *const *known_keys,
                                         size_t known_count) {
    if (!jw__svc_object_has_no_duplicate_keys(object)) {
        return false;
    }
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, object) {
        if (!child->string) {
            return false;
        }
        bool known = false;
        for (size_t i = 0; i < known_count; i++) {
            if (strcmp(child->string, known_keys[i]) == 0) {
                known = true;
                break;
            }
        }
        if (!known) {
            return false;
        }
    }
    return true;
}

/* cJSON stores decoded strings as NUL-terminated C strings, so an escaped
 * U+0000 would otherwise silently truncate an id/path/argument. Reject that
 * JSON escape before parsing. A doubled backslash is handled as its own
 * escape and therefore does not produce a false positive. */
static bool jw__svc_json_has_escaped_nul(const char *json) {
    bool in_string = false;
    for (size_t i = 0; json[i]; i++) {
        if (!in_string) {
            if (json[i] == '"') {
                in_string = true;
            }
            continue;
        }
        if (json[i] == '"') {
            in_string = false;
            continue;
        }
        if (json[i] != '\\') {
            continue;
        }
        char escaped = json[++i];
        if (!escaped) {
            break;
        }
        if (escaped == 'u' &&
            json[i + 1] == '0' && json[i + 2] == '0' &&
            json[i + 3] == '0' && json[i + 4] == '0') {
            return true;
        }
        if (escaped == 'u' && json[i + 1] && json[i + 2] &&
            json[i + 3] && json[i + 4]) {
            i += 4;
        }
    }
    return false;
}

/* Matches ^[a-z0-9]+(\.[a-z0-9]+)+$ -- at least two dot-separated segments,
 * each non-empty and restricted to lowercase ascii + digits. Hand-written
 * rather than <regex.h> so this has no platform-dependent regex engine
 * behavior to verify on the MLP1 cross-toolchain. */
bool jw_service_id_is_reverse_dns(const char *s) {
    if (!s || !s[0]) {
        return false;
    }
    if (strlen(s) > JW_SVC_ID_MAX) {
        return false;
    }
    int dot_count = 0;
    size_t seg_len = 0;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '.') {
            if (seg_len == 0) {
                return false; /* empty segment: leading/trailing/double dot */
            }
            dot_count++;
            seg_len = 0;
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            seg_len++;
        } else {
            return false;
        }
    }
    return seg_len > 0 && dot_count >= 1;
}

/* "absolute" if it starts with '/'; "traversal" if any '/'-delimited
 * segment is exactly "..". Neither implies the other; a caller checks
 * absolute first since a rooted path can't also be meaningfully classified
 * by its segments the same way. */
static bool jw__svc_path_is_absolute(const char *p) {
    return p && p[0] == '/';
}

static bool jw__svc_path_has_traversal(const char *p) {
    if (!p) {
        return false;
    }
    const char *cursor = p;
    while (*cursor) {
        const char *slash = strchr(cursor, '/');
        size_t seg_len = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (seg_len == 2 && cursor[0] == '.' && cursor[1] == '.') {
            return true;
        }
        if (!slash) {
            break;
        }
        cursor = slash + 1;
    }
    return false;
}

static bool jw__svc_path_under_root(const char *candidate_abs, const char *root_abs) {
    size_t root_len = strlen(root_abs);
    if (root_len == 1 && root_abs[0] == '/') {
        return candidate_abs[0] == '/';
    }
    if (strncmp(candidate_abs, root_abs, root_len) != 0) {
        return false;
    }
    char next = candidate_abs[root_len];
    return next == '\0' || next == '/';
}

static bool jw__svc_dirname_into(const char *rel_path, char *dir_out, size_t dir_out_size,
                                 char *base_out, size_t base_out_size) {
    const char *slash = strrchr(rel_path, '/');
    if (!slash) {
        dir_out[0] = '\0';
        int n = snprintf(base_out, base_out_size, "%s", rel_path);
        return n >= 0 && (size_t)n < base_out_size;
    }
    size_t dir_len = (size_t)(slash - rel_path);
    if (dir_len >= dir_out_size) {
        return false;
    }
    memcpy(dir_out, rel_path, dir_len);
    dir_out[dir_len] = '\0';
    int n = snprintf(base_out, base_out_size, "%s", slash + 1);
    return n >= 0 && (size_t)n < base_out_size;
}

/* Lexically join `base` (an absolute path, no trailing slash except "/"
 * itself) with a single ".."-or-plain path segment, without touching the
 * filesystem. Used only to resolve a symlink's readlink() target relative
 * to the directory that contains it -- by this point the surrounding
 * directory has already been proven real by realpath(), so this only
 * needs to get the ".." arithmetic right, not detect further symlinks. */
static bool jw__svc_lexical_join(char *base, size_t base_size, const char *rel) {
    char work[JW_SVC_RESOLVE_BUF];
    int copied = snprintf(work, sizeof(work), "%s", rel);
    if (copied < 0 || (size_t)copied >= sizeof(work)) {
        return false;
    }
    char *saveptr = NULL;
    char *tok = strtok_r(work, "/", &saveptr);
    while (tok) {
        if (strcmp(tok, ".") == 0 || tok[0] == '\0') {
            /* no-op */
        } else if (strcmp(tok, "..") == 0) {
            size_t len = strlen(base);
            while (len > 1 && base[len - 1] != '/') {
                len--;
            }
            if (len > 1) {
                len--;
            }
            base[len] = '\0';
        } else {
            size_t len = strlen(base);
            if (len == 0 || base[len - 1] != '/') {
                if (len + 1 >= base_size) {
                    return false;
                }
                base[len++] = '/';
                base[len] = '\0';
            }
            size_t tok_len = strlen(tok);
            if (tok_len >= base_size - len) {
                return false;
            }
            memcpy(base + len, tok, tok_len + 1);
        }
        tok = strtok_r(NULL, "/", &saveptr);
    }
    return true;
}

typedef enum {
    JW_SVC_RUN_PATH_OK,
    JW_SVC_RUN_PATH_MISSING,
    JW_SVC_RUN_PATH_ESCAPES,
    JW_SVC_RUN_PATH_NON_REGULAR,
    JW_SVC_RUN_PATH_NOT_EXECUTABLE,
} jw_svc_run_path_check;

/* Validates that pak_abs_path/rel_run_path resolves through every symlink
 * hop to a regular, executable file confined to pak_abs_path.
 *
 * Approach: resolve the CONTAINING DIRECTORY with real realpath(3) (which
 * requires it to exist, and correctly follows every symlink component in
 * it, however deep) and boundary-check that. Then lstat() the leaf
 * component directly: if it is itself a symlink, resolve its target
 * lexically against the already-proven-real containing directory. This
 * first boundary check detects a direct escaping symlink even when it is
 * dangling. A final realpath() then follows every remaining hop and its
 * result is boundary-checked again before stat/access.
 */
static jw_svc_run_path_check jw__svc_check_run_path(const char *pak_abs_path,
                                                     const char *rel_run_path) {
    char pak_real[JW_SVC_RESOLVE_BUF];
    if (!realpath(pak_abs_path, pak_real)) {
        return JW_SVC_RUN_PATH_MISSING;
    }

    char rel_dir[JW_SVC_RESOLVE_BUF];
    char base[JW_SVC_RESOLVE_BUF];
    if (!jw__svc_dirname_into(rel_run_path, rel_dir, sizeof(rel_dir),
                              base, sizeof(base))) {
        return JW_SVC_RUN_PATH_MISSING;
    }

    char dir_abs[JW_SVC_RESOLVE_BUF];
    int written = 0;
    if (rel_dir[0]) {
        written = snprintf(dir_abs, sizeof(dir_abs), "%s/%s", pak_real, rel_dir);
    } else {
        written = snprintf(dir_abs, sizeof(dir_abs), "%s", pak_real);
    }
    if (written < 0 || (size_t)written >= sizeof(dir_abs)) {
        return JW_SVC_RUN_PATH_MISSING;
    }

    char dir_real[JW_SVC_RESOLVE_BUF];
    if (!realpath(dir_abs, dir_real)) {
        /* Containing directory doesn't exist (or a component along the way
         * is not a directory) -- nothing runnable can live under it. A
         * dangling INTERMEDIATE symlink that also escapes is not
         * distinguished from a plain missing directory here; see the
         * module comment. */
        return JW_SVC_RUN_PATH_MISSING;
    }
    if (!jw__svc_path_under_root(dir_real, pak_real)) {
        return JW_SVC_RUN_PATH_ESCAPES;
    }

    char leaf_abs[JW_SVC_RESOLVE_BUF];
    written = snprintf(leaf_abs, sizeof(leaf_abs), "%s/%s", dir_real, base);
    if (written < 0 || (size_t)written >= sizeof(leaf_abs)) {
        return JW_SVC_RUN_PATH_MISSING;
    }

    struct stat leaf_lst;
    if (lstat(leaf_abs, &leaf_lst) != 0) {
        return JW_SVC_RUN_PATH_MISSING;
    }

    char target_abs[JW_SVC_RESOLVE_BUF];
    if (S_ISLNK(leaf_lst.st_mode)) {
        char link_buf[JW_SVC_RESOLVE_BUF];
        ssize_t n = readlink(leaf_abs, link_buf, sizeof(link_buf) - 1);
        if (n < 0) {
            return JW_SVC_RUN_PATH_MISSING;
        }
        if ((size_t)n == sizeof(link_buf) - 1) {
            return JW_SVC_RUN_PATH_MISSING;
        }
        link_buf[n] = '\0';

        if (link_buf[0] == '/') {
            written = snprintf(target_abs, sizeof(target_abs), "%s", link_buf);
            if (written < 0 || (size_t)written >= sizeof(target_abs)) {
                return JW_SVC_RUN_PATH_MISSING;
            }
        } else {
            written = snprintf(target_abs, sizeof(target_abs), "%s", dir_real);
            if (written < 0 || (size_t)written >= sizeof(target_abs) ||
                !jw__svc_lexical_join(target_abs, sizeof(target_abs), link_buf)) {
                return JW_SVC_RUN_PATH_MISSING;
            }
        }

        if (!jw__svc_path_under_root(target_abs, pak_real)) {
            return JW_SVC_RUN_PATH_ESCAPES;
        }
    } else {
        written = snprintf(target_abs, sizeof(target_abs), "%s", leaf_abs);
        if (written < 0 || (size_t)written >= sizeof(target_abs)) {
            return JW_SVC_RUN_PATH_MISSING;
        }
    }

    char target_real[JW_SVC_RESOLVE_BUF];
    if (!realpath(target_abs, target_real)) {
        /* A dangling path is unavailable. Direct dangling escapes were
         * already classified above; a later dangling hop still fails safe. */
        return JW_SVC_RUN_PATH_MISSING;
    }
    if (!jw__svc_path_under_root(target_real, pak_real)) {
        return JW_SVC_RUN_PATH_ESCAPES;
    }

    struct stat target_st;
    if (stat(target_real, &target_st) != 0) {
        return JW_SVC_RUN_PATH_MISSING;
    }
    if (!S_ISREG(target_st.st_mode)) {
        return JW_SVC_RUN_PATH_NON_REGULAR;
    }
    if (access(target_real, X_OK) != 0) {
        return JW_SVC_RUN_PATH_NOT_EXECUTABLE;
    }
    return JW_SVC_RUN_PATH_OK;
}

typedef enum {
    JW_SVC_REVOKE_PATH_OK,
    JW_SVC_REVOKE_PATH_HAS_SYMLINK,
    JW_SVC_REVOKE_PATH_CHECK_FAILED,
} jw_svc_revoke_path_check;

/* "No existing component may be a symlink" for a state.revoke_on_uninstall
 * entry, including state.root itself and the entry's own leaf component.
 * A component that does not exist is not a violation. This uses an
 * exact-sized allocation so no manifest path can be silently truncated
 * before lstat(). */
static jw_svc_revoke_path_check jw__svc_check_revoke_path(
        const char *userdata_root_abs,
        const char *state_root,
        const char *revoke_rel_path) {
    size_t userdata_len = strlen(userdata_root_abs);
    size_t root_len = strlen(state_root);
    size_t revoke_len = strlen(revoke_rel_path);
    if (userdata_len > SIZE_MAX - 3 ||
        root_len > SIZE_MAX - userdata_len - 3 ||
        revoke_len > SIZE_MAX - userdata_len - root_len - 3) {
        return JW_SVC_REVOKE_PATH_CHECK_FAILED;
    }

    size_t capacity = userdata_len + root_len + revoke_len + 3;
    char *current = (char *)malloc(capacity);
    if (!current) {
        return JW_SVC_REVOKE_PATH_CHECK_FAILED;
    }

    size_t current_len = userdata_len;
    memcpy(current, userdata_root_abs, userdata_len + 1);
    if (root_len > 0) {
        if (current_len == 0 || current[current_len - 1] != '/') {
            current[current_len++] = '/';
        }
        memcpy(current + current_len, state_root, root_len + 1);
        current_len += root_len;

        struct stat st;
        if (lstat(current, &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                free(current);
                return JW_SVC_REVOKE_PATH_HAS_SYMLINK;
            }
        } else if (errno == ENOENT || errno == ENOTDIR) {
            free(current);
            return JW_SVC_REVOKE_PATH_OK;
        } else {
            free(current);
            return JW_SVC_REVOKE_PATH_CHECK_FAILED;
        }
    }

    const char *cursor = revoke_rel_path;
    while (*cursor) {
        const char *slash = strchr(cursor, '/');
        size_t component_len = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (component_len > 0) {
            if (current_len == 0 || current[current_len - 1] != '/') {
                current[current_len++] = '/';
            }
            memcpy(current + current_len, cursor, component_len);
            current_len += component_len;
            current[current_len] = '\0';

            struct stat st;
            if (lstat(current, &st) == 0) {
                if (S_ISLNK(st.st_mode)) {
                    free(current);
                    return JW_SVC_REVOKE_PATH_HAS_SYMLINK;
                }
            } else if (errno == ENOENT || errno == ENOTDIR) {
                free(current);
                return JW_SVC_REVOKE_PATH_OK;
            } else {
                free(current);
                return JW_SVC_REVOKE_PATH_CHECK_FAILED;
            }
        }
        if (!slash) {
            break;
        }
        cursor = slash + 1;
    }

    free(current);
    return JW_SVC_REVOKE_PATH_OK;
}

static bool jw__svc_double_is_integer(double value) {
    /* Every finite IEEE-754 double whose magnitude is at least 2^52 has
     * integral spacing. Smaller values fit safely in long long. The range
     * checks also reject NaN and infinities without libm. */
    const double exact_integer_limit = 4503599627370496.0;
    if (!(value <= 1.7976931348623157e308 &&
          value >= -1.7976931348623157e308)) {
        return false;
    }
    if (value >= exact_integer_limit || value <= -exact_integer_limit) {
        return true;
    }
    return value == (double)(long long)value;
}

static bool jw__svc_state_root_is_valid(const char *state_root) {
    if (!state_root[0]) {
        return false;
    }
    return strcmp(state_root, "..") != 0 &&
           strchr(state_root, '/') == NULL &&
           strchr(state_root, '\\') == NULL;
}

bool jw_service_manifest_validate(const char *pak_json_text,
                                   const char *pak_abs_path,
                                   const char *userdata_root_abs_or_null,
                                   jw_service_manifest *out,
                                   char *reason, size_t reason_size) {
    static const char *const service_keys[] = {
        "schema", "id", "run", "restart", "default_enabled",
        "stop_grace_ms", "lifecycle",
    };
    static const char *const run_keys[] = {"path", "args"};
    static const char *const lifecycle_keys[] = {
        "game", "stop_on_storage_change", "stop_on_suspend",
    };
    static const char *const state_keys[] = {
        "root", "revoke_on_uninstall", "retained_roots",
    };

    if (!pak_json_text || !pak_abs_path || !out) {
        jw__svc_set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    memset(out, 0, sizeof(*out));

    if (jw__svc_json_has_escaped_nul(pak_json_text)) {
        jw__svc_set_reason(reason, reason_size, "invalid-json");
        return false;
    }
    cJSON *root = cJSON_ParseWithOpts(pak_json_text, NULL, true);
    if (!root) {
        jw__svc_set_reason(reason, reason_size, "invalid-json");
        return false;
    }
    bool ok = false;

    if (!jw__svc_object_has_no_duplicate_keys(root)) {
        jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
        goto done;
    }

    cJSON *top_id_item = jw__svc_object_item(root, "id");
    const char *top_id = (cJSON_IsString(top_id_item) && top_id_item->valuestring)
                              ? top_id_item->valuestring
                              : "";

    cJSON *service = jw__svc_object_item(root, "service");
    if (!cJSON_IsObject(service)) {
        jw__svc_set_reason(reason, reason_size, "missing-service");
        goto done;
    }
    if (!jw__svc_object_has_only_keys(service, service_keys,
                                      sizeof(service_keys) / sizeof(service_keys[0]))) {
        jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
        goto done;
    }

    cJSON *svc_id_item = jw__svc_object_item(service, "id");
    const char *svc_id = (cJSON_IsString(svc_id_item) && svc_id_item->valuestring)
                              ? svc_id_item->valuestring
                              : "";

    if (!jw_service_id_is_reverse_dns(top_id) || !jw_service_id_is_reverse_dns(svc_id)) {
        jw__svc_set_reason(reason, reason_size, "malformed-id");
        goto done;
    }
    if (strcmp(top_id, svc_id) != 0) {
        jw__svc_set_reason(reason, reason_size, "id-mismatch");
        goto done;
    }
    snprintf(out->id, sizeof(out->id), "%s", svc_id);

    cJSON *schema_item = jw__svc_object_item(service, "schema");
    /* cJSON tags true/false separately from numbers, so true is not schema 1. */
    if (!cJSON_IsNumber(schema_item) || schema_item->valuedouble != 1.0) {
        jw__svc_set_reason(reason, reason_size, "unknown-schema");
        goto done;
    }

    cJSON *run = jw__svc_object_item(service, "run");
    if (!cJSON_IsObject(run) ||
        !jw__svc_object_has_only_keys(run, run_keys,
                                      sizeof(run_keys) / sizeof(run_keys[0]))) {
        jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
        goto done;
    }
    cJSON *run_path_item = jw__svc_object_item(run, "path");
    if (!cJSON_IsString(run_path_item) || !run_path_item->valuestring) {
        jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
        goto done;
    }
    const char *run_path = run_path_item->valuestring;
    if (!run_path[0]) {
        jw__svc_set_reason(reason, reason_size, "run-path-missing");
        goto done;
    }
    if (strlen(run_path) > JW_SVC_RUN_PATH_MAX) {
        jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
        goto done;
    }
    if (jw__svc_path_is_absolute(run_path)) {
        jw__svc_set_reason(reason, reason_size, "absolute-run-path");
        goto done;
    }
    if (jw__svc_path_has_traversal(run_path)) {
        jw__svc_set_reason(reason, reason_size, "path-traversal");
        goto done;
    }

    jw_svc_run_path_check path_check = jw__svc_check_run_path(pak_abs_path, run_path);
    switch (path_check) {
        case JW_SVC_RUN_PATH_MISSING:
            jw__svc_set_reason(reason, reason_size, "run-path-missing");
            goto done;
        case JW_SVC_RUN_PATH_ESCAPES:
            jw__svc_set_reason(reason, reason_size, "escaping-symlink");
            goto done;
        case JW_SVC_RUN_PATH_NON_REGULAR:
            jw__svc_set_reason(reason, reason_size, "non-regular-executable");
            goto done;
        case JW_SVC_RUN_PATH_NOT_EXECUTABLE:
            jw__svc_set_reason(reason, reason_size, "run-path-not-executable");
            goto done;
        case JW_SVC_RUN_PATH_OK:
            break;
    }
    snprintf(out->run_path, sizeof(out->run_path), "%s", run_path);

    cJSON *args = jw__svc_object_item(run, "args");
    if (args) {
        if (!cJSON_IsArray(args)) {
            jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
            goto done;
        }
        int n = cJSON_GetArraySize(args);
        if (n > JW_SVC_MAX_ARGS) {
            jw__svc_set_reason(reason, reason_size, "args-count-over-limit");
            goto done;
        }
        for (int i = 0; i < n; i++) {
            cJSON *item = cJSON_GetArrayItem(args, i);
            if (!cJSON_IsString(item) || !item->valuestring) {
                jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
                goto done;
            }
            const char *s = item->valuestring;
            if (strlen(s) > JW_SVC_MAX_ARG_LEN) {
                jw__svc_set_reason(reason, reason_size, "args-item-too-long");
                goto done;
            }
            snprintf(out->run_args[i], sizeof(out->run_args[i]), "%s", s);
        }
        out->run_args_count = n;
    }

    cJSON *restart_item = jw__svc_object_item(service, "restart");
    const char *restart = (cJSON_IsString(restart_item) && restart_item->valuestring)
                               ? restart_item->valuestring
                               : "";
    if (strcmp(restart, "no") == 0) {
        out->restart_on_failure = false;
    } else if (strcmp(restart, "on-failure") == 0) {
        out->restart_on_failure = true;
    } else {
        jw__svc_set_reason(reason, reason_size, "unknown-restart-policy");
        goto done;
    }

    cJSON *default_enabled_item = jw__svc_object_item(service, "default_enabled");
    if (!cJSON_IsBool(default_enabled_item) || cJSON_IsTrue(default_enabled_item)) {
        jw__svc_set_reason(reason, reason_size, "default-enabled-true");
        goto done;
    }

    cJSON *stop_grace_item = jw__svc_object_item(service, "stop_grace_ms");
    if (stop_grace_item) {
        if (!cJSON_IsNumber(stop_grace_item) ||
            !jw__svc_double_is_integer(stop_grace_item->valuedouble)) {
            jw__svc_set_reason(reason, reason_size, "stop-grace-ms-non-integer");
            goto done;
        }
        double v = stop_grace_item->valuedouble;
        if (v < 0) {
            jw__svc_set_reason(reason, reason_size, "stop-grace-ms-negative");
            goto done;
        }
        out->stop_grace_ms = (v > 15000.0) ? 15000 : (int)v;
    } else {
        out->stop_grace_ms = 5000;
    }

    cJSON *lifecycle = jw__svc_object_item(service, "lifecycle");
    out->lifecycle_game = JW_SVC_LIFECYCLE_GAME_IGNORE;
    if (lifecycle) {
        if (!cJSON_IsObject(lifecycle)) {
            jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
            goto done;
        }
        if (!jw__svc_object_has_only_keys(
                    lifecycle, lifecycle_keys,
                    sizeof(lifecycle_keys) / sizeof(lifecycle_keys[0]))) {
            jw__svc_set_reason(reason, reason_size, "unknown-lifecycle-key");
            goto done;
        }
        cJSON *game_item = jw__svc_object_item(lifecycle, "game");
        if (game_item) {
            if (!cJSON_IsString(game_item) || !game_item->valuestring) {
                jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
                goto done;
            }
            const char *g = game_item->valuestring;
            if (strcmp(g, "ignore") == 0) {
                out->lifecycle_game = JW_SVC_LIFECYCLE_GAME_IGNORE;
            } else if (strcmp(g, "stop") == 0) {
                out->lifecycle_game = JW_SVC_LIFECYCLE_GAME_STOP;
            } else if (strcmp(g, "notify") == 0) {
                out->lifecycle_game = JW_SVC_LIFECYCLE_GAME_NOTIFY;
            } else {
                jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
                goto done;
            }
        }
        cJSON *storage_item = jw__svc_object_item(lifecycle, "stop_on_storage_change");
        if (storage_item) {
            if (!cJSON_IsBool(storage_item)) {
                jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
                goto done;
            }
            out->stop_on_storage_change = cJSON_IsTrue(storage_item);
        }
        cJSON *suspend_item = jw__svc_object_item(lifecycle, "stop_on_suspend");
        if (suspend_item) {
            if (!cJSON_IsBool(suspend_item)) {
                jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
                goto done;
            }
            out->stop_on_suspend = cJSON_IsTrue(suspend_item);
        }
    }

    cJSON *state = jw__svc_object_item(root, "state");
    if (state) {
        if (!cJSON_IsObject(state) ||
            !jw__svc_object_has_only_keys(
                    state, state_keys, sizeof(state_keys) / sizeof(state_keys[0]))) {
            jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
            goto done;
        }
        out->has_state = true;

        cJSON *root_item = jw__svc_object_item(state, "root");
        const char *state_root = "";
        if (root_item) {
            if (!cJSON_IsString(root_item) || !root_item->valuestring ||
                !jw__svc_state_root_is_valid(root_item->valuestring)) {
                jw__svc_set_reason(reason, reason_size, "invalid-state-root");
                goto done;
            }
            state_root = root_item->valuestring;
        }
        if (!jw__svc_dup_string(&out->state_root, state_root)) {
            jw__svc_set_reason(reason, reason_size, "out-of-memory");
            goto done;
        }

        cJSON *revoke = jw__svc_object_item(state, "revoke_on_uninstall");
        if (revoke) {
            if (!cJSON_IsArray(revoke)) {
                jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
                goto done;
            }
            int n = cJSON_GetArraySize(revoke);
            if (n > JW_SVC_MAX_STATE_LIST) {
                jw__svc_set_reason(reason, reason_size,
                                   "revoke-on-uninstall-count-over-limit");
                goto done;
            }
            if (n > 0 &&
                (!userdata_root_abs_or_null || !userdata_root_abs_or_null[0])) {
                jw__svc_set_reason(reason, reason_size,
                                   "revoke-on-uninstall-root-unavailable");
                goto done;
            }
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(revoke, i);
                if (!cJSON_IsString(item) || !item->valuestring ||
                    !item->valuestring[0]) {
                    jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
                    goto done;
                }
                const char *s = item->valuestring;
                if (jw__svc_path_is_absolute(s)) {
                    jw__svc_set_reason(reason, reason_size,
                                       "revoke-on-uninstall-absolute");
                    goto done;
                }
                if (jw__svc_path_has_traversal(s)) {
                    jw__svc_set_reason(reason, reason_size,
                                       "revoke-on-uninstall-traversal");
                    goto done;
                }
                jw_svc_revoke_path_check revoke_check =
                    jw__svc_check_revoke_path(userdata_root_abs_or_null,
                                              state_root, s);
                if (revoke_check == JW_SVC_REVOKE_PATH_HAS_SYMLINK) {
                    jw__svc_set_reason(
                        reason, reason_size,
                        "revoke-on-uninstall-symlink-component");
                    goto done;
                }
                if (revoke_check == JW_SVC_REVOKE_PATH_CHECK_FAILED) {
                    jw__svc_set_reason(reason, reason_size,
                                       "state-path-check-failed");
                    goto done;
                }
                if (!jw__svc_dup_string(&out->state_revoke_on_uninstall[i], s)) {
                    jw__svc_set_reason(reason, reason_size, "out-of-memory");
                    goto done;
                }
            }
            out->state_revoke_count = n;
        }

        cJSON *retained = jw__svc_object_item(state, "retained_roots");
        if (retained) {
            if (!cJSON_IsArray(retained)) {
                jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
                goto done;
            }
            int n = cJSON_GetArraySize(retained);
            if (n > JW_SVC_MAX_STATE_LIST) {
                jw__svc_set_reason(reason, reason_size,
                                   "retained-roots-count-over-limit");
                goto done;
            }
            for (int i = 0; i < n; i++) {
                cJSON *item = cJSON_GetArrayItem(retained, i);
                if (!cJSON_IsString(item) || !item->valuestring ||
                    !item->valuestring[0]) {
                    jw__svc_set_reason(reason, reason_size, "invalid-json-shape");
                    goto done;
                }
                const char *s = item->valuestring;
                if (jw__svc_path_is_absolute(s)) {
                    jw__svc_set_reason(reason, reason_size,
                                       "retained-roots-absolute");
                    goto done;
                }
                if (jw__svc_path_has_traversal(s)) {
                    jw__svc_set_reason(reason, reason_size,
                                       "retained-roots-traversal");
                    goto done;
                }
                if (!jw__svc_dup_string(&out->state_retained_roots[i], s)) {
                    jw__svc_set_reason(reason, reason_size, "out-of-memory");
                    goto done;
                }
            }
            out->state_retained_count = n;
        }
    }

    ok = true;

done:
    cJSON_Delete(root);
    if (!ok) {
        jw_service_manifest_destroy(out);
    }
    return ok;
}
