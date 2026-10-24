/* strdup() and fileno() are hidden by glibc under a bare -std=c11, and an
 * implicit declaration truncates strdup's returned pointer to 32 bits -- the
 * exact bug internal/ipc/ipc.c documents. Must precede every #include,
 * including the paired header. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/catalog/effective.h"

#include "internal/catalog/json.h"
#include "internal/catalog/merge.h"
#include "internal/platform/leaf_version.h"
#include "internal/platform/platform_id.h"
#include "internal/update/sha256.h"

#include "cJSON.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define JW_CAT_STAMP_SCHEMA 1
#define JW_CAT_MAX_TREE_FILES 4096
#define JW_CAT_MAX_FILE_BYTES (8u * 1024u * 1024u)
#define JW_CAT_COPY_CHUNK 65536

static void jw_cat__set(char *out, size_t out_size, const char *value) {
    if (out && out_size > 0) {
        snprintf(out, out_size, "%s", value ? value : "");
    }
}

static int jw_cat__join(char *out, size_t out_size, const char *a, const char *b) {
    int written = snprintf(out, out_size, "%s/%s", a, b);
    return (written < 0 || (size_t)written >= out_size) ? -1 : 0;
}

static int jw_cat__is_dir(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int jw_cat__is_file(const char *path) {
    struct stat st;
    return path && path[0] && stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int jw_cat__mkdir_p(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }
    char work[PATH_MAX];
    if (snprintf(work, sizeof(work), "%s", path) >= (int)sizeof(work)) {
        return -1;
    }
    for (char *p = work + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(work, 0755) != 0 && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }
    if (mkdir(work, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* Durability is the whole point of the publication protocol, so the fsyncs
   are not optional: a selector that reaches the disk before the generation
   it names would survive a power cut pointing at nothing. */
static int jw_cat__fsync_dir(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    int rc = fsync(fd);
    close(fd);
    return rc;
}

static char *jw_cat__read_file(const char *path, size_t *out_len) {
    if (out_len) {
        *out_len = 0;
    }
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return NULL;
    }
    if (st.st_size < 0 || (uintmax_t)st.st_size > JW_CAT_MAX_FILE_BYTES) {
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    size_t len = (size_t)st.st_size;
    char *buf = malloc(len + 1u);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = len > 0 ? fread(buf, 1u, len, f) : 0u;
    fclose(f);
    if (got != len) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    if (out_len) {
        *out_len = len;
    }
    return buf;
}

static int jw_cat__write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) {
        return -1;
    }
    int ok = (len == 0 || fwrite(data, 1u, len, f) == len);
    if (ok && fflush(f) != 0) {
        ok = 0;
    }
    if (ok && fsync(fileno(f)) != 0) {
        ok = 0;
    }
    if (fclose(f) != 0) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

static int jw_cat__copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return -1;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return -1;
    }
    char chunk[JW_CAT_COPY_CHUNK];
    int ok = 1;
    size_t got;
    while ((got = fread(chunk, 1u, sizeof(chunk), in)) > 0) {
        if (fwrite(chunk, 1u, got, out) != got) {
            ok = 0;
            break;
        }
    }
    if (ferror(in)) {
        ok = 0;
    }
    if (ok && fflush(out) != 0) {
        ok = 0;
    }
    if (ok && fsync(fileno(out)) != 0) {
        ok = 0;
    }
    fclose(in);
    if (fclose(out) != 0) {
        ok = 0;
    }
    return ok ? 0 : -1;
}

/* Bounded recursive delete that never follows a symlink out of the tree.
   Only ever called on a tmp-* directory this process created. */
static int jw_cat__rmtree(const char *path, int depth) {
    if (depth > 8) {
        return -1;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return unlink(path) == 0 ? 0 : -1;
    }
    struct dirent *entry;
    int rc = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char child[PATH_MAX];
        if (jw_cat__join(child, sizeof(child), path, entry->d_name) != 0) {
            rc = -1;
            continue;
        }
        struct stat st;
        if (lstat(child, &st) != 0) {
            rc = -1;
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (jw_cat__rmtree(child, depth + 1) != 0) {
                rc = -1;
            }
        } else if (unlink(child) != 0) {
            rc = -1;
        }
    }
    closedir(dir);
    if (rmdir(path) != 0) {
        rc = -1;
    }
    return rc;
}

/* --------------------------------------------------------------------------
   Relative-path collection, sorted in BYTE order.

   Not alphasort(): that sorts with strcoll(), which is locale-dependent, so
   the same directory could hash differently on two devices with different
   locales. The digest has to be locale-independent or the producer and the
   readers can disagree about which generation is current.
   -------------------------------------------------------------------------- */

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} jw_cat_path_list;

static void jw_cat__paths_free(jw_cat_path_list *list) {
    for (size_t i = 0; i < list->count; i++) {
        free(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static int jw_cat__paths_push(jw_cat_path_list *list, const char *value) {
    if (list->count >= JW_CAT_MAX_TREE_FILES) {
        return -1;
    }
    if (list->count == list->capacity) {
        size_t capacity = list->capacity ? list->capacity * 2u : 32u;
        char **items = realloc(list->items, capacity * sizeof(*items));
        if (!items) {
            return -1;
        }
        list->items = items;
        list->capacity = capacity;
    }
    char *copy = strdup(value);
    if (!copy) {
        return -1;
    }
    list->items[list->count++] = copy;
    return 0;
}

static int jw_cat__path_cmp(const void *a, const void *b) {
    const char *const *left = a;
    const char *const *right = b;
    return strcmp(*left, *right);
}

static int jw_cat__collect(const char *root, const char *prefix,
                           jw_cat_path_list *out, int depth) {
    if (depth > 8) {
        return -1;
    }
    char dir_path[PATH_MAX];
    if (prefix[0]) {
        if (jw_cat__join(dir_path, sizeof(dir_path), root, prefix) != 0) {
            return -1;
        }
    } else if (snprintf(dir_path, sizeof(dir_path), "%s", root) >= (int)sizeof(dir_path)) {
        return -1;
    }

    DIR *dir = opendir(dir_path);
    if (!dir) {
        return -1;
    }
    struct dirent *entry;
    int rc = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        char rel[PATH_MAX];
        if (prefix[0]) {
            if (jw_cat__join(rel, sizeof(rel), prefix, entry->d_name) != 0) {
                rc = -1;
                break;
            }
        } else if (snprintf(rel, sizeof(rel), "%s", entry->d_name) >= (int)sizeof(rel)) {
            rc = -1;
            break;
        }
        char full[PATH_MAX];
        if (jw_cat__join(full, sizeof(full), root, rel) != 0) {
            rc = -1;
            break;
        }
        struct stat st;
        /* lstat, not stat: a symlink inside the info directory is not a
           regular file we can reproduce byte-for-byte in a generation, and
           silently following one would let content outside the tree change
           the digest. */
        if (lstat(full, &st) != 0) {
            rc = -1;
            break;
        }
        if (S_ISDIR(st.st_mode)) {
            if (jw_cat__collect(root, rel, out, depth + 1) != 0) {
                rc = -1;
                break;
            }
        } else if (S_ISREG(st.st_mode)) {
            if (jw_cat__paths_push(out, rel) != 0) {
                rc = -1;
                break;
            }
        }
    }
    closedir(dir);
    return rc;
}

static void jw_cat__store_be64(unsigned char out[8], uint64_t value) {
    for (int i = 7; i >= 0; i--) {
        out[i] = (unsigned char)(value & 0xffu);
        value >>= 8u;
    }
}

int jw_catalog_tree_sha256(const char *dir, char out_hex[65]) {
    if (!out_hex) {
        return -1;
    }
    jw_sha256_ctx ctx;
    jw_sha256_init(&ctx);

    if (jw_cat__is_dir(dir)) {
        jw_cat_path_list paths = {0};
        if (jw_cat__collect(dir, "", &paths, 0) != 0) {
            jw_cat__paths_free(&paths);
            return -1;
        }
        qsort(paths.items, paths.count, sizeof(*paths.items), jw_cat__path_cmp);

        for (size_t i = 0; i < paths.count; i++) {
            char full[PATH_MAX];
            if (jw_cat__join(full, sizeof(full), dir, paths.items[i]) != 0) {
                jw_cat__paths_free(&paths);
                return -1;
            }
            size_t len = 0;
            char *data = jw_cat__read_file(full, &len);
            if (!data) {
                jw_cat__paths_free(&paths);
                return -1;
            }
            unsigned char len_be[8];
            jw_cat__store_be64(len_be, (uint64_t)len);
            jw_sha256_update(&ctx, paths.items[i], strlen(paths.items[i]));
            jw_sha256_update(&ctx, "\0", 1u);
            jw_sha256_update(&ctx, len_be, sizeof(len_be));
            if (len > 0) {
                jw_sha256_update(&ctx, data, len);
            }
            free(data);
        }
        jw_cat__paths_free(&paths);
    }

    jw_sha256_final_hex(&ctx, out_hex);
    return 0;
}

/* --------------------------------------------------------------------------
   Canonical JSON

   Keys sorted by code point, separators "," and ":" with no padding, UTF-8,
   no BOM, non-ASCII left unescaped, plus exactly one trailing newline. The
   stamp shape is fixed and small, so the emitter writes keys in sorted order
   directly rather than sorting a generic tree at runtime -- but the ORDER
   below is normative and must not be rearranged for readability.
   -------------------------------------------------------------------------- */

typedef struct {
    char *data;
    size_t len;
    size_t capacity;
    int failed;
} jw_cat_buf;

static void jw_cat__buf_free(jw_cat_buf *buf) {
    free(buf->data);
    buf->data = NULL;
    buf->len = 0;
    buf->capacity = 0;
}

static void jw_cat__buf_append(jw_cat_buf *buf, const char *data, size_t len) {
    if (buf->failed) {
        return;
    }
    if (buf->len + len + 1u > buf->capacity) {
        size_t capacity = buf->capacity ? buf->capacity : 512u;
        while (capacity < buf->len + len + 1u) {
            capacity *= 2u;
        }
        char *grown = realloc(buf->data, capacity);
        if (!grown) {
            buf->failed = 1;
            return;
        }
        buf->data = grown;
        buf->capacity = capacity;
    }
    memcpy(buf->data + buf->len, data, len);
    buf->len += len;
    buf->data[buf->len] = '\0';
}

static void jw_cat__buf_puts(jw_cat_buf *buf, const char *text) {
    jw_cat__buf_append(buf, text, strlen(text));
}

/* Matches Python's json.dumps(ensure_ascii=False) escape table exactly:
   backslash, quote, the five shortcut controls, and \u00xx for the rest
   below 0x20. Nothing else -- including DEL -- is escaped. The fixture
   `canonical-stamp-bytes` in leaf-contracts/contracts/leaf-content locks this. */
static void jw_cat__buf_json_string(jw_cat_buf *buf, const char *value) {
    jw_cat__buf_puts(buf, "\"");
    for (const unsigned char *p = (const unsigned char *)(value ? value : ""); *p; p++) {
        switch (*p) {
        case '"': jw_cat__buf_puts(buf, "\\\""); break;
        case '\\': jw_cat__buf_puts(buf, "\\\\"); break;
        case '\b': jw_cat__buf_puts(buf, "\\b"); break;
        case '\f': jw_cat__buf_puts(buf, "\\f"); break;
        case '\n': jw_cat__buf_puts(buf, "\\n"); break;
        case '\r': jw_cat__buf_puts(buf, "\\r"); break;
        case '\t': jw_cat__buf_puts(buf, "\\t"); break;
        default:
            if (*p < 0x20u) {
                char escaped[8];
                snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned int)*p);
                jw_cat__buf_puts(buf, escaped);
            } else {
                jw_cat__buf_append(buf, (const char *)p, 1u);
            }
            break;
        }
    }
    jw_cat__buf_puts(buf, "\"");
}

