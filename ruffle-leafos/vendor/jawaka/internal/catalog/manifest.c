/* realpath(), lstat(), readlink(), and strtok_r() need this under -std=c11. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/catalog/manifest.h"

#include "internal/retroarch/catalog.h"

#include "cJSON.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define JW_CONTENT_PATH_BUF (PATH_MAX + 1)

static void jw_content__reason(char *out, size_t out_size, const char *reason) {
    if (out && out_size > 0) {
        snprintf(out, out_size, "%s", reason);
    }
}

void jw_content_manifest_destroy(jw_content_manifest *manifest) {
    if (!manifest) {
        return;
    }
    cJSON_Delete(manifest->document);
    memset(manifest, 0, sizeof(*manifest));
}

static cJSON *jw_content__item(const cJSON *object, const char *key) {
    return cJSON_GetObjectItemCaseSensitive(object, key);
}

static bool jw_content__object_keys(const cJSON *object,
                                    const char *const *known,
                                    size_t known_count) {
    if (!cJSON_IsObject(object)) {
        return false;
    }
    cJSON *child = NULL;
    cJSON_ArrayForEach(child, object) {
        if (!child->string) {
            return false;
        }
        bool found = false;
        for (size_t i = 0; i < known_count; i++) {
            if (strcmp(child->string, known[i]) == 0) {
                found = true;
                break;
            }
        }
        if (!found) {
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

/* cJSON represents decoded strings as NUL-terminated buffers. Reject an
   escaped U+0000 before parsing so a field cannot be silently truncated. */
static bool jw_content__escaped_nul(const char *json) {
    bool in_string = false;
    for (size_t i = 0; json && json[i]; i++) {
        if (!in_string) {
            in_string = json[i] == '"';
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
        if (escaped == 'u' && json[i + 1] == '0' && json[i + 2] == '0' &&
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

/* Preserve CONTENT-1's NUL rejection outside the optional art value. cJSON
   cannot retain embedded NULs, so invalidate that whole companion before parsing. */
static char *jw_content__without_nul_art(const char *json) {
    char *copy = strdup(json);
    if (!copy) return NULL;
    const char *p = json;
    while (*p && *p != '{') p++;
    if (*p) p++;
    while (*p) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ',') p++;
        if (*p != '"') break;
        const char *end = NULL;
        cJSON *key = cJSON_ParseWithOpts(p, &end, false);
        if (!cJSON_IsString(key) || !end) { cJSON_Delete(key); break; }
        bool art = strcmp(key->valuestring, "content_art") == 0;
        cJSON_Delete(key);
        p = end;
        while (*p && *p != ':') p++;
        if (!*p) break;
        p++;
        const char *value = p;
        cJSON *parsed = cJSON_ParseWithOpts(value, &end, false);
        if (!parsed || !end) { cJSON_Delete(parsed); break; }
        cJSON_Delete(parsed);
        if (art) {
            char *raw = strndup(value, (size_t)(end - value));
            bool invalid = raw && jw_content__escaped_nul(raw);
            free(raw);
            if (invalid && end - value >= 4) {
                memset(copy + (value - json), ' ', (size_t)(end - value));
                memcpy(copy + (value - json), "null", 4);
            }
        }
        p = end;
    }
    return copy;
}

static bool jw_content__system_id(const char *s) {
    size_t len = s ? strlen(s) : 0;
    if (len < 2 || len > 32) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
            return false;
        }
    }
    return true;
}

static bool jw_content__core_id(const char *s) {
    size_t len = s ? strlen(s) : 0;
    if (len < 2 || len > 64) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) {
            return false;
        }
    }
    return true;
}

static bool jw_content__ascii_component(const char *s, size_t max_len,
                                        bool underscore) {
    size_t len = s ? strlen(s) : 0;
    if (len < 1 || len > max_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= 'A' && s[i] <= 'Z') ||
              (s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') || s[i] == '-' ||
              (underscore && s[i] == '_'))) {
            return false;
        }
    }
    return true;
}

static bool jw_content__root(const char *s, const char *prefix) {
    size_t n = strlen(prefix);
    return s && strncmp(s, prefix, n) == 0 &&
           jw_content__ascii_component(s + n, 32, true);
}

static bool jw_content__extension(const char *s, size_t max_len, bool underscore) {
    size_t len = s ? strlen(s) : 0;
    if (len < 1 || len > max_len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (!((s[i] >= 'a' && s[i] <= 'z') ||
              (s[i] >= '0' && s[i] <= '9') ||
              (underscore && s[i] == '_'))) {
            return false;
        }
    }
    return true;
}

static bool jw_content__string(const cJSON *item, size_t min_len, size_t max_len) {
    return cJSON_IsString(item) && item->valuestring &&
           strlen(item->valuestring) >= min_len && strlen(item->valuestring) <= max_len;
}

