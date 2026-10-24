#include "internal/discovery/content.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

/* One game's files. Names stay high because recovering a reference's on-disk
   spelling lists its whole directory, which can be a large system folder. */
#define JW_CONTENT_MAX_FILES 4096
#define JW_CONTENT_MAX_BYTES (16 * 1024 * 1024)
#define JW_CONTENT_MAX_DEPTH 32
#define JW_CONTENT_MAX_DIRS 4096
#define JW_CONTENT_MAX_NAMES 262144
#define JW_CONTENT_RELPATH_MAX 512
#define JW_CONTENT_NAME_INDEX_MIN 32

/* Open-addressed string index over an array the caller owns. A slot holds
   item + 1 (0 is empty); the table stays at most half full. */
typedef struct {
    size_t *slots;
    size_t size;
} jw_content_index;

typedef const char *(*jw_content_key)(const void *items, size_t item);

typedef struct {
    char *path;
    char **names;
    size_t count;
    size_t capacity;
    jw_content_index index;
} jw_content_dir;

typedef struct {
    const jw_storage_source_list *sources;
    const jw_ra_system *system;
    jw_content *out;
    char *error;
    size_t error_size;
    jw_content_dir **dirs;
    size_t dir_count;
    size_t dir_capacity;
    jw_content_index dir_index;
    size_t name_count;
    size_t bytes_read;
    unsigned char *state;
    size_t file_capacity;
    size_t reference_capacity;
    jw_content_index file_index;
    size_t parents[JW_CONTENT_MAX_DEPTH + 1];
    bool located;
    char roots[JW_STORAGE_MAX_SOURCES][JW_STORAGE_PATH_MAX];
    struct stat root_stats[JW_STORAGE_MAX_SOURCES];
} jw_content_reader;

static int jw__fail(jw_content_reader *r, const char *format, ...) {
    r->located = false;
    if (r->error && r->error_size) {
        va_list args;
        va_start(args, format);
        vsnprintf(r->error, r->error_size, format, args);
        va_end(args);
    }
    return -1;
}

/* Name the innermost descriptor that failed, once, so a malformed file can be
   found. */
static int jw__locate(jw_content_reader *r, const char *path) {
    if (r->located || !r->error || !r->error_size || strstr(r->error, path)) return -1;
    r->located = true;
    size_t used = strlen(r->error);
    if (used + 1 < r->error_size)
        snprintf(r->error + used, r->error_size - used, " (in %.200s)", path);
    return -1;
}

static int jw__copy(jw_content_reader *r, char *out, size_t size, const char *text) {
    if (strlen(text) >= size) return jw__fail(r, "Content path is too long: %.100s", text);
    memcpy(out, text, strlen(text) + 1);
    return 0;
}

static int jw__join(jw_content_reader *r, char *out, size_t size,
                     const char *parent, const char *name) {
    int n = snprintf(out, size, "%s%s%s", parent,
                     strcmp(parent, "/") == 0 ? "" : "/", name);
    return n < 0 || (size_t)n >= size ? jw__fail(r, "Content path is too long") : 0;
}

static const char *jw__extension(const char *path) {
    const char *base = strrchr(path, '/');
    const char *dot = strrchr(base ? base + 1 : path, '.');
    return dot ? dot + 1 : "";
}

static int jw__descriptor_kind(const char *path) {
    const char *ext = jw__extension(path);
    if (!strcasecmp(ext, "m3u") || !strcasecmp(ext, "m3u8")) return 1;
    if (!strcasecmp(ext, "cue")) return 2;
    if (!strcasecmp(ext, "gdi")) return 3;
    if (!strcasecmp(ext, "toc")) return 4;
    if (!strcasecmp(ext, "cmd")) return 5;
    return 0;
}

bool jw_content_is_descriptor_path(const char *path) {
    return path && jw__descriptor_kind(path) != 0;
}

/* Geometric growth; on failure the caller keeps the old block. */
static void *jw__grow(void *items, size_t *capacity, size_t need, size_t size) {
    if (need <= *capacity) return items;
    size_t grown = *capacity ? *capacity * 2 : 16;
    while (grown < need) grown *= 2;
    void *next = realloc(items, grown * size);
    if (next) *capacity = grown;
    return next;
}

static size_t jw__hash(const char *text) {
    uint64_t hash = 14695981039346656037ULL;
    for (; *text; text++) hash = (hash ^ (unsigned char)*text) * 1099511628211ULL;
    return (size_t)hash;
}