typedef struct {
    char systems_sha256[65];
    char cores_sha256[65];
    char info_sha256[65];
} jw_cat_hashes;

typedef struct {
    char *rel;
    char sha256[65];
} jw_cat_file_stamp;

typedef struct {
    char *provider;
    char *source_id;
    char *pak_version;
    char provides_sha256[65];
    jw_cat_file_stamp *files;
    size_t file_count;
} jw_cat_contributor_stamp;

typedef struct {
    char platform[64];
    char release_id[JW_LEAF_RELEASE_ID_MAX];
    jw_cat_hashes base;
    jw_cat_hashes output;
    jw_cat_contributor_stamp *contributors;
    size_t contributor_count;
} jw_cat_stamp;

static void jw_cat__stamp_free(jw_cat_stamp *stamp) {
    for (size_t i = 0; i < stamp->contributor_count; i++) {
        jw_cat_contributor_stamp *contributor = &stamp->contributors[i];
        free(contributor->provider);
        free(contributor->source_id);
        free(contributor->pak_version);
        for (size_t f = 0; f < contributor->file_count; f++) {
            free(contributor->files[f].rel);
        }
        free(contributor->files);
    }
    free(stamp->contributors);
    stamp->contributors = NULL;
    stamp->contributor_count = 0;
}

static void jw_cat__emit_hashes(jw_cat_buf *buf, const jw_cat_hashes *hashes) {
    jw_cat__buf_puts(buf, "{\"cores_sha256\":");
    jw_cat__buf_json_string(buf, hashes->cores_sha256);
    jw_cat__buf_puts(buf, ",\"info_sha256\":");
    jw_cat__buf_json_string(buf, hashes->info_sha256);
    jw_cat__buf_puts(buf, ",\"systems_sha256\":");
    jw_cat__buf_json_string(buf, hashes->systems_sha256);
    jw_cat__buf_puts(buf, "}");
}

static char *jw_cat__stamp_canonical(const jw_cat_stamp *stamp, size_t *out_len) {
    jw_cat_buf buf = {0};
    jw_cat__buf_puts(&buf, "{\"base\":");
    jw_cat__emit_hashes(&buf, &stamp->base);
    jw_cat__buf_puts(&buf, ",\"contributors\":[");
    for (size_t i = 0; i < stamp->contributor_count; i++) {
        const jw_cat_contributor_stamp *contributor = &stamp->contributors[i];
        if (i) {
            jw_cat__buf_puts(&buf, ",");
        }
        /* Canonical key order: files, pak_version, provider,
           provides_sha256, source_id. */
        jw_cat__buf_puts(&buf, "{\"files\":[");
        for (size_t f = 0; f < contributor->file_count; f++) {
            if (f) {
                jw_cat__buf_puts(&buf, ",");
            }
            jw_cat__buf_puts(&buf, "{\"rel\":");
            jw_cat__buf_json_string(&buf, contributor->files[f].rel);
            jw_cat__buf_puts(&buf, ",\"sha256\":");
            jw_cat__buf_json_string(&buf, contributor->files[f].sha256);
            jw_cat__buf_puts(&buf, "}");
        }
        jw_cat__buf_puts(&buf, "],\"pak_version\":");
        jw_cat__buf_json_string(&buf, contributor->pak_version);
        jw_cat__buf_puts(&buf, ",\"provider\":");
        jw_cat__buf_json_string(&buf, contributor->provider);
        jw_cat__buf_puts(&buf, ",\"provides_sha256\":");
        jw_cat__buf_json_string(&buf, contributor->provides_sha256);
        jw_cat__buf_puts(&buf, ",\"source_id\":");
        jw_cat__buf_json_string(&buf, contributor->source_id);
        jw_cat__buf_puts(&buf, "}");
    }
    jw_cat__buf_puts(&buf, "],\"output\":");
    jw_cat__emit_hashes(&buf, &stamp->output);
    jw_cat__buf_puts(&buf, ",\"platform\":");
    jw_cat__buf_json_string(&buf, stamp->platform);
    jw_cat__buf_puts(&buf, ",\"release_id\":");
    jw_cat__buf_json_string(&buf, stamp->release_id);
    jw_cat__buf_puts(&buf, ",\"schema\":1}\n");

    if (buf.failed) {
        jw_cat__buf_free(&buf);
        return NULL;
    }
    if (out_len) {
        *out_len = buf.len;
    }
    return buf.data;
}

/* --------------------------------------------------------------------------
   Paths
   -------------------------------------------------------------------------- */

static int jw_cat__state_dir(const char *sdcard_root, char *out, size_t out_size) {
    const char *internal = getenv("UMRK_INTERNAL_DATA_PATH");
    if (internal && internal[0]) {
        return snprintf(out, out_size, "%s", internal) >= (int)out_size ? -1 : 0;
    }
    if (!sdcard_root || !sdcard_root[0]) {
        return -1;
    }
    int written = snprintf(out, out_size, "%s/.umrk/%s", sdcard_root,
                           jw_platform_compiled_id());
    return (written < 0 || (size_t)written >= out_size) ? -1 : 0;
}

int jw_catalog_dir(const char *sdcard_root, char *out, size_t out_size) {
    char state_dir[PATH_MAX];
    if (jw_cat__state_dir(sdcard_root, state_dir, sizeof(state_dir)) != 0) {
        return -1;
    }
    return jw_cat__join(out, out_size, state_dir, "catalog");
}

/* --------------------------------------------------------------------------
   Selector
   -------------------------------------------------------------------------- */