typedef bool (*jw_content_string_rule)(const char *);

static bool jw_content__string_list(const cJSON *array, int min_count, int max_count,
                                    size_t min_len, size_t max_len,
                                    jw_content_string_rule rule) {
    if (!cJSON_IsArray(array)) {
        return false;
    }
    int count = cJSON_GetArraySize(array);
    if (count < min_count || count > max_count) {
        return false;
    }
    cJSON *item = NULL;
    cJSON_ArrayForEach(item, array) {
        if (!jw_content__string(item, min_len, max_len) ||
            (rule && !rule(item->valuestring))) {
            return false;
        }
    }
    return true;
}

static bool jw_content__rom_ext(const char *s) {
    return jw_content__extension(s, 16, true);
}

static bool jw_content__archive_ext(const char *s) {
    return jw_content__extension(s, 8, false);
}

static bool jw_content__path_absolute(const char *path) {
    return path && path[0] == '/';
}

static bool jw_content__path_traversal(const char *path) {
    const char *cursor = path;
    while (cursor && *cursor) {
        const char *slash = strchr(cursor, '/');
        size_t len = slash ? (size_t)(slash - cursor) : strlen(cursor);
        if (len == 2 && cursor[0] == '.' && cursor[1] == '.') {
            return true;
        }
        cursor = slash ? slash + 1 : NULL;
    }
    return false;
}

static bool jw_content__under(const char *candidate, const char *root) {
    size_t len = strlen(root);
    return strncmp(candidate, root, len) == 0 &&
           (candidate[len] == '\0' || candidate[len] == '/');
}

static bool jw_content__dirname(const char *rel, char *dir, size_t dir_size,
                                char *base, size_t base_size) {
    const char *slash = strrchr(rel, '/');
    if (!slash) {
        dir[0] = '\0';
        return snprintf(base, base_size, "%s", rel) < (int)base_size;
    }
    size_t len = (size_t)(slash - rel);
    if (len >= dir_size || snprintf(base, base_size, "%s", slash + 1) >=
                                    (int)base_size) {
        return false;
    }
    memcpy(dir, rel, len);
    dir[len] = '\0';
    return true;
}

static bool jw_content__lexical_join(char *base, size_t base_size, const char *rel) {
    char work[JW_CONTENT_PATH_BUF];
    if (snprintf(work, sizeof(work), "%s", rel) >= (int)sizeof(work)) {
        return false;
    }
    char *save = NULL;
    for (char *part = strtok_r(work, "/", &save); part;
         part = strtok_r(NULL, "/", &save)) {
        if (strcmp(part, ".") == 0 || !part[0]) {
            continue;
        }
        if (strcmp(part, "..") == 0) {
            size_t len = strlen(base);
            while (len > 1 && base[len - 1] != '/') {
                len--;
            }
            if (len > 1) {
                len--;
            }
            base[len] = '\0';
            continue;
        }
        size_t len = strlen(base);
        int written = snprintf(base + len, base_size - len, "%s%s",
                               (len && base[len - 1] == '/') ? "" : "/", part);
        if (written < 0 || (size_t)written >= base_size - len) {
            return false;
        }
    }
    return true;
}

typedef enum {
    JW_CONTENT_PATH_OK,
    JW_CONTENT_PATH_MISSING,
    JW_CONTENT_PATH_ESCAPES,
    JW_CONTENT_PATH_NON_REGULAR,
    JW_CONTENT_PATH_NOT_EXECUTABLE,
} jw_content_path_result;

/* Resolve every component. The explicit leaf-symlink branch preserves the
   useful `escaping-symlink` diagnosis even when the external target is
   dangling and realpath(3) therefore cannot resolve it. */