static size_t jw__index_find(const jw_content_index *index, const void *items,
                             jw_content_key key, const char *text) {
    if (!index->size) return SIZE_MAX;
    size_t mask = index->size - 1;
    for (size_t slot = jw__hash(text) & mask; index->slots[slot]; slot = (slot + 1) & mask)
        if (!strcmp(key(items, index->slots[slot] - 1), text)) return index->slots[slot] - 1;
    return SIZE_MAX;
}

/* Indexes items[item], whose key is text; items [0, count) are already indexed. */
static int jw__index_put(jw_content_index *index, const void *items, jw_content_key key,
                         size_t count, const char *text, size_t item) {
    if ((count + 1) * 2 > index->size) {
        size_t size = index->size ? index->size * 2 : 64;
        while ((count + 1) * 2 > size) size *= 2;
        size_t *slots = calloc(size, sizeof(*slots));
        if (!slots) return -1;
        for (size_t i = 0; i < count; i++) {
            size_t slot = jw__hash(key(items, i)) & (size - 1);
            while (slots[slot]) slot = (slot + 1) & (size - 1);
            slots[slot] = i + 1;
        }
        free(index->slots);
        index->slots = slots;
        index->size = size;
    }
    size_t slot = jw__hash(text) & (index->size - 1);
    while (index->slots[slot]) slot = (slot + 1) & (index->size - 1);
    index->slots[slot] = item + 1;
    return 0;
}

static const char *jw__dir_key(const void *items, size_t item) {
    return ((jw_content_dir *const *)items)[item]->path;
}

static const char *jw__name_key(const void *items, size_t item) {
    return ((char *const *)items)[item];
}

static const char *jw__file_key(const void *items, size_t item) {
    return ((const jw_content_file *)items)[item].path;
}

static void jw__free_dir(jw_content_dir *dir) {
    if (!dir) return;
    for (size_t i = 0; i < dir->count; i++) free(dir->names[i]);
    free(dir->names);
    free(dir->index.slots);
    free(dir->path);
    free(dir);
}

static int jw__directory(jw_content_reader *r, const char *path, jw_content_dir **out) {
    size_t found = jw__index_find(&r->dir_index, r->dirs, jw__dir_key, path);
    if (found != SIZE_MAX) { *out = r->dirs[found]; return 0; }
    if (r->dir_count == JW_CONTENT_MAX_DIRS)
        return jw__fail(r, "Content uses too many directories");
    DIR *dir = opendir(path);
    if (!dir) return jw__fail(r, "Cannot read directory %.160s: %s", path, strerror(errno));
    jw_content_dir *cached = calloc(1, sizeof(*cached));
    jw_content_dir **dirs = cached ? jw__grow(r->dirs, &r->dir_capacity, r->dir_count + 1, sizeof(*dirs)) : NULL;
    if (dirs) r->dirs = dirs;
    if (cached) cached->path = strdup(path);
    if (!dirs || !cached->path ||
        jw__index_put(&r->dir_index, r->dirs, jw__dir_key, r->dir_count, path, r->dir_count) < 0) {
        jw__free_dir(cached);
        closedir(dir);
        return jw__fail(r, "Out of memory inspecting content");
    }
    r->dirs[r->dir_count++] = cached;
    int saved = 0;
    struct dirent *entry;
    errno = 0;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (r->name_count == JW_CONTENT_MAX_NAMES) { saved = E2BIG; break; }
        char **names = jw__grow(cached->names, &cached->capacity, cached->count + 1, sizeof(*names));
        if (!names) { saved = ENOMEM; break; }
        cached->names = names;
        names[cached->count] = strdup(entry->d_name);
        if (!names[cached->count]) { saved = ENOMEM; break; }
        cached->count++;
        r->name_count++;
        errno = 0;
    }
    if (!saved) saved = errno;
    if (closedir(dir) && !saved) saved = errno;
    /* Big directories get a name index so each lookup is not a scan. */
    for (size_t i = 0; !saved && cached->count >= JW_CONTENT_NAME_INDEX_MIN && i < cached->count; i++)
        if (jw__index_put(&cached->index, cached->names, jw__name_key, i, cached->names[i], i) < 0) saved = ENOMEM;
    if (saved) return jw__fail(r, "Cannot read directory %.160s: %s", path, strerror(saved));
    *out = cached;
    return 0;
}