bool jw_catalog_parse_selector(const char *raw, char *out, size_t out_size) {
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    if (!raw) {
        return false;
    }
    size_t len = strlen(raw);
    /* "gen-" + 64 hex + exactly one trailing newline, and nothing after it. */
    if (len != 4u + 64u + 1u || raw[len - 1u] != '\n') {
        return false;
    }
    if (strncmp(raw, "gen-", 4u) != 0) {
        return false;
    }
    for (size_t i = 4u; i < len - 1u; i++) {
        char c = raw[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
            return false;
        }
    }
    if (!out || out_size < len) {
        return false;
    }
    memcpy(out, raw, len - 1u);
    out[len - 1u] = '\0';
    return true;
}

static int jw_cat__read_selector(const char *catalog_dir, char *out, size_t out_size) {
    char path[PATH_MAX];
    if (jw_cat__join(path, sizeof(path), catalog_dir, "current") != 0) {
        return -1;
    }
    size_t len = 0;
    char *raw = jw_cat__read_file(path, &len);
    if (!raw) {
        return 1; /* absent */
    }
    int ok = jw_catalog_parse_selector(raw, out, out_size) ? 0 : 2;
    free(raw);
    return ok;
}

static int jw_cat__write_selector(const char *catalog_dir, const char *generation) {
    char tmp_path[PATH_MAX];
    char final_path[PATH_MAX];
    if (jw_cat__join(tmp_path, sizeof(tmp_path), catalog_dir, "current.tmp") != 0 ||
        jw_cat__join(final_path, sizeof(final_path), catalog_dir, "current") != 0) {
        return -1;
    }
    char line[JW_CAT_GEN_NAME_MAX + 2];
    int written = snprintf(line, sizeof(line), "%s\n", generation);
    if (written < 0 || (size_t)written >= sizeof(line)) {
        return -1;
    }
    if (jw_cat__write_file(tmp_path, line, (size_t)written) != 0) {
        return -1;
    }
    /* rename() of a small FILE is atomic, including on vfat. This is the one
       operation the whole protocol leans on. */
    if (rename(tmp_path, final_path) != 0) {
        unlink(tmp_path);
        return -1;
    }
    /* Best-effort past this point. Once the rename returns, every reader
       sees the new selector; a failed directory fsync weakens crash
       durability but does not make the publication wrong, and reporting it
       as a compile failure would tell the user the catalog is unpublished
       when it is published and correct. (Directory fsync does work on the
       MLP1's vfat card -- this is about not conflating the two failures.) */
    (void)jw_cat__fsync_dir(catalog_dir);
    return 0;
}

static void jw_cat__invalidate_selector(const char *catalog_dir) {
    char path[PATH_MAX];
    if (jw_cat__join(path, sizeof(path), catalog_dir, "current") != 0) {
        return;
    }
    /* Unlink and fsync BEFORE recompiling, so every new reader falls back to
       release defaults during the gap and a crash mid-compile leaves no
       selector rather than one pointing at unverified output. */
    unlink(path);
    jw_cat__fsync_dir(catalog_dir);
}

/* --------------------------------------------------------------------------
   Release identity
   -------------------------------------------------------------------------- */

static int jw_cat__release_id(const char *sdcard_root, char *out, size_t out_size) {
    char state_dir[PATH_MAX];
    if (jw_cat__state_dir(sdcard_root, state_dir, sizeof(state_dir)) != 0) {
        return -1;
    }
    jw_installed_release release;
    memset(&release, 0, sizeof(release));
    /* Missing, empty, or invalid: never guessed. The caller falls back to
       release defaults and records release-identity-unavailable. */
    if (jw_installed_release_read(state_dir, &release) != 0 ||
        release.release_id[0] == '\0') {
        return -1;
    }
    return snprintf(out, out_size, "%s", release.release_id) >= (int)out_size ? -1 : 0;
}

/* --------------------------------------------------------------------------
   Structural validation -- the reader path
   -------------------------------------------------------------------------- */

static int jw_cat__stamp_field_equals(const cJSON *root, const char *key,
                                      const char *expected) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    return cJSON_IsString(item) && item->valuestring &&
           strcmp(item->valuestring, expected) == 0;
}

jw_catalog_resolution jw_catalog_effective_dir(const char *sdcard_root,
                                               char *out_dir,
                                               size_t out_dir_size,
                                               char *reason,
                                               size_t reason_size) {
    if (out_dir && out_dir_size > 0) {
        out_dir[0] = '\0';
    }
    jw_cat__set(reason, reason_size, "");

    char catalog_dir[PATH_MAX];
    if (jw_catalog_dir(sdcard_root, catalog_dir, sizeof(catalog_dir)) != 0) {
        jw_cat__set(reason, reason_size, "selector-missing");
        return JW_CAT_RELEASE_DEFAULTS;
    }

    char release_id[JW_LEAF_RELEASE_ID_MAX];
    if (jw_cat__release_id(sdcard_root, release_id, sizeof(release_id)) != 0) {
        jw_cat__set(reason, reason_size, "release-identity-unavailable");
        return JW_CAT_RELEASE_DEFAULTS;
    }

    char generation[JW_CAT_GEN_NAME_MAX];
    int selector = jw_cat__read_selector(catalog_dir, generation, sizeof(generation));
    if (selector == 1) {
        jw_cat__set(reason, reason_size, "selector-missing");
        return JW_CAT_RELEASE_DEFAULTS;
    }
    if (selector != 0) {
        jw_cat__set(reason, reason_size, "selector-malformed");
        return JW_CAT_RELEASE_DEFAULTS;
    }

    char gen_dir[PATH_MAX];
    if (jw_cat__join(gen_dir, sizeof(gen_dir), catalog_dir, generation) != 0 ||
        !jw_cat__is_dir(gen_dir)) {
        jw_cat__set(reason, reason_size, "generation-missing");
        return JW_CAT_RELEASE_DEFAULTS;
    }

    /* A partially written generation must never be served, so every output
       is required before the stamp is even parsed. */
    char systems_path[PATH_MAX];
    char cores_path[PATH_MAX];
    char info_dir[PATH_MAX];
    char stamp_path[PATH_MAX];
    if (jw_cat__join(systems_path, sizeof(systems_path), gen_dir, "systems.json") != 0 ||
        jw_cat__join(cores_path, sizeof(cores_path), gen_dir, "cores.json") != 0 ||
        jw_cat__join(info_dir, sizeof(info_dir), gen_dir, "info") != 0 ||
        jw_cat__join(stamp_path, sizeof(stamp_path), gen_dir, "stamp.json") != 0) {
        jw_cat__set(reason, reason_size, "generation-missing");
        return JW_CAT_RELEASE_DEFAULTS;
    }
    if (!jw_cat__is_file(systems_path) || !jw_cat__is_file(cores_path) ||
        !jw_cat__is_dir(info_dir)) {
        jw_cat__set(reason, reason_size, "generation-missing");
        return JW_CAT_RELEASE_DEFAULTS;
    }

    size_t stamp_len = 0;
    char *stamp_text = jw_cat__read_file(stamp_path, &stamp_len);
    if (!stamp_text) {
        jw_cat__set(reason, reason_size, "stamp-missing");
        return JW_CAT_RELEASE_DEFAULTS;
    }
    cJSON *stamp = cJSON_Parse(stamp_text);
    free(stamp_text);
    if (!stamp) {
        jw_cat__set(reason, reason_size, "stamp-missing");
        return JW_CAT_RELEASE_DEFAULTS;
    }

    const char *result_reason = NULL;
    const cJSON *schema = cJSON_GetObjectItemCaseSensitive(stamp, "schema");
    if (!cJSON_IsNumber(schema) || schema->valueint != JW_CAT_STAMP_SCHEMA) {
        result_reason = "stamp-schema-unsupported";
    } else if (!jw_cat__stamp_field_equals(stamp, "platform", jw_platform_compiled_id())) {
        result_reason = "stamp-platform-mismatch";
    } else if (!jw_cat__stamp_field_equals(stamp, "release_id", release_id)) {
        result_reason = "stamp-release-mismatch";
    }
    cJSON_Delete(stamp);

    if (result_reason) {
        jw_cat__set(reason, reason_size, result_reason);
        return JW_CAT_RELEASE_DEFAULTS;
    }

    if (!out_dir || snprintf(out_dir, out_dir_size, "%s", gen_dir) >= (int)out_dir_size) {
        jw_cat__set(reason, reason_size, "generation-missing");
        return JW_CAT_RELEASE_DEFAULTS;
    }
    return JW_CAT_EFFECTIVE;
}