static jw_content_path_result jw_content__check_path(const char *pak_root,
                                                     const char *rel,
                                                     bool executable) {
    char root_real[JW_CONTENT_PATH_BUF];
    if (!realpath(pak_root, root_real)) {
        return JW_CONTENT_PATH_MISSING;
    }
    char rel_dir[JW_CONTENT_PATH_BUF], base[JW_CONTENT_PATH_BUF];
    if (!jw_content__dirname(rel, rel_dir, sizeof(rel_dir), base, sizeof(base))) {
        return JW_CONTENT_PATH_MISSING;
    }
    char dir_abs[JW_CONTENT_PATH_BUF];
    int n = snprintf(dir_abs, sizeof(dir_abs), "%s%s%s", root_real,
                     rel_dir[0] ? "/" : "", rel_dir);
    if (n < 0 || (size_t)n >= sizeof(dir_abs)) {
        return JW_CONTENT_PATH_MISSING;
    }
    char dir_real[JW_CONTENT_PATH_BUF];
    if (!realpath(dir_abs, dir_real)) {
        return JW_CONTENT_PATH_MISSING;
    }
    if (!jw_content__under(dir_real, root_real)) {
        return JW_CONTENT_PATH_ESCAPES;
    }
    char leaf[JW_CONTENT_PATH_BUF];
    if (snprintf(leaf, sizeof(leaf), "%s/%s", dir_real, base) >= (int)sizeof(leaf)) {
        return JW_CONTENT_PATH_MISSING;
    }
    struct stat lst;
    if (lstat(leaf, &lst) != 0) {
        return JW_CONTENT_PATH_MISSING;
    }
    char target[JW_CONTENT_PATH_BUF];
    if (S_ISLNK(lst.st_mode)) {
        char link[JW_CONTENT_PATH_BUF];
        ssize_t got = readlink(leaf, link, sizeof(link) - 1);
        if (got < 0 || (size_t)got == sizeof(link) - 1) {
            return JW_CONTENT_PATH_MISSING;
        }
        link[got] = '\0';
        if (link[0] == '/') {
            if (snprintf(target, sizeof(target), "%s", link) >= (int)sizeof(target)) {
                return JW_CONTENT_PATH_MISSING;
            }
        } else {
            if (snprintf(target, sizeof(target), "%s", dir_real) >= (int)sizeof(target) ||
                !jw_content__lexical_join(target, sizeof(target), link)) {
                return JW_CONTENT_PATH_MISSING;
            }
        }
        if (!jw_content__under(target, root_real)) {
            return JW_CONTENT_PATH_ESCAPES;
        }
    } else if (snprintf(target, sizeof(target), "%s", leaf) >= (int)sizeof(target)) {
        return JW_CONTENT_PATH_MISSING;
    }
    char resolved[JW_CONTENT_PATH_BUF];
    if (!realpath(target, resolved)) {
        return JW_CONTENT_PATH_MISSING;
    }
    if (!jw_content__under(resolved, root_real)) {
        return JW_CONTENT_PATH_ESCAPES;
    }
    struct stat st;
    if (stat(resolved, &st) != 0) {
        return JW_CONTENT_PATH_MISSING;
    }
    if (!S_ISREG(st.st_mode)) {
        return JW_CONTENT_PATH_NON_REGULAR;
    }
    if (executable && access(resolved, X_OK) != 0) {
        return JW_CONTENT_PATH_NOT_EXECUTABLE;
    }
    return JW_CONTENT_PATH_OK;
}

static const char *jw_content__path_reason(const cJSON *item, const char *pak_root,
                                           bool executable) {
    if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0]) {
        return "missing-file";
    }
    const char *path = item->valuestring;
    if (jw_content__path_absolute(path)) {
        return "absolute-path";
    }
    if (jw_content__path_traversal(path)) {
        return "path-traversal";
    }
    switch (jw_content__check_path(pak_root, path, executable)) {
    case JW_CONTENT_PATH_ESCAPES: return "escaping-symlink";
    case JW_CONTENT_PATH_MISSING: return "missing-file";
    case JW_CONTENT_PATH_NON_REGULAR: return "non-regular-file";
    case JW_CONTENT_PATH_NOT_EXECUTABLE: return "not-executable";
    case JW_CONTENT_PATH_OK: return NULL;
    }
    return "missing-file";
}

static const char *jw_content__forbidden(const cJSON *node) {
    if (cJSON_IsObject(node)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            if (child->string) {
                if (strcmp(child->string, "requires_direct_drm") == 0) {
                    return "forbidden-requires-direct-drm";
                }
                if (strcmp(child->string, "legacy_flat_core") == 0) {
                    return "forbidden-legacy-flat-core";
                }
                if (strcmp(child->string, "name_map") == 0) {
                    return "forbidden-name-map";
                }
                if (strcmp(child->string, "status") == 0) {
                    return "forbidden-status";
                }
            }
            const char *reason = jw_content__forbidden(child);
            if (reason) {
                return reason;
            }
        }
    } else if (cJSON_IsArray(node)) {
        cJSON *child = NULL;
        cJSON_ArrayForEach(child, node) {
            const char *reason = jw_content__forbidden(child);
            if (reason) {
                return reason;
            }
        }
    }
    return NULL;
}