static const char *jw__find_name(const jw_content_dir *dir, const char *name) {
    if (dir->index.size) {
        size_t found = jw__index_find(&dir->index, dir->names, jw__name_key, name);
        return found == SIZE_MAX ? NULL : dir->names[found];
    }
    for (size_t i = 0; i < dir->count; i++)
        if (!strcmp(dir->names[i], name)) return dir->names[i];
    return NULL;
}

/* Lookup first lets the host filesystem decide case equivalence. Inodes only
   recover the spelling of that lookup, never a persistent content identity. */
static int jw__actual_spelling(jw_content_reader *r, const char *path,
                               char *out, size_t out_size) {
    char parts[JW_STORAGE_PATH_MAX];
    if (jw__copy(r, parts, sizeof(parts), path) < 0 ||
        jw__copy(r, out, out_size, "/") < 0) return -1;
    char *save = NULL;
    for (char *part = strtok_r(parts, "/", &save); part; part = strtok_r(NULL, "/", &save)) {
        jw_content_dir *dir = NULL;
        if (jw__directory(r, out, &dir) < 0) return -1;
        char candidate[JW_STORAGE_PATH_MAX];
        if (jw__join(r, candidate, sizeof(candidate), out, part) < 0) return -1;
        struct stat requested;
        if (lstat(candidate, &requested))
            return jw__fail(r, "Cannot inspect %.160s: %s", candidate, strerror(errno));
        const char *name = jw__find_name(dir, part);
        if (!name) {
            for (size_t i = 0; i < dir->count; i++) {
                if (jw__join(r, candidate, sizeof(candidate), out, dir->names[i]) < 0) return -1;
                struct stat actual;
                if (lstat(candidate, &actual))
                    return jw__fail(r, "Cannot inspect %.160s: %s", candidate, strerror(errno));
                if (actual.st_dev == requested.st_dev && actual.st_ino == requested.st_ino) {
                    if (name) return jw__fail(r, "Ambiguous filesystem name: %.160s", path);
                    name = dir->names[i];
                }
            }
        }
        if (!name) return jw__fail(r, "Filesystem name changed during inspection: %.160s", path);
        if (jw__join(r, candidate, sizeof(candidate), out, name) < 0 ||
            jw__copy(r, out, out_size, candidate) < 0) return -1;
    }
    return 0;
}

static int jw__normalize(jw_content_reader *r, const char *path,
                         char *out, size_t out_size, unsigned depth) {
    if (depth > JW_CONTENT_MAX_DEPTH) return jw__fail(r, "Content path has too many missing directories");
    char resolved[JW_STORAGE_PATH_MAX];
    if (realpath(path, resolved)) return jw__actual_spelling(r, resolved, out, out_size);
    if (errno != ENOENT) return jw__fail(r, "Cannot resolve %.160s: %s", path, strerror(errno));
    char parent[JW_STORAGE_PATH_MAX];
    if (jw__copy(r, parent, sizeof(parent), path) < 0) return -1;
    char *slash = strrchr(parent, '/');
    if (!slash || !slash[1]) return jw__fail(r, "Invalid content path: %.160s", path);
    char name[JW_STORAGE_PATH_MAX];
    if (jw__copy(r, name, sizeof(name), slash + 1) < 0) return -1;
    if (slash == parent) slash[1] = '\0'; else *slash = '\0';
    if (jw__normalize(r, parent, resolved, sizeof(resolved), depth + 1) < 0) return -1;
    if (!strcmp(name, ".") || !strcmp(name, "..")) {
        struct stat st;
        if (stat(resolved, &st) || !S_ISDIR(st.st_mode))
            return jw__fail(r, "Cannot traverse missing directory: %.160s", resolved);
    }
    if (!strcmp(name, ".")) return jw__copy(r, out, out_size, resolved);
    if (!strcmp(name, "..")) {
        slash = strrchr(resolved, '/');
        if (slash == resolved) slash[1] = '\0'; else if (slash) *slash = '\0';
        return jw__copy(r, out, out_size, resolved);
    }
    return jw__join(r, out, out_size, resolved, name);
}

static bool jw__inside(const char *path, const char *root) {
    size_t n = strlen(root);
    return n && !strncmp(path, root, n) && path[n] == '/';
}