/* --------------------------------------------------------------------------
   Diagnostics -- outside every generation, so a compile that produced no
   directory at all still leaves an explanation.
   -------------------------------------------------------------------------- */

static int jw_cat__write_diagnostics(const char *catalog_dir, const char *reason,
                                     const char *detail) {
    char path[PATH_MAX];
    if (jw_cat__join(path, sizeof(path), catalog_dir, "diagnostics.json") != 0) {
        return -1;
    }
    jw_cat_buf buf = {0};
    jw_cat__buf_puts(&buf, "{\n  \"schema\": 1,\n  \"entries\": [\n    {\n      \"reason\": ");
    jw_cat__buf_json_string(&buf, reason);
    jw_cat__buf_puts(&buf, ",\n      \"detail\": ");
    jw_cat__buf_json_string(&buf, detail ? detail : "");
    jw_cat__buf_puts(&buf, "\n    }\n  ]\n}\n");
    int rc = buf.failed ? -1 : jw_cat__write_file(path, buf.data, buf.len);
    jw_cat__buf_free(&buf);
    return rc;
}

static int jw_cat__record_failure_at(const char *catalog_dir,
                                     const char *reason,
                                     const char *detail) {
    jw_cat__invalidate_selector(catalog_dir);
    return jw_cat__write_diagnostics(catalog_dir, reason, detail);
}

int jw_catalog_record_failure(const char *sdcard_root,
                              const char *reason,
                              const char *detail) {
    char catalog_dir[PATH_MAX];
    if (jw_catalog_dir(sdcard_root, catalog_dir, sizeof(catalog_dir)) != 0 ||
        jw_cat__mkdir_p(catalog_dir) != 0) {
        return -1;
    }
    return jw_cat__record_failure_at(
        catalog_dir,
        (reason && reason[0]) ? reason : "compile-failed",
        detail ? detail : "catalog compilation failed");
}

static void jw_cat__clear_diagnostics(const char *catalog_dir) {
    char path[PATH_MAX];
    if (jw_cat__join(path, sizeof(path), catalog_dir, "diagnostics.json") == 0) {
        unlink(path);
    }
}

static void jw_cat__write_entry_diagnostics(const char *catalog_dir,
                                            const cJSON *enumeration,
                                            const cJSON *merge) {
    int enumeration_count = cJSON_IsArray(enumeration)
                                ? cJSON_GetArraySize(enumeration) : 0;
    int merge_count = cJSON_IsArray(merge) ? cJSON_GetArraySize(merge) : 0;
    if (enumeration_count + merge_count == 0) {
        jw_cat__clear_diagnostics(catalog_dir);
        return;
    }
    cJSON *document = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateArray();
    if (!document || !entries || !cJSON_AddNumberToObject(document, "schema", 1)) {
        cJSON_Delete(document);
        cJSON_Delete(entries);
        return;
    }
    cJSON_AddItemToObject(document, "entries", entries);
    const cJSON *row = NULL;
    cJSON_ArrayForEach(row, enumeration) {
        cJSON_AddItemToArray(entries, cJSON_Duplicate(row, true));
    }
    cJSON_ArrayForEach(row, merge) {
        cJSON_AddItemToArray(entries, cJSON_Duplicate(row, true));
    }
    size_t size = 0;
    char *canonical = jw_catalog_json_canonical(document, &size);
    cJSON_Delete(document);
    char path[PATH_MAX];
    if (canonical &&
        jw_cat__join(path, sizeof(path), catalog_dir, "diagnostics.json") == 0) {
        (void)jw_cat__write_file(path, canonical, size);
    }
    free(canonical);
}

/* --------------------------------------------------------------------------
   Compile and publish
   -------------------------------------------------------------------------- */

/* A base-content change and a release change are both "recompile", but they
   are different stories in a log: one is an OTA, the other is somebody
   dragging a ZIP over the payload. base.info_sha256 exists so the second
   one is still caught when release_id did not move. */
static const char *jw_cat__classify_mismatch(const char *catalog_dir,
                                             const char *published,
                                             const jw_cat_stamp *fresh) {
    char stamp_path[PATH_MAX];
    char gen_dir[PATH_MAX];
    if (jw_cat__join(gen_dir, sizeof(gen_dir), catalog_dir, published) != 0 ||
        jw_cat__join(stamp_path, sizeof(stamp_path), gen_dir, "stamp.json") != 0) {
        return "stamp-base-mismatch";
    }
    size_t len = 0;
    char *text = jw_cat__read_file(stamp_path, &len);
    if (!text) {
        return "stamp-missing";
    }
    cJSON *stamp = cJSON_Parse(text);
    free(text);
    if (!stamp) {
        return "stamp-missing";
    }
    const char *result = "stamp-base-mismatch";
    if (!jw_cat__stamp_field_equals(stamp, "release_id", fresh->release_id)) {
        result = "stamp-release-mismatch";
    } else if (!jw_cat__stamp_field_equals(stamp, "platform", fresh->platform)) {
        result = "stamp-platform-mismatch";
    } else {
        const cJSON *base = cJSON_GetObjectItemCaseSensitive(stamp, "base");
        bool base_matches = cJSON_IsObject(base) &&
            jw_cat__stamp_field_equals(base, "systems_sha256",
                                       fresh->base.systems_sha256) &&
            jw_cat__stamp_field_equals(base, "cores_sha256",
                                       fresh->base.cores_sha256) &&
            jw_cat__stamp_field_equals(base, "info_sha256",
                                       fresh->base.info_sha256);
        if (base_matches) {
            const cJSON *contributors =
                cJSON_GetObjectItemCaseSensitive(stamp, "contributors");
            bool contributor_matches = cJSON_IsArray(contributors) &&
                cJSON_GetArraySize(contributors) == (int)fresh->contributor_count;
            for (size_t i = 0; contributor_matches &&
                               i < fresh->contributor_count; i++) {
                const jw_cat_contributor_stamp *want = &fresh->contributors[i];
                const cJSON *have = cJSON_GetArrayItem(contributors, (int)i);
                const cJSON *files = cJSON_GetObjectItemCaseSensitive(have, "files");
                contributor_matches =
                    jw_cat__stamp_field_equals(have, "provider", want->provider) &&
                    jw_cat__stamp_field_equals(have, "source_id", want->source_id) &&
                    jw_cat__stamp_field_equals(have, "pak_version", want->pak_version) &&
                    jw_cat__stamp_field_equals(have, "provides_sha256",
                                               want->provides_sha256) &&
                    cJSON_IsArray(files) &&
                    cJSON_GetArraySize(files) == (int)want->file_count;
                for (size_t f = 0; contributor_matches && f < want->file_count; f++) {
                    const cJSON *file = cJSON_GetArrayItem(files, (int)f);
                    contributor_matches =
                        jw_cat__stamp_field_equals(file, "rel", want->files[f].rel) &&
                        jw_cat__stamp_field_equals(file, "sha256",
                                                   want->files[f].sha256);
                }
            }
            if (!contributor_matches) {
                result = "stamp-contributor-mismatch";
            }
        }
    }
    cJSON_Delete(stamp);
    return result;
}


static int jw_cat__copy_tree(const char *src, const char *dst) {
    if (jw_cat__mkdir_p(dst) != 0) {
        return -1;
    }
    if (!jw_cat__is_dir(src)) {
        return 0; /* nothing to materialize; hashes as empty */
    }
    jw_cat_path_list paths = {0};
    if (jw_cat__collect(src, "", &paths, 0) != 0) {
        jw_cat__paths_free(&paths);
        return -1;
    }
    int rc = 0;
    for (size_t i = 0; i < paths.count && rc == 0; i++) {
        char src_path[PATH_MAX];
        char dst_path[PATH_MAX];
        if (jw_cat__join(src_path, sizeof(src_path), src, paths.items[i]) != 0 ||
            jw_cat__join(dst_path, sizeof(dst_path), dst, paths.items[i]) != 0) {
            rc = -1;
            break;
        }
        char *slash = strrchr(dst_path, '/');
        if (slash) {
            *slash = '\0';
            if (jw_cat__mkdir_p(dst_path) != 0) {
                rc = -1;
                break;
            }
            *slash = '/';
        }
        rc = jw_cat__copy_file(src_path, dst_path);
    }
    jw_cat__paths_free(&paths);
    return rc;
}

static const char *jw_cat__json_text(const cJSON *object, const char *key);

static const cJSON *jw_cat__contributor_for_provider(const cJSON *contributors,
                                                     const char *provider) {
    const cJSON *row = NULL;
    cJSON_ArrayForEach(row, contributors) {
        if (strcmp(jw_cat__json_text(row, "provider"), provider) == 0) {
            return row;
        }
    }
    return NULL;
}