static const char *jw_content__validate_system(const cJSON *system,
                                               const char *pak_root,
                                               bool *warning) {
    static const char *const keys[] = {
        "id", "name", "patterns", "extensions", "archive_extensions",
        "archive_inner_extensions", "archive_mode", "file_names",
        "ignore_file_names", "playlist_extensions", "m3u_generation",
        "default_core", "alternate_cores", "rom_root", "image_root",
        "bios_notes", "icon_flat", "icon_photographic",
        "screenscraper_platform_ids", "group", "bios_directory",
        "requires_direct_drm", "legacy_flat_core", "name_map", "status",
    };
    if (!jw_content__object_keys(system, keys, sizeof(keys) / sizeof(keys[0]))) {
        return "unknown-field";
    }
    cJSON *id = jw_content__item(system, "id");
    if (!cJSON_IsString(id) || !jw_content__system_id(id->valuestring)) {
        return "malformed-system-id";
    }
    if (!jw_content__string(jw_content__item(system, "name"), 1, 64)) {
        return "malformed-system-name";
    }
    cJSON *patterns = jw_content__item(system, "patterns");
    if (!jw_content__string_list(patterns, 1, 32, 1, 64, NULL)) {
        return "malformed-patterns";
    }
    for (cJSON *a = patterns->child; a; a = a->next) {
        for (cJSON *b = a->next; b; b = b->next) {
            if (a->valuestring && b->valuestring &&
                strcasecmp(a->valuestring, b->valuestring) == 0) {
                *warning = true;
            }
        }
    }
    if (!jw_content__string_list(jw_content__item(system, "extensions"),
                                 1, 64, 1, 16, jw_content__rom_ext)) {
        return "malformed-extensions";
    }
    cJSON *item = jw_content__item(system, "archive_extensions");
    if (item && !jw_content__string_list(item, 0, 8, 1, 8, jw_content__archive_ext)) {
        return "malformed-archive-extensions";
    }
    item = jw_content__item(system, "archive_inner_extensions");
    if (item && !jw_content__string_list(item, 0, 64, 1, 16, jw_content__rom_ext)) {
        return "malformed-archive-inner-extensions";
    }
    item = jw_content__item(system, "playlist_extensions");
    if (item && !jw_content__string_list(item, 0, 8, 1, 8, jw_content__archive_ext)) {
        return "malformed-playlist-extensions";
    }
    item = jw_content__item(system, "file_names");
    if (item && !jw_content__string_list(item, 0, 32, 1, 128, NULL)) {
        return "malformed-file-names";
    }
    item = jw_content__item(system, "ignore_file_names");
    if (item && !jw_content__string_list(item, 0, 32, 1, 128, NULL)) {
        return "malformed-ignore-file-names";
    }
    item = jw_content__item(system, "bios_notes");
    if (item && !jw_content__string_list(item, 0, 8, 1, 256, NULL)) {
        return "malformed-bios-notes";
    }
    item = jw_content__item(system, "archive_mode");
    if (item && (!cJSON_IsString(item) ||
        (strcmp(item->valuestring, "pass_through") != 0 &&
         strcmp(item->valuestring, "extract") != 0 &&
         strcmp(item->valuestring, "none") != 0))) {
        return "unknown-archive-mode";
    }
    item = jw_content__item(system, "m3u_generation");
    if (item && (!cJSON_IsString(item) ||
        (strcmp(item->valuestring, "none") != 0 &&
         strcmp(item->valuestring, "auto") != 0 &&
         strcmp(item->valuestring, "manual") != 0))) {
        return "unknown-m3u-generation";
    }
    item = jw_content__item(system, "rom_root");
    if (!cJSON_IsString(item) || !jw_content__root(item->valuestring, "Roms/")) {
        return "malformed-rom-root";
    }
    item = jw_content__item(system, "image_root");
    if (!cJSON_IsString(item) || !jw_content__root(item->valuestring, "Images/")) {
        return "malformed-image-root";
    }
    item = jw_content__item(system, "default_core");
    if (!cJSON_IsString(item) || !jw_content__core_id(item->valuestring)) {
        return "malformed-core-id";
    }
    item = jw_content__item(system, "alternate_cores");
    if (item && !jw_content__string_list(item, 0, 16, 2, 64, jw_content__core_id)) {
        return "malformed-core-id";
    }
    const char *reason = jw_content__path_reason(
        jw_content__item(system, "icon_flat"), pak_root, false);
    if (reason) {
        return reason;
    }
    item = jw_content__item(system, "icon_photographic");
    if (item && !cJSON_IsNull(item)) {
        reason = jw_content__path_reason(item, pak_root, false);
        if (reason) {
            return reason;
        }
    }
    item = jw_content__item(system, "screenscraper_platform_ids");
    if (item) {
        if (!cJSON_IsArray(item) || cJSON_GetArraySize(item) > 8) {
            return "malformed-screenscraper-platform-ids";
        }
        cJSON *value = NULL;
        cJSON_ArrayForEach(value, item) {
            if (!cJSON_IsNumber(value) || value->valuedouble != (double)value->valueint ||
                value->valueint < 1 || value->valueint > 99999) {
                return "malformed-screenscraper-platform-ids";
            }
        }
    }
    item = jw_content__item(system, "group");
    if (item && !cJSON_IsNull(item) && !jw_content__string(item, 1, 32)) {
        return "malformed-group";
    }
    item = jw_content__item(system, "bios_directory");
    if (item && !cJSON_IsNull(item) &&
        (!cJSON_IsString(item) || !jw_content__ascii_component(item->valuestring, 32, true))) {
        return "malformed-bios-directory";
    }
    return NULL;
}