static int jw__origin_source(jw_content_reader *r, const char *path) {
    char prefix[JW_STORAGE_PATH_MAX];
    memcpy(prefix, path, strlen(path) + 1);
    /* Compare directory identities so an alias such as /tmp -> /private/tmp,
       or a differently cased mount path, cannot hide a symlink escape. */
    for (char *p = prefix + 1; *p; p++) {
        if (*p != '/') continue;
        *p = '\0';
        struct stat st;
        int found = stat(prefix, &st) == 0;
        *p = '/';
        if (!found) break;
        for (int i = 0; i < r->sources->count; i++) {
            if (r->roots[i][0] && S_ISDIR(r->root_stats[i].st_mode) &&
                st.st_dev == r->root_stats[i].st_dev && st.st_ino == r->root_stats[i].st_ino)
                return i;
        }
    }
    return -1;
}

/* The file owns copies of its normalized path and ROM-relative key. */
static int jw__own(jw_content_reader *r, jw_content_file *file, const char *path, const char *relative) {
    if (strlen(relative) >= JW_CONTENT_RELPATH_MAX)
        return jw__fail(r, "Content path is too long: %.100s", relative);
    file->path = strdup(path);
    file->rom_relpath = strdup(relative);
    return file->path && file->rom_relpath ? 0 : jw__fail(r, "Out of memory inspecting files");
}

static void jw__release(jw_content_file *file) {
    free(file->path);
    free(file->rom_relpath);
    file->path = file->rom_relpath = NULL;
}

static int jw__resolve(jw_content_reader *r, const char *parent, const char *reference,
                       jw_content_file *file) {
    char ref[JW_STORAGE_PATH_MAX], candidate[JW_STORAGE_PATH_MAX], path[JW_STORAGE_PATH_MAX];
    if (!reference[0] || jw__copy(r, ref, sizeof(ref), reference) < 0)
        return jw__fail(r, "Empty or oversized content reference");
    for (char *p = ref; *p; p++) if (*p == '\\') *p = '/';
    if (ref[0] == '/') {
        if (jw__copy(r, candidate, sizeof(candidate), ref) < 0) return -1;
    } else {
        if (isalpha((unsigned char)ref[0]) && ref[1] == ':')
            return jw__fail(r, "Windows drive paths cannot be resolved here: %.160s", ref);
        if (jw__join(r, candidate, sizeof(candidate), parent, ref) < 0) return -1;
    }
    if (jw__normalize(r, candidate, path, sizeof(path), 0) < 0) return -1;
    int source_index = -1;
    for (int i = 0; i < r->sources->count; i++) {
        if (r->roots[i][0] && jw__inside(path, r->roots[i])) {
            if (source_index >= 0) return jw__fail(r, "Ambiguous ROM source: %.160s", path);
            source_index = i;
        }
    }
    if (source_index < 0) return jw__fail(r, "Content is outside the mounted ROM roots: %.160s", reference);
    /* Relative traversal and symlinks cannot escape their original ROM root;
       an explicit absolute reference may name another mounted source. */
    int origin = jw__origin_source(r, candidate);
    if (origin >= 0 && origin != source_index)
        return jw__fail(r, "Content reference escapes its ROM source: %.160s", reference);
    const jw_storage_source *source = &r->sources->sources[source_index];
    if (jw__copy(r, file->source_id, sizeof(file->source_id), source->id) < 0 ||
        jw__own(r, file, path, path + strlen(r->roots[source_index]) + 1) < 0) return -1;
    struct stat st;
    if (lstat(file->path, &st)) {
        if (errno != ENOENT) return jw__fail(r, "Cannot inspect %.160s: %s", file->path, strerror(errno));
        file->missing = true;
    } else {
        if (!S_ISREG(st.st_mode)) return jw__fail(r, "Content is not a regular file: %.160s", file->path);
        file->size = (uint64_t)st.st_size;
    }
    return 0;
}

static char *jw__trim(char *text) {
    while (isspace((unsigned char)*text)) text++;
    size_t n = strlen(text);
    while (n && isspace((unsigned char)text[n - 1])) text[--n] = '\0';
    return text;
}