/* RetroArch accepts one info directory. Start with the release tree, then
   add only .info files belonging to cores that survived the namespace merge.
   The merge already owns basename collision handling. */
static int jw_cat__materialize_info(const char *release_info,
                                    const cJSON *contributors,
                                    const char *cores_output,
                                    const char *dst) {
    if (jw_cat__copy_tree(release_info, dst) != 0) {
        return -1;
    }
    cJSON *doc = cores_output ? cJSON_Parse(cores_output) : NULL;
    const cJSON *cores = doc ? cJSON_GetObjectItemCaseSensitive(doc, "cores") : NULL;
    if (!cJSON_IsArray(cores)) {
        cJSON_Delete(doc);
        return -1;
    }
    const cJSON *core = NULL;
    int rc = 0;
    cJSON_ArrayForEach(core, cores) {
        const char *provider = jw_cat__json_text(core, "provider");
        const char *info_name = jw_cat__json_text(core, "info_name");
        if (!provider[0] || !info_name[0] ||
            strcmp(jw_cat__json_text(core, "type"), "retroarch") != 0) {
            continue;
        }
        const cJSON *contributor =
            jw_cat__contributor_for_provider(contributors, provider);
        const char *pak_dir = jw_cat__json_text(contributor, "pak_dir");
        const char *base = strrchr(info_name, '/');
        base = base ? base + 1 : info_name;
        char src[PATH_MAX];
        char target[PATH_MAX];
        if (!pak_dir[0] || !base[0] ||
            jw_cat__join(src, sizeof(src), pak_dir, info_name) != 0 ||
            jw_cat__join(target, sizeof(target), dst, base) != 0 ||
            jw_cat__copy_file(src, target) != 0) {
            rc = -1;
            break;
        }
    }
    cJSON_Delete(doc);
    return rc;
}

static int jw_cat__merged_info_hash(const char *catalog_dir,
                                    const char *release_info,
                                    const cJSON *contributors,
                                    const char *cores_output,
                                    char out_hex[65]) {
    char scratch[PATH_MAX];
    int created = 0;
    for (unsigned attempt = 0; attempt < 64u && !created; attempt++) {
        int written = snprintf(scratch, sizeof(scratch), "%s/tmp-info-%ld-%u",
                               catalog_dir, (long)getpid(), attempt);
        if (written < 0 || (size_t)written >= sizeof(scratch)) {
            break;
        }
        if (mkdir(scratch, 0755) == 0) {
            created = 1;
        } else if (errno != EEXIST) {
            break;
        }
    }
    if (!created) {
        return -1;
    }
    int rc = jw_cat__materialize_info(release_info, contributors,
                                      cores_output, scratch);
    if (rc == 0) {
        rc = jw_catalog_tree_sha256(scratch, out_hex);
    }
    if (jw_cat__rmtree(scratch, 0) != 0) {
        rc = -1;
    }
    return rc;
}

static int jw_cat__hash_inputs(const char *systems_path, const char *cores_path,
                               const char *info_dir, jw_cat_hashes *out) {
    char error[128];
    if (jw_sha256_file_hex(systems_path, out->systems_sha256, error, sizeof(error)) != 0 ||
        jw_sha256_file_hex(cores_path, out->cores_sha256, error, sizeof(error)) != 0 ||
        jw_catalog_tree_sha256(info_dir, out->info_sha256) != 0) {
        return -1;
    }
    return 0;
}

static const char *jw_cat__json_text(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : "";
}

static int jw_cat__apply_content_scrape(cJSON *systems,
                                        const cJSON *contributors) {
    const cJSON *contributor = NULL;
    cJSON_ArrayForEach(contributor, contributors) {
        const char *provider = jw_cat__json_text(contributor, "provider");
        const cJSON *block = cJSON_GetObjectItemCaseSensitive(
            contributor, "content_scrape");
        const cJSON *policy = NULL;
        cJSON_ArrayForEach(policy,
                           cJSON_GetObjectItemCaseSensitive(block, "systems")) {
            cJSON *system = NULL;
            cJSON_ArrayForEach(system, systems) {
                if (strcmp(jw_cat__json_text(system, "id"),
                           jw_cat__json_text(policy, "id")) != 0 ||
                    strcmp(jw_cat__json_text(system, "provider"), provider) != 0) {
                    continue;
                }
                cJSON_DeleteItemFromObjectCaseSensitive(
                    system, "screenscraper_name_source");
                cJSON_DeleteItemFromObjectCaseSensitive(
                    system, "screenscraper_lookup_extension");
                if (!cJSON_AddStringToObject(
                        system, "screenscraper_name_source",
                        jw_cat__json_text(policy, "name_source")) ||
                    !cJSON_AddStringToObject(
                        system, "screenscraper_lookup_extension",
                        jw_cat__json_text(policy, "lookup_extension"))) {
                    return -1;
                }
                break;
            }
        }
    }
    return 0;
}

static bool jw_cat__content_scrape_affects(const cJSON *systems,
                                           const cJSON *contributor) {
    const char *provider = jw_cat__json_text(contributor, "provider");
    const cJSON *block = cJSON_GetObjectItemCaseSensitive(
        contributor, "content_scrape");
    const cJSON *policy = NULL;
    cJSON_ArrayForEach(policy,
                       cJSON_GetObjectItemCaseSensitive(block, "systems")) {
        const cJSON *system = NULL;
        cJSON_ArrayForEach(system, systems) {
            if (strcmp(jw_cat__json_text(system, "id"),
                       jw_cat__json_text(policy, "id")) == 0 &&
                strcmp(jw_cat__json_text(system, "provider"), provider) == 0 &&
                strcmp(jw_cat__json_text(system,
                                         "screenscraper_name_source"),
                       jw_cat__json_text(policy, "name_source")) == 0 &&
                strcmp(jw_cat__json_text(system,
                                         "screenscraper_lookup_extension"),
                       jw_cat__json_text(policy, "lookup_extension")) == 0) {
                return true;
            }
        }
    }
    return false;
}

static int jw_cat__file_stamp_cmp(const void *left, const void *right) {
    const jw_cat_file_stamp *a = left;
    const jw_cat_file_stamp *b = right;
    return strcmp(a->rel, b->rel);
}

static int jw_cat__contributor_stamp_cmp(const void *left, const void *right) {
    const jw_cat_contributor_stamp *a = left;
    const jw_cat_contributor_stamp *b = right;
    return strcmp(a->provider, b->provider);
}

static int jw_cat__stamp_add_file(jw_cat_contributor_stamp *stamp,
                                  const char *pak_dir,
                                  const char *rel) {
    for (size_t i = 0; i < stamp->file_count; i++) {
        if (strcmp(stamp->files[i].rel, rel) == 0) {
            return 0;
        }
    }
    jw_cat_file_stamp *grown = realloc(
        stamp->files, (stamp->file_count + 1u) * sizeof(*grown));
    if (!grown) {
        return -1;
    }
    stamp->files = grown;
    jw_cat_file_stamp *file = &stamp->files[stamp->file_count];
    memset(file, 0, sizeof(*file));
    file->rel = strdup(rel);
    char path[PATH_MAX];
    char error[128];
    if (!file->rel || jw_cat__join(path, sizeof(path), pak_dir, rel) != 0 ||
        jw_sha256_file_hex(path, file->sha256, error, sizeof(error)) != 0) {
        free(file->rel);
        file->rel = NULL;
        return -1;
    }
    stamp->file_count++;
    return 0;
}

static int jw_cat__stamp_declared_files(jw_cat_contributor_stamp *stamp,
                                        const char *pak_dir,
                                        const cJSON *provides) {
    const cJSON *system = NULL;
    cJSON_ArrayForEach(system,
                       cJSON_GetObjectItemCaseSensitive(provides, "systems")) {
        const cJSON *flat = cJSON_GetObjectItemCaseSensitive(system, "icon_flat");
        const cJSON *photo = cJSON_GetObjectItemCaseSensitive(system,
                                                               "icon_photographic");
        if (cJSON_IsString(flat) &&
            jw_cat__stamp_add_file(stamp, pak_dir, flat->valuestring) != 0) {
            return -1;
        }
        if (cJSON_IsString(photo) &&
            jw_cat__stamp_add_file(stamp, pak_dir, photo->valuestring) != 0) {
            return -1;
        }
    }
    const cJSON *core = NULL;
    cJSON_ArrayForEach(core,
                       cJSON_GetObjectItemCaseSensitive(provides, "cores")) {
        static const char *const keys[] = {"file_name", "info_name", "path"};
        for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
            const cJSON *path = cJSON_GetObjectItemCaseSensitive(core, keys[i]);
            if (cJSON_IsString(path) &&
                jw_cat__stamp_add_file(stamp, pak_dir, path->valuestring) != 0) {
                return -1;
            }
        }
        /* A LEAF-RA-1 frontend is a declared file too: replacing it must
           invalidate the generation like replacing the core would. */
        const cJSON *frontend = cJSON_GetObjectItemCaseSensitive(core, "frontend");
        const cJSON *frontend_path = cJSON_GetObjectItemCaseSensitive(frontend, "path");
        if (cJSON_IsString(frontend_path) &&
            jw_cat__stamp_add_file(stamp, pak_dir, frontend_path->valuestring) != 0) {
            return -1;
        }
    }
    return 0;
}