/* LEAF-RA-1: a pak-supplied RetroArch that Leaf launches in place of its own
   (leaf-contracts docs/leaf-retroarch-frontend.md). Checked in the contract's
   order: shape, then protocol, then the executable itself. */
static const char *jw_content__validate_frontend(const cJSON *frontend, const char *pak_root) {
    static const char *const keys[] = {"path", "protocol"};
    if (!jw_content__object_keys(frontend, keys, sizeof(keys) / sizeof(keys[0]))) {
        return "malformed-frontend";
    }
    cJSON *path = jw_content__item(frontend, "path");
    if (!cJSON_IsString(path) || !path->valuestring[0]) {
        return "malformed-frontend";
    }
    cJSON *protocol = jw_content__item(frontend, "protocol");
    if (!cJSON_IsString(protocol) || strcmp(protocol->valuestring, JW_RA_FRONTEND_PROTOCOL) != 0) {
        return "unknown-frontend-protocol";
    }
    return jw_content__path_reason(path, pak_root, true);
}

static const char *jw_content__validate_core(const cJSON *core, const char *pak_root) {
    static const char *const keys[] = {
        "id", "display_name", "type", "libretro_name", "file_name",
        "info_name", "config_folder", "path", "supports_menu",
        "supports_savestate", "supports_disk_control", "needs_swap",
        "requires_direct_drm", "legacy_flat_core", "name_map", "status",
        "frontend",
    };
    if (!jw_content__object_keys(core, keys, sizeof(keys) / sizeof(keys[0]))) {
        return "unknown-field";
    }
    cJSON *item = jw_content__item(core, "id");
    if (!cJSON_IsString(item) || !jw_content__core_id(item->valuestring)) {
        return "malformed-core-id";
    }
    if (!jw_content__string(jw_content__item(core, "display_name"), 1, 64)) {
        return "malformed-core-display-name";
    }
    static const char *const bools[] = {
        "supports_menu", "supports_savestate", "supports_disk_control", "needs_swap",
    };
    for (size_t i = 0; i < sizeof(bools) / sizeof(bools[0]); i++) {
        item = jw_content__item(core, bools[i]);
        if (item && !cJSON_IsBool(item)) {
            return "malformed-core-flag";
        }
    }
    cJSON *type = jw_content__item(core, "type");
    if (!cJSON_IsString(type) ||
        (strcmp(type->valuestring, "retroarch") != 0 && strcmp(type->valuestring, "path") != 0)) {
        return "unknown-core-type";
    }
    if (strcmp(type->valuestring, "retroarch") == 0) {
        if (jw_content__item(core, "path")) {
            return "core-type-field-mismatch";
        }
        item = jw_content__item(core, "libretro_name");
        if (!cJSON_IsString(item) || !jw_content__core_id(item->valuestring)) {
            return "malformed-libretro-name";
        }
        item = jw_content__item(core, "file_name");
        if (!item) {
            return "missing-core-file-name";
        }
        const char *reason = jw_content__path_reason(item, pak_root, false);
        if (reason) {
            return reason;
        }
        item = jw_content__item(core, "info_name");
        if (!item) {
            return "missing-core-info-name";
        }
        reason = jw_content__path_reason(item, pak_root, false);
        if (reason) {
            return reason;
        }
        item = jw_content__item(core, "config_folder");
        if (!item) {
            return "missing-config-folder";
        }
        if (!cJSON_IsString(item) || !jw_ra_core_folder_is_safe(item->valuestring)) {
            return "unsafe-config-folder";
        }
        item = jw_content__item(core, "frontend");
        if (item) {
            reason = jw_content__validate_frontend(item, pak_root);
            if (reason) {
                return reason;
            }
        }
    } else {
        static const char *const retro_only[] = {
            "libretro_name", "file_name", "info_name", "config_folder", "frontend",
        };
        for (size_t i = 0; i < sizeof(retro_only) / sizeof(retro_only[0]); i++) {
            if (jw_content__item(core, retro_only[i])) {
                return "core-type-field-mismatch";
            }
        }
        item = jw_content__item(core, "path");
        if (!item) {
            return "missing-core-path";
        }
        const char *reason = jw_content__path_reason(item, pak_root, true);
        if (reason) {
            return reason;
        }
    }
    return NULL;
}

