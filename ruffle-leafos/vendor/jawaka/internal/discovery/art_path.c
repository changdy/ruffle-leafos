#include "internal/discovery/art_path.h"

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define JW__ART_DIR_BUCKETS 64

typedef struct {
    char    *name;        /* filename as it exists on disk */
    size_t   stem_len;
    uint32_t stem_hash;   /* of the stem, case-folded when the folder folds case */
    int      format;      /* 0 png, 1 jpg, 2 jpeg */
    int      spelling;    /* 0 all lower, 1 all upper, 2 mixed */
    size_t   next;        /* 1-based chain within a hash bucket, 0 ends it */
} jw__art_entry;

typedef struct jw__art_dir {
    char               *path;
    uint32_t            path_hash;
    bool                folds_case;
    jw__art_entry      *entries;
    size_t              count;
    size_t             *heads;        /* 1-based entry index per bucket */
    size_t              bucket_count;
    struct jw__art_dir *next;
} jw__art_dir;

struct jw_art_index {
    jw__art_dir *buckets[JW__ART_DIR_BUCKETS];
};

/* Lower-case extensions in precedence order. */
static const char *const jw__art_extensions[] = { "png", "jpg", "jpeg" };

static uint32_t jw__art_hash(const char *s, size_t len, bool fold) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        h ^= fold ? (unsigned char)tolower(c) : c;
        h *= 16777619u;
    }
    return h;
}