static int jw_cat__build_contributor_stamps(jw_cat_stamp *stamp,
                                            const cJSON *contributors,
                                            const char *systems_output) {
    int count = cJSON_IsArray(contributors) ? cJSON_GetArraySize(contributors) : 0;
    if (count <= 0) {
        return 0;
    }
    cJSON *systems_doc = systems_output ? cJSON_Parse(systems_output) : NULL;
    const cJSON *systems = cJSON_GetObjectItemCaseSensitive(systems_doc, "systems");
    if (!cJSON_IsArray(systems)) {
        cJSON_Delete(systems_doc);
        return -1;
    }
    stamp->contributors = calloc((size_t)count, sizeof(*stamp->contributors));
    if (!stamp->contributors) {
        cJSON_Delete(systems_doc);
        return -1;
    }
    const cJSON *row = NULL;
    cJSON_ArrayForEach(row, contributors) {
        jw_cat_contributor_stamp *out =
            &stamp->contributors[stamp->contributor_count];
        stamp->contributor_count++;
        out->provider = strdup(jw_cat__json_text(row, "provider"));
        out->source_id = strdup(jw_cat__json_text(row, "source_id"));
        out->pak_version = strdup(jw_cat__json_text(row, "pak_version"));
        const char *pak_dir = jw_cat__json_text(row, "pak_dir");
        const cJSON *provides = cJSON_GetObjectItemCaseSensitive(row, "provides");
        size_t canonical_size = 0;
        char *canonical = jw_catalog_json_canonical(provides, &canonical_size);
        if (!out->provider || !out->source_id || !out->pak_version ||
            !pak_dir[0] || !canonical) {
            free(canonical);
            cJSON_Delete(systems_doc);
            return -1;
        }
        jw_sha256_buf_hex(canonical, canonical_size, out->provides_sha256);
        free(canonical);
        if (jw_cat__stamp_declared_files(out, pak_dir, provides) != 0 ||
            (jw_cat__content_scrape_affects(systems, row) &&
             jw_cat__stamp_add_file(out, pak_dir, "pak.json") != 0)) {
            cJSON_Delete(systems_doc);
            return -1;
        }
        const cJSON *system = NULL;
        cJSON_ArrayForEach(system, systems) {
            const char *slots[] = {"wordmark", "grid_icon", "wordmark_color"};
            const char *providers[] = {"wordmark_provider", "grid_icon_provider",
                                       "wordmark_color_provider"};
            for (int slot = 0; slot < 3; slot++) {
                if (strcmp(jw_cat__json_text(system, providers[slot]), out->provider)) continue;
                if (jw_cat__stamp_add_file(out, pak_dir, "pak.json") != 0 ||
                    jw_cat__stamp_add_file(out, pak_dir, jw_cat__json_text(system, slots[slot])) != 0) {
                    cJSON_Delete(systems_doc);
                    return -1;
                }
            }
        }
        qsort(out->files, out->file_count, sizeof(*out->files),
              jw_cat__file_stamp_cmp);
    }
    cJSON_Delete(systems_doc);
    qsort(stamp->contributors, stamp->contributor_count,
          sizeof(*stamp->contributors), jw_cat__contributor_stamp_cmp);
    return 0;
}

static int jw_cat__merge_outputs(const char *systems_path,
                                 const char *cores_path,
                                 const cJSON *contributors,
                                 char **out_systems,
                                 size_t *out_systems_size,
                                 char **out_cores,
                                 size_t *out_cores_size,
                                 cJSON **out_merge_diagnostics) {
    size_t systems_input_size = 0, cores_input_size = 0;
    char *systems_input = jw_cat__read_file(systems_path, &systems_input_size);
    char *cores_input = jw_cat__read_file(cores_path, &cores_input_size);
    cJSON *systems_doc = systems_input ? cJSON_Parse(systems_input) : NULL;
    cJSON *cores_doc = cores_input ? cJSON_Parse(cores_input) : NULL;
    free(systems_input);
    free(cores_input);
    if (!cJSON_IsObject(systems_doc) || !cJSON_IsObject(cores_doc)) {
        cJSON_Delete(systems_doc);
        cJSON_Delete(cores_doc);
        return -1;
    }
    const cJSON *systems = cJSON_GetObjectItemCaseSensitive(systems_doc, "systems");
    const cJSON *cores = cJSON_GetObjectItemCaseSensitive(cores_doc, "cores");
    const char *platform = jw_cat__json_text(systems_doc, "platform");
    if (!cJSON_IsArray(systems) || !cJSON_IsArray(cores) || !platform[0]) {
        cJSON_Delete(systems_doc);
        cJSON_Delete(cores_doc);
        return -1;
    }
    cJSON *base = cJSON_CreateObject();
    cJSON *merged = NULL;
    cJSON *diagnostics = NULL;
    if (!base || !cJSON_AddStringToObject(base, "platform", platform)) {
        cJSON_Delete(base);
        cJSON_Delete(systems_doc);
        cJSON_Delete(cores_doc);
        return -1;
    }
    cJSON_AddItemToObject(base, "systems", cJSON_Duplicate(systems, true));
    cJSON_AddItemToObject(base, "cores", cJSON_Duplicate(cores, true));
    if (jw_catalog_merge(base, contributors, &merged, &diagnostics) != 0) {
        cJSON_Delete(base);
        cJSON_Delete(systems_doc);
        cJSON_Delete(cores_doc);
        return -1;
    }
    cJSON_Delete(base);
    const cJSON *diagnostic = NULL;
    cJSON_ArrayForEach(diagnostic, diagnostics) {
        fprintf(stderr, "Content catalog: provider=%s reason=%s detail=%s\n",
                jw_cat__json_text(diagnostic, "provider"),
                jw_cat__json_text(diagnostic, "reason"),
                jw_cat__json_text(diagnostic, "detail"));
    }
    cJSON *merged_systems = cJSON_Duplicate(
        cJSON_GetObjectItemCaseSensitive(merged, "systems"), true);
    cJSON *merged_cores = cJSON_Duplicate(
        cJSON_GetObjectItemCaseSensitive(merged, "cores"), true);
    cJSON_Delete(merged);
    if (!merged_systems || !merged_cores) {
        cJSON_Delete(merged_systems);
        cJSON_Delete(merged_cores);
        cJSON_Delete(diagnostics);
        cJSON_Delete(systems_doc);
        cJSON_Delete(cores_doc);
        return -1;
    }
    if (jw_cat__apply_content_scrape(merged_systems, contributors) != 0 ||
        jw_catalog_apply_content_art(merged_systems, merged_cores,
                                      contributors, diagnostics) != 0) {
        cJSON_Delete(merged_systems);
        cJSON_Delete(merged_cores);
        cJSON_Delete(diagnostics);
        cJSON_Delete(systems_doc);
        cJSON_Delete(cores_doc);
        return -1;
    }
    if (!cJSON_ReplaceItemInObjectCaseSensitive(systems_doc, "systems",
                                                merged_systems)) {
        cJSON_Delete(merged_systems);
        cJSON_Delete(merged_cores);
        cJSON_Delete(diagnostics);
        cJSON_Delete(systems_doc);
        cJSON_Delete(cores_doc);
        return -1;
    }
    if (!cJSON_ReplaceItemInObjectCaseSensitive(cores_doc, "cores", merged_cores)) {
        cJSON_Delete(merged_cores);
        cJSON_Delete(diagnostics);
        cJSON_Delete(systems_doc);
        cJSON_Delete(cores_doc);
        return -1;
    }
    *out_systems = jw_catalog_json_canonical(systems_doc, out_systems_size);
    *out_cores = jw_catalog_json_canonical(cores_doc, out_cores_size);
    cJSON_Delete(systems_doc);
    cJSON_Delete(cores_doc);
    if (!*out_systems || !*out_cores) {
        free(*out_systems);
        free(*out_cores);
        cJSON_Delete(diagnostics);
        return -1;
    }
    *out_merge_diagnostics = diagnostics;
    return 0;
}