static const char *jw_content__validate_extension(const cJSON *extension) {
    static const char *const keys[] = {"system_id", "add_alternate_cores"};
    if (!jw_content__object_keys(extension, keys, sizeof(keys) / sizeof(keys[0]))) {
        return "unknown-extension-field";
    }
    cJSON *item = jw_content__item(extension, "system_id");
    if (!cJSON_IsString(item) || !jw_content__system_id(item->valuestring)) {
        return "malformed-system-id";
    }
    item = jw_content__item(extension, "add_alternate_cores");
    if (cJSON_IsArray(item) && cJSON_GetArraySize(item) == 0) {
        return "empty-add-alternate-cores";
    }
    if (!jw_content__string_list(item, 1, 16, 2, 64, jw_content__core_id)) {
        return "malformed-core-id";
    }
    return NULL;
}

int jw_content_art_validate(const cJSON *document, const char *pak_dir,
                            const cJSON **out, char *reason, size_t reason_size) {
    static const char *const block_keys[] = {"schema", "systems"};
    /* Image slots in the contract's validation order; v1 has only the wordmark. */
    static const char *const row_keys[] = {"id", "wordmark", "wordmark_color", "grid_icon"};
    static const char *const malformed[] = {"", "malformed-content-art-wordmark",
        "malformed-content-art-wordmark-color", "malformed-content-art-grid-icon"};
    const char *error = NULL;
    if (out) *out = NULL;
    jw_content__reason(reason, reason_size, "");
    if (!out || !pak_dir || !cJSON_IsObject(document)) {
        jw_content__reason(reason, reason_size, "malformed-content-art");
        return -1;
    }
    const cJSON *block = jw_content__item(document, "content_art");
    if (!block) return 0;
    if (!cJSON_IsObject(block)) error = "malformed-content-art";
    else if (!jw_content__object_keys(block, block_keys, 2))
        error = "unknown-content-art-field";
    const cJSON *schema = jw_content__item(block, "schema");
    if (!error && (!cJSON_IsNumber(schema) || (schema->valuedouble != 1 && schema->valuedouble != 2)))
        error = "unknown-content-art-schema";
    bool v2 = !error && schema->valuedouble == 2;
    const cJSON *rows = jw_content__item(block, "systems");
    if (!error && (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) < 1 ||
                   cJSON_GetArraySize(rows) > 32))
        error = "malformed-content-art-systems";
    const cJSON *row = NULL;
    cJSON_ArrayForEach(row, rows) {
        if (error) break;
        if (!cJSON_IsObject(row)) { error = "malformed-content-art-system"; break; }
        if (!jw_content__object_keys(row, row_keys, v2 ? 4 : 2)) {
            error = "unknown-content-art-field"; break;
        }
        const cJSON *id = jw_content__item(row, "id");
        if (!cJSON_IsString(id) || !jw_content__system_id(id->valuestring)) {
            error = "malformed-content-art-system-id"; break;
        }
        for (const cJSON *other = row->next; other; other = other->next) {
            const cJSON *other_id = jw_content__item(other, "id");
            if (cJSON_IsString(other_id) && !strcmp(id->valuestring, other_id->valuestring))
                error = "duplicate-content-art-system";
        }
        if (error) break;
        if (v2 && !jw_content__item(row, "wordmark") && !jw_content__item(row, "grid_icon") &&
            !jw_content__item(row, "wordmark_color")) {
            error = "missing-content-art-image"; break;
        }
        for (int slot = 1; slot <= (v2 ? 3 : 1) && !error; slot++) {
            const cJSON *path = jw_content__item(row, row_keys[slot]);
            if (!path && v2) continue;
            if (!cJSON_IsString(path) || !path->valuestring[0] || strlen(path->valuestring) > 4096) {
                error = malformed[slot];
                break;
            }
            const char *path_error = jw_content__path_reason(path, pak_dir, false);
            if (path_error) {
                snprintf(reason, reason_size, "content-art-%s", path_error);
                return -1;
            }
            char full[PATH_MAX];
            if (snprintf(full, sizeof(full), "%s/%s", pak_dir, path->valuestring) >= (int)sizeof(full)) {
                error = "content-art-missing-file"; break;
            }
            FILE *file = fopen(full, "rb");
            if (!file) { error = "unreadable-content-art-image"; break; }
            unsigned char header[24];
            size_t got = fread(header, 1, sizeof(header), file);
            bool read_error = ferror(file);
            fclose(file);
            size_t len = strlen(path->valuestring);
            if (read_error) error = "unreadable-content-art-image";
            else if (len < 4 || strcasecmp(path->valuestring + len - 4, ".png") ||
                     got < 8 || memcmp(header, "\x89PNG\r\n\x1a\n", 8))
                error = "unsupported-content-art-image";
            /* Every image slot carries the same 1..1024 IHDR bound; the launcher
               decodes before downscaling, so this is what caps decode memory. */
            else if (got != 24 || memcmp(header + 8, "\0\0\0\rIHDR", 8))
                error = "unsupported-content-art-image";
            else for (int offset = 16; offset <= 20; offset += 4) {
                unsigned dim = ((unsigned)header[offset] << 24) |
                    ((unsigned)header[offset + 1] << 16) |
                    ((unsigned)header[offset + 2] << 8) | header[offset + 3];
                if (dim == 0 || dim > 1024)
                    error = slot == 3 ? "invalid-content-art-grid-dimensions"
                                      : "invalid-content-art-wordmark-dimensions";
            }
        }
    }
    if (error) { jw_content__reason(reason, reason_size, error); return -1; }
    *out = block;
    return 1;
}