static int jw__art_format(const char *ext) {
    for (size_t i = 0; i < sizeof(jw__art_extensions) / sizeof(jw__art_extensions[0]); i++) {
        if (strcasecmp(ext, jw__art_extensions[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static int jw__art_spelling(const char *ext) {
    bool lower = true, upper = true;
    for (const char *p = ext; *p; p++) {
        if (islower((unsigned char)*p)) upper = false;
        if (isupper((unsigned char)*p)) lower = false;
    }
    return lower ? 0 : upper ? 1 : 2;
}

static bool jw__art_is_regular(const char *dir, const char *name, unsigned char type) {
#ifdef DT_UNKNOWN
    if (type == DT_REG) return true;
    if (type != DT_UNKNOWN && type != DT_LNK) return false;
#else
    (void)type;
#endif
    /* Unknown type, or a symlink: follow it, as opening the art would. */
    char path[4096];
    struct stat st;
    return snprintf(path, sizeof(path), "%s/%s", dir, name) < (int)sizeof(path) &&
           stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/* Whether this folder's filesystem resolves names without case: look the
   first art file up under its case-swapped name and compare inodes. Art names
   always contain letters (the extension), so there is always something to
   swap. */
static bool jw__art_folds_case(const char *dir, const char *name) {
    char original[4096], swapped[4096];
    if (snprintf(original, sizeof(original), "%s/%s", dir, name) >= (int)sizeof(original)) {
        return false;
    }
    memcpy(swapped, original, sizeof(swapped));
    for (char *p = swapped + strlen(dir) + 1; *p; p++) {
        unsigned char c = (unsigned char)*p;
        *p = (char)(islower(c) ? toupper(c) : tolower(c));
    }
    struct stat a, b;
    return stat(original, &a) == 0 && stat(swapped, &b) == 0 &&
           a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

static void jw__art_dir_free(jw__art_dir *d) {
    if (!d) return;
    for (size_t i = 0; i < d->count; i++) {
        free(d->entries[i].name);
    }
    free(d->entries);
    free(d->heads);
    free(d->path);
    free(d);
}

/* Read one folder. A missing or unreadable folder loads as empty. Returns NULL
   only when out of memory. */
static jw__art_dir *jw__art_dir_load(const char *path) {
    jw__art_dir *d = calloc(1, sizeof(*d));
    if (!d || !(d->path = strdup(path))) {
        free(d);
        return NULL;
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return d;
    }
    size_t capacity = 0;
    struct dirent *e;
    while ((e = readdir(dir))) {
        const char *dot = strrchr(e->d_name, '.');
        if (!dot || dot == e->d_name) continue;
        int format = jw__art_format(dot + 1);
        if (format < 0) continue;
#ifdef DT_UNKNOWN
        unsigned char type = e->d_type;
#else
        unsigned char type = 0;
#endif
        if (!jw__art_is_regular(path, e->d_name, type)) continue;
        if (d->count == capacity) {
            size_t grown = capacity ? capacity * 2 : 64;
            jw__art_entry *entries = realloc(d->entries, grown * sizeof(*entries));
            if (!entries) break;
            d->entries = entries;
            capacity = grown;
        }
        jw__art_entry *entry = &d->entries[d->count];
        memset(entry, 0, sizeof(*entry));
        if (!(entry->name = strdup(e->d_name))) break;
        entry->stem_len = (size_t)(dot - e->d_name);
        entry->format = format;
        entry->spelling = jw__art_spelling(dot + 1);
        d->count++;
    }
    closedir(dir);

    if (d->count == 0) {
        return d;
    }
    d->folds_case = jw__art_folds_case(path, d->entries[0].name);
    d->bucket_count = 1;
    while (d->bucket_count < d->count * 2) d->bucket_count *= 2;
    d->heads = calloc(d->bucket_count, sizeof(*d->heads));
    if (!d->heads) {
        jw__art_dir_free(d);
        return NULL;
    }
    for (size_t i = 0; i < d->count; i++) {
        jw__art_entry *entry = &d->entries[i];
        entry->stem_hash = jw__art_hash(entry->name, entry->stem_len, d->folds_case);
        size_t bucket = entry->stem_hash & (d->bucket_count - 1);
        entry->next = d->heads[bucket];
        d->heads[bucket] = i + 1;
    }
    return d;
}

/* True when a ranks before b: format, then spelling class, then bytewise. */
static bool jw__art_better(const jw__art_entry *a, const jw__art_entry *b) {
    if (a->format != b->format) return a->format < b->format;
    if (a->spelling != b->spelling) return a->spelling < b->spelling;
    int ext = strcmp(a->name + a->stem_len, b->name + b->stem_len);
    if (ext != 0) return ext < 0;
    return strcmp(a->name, b->name) < 0;
}

static int jw__art_dir_find(const jw__art_dir *d, const char *stem,
                            char *out_name, size_t out_name_size) {
    if (d->count == 0) {
        return 1;
    }
    size_t stem_len = strlen(stem);
    uint32_t hash = jw__art_hash(stem, stem_len, d->folds_case);
    const jw__art_entry *best = NULL;
    for (size_t i = d->heads[hash & (d->bucket_count - 1)]; i; i = d->entries[i - 1].next) {
        const jw__art_entry *entry = &d->entries[i - 1];
        if (entry->stem_hash != hash || entry->stem_len != stem_len) continue;
        bool same = d->folds_case ? strncasecmp(entry->name, stem, stem_len) == 0
                                  : memcmp(entry->name, stem, stem_len) == 0;
        if (same && (!best || jw__art_better(entry, best))) {
            best = entry;
        }
    }
    if (!best) {
        return 1;
    }
    return snprintf(out_name, out_name_size, "%s", best->name) < (int)out_name_size ? 0 : -1;
}

jw_art_index *jw_art_index_new(void) {
    return calloc(1, sizeof(jw_art_index));
}

void jw_art_index_free(jw_art_index *index) {
    if (!index) return;
    for (size_t b = 0; b < JW__ART_DIR_BUCKETS; b++) {
        jw__art_dir *d = index->buckets[b];
        while (d) {
            jw__art_dir *next = d->next;
            jw__art_dir_free(d);
            d = next;
        }
    }
    free(index);
}

static const jw__art_dir *jw__art_index_dir(jw_art_index *index, const char *path) {
    uint32_t hash = jw__art_hash(path, strlen(path), false);
    jw__art_dir **bucket = &index->buckets[hash % JW__ART_DIR_BUCKETS];
    for (jw__art_dir *d = *bucket; d; d = d->next) {
        if (d->path_hash == hash && strcmp(d->path, path) == 0) {
            return d;
        }
    }
    jw__art_dir *d = jw__art_dir_load(path);
    if (!d) {
        return NULL;
    }
    d->path_hash = hash;
    d->next = *bucket;
    *bucket = d;
    return d;
}

int jw_art_find(jw_art_index *index, const char *dir, const char *stem,
                char *out_name, size_t out_name_size) {
    if (!dir || !dir[0] || !stem || !stem[0] || !out_name || out_name_size == 0) {
        return 1;
    }
    out_name[0] = '\0';
    if (index) {
        const jw__art_dir *d = jw__art_index_dir(index, dir);
        if (d) {
            return jw__art_dir_find(d, stem, out_name, out_name_size);
        }
    }
    jw__art_dir *d = jw__art_dir_load(dir);
    if (!d) {
        return 1;
    }
    int rc = jw__art_dir_find(d, stem, out_name, out_name_size);
    jw__art_dir_free(d);
    return rc;
}

static void jw__art_extension_lower(const char *name, char *out, size_t out_size) {
    out[0] = '\0';
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (!dot || dot == name || !dot[1]) {
        return;
    }
    size_t i = 0;
    for (const char *p = dot + 1; *p && i + 1u < out_size; p++, i++) {
        out[i] = (char)tolower((unsigned char)*p);
    }
    out[i] = '\0';
}

static void jw__art_strip_last_extension(char *name) {
    char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        *dot = '\0';
    }
}

void jw_art_stem_for_rom(const jw_ra_system *system, const char *filename,
                         char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", filename ? filename : "");
    if (!system) {
        jw__art_strip_last_extension(out);
        return;
    }

    char outer_ext[64];
    jw__art_extension_lower(filename, outer_ext, sizeof(outer_ext));
    int archive = jw_ra_string_list_contains_casefold(&system->archive_extensions, outer_ext);
    int playlist = jw_ra_string_list_contains_casefold(&system->playlist_extensions, outer_ext);
    int content = jw_ra_string_list_contains_casefold(&system->extensions, outer_ext);

    if (archive || playlist || content) {
        jw__art_strip_last_extension(out);
    }

    if (archive || content) {
        /* Also strip a content double-extension ("cart.p8.png") so titles
           don't keep the inner suffix. */
        char inner_ext[64];
        jw__art_extension_lower(out, inner_ext, sizeof(inner_ext));
        if (jw_ra_string_list_contains_casefold(&system->archive_inner_extensions, inner_ext) ||
            jw_ra_string_list_contains_casefold(&system->extensions, inner_ext)) {
            jw__art_strip_last_extension(out);
        }
    }
}

static int jw__art_find_in(jw_art_index *index, const char *dir, const char *rel_dir,
                           const char *stem,
                           char *image_abs, size_t image_abs_size,
                           char *image_rel, size_t image_rel_size) {
    char name[1024];
    int rc = jw_art_find(index, dir, stem, name, sizeof(name));
    if (rc != 0) {
        return rc;
    }
    if (snprintf(image_abs, image_abs_size, "%s/%s", dir, name) >= (int)image_abs_size ||
        snprintf(image_rel, image_rel_size, "%s/%s", rel_dir, name) >= (int)image_rel_size) {
        return -1;
    }
    return 0;
}

int jw_art_find_for_rom(jw_art_index *index,
                        const jw_storage_source *source,
                        const char *image_root,
                        const char *physical_folder,
                        const char *stem,
                        char *image_abs, size_t image_abs_size,
                        char *image_rel, size_t image_rel_size) {
    if (!source || !stem || !stem[0] || !image_abs || image_abs_size == 0 ||
        !image_rel || image_rel_size == 0) {
        return 1;
    }

    char canonical_dir[4096] = "";
    if (image_root && image_root[0] &&
        snprintf(canonical_dir, sizeof(canonical_dir), "%s/%s",
                 source->root, image_root) < (int)sizeof(canonical_dir)) {
        int rc = jw__art_find_in(index, canonical_dir, image_root, stem,
                                 image_abs, image_abs_size,
                                 image_rel, image_rel_size);
        if (rc <= 0) return rc;
    }

    if (!physical_folder || !physical_folder[0]) {
        return 1;
    }

    char dir[4096];
    char rel_dir[4096];
    if (snprintf(dir, sizeof(dir), "%s/%s", source->images_path, physical_folder) <
            (int)sizeof(dir) &&
        snprintf(rel_dir, sizeof(rel_dir), "Images/%s", physical_folder) <
            (int)sizeof(rel_dir) &&
        /* The canonical folder usually is Images/<folder>: skip the repeat. */
        strcmp(dir, canonical_dir) != 0) {
        int rc = jw__art_find_in(index, dir, rel_dir, stem, image_abs, image_abs_size,
                                 image_rel, image_rel_size);
        if (rc <= 0) return rc;
    }

    if (snprintf(dir, sizeof(dir), "%s/%s/Imgs", source->roms_path, physical_folder) <
            (int)sizeof(dir) &&
        snprintf(rel_dir, sizeof(rel_dir), "Roms/%s/Imgs", physical_folder) <
            (int)sizeof(rel_dir)) {
        int rc = jw__art_find_in(index, dir, rel_dir, stem, image_abs, image_abs_size,
                                 image_rel, image_rel_size);
        if (rc <= 0) return rc;
    }

    return 1;
}