int jw_catalog_refresh_with_contributors(const char *sdcard_root,
                                         const char *release_defaults_dir,
                                         const cJSON *contributors,
                                         const cJSON *diagnostics,
                                         char *generation,
                                         size_t generation_size,
                                         char *reason,
                                         size_t reason_size) {
    if (generation && generation_size > 0) {
        generation[0] = '\0';
    }
    jw_cat__set(reason, reason_size, "");

    char catalog_dir[PATH_MAX];
    if (jw_catalog_dir(sdcard_root, catalog_dir, sizeof(catalog_dir)) != 0 ||
        jw_cat__mkdir_p(catalog_dir) != 0) {
        jw_cat__set(reason, reason_size, "compile-failed");
        return -1;
    }

    char release_id[JW_LEAF_RELEASE_ID_MAX];
    if (jw_cat__release_id(sdcard_root, release_id, sizeof(release_id)) != 0) {
        jw_cat__invalidate_selector(catalog_dir);
        jw_cat__write_diagnostics(catalog_dir, "release-identity-unavailable",
                                  "release.json is missing, empty, or invalid; "
                                  "running on release defaults rather than "
                                  "guessing an identity");
        jw_cat__set(reason, reason_size, "release-identity-unavailable");
        return 1;
    }

    /* Release inputs. `info` is a sibling of `defaults`, not a child: the
       defaults directory also holds arcade_names.txt, retroarch.cfg,
       retroarch-record.cfg, and pulse-default.pa, all of which stay
       release-owned and are never copied into a generation. */
    char systems_path[PATH_MAX];
    char cores_path[PATH_MAX];
    char info_dir[PATH_MAX];
    char platform_dir[PATH_MAX];
    if (snprintf(platform_dir, sizeof(platform_dir), "%s", release_defaults_dir) >=
        (int)sizeof(platform_dir)) {
        jw_cat__record_failure_at(catalog_dir, "compile-failed",
                                  "release defaults path is too long");
        jw_cat__set(reason, reason_size, "compile-failed");
        return -1;
    }
    char *slash = strrchr(platform_dir, '/');
    if (slash) {
        *slash = '\0';
    }
    if (jw_cat__join(systems_path, sizeof(systems_path), release_defaults_dir,
                     "systems.json") != 0 ||
        jw_cat__join(cores_path, sizeof(cores_path), release_defaults_dir,
                     "cores.json") != 0 ||
        jw_cat__join(info_dir, sizeof(info_dir), platform_dir, "info") != 0) {
        jw_cat__record_failure_at(catalog_dir, "compile-failed",
                                  "release catalog path is too long");
        jw_cat__set(reason, reason_size, "compile-failed");
        return -1;
    }

    /* The v2 fallback lives in catalog.c, which owns knowing which release
       cores file wins. By the time a generation exists the question is
       settled: a generation always holds exactly one cores.json. */
    char cores_v2_path[PATH_MAX];
    if (jw_cat__join(cores_v2_path, sizeof(cores_v2_path), release_defaults_dir,
                     "cores.v2.json") == 0 &&
        !jw_cat__is_file(cores_path) && jw_cat__is_file(cores_v2_path)) {
        snprintf(cores_path, sizeof(cores_path), "%s", cores_v2_path);
    }
    if (!jw_cat__is_file(systems_path) || !jw_cat__is_file(cores_path)) {
        jw_cat__invalidate_selector(catalog_dir);
        jw_cat__write_diagnostics(catalog_dir, "compile-failed",
                                  "release systems.json or cores.json is missing");
        jw_cat__set(reason, reason_size, "compile-failed");
        return 1;
    }

    jw_cat_stamp stamp;
    memset(&stamp, 0, sizeof(stamp));
    snprintf(stamp.platform, sizeof(stamp.platform), "%s", jw_platform_compiled_id());
    snprintf(stamp.release_id, sizeof(stamp.release_id), "%s", release_id);
    if (jw_cat__hash_inputs(systems_path, cores_path, info_dir, &stamp.base) != 0) {
        jw_cat__invalidate_selector(catalog_dir);
        jw_cat__write_diagnostics(catalog_dir, "compile-failed",
                                  "could not hash the release catalog inputs");
        jw_cat__set(reason, reason_size, "compile-failed");
        return 1;
    }
    /* No contributors intentionally remains a byte-for-byte copy. Besides
       preserving zero-behavior-change, uninstall then reuses the original
       Phase-1 generation instead of manufacturing a canonically reformatted
       copy of the release. */
    stamp.output = stamp.base;

    char *systems_output = NULL;
    char *cores_output = NULL;
    size_t systems_output_size = 0;
    size_t cores_output_size = 0;
    cJSON *merge_diagnostics = NULL;
    int contributor_count = cJSON_IsArray(contributors)
                                ? cJSON_GetArraySize(contributors) : 0;
    if (contributor_count > 0) {
        if (jw_cat__merge_outputs(systems_path, cores_path, contributors,
                                  &systems_output, &systems_output_size,
                                  &cores_output, &cores_output_size,
                                  &merge_diagnostics) != 0) {
            jw_cat__invalidate_selector(catalog_dir);
            jw_cat__write_diagnostics(catalog_dir, "compile-failed",
                                      "could not merge the validated content contributors");
            jw_cat__set(reason, reason_size, "compile-failed");
            return 1;
        }
        jw_sha256_buf_hex(systems_output, systems_output_size,
                          stamp.output.systems_sha256);
        jw_sha256_buf_hex(cores_output, cores_output_size,
                          stamp.output.cores_sha256);
        if (jw_cat__merged_info_hash(catalog_dir, info_dir, contributors,
                                     cores_output,
                                     stamp.output.info_sha256) != 0) {
            free(systems_output);
            free(cores_output);
            cJSON_Delete(merge_diagnostics);
            jw_cat__invalidate_selector(catalog_dir);
            jw_cat__write_diagnostics(catalog_dir, "compile-failed",
                                      "could not materialize merged core info");
            jw_cat__set(reason, reason_size, "compile-failed");
            return 1;
        }
    }
    if (jw_cat__build_contributor_stamps(&stamp, contributors,
                                         systems_output) != 0) {
        free(systems_output);
        free(cores_output);
        cJSON_Delete(merge_diagnostics);
        jw_cat__stamp_free(&stamp);
        jw_cat__invalidate_selector(catalog_dir);
        jw_cat__write_diagnostics(catalog_dir, "compile-failed",
                                  "could not fingerprint a declared contributor file");
        jw_cat__set(reason, reason_size, "compile-failed");
        return 1;
    }

    size_t canonical_len = 0;
    char *canonical = jw_cat__stamp_canonical(&stamp, &canonical_len);
    if (!canonical) {
        free(systems_output);
        free(cores_output);
        cJSON_Delete(merge_diagnostics);
        jw_cat__stamp_free(&stamp);
        jw_cat__record_failure_at(catalog_dir, "compile-failed",
                                  "could not serialize the generation stamp");
        jw_cat__set(reason, reason_size, "compile-failed");
        return -1;
    }
    char digest[65];
    jw_sha256_buf_hex(canonical, canonical_len, digest);

    char gen_name[JW_CAT_GEN_NAME_MAX];
    snprintf(gen_name, sizeof(gen_name), "gen-%s", digest);
    char gen_dir[PATH_MAX];
    if (jw_cat__join(gen_dir, sizeof(gen_dir), catalog_dir, gen_name) != 0) {
        free(canonical);
        free(systems_output);
        free(cores_output);
        cJSON_Delete(merge_diagnostics);
        jw_cat__stamp_free(&stamp);
        jw_cat__record_failure_at(catalog_dir, "compile-failed",
                                  "generation path is too long");
        jw_cat__set(reason, reason_size, "compile-failed");
        return -1;
    }

    /* Full provenance validation, expressed as content addressing: the stamp
       fingerprints every input, so "the freshly computed name differs from
       the published one" IS the mismatch. When it does, unlink the selector
       and fsync BEFORE doing any compile work -- during the gap every new
       reader falls back to release defaults, and a crash mid-compile leaves
       no selector rather than one pointing at unverified output. */
    char published[JW_CAT_GEN_NAME_MAX];
    if (jw_cat__read_selector(catalog_dir, published, sizeof(published)) == 0 &&
        strcmp(published, gen_name) != 0) {
        const char *mismatch = jw_cat__classify_mismatch(catalog_dir, published, &stamp);
        jw_cat__invalidate_selector(catalog_dir);
        jw_cat__write_diagnostics(catalog_dir, mismatch,
                                  "the published generation no longer matches the "
                                  "release catalog inputs; recompiling");
    }

    if (jw_cat__is_dir(gen_dir)) {
        /* Content addressing means an identical recompile lands on the same
           name. Verify before reusing: the same name holding different bytes
           is corruption, and the answer to corruption is to fail closed, not
           to overwrite a directory a reader may be inside. */
        char existing_stamp[PATH_MAX];
        size_t existing_len = 0;
        char *existing = NULL;
        if (jw_cat__join(existing_stamp, sizeof(existing_stamp), gen_dir,
                         "stamp.json") == 0) {
            existing = jw_cat__read_file(existing_stamp, &existing_len);
        }
        int identical = existing && existing_len == canonical_len &&
                        memcmp(existing, canonical, canonical_len) == 0;
        free(existing);

        char existing_systems[PATH_MAX];
        char existing_cores[PATH_MAX];
        char existing_info[PATH_MAX];
        if (identical &&
            (jw_cat__join(existing_systems, sizeof(existing_systems), gen_dir,
                          "systems.json") != 0 ||
             jw_cat__join(existing_cores, sizeof(existing_cores), gen_dir,
                          "cores.json") != 0 ||
             jw_cat__join(existing_info, sizeof(existing_info), gen_dir, "info") != 0 ||
             !jw_cat__is_file(existing_systems) || !jw_cat__is_file(existing_cores) ||
             !jw_cat__is_dir(existing_info))) {
            identical = 0;
        }

        free(canonical);
        free(systems_output);
        free(cores_output);
        if (!identical) {
            cJSON_Delete(merge_diagnostics);
            jw_cat__stamp_free(&stamp);
            jw_cat__invalidate_selector(catalog_dir);
            jw_cat__write_diagnostics(catalog_dir, "generation-digest-conflict",
                                      "an existing generation directory holds "
                                      "different bytes than the compile that "
                                      "produced its name; refusing to overwrite it");
            jw_cat__set(reason, reason_size, "generation-digest-conflict");
            return 1;
        }
        if (jw_cat__write_selector(catalog_dir, gen_name) != 0) {
            cJSON_Delete(merge_diagnostics);
            jw_cat__stamp_free(&stamp);
            jw_cat__record_failure_at(catalog_dir, "compile-failed",
                                      "could not publish the generation selector");
            jw_cat__set(reason, reason_size, "compile-failed");
            return -1;
        }
        jw_cat__write_entry_diagnostics(catalog_dir, diagnostics, merge_diagnostics);
        cJSON_Delete(merge_diagnostics);
        jw_cat__stamp_free(&stamp);
        jw_cat__set(generation, generation_size, gen_name);
        jw_cat__set(reason, reason_size, "reused");
        return 0;
    }

    /* mkdir(2) is the exclusive primitive here, not mkdtemp: mkdtemp needs a
       feature-test macro to be visible under -std=c11 on the device
       toolchain, and mkdir already fails EEXIST atomically. */
    char tmp_dir[PATH_MAX];
    int created = 0;
    for (unsigned attempt = 0; attempt < 64u && !created; attempt++) {
        int written = snprintf(tmp_dir, sizeof(tmp_dir), "%s/tmp-%ld-%u",
                               catalog_dir, (long)getpid(), attempt);
        if (written < 0 || (size_t)written >= sizeof(tmp_dir)) {
            break;
        }
        if (mkdir(tmp_dir, 0755) == 0) {
            created = 1;
        } else if (errno != EEXIST) {
            break;
        }
    }
    if (!created) {
        free(canonical);
        free(systems_output);
        free(cores_output);
        cJSON_Delete(merge_diagnostics);
        jw_cat__stamp_free(&stamp);
        jw_cat__record_failure_at(catalog_dir, "compile-failed",
                                  "could not create a temporary generation directory");
        jw_cat__set(reason, reason_size, "compile-failed");
        return -1;
    }

    char tmp_systems[PATH_MAX];
    char tmp_cores[PATH_MAX];
    char tmp_info[PATH_MAX];
    char tmp_stamp[PATH_MAX];
    int rc = 0;
    if (jw_cat__join(tmp_systems, sizeof(tmp_systems), tmp_dir, "systems.json") != 0 ||
        jw_cat__join(tmp_cores, sizeof(tmp_cores), tmp_dir, "cores.json") != 0 ||
        jw_cat__join(tmp_info, sizeof(tmp_info), tmp_dir, "info") != 0 ||
        jw_cat__join(tmp_stamp, sizeof(tmp_stamp), tmp_dir, "stamp.json") != 0) {
        rc = -1;
    }
    if (rc == 0) {
        rc = systems_output
                 ? jw_cat__write_file(tmp_systems, systems_output, systems_output_size)
                 : jw_cat__copy_file(systems_path, tmp_systems);
    }
    if (rc == 0) {
        rc = cores_output
                 ? jw_cat__write_file(tmp_cores, cores_output, cores_output_size)
                 : jw_cat__copy_file(cores_path, tmp_cores);
    }
    if (rc == 0) {
        rc = contributor_count > 0
                 ? jw_cat__materialize_info(info_dir, contributors,
                                            cores_output, tmp_info)
                 : jw_cat__copy_tree(info_dir, tmp_info);
    }
    if (rc == 0) {
        rc = jw_cat__write_file(tmp_stamp, canonical, canonical_len);
    }
    free(canonical);
    free(systems_output);
    free(cores_output);

    if (rc == 0) {
        rc = jw_cat__fsync_dir(tmp_dir);
    }
    if (rc != 0) {
        cJSON_Delete(merge_diagnostics);
        jw_cat__stamp_free(&stamp);
        jw_cat__rmtree(tmp_dir, 0);
        jw_cat__invalidate_selector(catalog_dir);
        jw_cat__write_diagnostics(catalog_dir, "compile-failed",
                                  "could not materialize the generation directory");
        jw_cat__set(reason, reason_size, "compile-failed");
        return 1;
    }

    if (rename(tmp_dir, gen_dir) != 0) {
        /* Lost a race with another producer that published the same
           content-addressed name. That is the reuse case, not a failure --
           but only if what landed there is actually identical, which the
           next refresh verifies. */
        jw_cat__rmtree(tmp_dir, 0);
        if (!jw_cat__is_dir(gen_dir)) {
            cJSON_Delete(merge_diagnostics);
            jw_cat__stamp_free(&stamp);
            jw_cat__invalidate_selector(catalog_dir);
            jw_cat__write_diagnostics(catalog_dir, "compile-failed",
                                      "could not publish the generation directory");
            jw_cat__set(reason, reason_size, "compile-failed");
            return 1;
        }
    }
    (void)jw_cat__fsync_dir(catalog_dir);
    if (jw_cat__write_selector(catalog_dir, gen_name) != 0) {
        cJSON_Delete(merge_diagnostics);
        jw_cat__stamp_free(&stamp);
        jw_cat__record_failure_at(catalog_dir, "compile-failed",
                                  "could not publish the generation selector");
        jw_cat__set(reason, reason_size, "compile-failed");
        return -1;
    }

    jw_cat__write_entry_diagnostics(catalog_dir, diagnostics, merge_diagnostics);
    cJSON_Delete(merge_diagnostics);
    jw_cat__stamp_free(&stamp);
    jw_cat__set(generation, generation_size, gen_name);
    jw_cat__set(reason, reason_size, "published");
    return 0;
}