int jw_content_scrape_validate(const cJSON *document,
                               const cJSON **out,
                               char *reason,
                               size_t reason_size) {
    static const char *const block_keys[] = {"schema", "systems"};
    static const char *const system_keys[] = {
        "id", "name_source", "lookup_extension",
    };
    if (out) {
        *out = NULL;
    }
    jw_content__reason(reason, reason_size, "");
    if (!cJSON_IsObject(document) || !out) {
        jw_content__reason(reason, reason_size, "malformed-content-scrape");
        return -1;
    }

    const cJSON *block = jw_content__item(document, "content_scrape");
    if (!block) {
        return 0;
    }
    if (!cJSON_IsObject(block)) {
        jw_content__reason(reason, reason_size, "malformed-content-scrape");
        return -1;
    }
    if (!jw_content__object_keys(block, block_keys,
                                 sizeof(block_keys) / sizeof(block_keys[0]))) {
        jw_content__reason(reason, reason_size, "unknown-content-scrape-field");
        return -1;
    }
    const cJSON *schema = jw_content__item(block, "schema");
    if (!cJSON_IsNumber(schema) || schema->valuedouble != 1.0) {
        jw_content__reason(reason, reason_size, "unknown-content-scrape-schema");
        return -1;
    }
    const cJSON *rows = jw_content__item(block, "systems");
    if (!cJSON_IsArray(rows) || cJSON_GetArraySize(rows) < 1 ||
        cJSON_GetArraySize(rows) > 32) {
        jw_content__reason(reason, reason_size,
                           "malformed-content-scrape-systems");
        return -1;
    }

    const cJSON *provides = jw_content__item(document, "provides");
    const cJSON *provided_systems = jw_content__item(provides, "systems");
    const cJSON *row = NULL;
    cJSON_ArrayForEach(row, rows) {
        if (!cJSON_IsObject(row)) {
            jw_content__reason(reason, reason_size,
                               "malformed-content-scrape-system");
            return -1;
        }
        if (!jw_content__object_keys(row, system_keys,
                                     sizeof(system_keys) / sizeof(system_keys[0]))) {
            jw_content__reason(reason, reason_size,
                               "unknown-content-scrape-field");
            return -1;
        }
        const cJSON *id = jw_content__item(row, "id");
        if (!cJSON_IsString(id) || !jw_content__system_id(id->valuestring)) {
            jw_content__reason(reason, reason_size,
                               "malformed-content-scrape-system-id");
            return -1;
        }
        for (const cJSON *other = row->next; other; other = other->next) {
            const cJSON *other_id = jw_content__item(other, "id");
            if (cJSON_IsString(other_id) &&
                strcmp(id->valuestring, other_id->valuestring) == 0) {
                jw_content__reason(reason, reason_size,
                                   "duplicate-content-scrape-system");
                return -1;
            }
        }
        const cJSON *name_source = jw_content__item(row, "name_source");
        if (!cJSON_IsString(name_source) ||
            strcmp(name_source->valuestring, "descriptor") != 0) {
            jw_content__reason(reason, reason_size,
                               "unknown-content-scrape-name-source");
            return -1;
        }
        const cJSON *lookup_extension = jw_content__item(row, "lookup_extension");
        if (!cJSON_IsString(lookup_extension) ||
            !jw_content__extension(lookup_extension->valuestring, 16, true)) {
            jw_content__reason(reason, reason_size,
                               "malformed-content-scrape-lookup-extension");
            return -1;
        }

        const cJSON *provided = NULL;
        const cJSON *candidate = NULL;
        cJSON_ArrayForEach(candidate, provided_systems) {
            const cJSON *candidate_id = jw_content__item(candidate, "id");
            if (cJSON_IsString(candidate_id) &&
                strcmp(candidate_id->valuestring, id->valuestring) == 0) {
                provided = candidate;
                break;
            }
        }
        if (!provided) {
            jw_content__reason(reason, reason_size,
                               "unknown-content-scrape-system");
            return -1;
        }
        bool declared = false;
        const cJSON *extension = NULL;
        cJSON_ArrayForEach(extension, jw_content__item(provided, "extensions")) {
            if (cJSON_IsString(extension) &&
                strcmp(extension->valuestring,
                       lookup_extension->valuestring) == 0) {
                declared = true;
                break;
            }
        }
        if (!declared) {
            jw_content__reason(reason, reason_size,
                               "undeclared-content-scrape-extension");
            return -1;
        }
    }

    *out = block;
    return 1;
}