/* CMD uses double-quoted arguments; backslashes are path separators, not escapes. */
static int jw__token(jw_content_reader *r, const char **cursor, char *out, size_t size) {
    const char *p = *cursor;
    while (isspace((unsigned char)*p)) p++;
    if (!*p) { *cursor = p; return 0; }
    size_t n = 0;
    bool quoted = false;
    while (*p && (quoted || !isspace((unsigned char)*p))) {
        if (*p == '"') quoted = !quoted;
        else {
            if (n + 1 >= size) return jw__fail(r, "Descriptor argument is too long");
            out[n++] = *p;
        }
        p++;
    }
    if (quoted) return jw__fail(r, "Descriptor has an unterminated quote");
    out[n] = '\0';
    *cursor = p;
    return 1;
}

static bool jw__listed(const jw_ra_string_list *list, const char *value) {
    for (size_t i = 0; i < list->count; i++) {
        const char *item = list->items[i];
        if (item && !strcasecmp(item[0] == '.' ? item + 1 : item, value)) return true;
    }
    return false;
}

static bool jw__cmd_file(const jw_ra_system *system, const char *token) {
    const char *base = token;
    for (const char *p = token; *p; p++) if (*p == '/' || *p == '\\') base = p + 1;
    const char *ext = jw__extension(base);
    return jw__listed(&system->extensions, ext) ||
           jw__listed(&system->archive_extensions, ext) ||
           jw__listed(&system->playlist_extensions, ext) ||
           jw__listed(&system->file_names, base);
}

static int jw__visit(jw_content_reader *r, const char *parent, const char *reference,
                     unsigned depth, size_t *out_index);

/* Like RetroArch, only a '#' directly after .zip, .7z or .apk selects an
   archive member; "Game #1 (Disc 1).cue" is an ordinary filename. */
static char *jw__archive_delimiter(char *path) {
    for (char *p = strchr(path, '#'); p; p = strchr(p + 1, '#')) {
        size_t n = (size_t)(p - path);
        if ((n > 4 && (!strncasecmp(p - 4, ".zip", 4) || !strncasecmp(p - 4, ".apk", 4))) ||
            (n > 3 && !strncasecmp(p - 3, ".7z", 3))) return p;
    }
    return NULL;
}

/* Directives, comments and "|label" suffixes name nothing on disk. */
static int jw__parse_m3u(jw_content_reader *r, char *data, const char *parent, unsigned depth) {
    char *save = NULL;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *s = jw__trim(line);
        if (!*s || *s == '#') continue;
        char *label = strchr(s, '|');
        if (label) *label = '\0';
        s = jw__trim(s);
        size_t n = strlen(s);
        if (n && s[0] == '"') {
            if (n < 2 || s[n - 1] != '"') return jw__fail(r, "Playlist has an unterminated quote");
            s[n - 1] = '\0'; s++;
        }
        char *member = jw__archive_delimiter(s);
        if (member) {
            *member++ = '\0';
            if (!*member) return jw__fail(r, "Playlist has an empty archive member");
        }
        size_t index;
        if (jw__visit(r, parent, s, depth + 1, &index) < 0) return -1;
    }
    return 0;
}