int jw_catalog_refresh(const char *sdcard_root,
                       const char *release_defaults_dir,
                       char *generation,
                       size_t generation_size,
                       char *reason,
                       size_t reason_size) {
    return jw_catalog_refresh_with_contributors(
        sdcard_root, release_defaults_dir, NULL, NULL,
        generation, generation_size, reason, reason_size);
}

/* --------------------------------------------------------------------------
   Temp cleanup
   -------------------------------------------------------------------------- */

int jw_catalog_cleanup_temps(const char *sdcard_root, size_t *out_removed) {
    if (out_removed) {
        *out_removed = 0;
    }
    char catalog_dir[PATH_MAX];
    if (jw_catalog_dir(sdcard_root, catalog_dir, sizeof(catalog_dir)) != 0) {
        return -1;
    }
    DIR *dir = opendir(catalog_dir);
    if (!dir) {
        return 0;
    }
    struct dirent *entry;
    size_t removed = 0;
    while ((entry = readdir(dir)) != NULL) {
        /* Only ever tmp-*. A finalized generation is never pruned in v1: a
           CentralScrutinizer process can outlive a jawakad crash, so even
           daemon startup is not a reader-free window. */
        if (strncmp(entry->d_name, "tmp-", 4u) != 0) {
            continue;
        }
        char path[PATH_MAX];
        if (jw_cat__join(path, sizeof(path), catalog_dir, entry->d_name) != 0) {
            continue;
        }
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        if (jw_cat__rmtree(path, 0) == 0) {
            removed++;
        }
    }
    closedir(dir);
    if (out_removed) {
        *out_removed = removed;
    }
    return 0;
}