bool jw_content_manifest_validate(const char *pak_json_text,
                                  const char *pak_abs_path,
                                  jw_content_install_lane lane,
                                  const char *source_id,
                                  jw_content_manifest *out,
                                  char *reason,
                                  size_t reason_size) {
    static const char *const provides_keys[] = {
        "schema", "systems", "system_extensions", "cores",
    };
    if (!pak_json_text || !pak_abs_path || !source_id || !out) {
        jw_content__reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (lane == JW_CONTENT_SHARED_LANE) {
        jw_content__reason(reason, reason_size, "shared-content-unsupported");
        return false;
    }
    if (strcmp(source_id, "primary") != 0) {
        jw_content__reason(reason, reason_size, "secondary-source-unsupported");
        return false;
    }
    char *sanitized = jw_content__escaped_nul(pak_json_text)
                          ? jw_content__without_nul_art(pak_json_text) : NULL;
    const char *input = sanitized ? sanitized : pak_json_text;
    if (jw_content__escaped_nul(input)) {
        free(sanitized);
        jw_content__reason(reason, reason_size, "invalid-json");
        return false;
    }
    cJSON *document = cJSON_ParseWithOpts(input, NULL, true);
    free(sanitized);
    if (!cJSON_IsObject(document)) {
        cJSON_Delete(document);
        jw_content__reason(reason, reason_size, "invalid-json");
        return false;
    }
    cJSON *provides = jw_content__item(document, "provides");
    if (!jw_content__object_keys(provides, provides_keys,
                                 sizeof(provides_keys) / sizeof(provides_keys[0]))) {
        cJSON_Delete(document);
        jw_content__reason(reason, reason_size, "unknown-field");
        return false;
    }
    cJSON *schema = jw_content__item(provides, "schema");
    if (!cJSON_IsNumber(schema) || schema->valuedouble != 1.0) {
        cJSON_Delete(document);
        jw_content__reason(reason, reason_size, "unknown-schema");
        return false;
    }
    const char *failure = jw_content__forbidden(provides);
    if (failure) {
        cJSON_Delete(document);
        jw_content__reason(reason, reason_size, failure);
        return false;
    }
    cJSON *systems = jw_content__item(provides, "systems");
    cJSON *extensions = jw_content__item(provides, "system_extensions");
    cJSON *cores = jw_content__item(provides, "cores");
    if (systems && !cJSON_IsArray(systems)) {
        failure = "unknown-field";
    } else if (systems && cJSON_GetArraySize(systems) > 32) {
        failure = "too-many-systems";
    } else if (extensions && !cJSON_IsArray(extensions)) {
        failure = "unknown-field";
    } else if (extensions && cJSON_GetArraySize(extensions) > 32) {
        failure = "too-many-system-extensions";
    } else if (cores && !cJSON_IsArray(cores)) {
        failure = "unknown-field";
    } else if (cores && cJSON_GetArraySize(cores) > 32) {
        failure = "too-many-cores";
    } else if ((!systems || cJSON_GetArraySize(systems) == 0) &&
               (!extensions || cJSON_GetArraySize(extensions) == 0) &&
               (!cores || cJSON_GetArraySize(cores) == 0)) {
        failure = "empty-provides";
    }
    bool warning = false;
    if (!failure && systems) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, systems) {
            failure = jw_content__validate_system(item, pak_abs_path, &warning);
            if (failure) break;
        }
    }
    if (!failure && cores) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, cores) {
            failure = jw_content__validate_core(item, pak_abs_path);
            if (failure) break;
        }
    }
    if (!failure && extensions) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, extensions) {
            failure = jw_content__validate_extension(item);
            if (failure) break;
        }
    }
    if (failure) {
        cJSON_Delete(document);
        jw_content__reason(reason, reason_size, failure);
        return false;
    }
    out->document = document;
    out->provides = provides;
    out->redundant_case_variant = warning;
    return true;
}