static int jw__parse_tracks(jw_content_reader *r, char *data, const char *parent,
                            int kind, unsigned depth) {
    char *save = NULL;
    size_t references = 0;
    long expected = -1;
    for (char *line = strtok_r(data, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        const char *cursor = jw__trim(line);
        if (!*cursor || (kind == 3 && *cursor == '#')) continue;
        char token[JW_STORAGE_PATH_MAX];
        int got = jw__token(r, &cursor, token, sizeof(token));
        if (got < 0) return -1;
        if (!got) continue;
        if (kind == 3) {
            char *end;
            errno = 0;
            long number = strtol(token, &end, 10);
            if (errno || *end || number < 1 || number > 999)
                return jw__fail(r, "GDI has an invalid track count or number");
            if (expected < 0) {
                if (*jw__trim((char *)cursor)) return jw__fail(r, "GDI track count has extra fields");
                expected = number;
                continue;
            }
            /* Track, LBA, type, sector size, filename, offset. */
            for (int field = 0; field < 3; field++) {
                if (jw__token(r, &cursor, token, sizeof(token)) != 1)
                    return jw__fail(r, "GDI has an incomplete track record");
                errno = 0;
                (void)strtol(token, &end, 10);
                if (errno || *end || !token[0]) return jw__fail(r, "GDI has an invalid track field");
            }
            if (jw__token(r, &cursor, token, sizeof(token)) != 1 || !token[0])
                return jw__fail(r, "GDI has no track filename");
            char offset[64];
            if (jw__token(r, &cursor, offset, sizeof(offset)) != 1)
                return jw__fail(r, "GDI has no track offset");
            errno = 0;
            (void)strtol(offset, &end, 10);
            if (errno || *end || !offset[0] || *jw__trim((char *)cursor))
                return jw__fail(r, "GDI has an invalid track offset");
        } else {
            if (strcasecmp(token, "FILE") && !(kind == 4 && !strcasecmp(token, "DATAFILE"))) continue;
            if (jw__token(r, &cursor, token, sizeof(token)) != 1 || !token[0])
                return jw__fail(r, "Disc descriptor has no FILE filename");
            if (kind == 2) {
                char type[64];
                if (jw__token(r, &cursor, type, sizeof(type)) != 1 || !type[0])
                    return jw__fail(r, "CUE FILE record has no file type");
            }
        }
        size_t index;
        if (jw__visit(r, parent, token, depth + 1, &index) < 0) return -1;
        references++;
    }
    if (!references || (kind == 3 && (long)references != expected))
        return jw__fail(r, "Disc descriptor has missing or inconsistent track records");
    return 0;
}

static int jw__parse_cmd(jw_content_reader *r, char *data, const char *parent, unsigned depth) {
    const jw_ra_system *system = r->system;
    if (!system) return jw__fail(r, "CMD inspection needs the system's file formats");
    const char *cursor = data;
    char token[JW_STORAGE_PATH_MAX];
    if (jw__token(r, &cursor, token, sizeof(token)) != 1 || !token[0])
        return jw__fail(r, "CMD has no executable name");
    int got;
    while ((got = jw__token(r, &cursor, token, sizeof(token))) > 0) {
        if (!token[0] || token[0] == '-' || !jw__cmd_file(system, token)) continue;
        size_t index;
        if (jw__visit(r, parent, token, depth + 1, &index) < 0) return -1;
    }
    return got < 0 ? -1 : 0;
}

static int jw__add_reference(jw_content_reader *r, size_t parent, size_t child) {
    jw_content_reference *refs = jw__grow(r->out->references, &r->reference_capacity,
                                          r->out->reference_count + 1, sizeof(*refs));
    if (!refs) return jw__fail(r, "Out of memory inspecting references");
    r->out->references = refs;
    refs[r->out->reference_count++] = (jw_content_reference){parent, child};
    return 0;
}

/* files and state grow together so state[i] always exists for files[i]. */
static int jw__reserve_file(jw_content_reader *r) {
    size_t capacity = r->file_capacity;
    jw_content_file *files = jw__grow(r->out->files, &capacity, r->out->file_count + 1, sizeof(*files));
    if (!files) return -1;
    r->out->files = files;
    if (capacity == r->file_capacity) return 0;
    unsigned char *state = realloc(r->state, capacity);
    if (!state) return -1;
    r->state = state;
    r->file_capacity = capacity;
    return 0;
}

static int jw__visit(jw_content_reader *r, const char *parent, const char *reference,
                     unsigned depth, size_t *out_index) {
    if (depth > JW_CONTENT_MAX_DEPTH) return jw__fail(r, "Content descriptors are nested too deeply");
    jw_content_file file = {0};
    if (jw__resolve(r, parent, reference, &file) < 0) { jw__release(&file); return -1; }
    size_t seen = jw__index_find(&r->file_index, r->out->files, jw__file_key, file.path);
    if (seen != SIZE_MAX) {
        if (r->state[seen] == 1) {
            jw__fail(r, "Content descriptors contain a cycle: %.160s", file.rom_relpath);
            jw__release(&file);
            return -1;
        }
        jw__release(&file);
        *out_index = seen;
        return depth ? jw__add_reference(r, r->parents[depth - 1], seen) : 0;
    }
    if (r->out->file_count == JW_CONTENT_MAX_FILES) {
        jw__release(&file);
        return jw__fail(r, "Content references too many files");
    }
    size_t index = r->out->file_count;
    if (jw__reserve_file(r) < 0 ||
        jw__index_put(&r->file_index, r->out->files, jw__file_key, index, file.path, index) < 0) {
        jw__release(&file);
        return jw__fail(r, "Out of memory inspecting files");
    }
    r->out->file_count++;
    jw_content_file *files = r->out->files;
    files[index] = file;
    r->state[index] = 1;
    *out_index = index;
    r->parents[depth] = index;
    if (depth && jw__add_reference(r, r->parents[depth - 1], index) < 0) return -1;
    int kind = jw__descriptor_kind(file.path);
    if (!file.missing && kind) {
        if (file.size > 4 * 1024 * 1024 || file.size > JW_CONTENT_MAX_BYTES - r->bytes_read)
            return jw__fail(r, "Content descriptors exceed the inspection size limit: %.160s", file.path);
        FILE *fp = fopen(file.path, "rb");
        if (!fp) return jw__fail(r, "Cannot read descriptor %.160s: %s", file.path, strerror(errno));
        char *data = malloc((size_t)file.size + 1);
        if (!data) { fclose(fp); return jw__fail(r, "Out of memory reading descriptor"); }
        size_t count = fread(data, 1, (size_t)file.size, fp);
        int extra = fgetc(fp);
        bool bad = count != file.size || extra != EOF || ferror(fp);
        if (fclose(fp)) bad = true;
        if (bad) { free(data); return jw__fail(r, "Could not read complete descriptor: %.160s", file.path); }
        if (memchr(data, '\0', count)) { free(data); return jw__fail(r, "Descriptor contains binary data: %.160s", file.path); }
        data[count] = '\0';
        r->bytes_read += count;
        char *body = data;
        if (count >= 3 && !memcmp(body, "\xef\xbb\xbf", 3)) body += 3;
        char directory[JW_STORAGE_PATH_MAX];
        memcpy(directory, file.path, strlen(file.path) + 1);
        char *slash = strrchr(directory, '/');
        if (slash == directory) slash[1] = '\0'; else *slash = '\0';
        int result = kind == 1 ? jw__parse_m3u(r, body, directory, depth) :
                     kind == 5 ? jw__parse_cmd(r, body, directory, depth) :
                                 jw__parse_tracks(r, body, directory, kind, depth);
        free(data);
        if (result < 0) return jw__locate(r, r->out->files[index].path);
    }
    r->state[index] = 2;
    return 0;
}

void jw_content_free(jw_content *content) {
    if (!content) return;
    for (size_t i = 0; i < content->file_count; i++) {
        free(content->files[i].path);
        free(content->files[i].rom_relpath);
    }
    free(content->files);
    free(content->references);
    memset(content, 0, sizeof(*content));
}

int jw_content_inspect(const jw_storage_source_list *sources,
                       const char *source_id, const char *rom_relpath,
                       const jw_ra_system *system, jw_content *out,
                       char *error, size_t error_size) {
    if (error && error_size) error[0] = '\0';
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    jw_content_reader reader = {.sources = sources, .system = system, .out = out,
                                .error = error, .error_size = error_size};
    if (!sources || sources->count < 1 || sources->count > JW_STORAGE_MAX_SOURCES ||
        !source_id || !rom_relpath || !*rom_relpath || rom_relpath[0] == '/' || rom_relpath[0] == '\\')
        return jw__fail(&reader, "Invalid content identity");
    int result = -1;
    int selected = -1;
    for (int i = 0; i < sources->count; i++) {
        if (!strcmp(sources->sources[i].id, source_id)) selected = i;
        if (!sources->sources[i].available) continue;
        if (jw__normalize(&reader, sources->sources[i].roms_path, reader.roots[i],
                          sizeof(reader.roots[i]), 0) < 0) goto done;
        if (stat(reader.roots[i], &reader.root_stats[i]) && errno != ENOENT) {
            jw__fail(&reader, "Cannot inspect ROM source %s: %s", sources->sources[i].id, strerror(errno));
            goto done;
        }
    }
    if (selected < 0 || !sources->sources[selected].available) {
        jw__fail(&reader, "ROM source is not mounted: %s", source_id);
        goto done;
    }
    if (jw__visit(&reader, reader.roots[selected], rom_relpath, 0, &out->launch_file) < 0) goto done;
    if (strcmp(out->files[out->launch_file].source_id, source_id)) {
        jw__fail(&reader, "Launch file escapes the selected ROM source");
        goto done;
    }
    result = 0;
done:
    for (size_t i = 0; i < reader.dir_count; i++) jw__free_dir(reader.dirs[i]);
    free(reader.dirs);
    free(reader.dir_index.slots);
    free(reader.file_index.slots);
    free(reader.state);
    if (result < 0) jw_content_free(out);
    return result;
}
