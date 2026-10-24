#include "internal/retroarch/states.h"

#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static const char *jw__states_basename(const char *path) {
    const char *slash = path ? strrchr(path, '/') : NULL;
    return slash && slash[1] ? slash + 1 : (path ? path : "");
}

static void jw__states_rom_stem(const char *rom_path, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", jw__states_basename(rom_path));
    char *dot = strrchr(out, '.');
    if (dot) {
        *dot = '\0';
    }
}

static bool jw__states_is_regular(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool jw__states_try(const char *dir, const char *name,
                           char *out, size_t out_size) {
    return snprintf(out, out_size, "%s/%s", dir, name) < (int)out_size &&
           jw__states_is_regular(out);
}

static bool jw__states_try_stat(const char *dir, const char *name,
                                char *out, size_t out_size, struct stat *out_st) {
    if (snprintf(out, out_size, "%s/%s", dir, name) >= (int)out_size) {
        return false;
    }
    struct stat st;
    if (stat(out, &st) != 0 || !S_ISREG(st.st_mode)) {
        return false;
    }
    if (out_st) {
        *out_st = st;
    }
    return true;
}

static void jw__states_slot_name(const char *stem, int slot, bool thumb,
                                 char *out, size_t out_size) {
    if (slot < 0) {
        snprintf(out, out_size, thumb ? "%s.state.auto.png" : "%s.state.auto", stem);
    } else if (slot == 0) {
        snprintf(out, out_size, thumb ? "%s.state.png" : "%s.state", stem);
    } else {
        snprintf(out, out_size, thumb ? "%s.state%d.png" : "%s.state%d", stem, slot);
    }
}

bool jw_ra_find_slot_thumb(const char *states_dir, const char *rom_path,
                           int slot, char *out, size_t out_size) {
    if (!states_dir || !states_dir[0] || !rom_path || !rom_path[0] ||
        !out || out_size == 0) {
        return false;
    }

    char stem[512];
    jw__states_rom_stem(rom_path, stem, sizeof(stem));
    char name[576];
    jw__states_slot_name(stem, slot, true, name, sizeof(name));

    /* Flat layout first. */
    if (jw__states_try(states_dir, name, out, out_size)) {
        return true;
    }

    /* Per-core subfolders. */
    DIR *d = opendir(states_dir);
    if (!d) {
        return false;
    }
    struct dirent *ent;
    bool found = false;
    while (!found && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof(sub), "%s/%s", states_dir, ent->d_name) >=
                (int)sizeof(sub)) {
            continue;
        }
        struct stat st;
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        if (jw__states_try(sub, name, out, out_size)) {
            found = true;
        }
    }
    closedir(d);
    return found;
}

bool jw_ra_find_slot_state(const char *states_dir, const char *rom_path,
                           int slot, char *out, size_t out_size) {
    if (!states_dir || !states_dir[0] || !rom_path || !rom_path[0] ||
        !out || out_size == 0) {
        return false;
    }

    char stem[512];
    jw__states_rom_stem(rom_path, stem, sizeof(stem));
    char name[576];
    jw__states_slot_name(stem, slot, false, name, sizeof(name));

    if (jw__states_try(states_dir, name, out, out_size)) {
        return true;
    }

    DIR *d = opendir(states_dir);
    if (!d) {
        return false;
    }
    struct dirent *ent;
    bool found = false;
    while (!found && (ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof(sub), "%s/%s", states_dir, ent->d_name) >=
                (int)sizeof(sub)) {
            continue;
        }
        struct stat st;
        if (stat(sub, &st) != 0 || !S_ISDIR(st.st_mode)) {
            continue;
        }
        if (jw__states_try(sub, name, out, out_size)) {
            found = true;
        }
    }
    closedir(d);
    return found;
}

bool jw_ra_core_states_dir(const char *states_dir, const char *core_folder,
                           char *out, size_t out_size) {
    if (!states_dir || !states_dir[0] || !core_folder || !core_folder[0] ||
        !out || out_size == 0 || strchr(core_folder, '/') ||
        strchr(core_folder, '\\') || strcmp(core_folder, ".") == 0 ||
        strcmp(core_folder, "..") == 0) {
        return false;
    }
    return snprintf(out, out_size, "%s/%s", states_dir, core_folder) <
           (int)out_size;
}

bool jw_ra_slot_state_path_for_core(const char *states_dir,
                                    const char *core_folder,
                                    const char *rom_path, int slot, bool thumb,
                                    char *out, size_t out_size) {
    if (!rom_path || !rom_path[0] || !out || out_size == 0) {
        return false;
    }
    char core_dir[PATH_MAX];
    if (!jw_ra_core_states_dir(states_dir, core_folder,
                               core_dir, sizeof(core_dir))) {
        return false;
    }
    char stem[512];
    jw__states_rom_stem(rom_path, stem, sizeof(stem));
    if (!stem[0] || strlen(jw__states_basename(rom_path)) >= sizeof(stem)) {
        return false;
    }
    char name[576];
    jw__states_slot_name(stem, slot, thumb, name, sizeof(name));
    return snprintf(out, out_size, "%s/%s", core_dir, name) < (int)out_size;
}

bool jw_ra_find_slot_state_for_core(const char *states_dir,
                                    const char *core_folder,
                                    const char *rom_path,
                                    int slot, char *out, size_t out_size) {
    if (!core_folder || !core_folder[0] || !rom_path || !rom_path[0] ||
        !out || out_size == 0) {
        return false;
    }

    char core_dir[PATH_MAX];
    if (!jw_ra_core_states_dir(states_dir, core_folder,
                               core_dir, sizeof(core_dir))) {
        return false;
    }
    char stem[512];
    jw__states_rom_stem(rom_path, stem, sizeof(stem));
    char name[576];
    jw__states_slot_name(stem, slot, false, name, sizeof(name));
    return jw__states_try(core_dir, name, out, out_size);
}

static bool jw__states_parse_suffix(const char *suffix, bool thumb, int *out_slot) {
    if (!suffix || !out_slot) {
        return false;
    }

    if (thumb) {
        if (strcmp(suffix, ".png") == 0) {
            *out_slot = 0;
            return true;
        }
        if (strcmp(suffix, ".auto.png") == 0) {
            *out_slot = JW_RA_AUTO_STATE_SLOT;
            return true;
        }
    } else {
        if (suffix[0] == '\0') {
            *out_slot = 0;
            return true;
        }
        if (strcmp(suffix, ".auto") == 0) {
            *out_slot = JW_RA_AUTO_STATE_SLOT;
            return true;
        }
    }

    const char *digits = suffix;
    if (!thumb && *digits == '\0') {
        return false;
    }
    if (!isdigit((unsigned char)*digits)) {
        return false;
    }

    long value = 0;
    while (isdigit((unsigned char)*digits)) {
        value = value * 10L + (long)(*digits - '0');
        if (value > 999999L) {
            return false;
        }
        digits++;
    }

    if (thumb) {
        if (strcmp(digits, ".png") != 0) {
            return false;
        }
    } else if (*digits != '\0') {
        return false;
    }

    *out_slot = (int)value;
    return true;
}

static bool jw__states_match_slot_name(const char *name, const char *stem,
                                       bool thumb, int *out_slot) {
    if (!name || !stem || !stem[0] || !out_slot) {
        return false;
    }
    char prefix[576];
    int plen = snprintf(prefix, sizeof(prefix), "%s.state", stem);
    if (plen <= (int)sizeof(".state") - 1 || plen >= (int)sizeof(prefix)) {
        return false;
    }
    if (strncmp(name, prefix, (size_t)plen) != 0) {
        return false;
    }
    return jw__states_parse_suffix(name + plen, thumb, out_slot);
}

typedef struct {
    bool found;
    int slot;
    time_t mtime;
    char path[PATH_MAX];
} jw__state_candidate;

static void jw__states_consider_resume(jw__state_candidate *best,
                                       const char *dir,
                                       const char *name,
                                       const char *stem) {
    int slot = 0;
    if (!jw__states_match_slot_name(name, stem, false, &slot)) {
        return;
    }

    char full[PATH_MAX];
    struct stat st;
    if (!jw__states_try_stat(dir, name, full, sizeof(full), &st)) {
        return;
    }

    if (!best->found || st.st_mtime > best->mtime) {
        best->found = true;
        best->slot = slot;
        best->mtime = st.st_mtime;
        snprintf(best->path, sizeof(best->path), "%s", full);
    }
}

static void jw__states_scan_resume_dir(jw__state_candidate *best,
                                       const char *dir,
                                       const char *stem,
                                       bool recurse) {
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name) >=
                (int)sizeof(sub)) {
            continue;
        }
        struct stat st;
        if (stat(sub, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (recurse) {
                jw__states_scan_resume_dir(best, sub, stem, false);
            }
        } else if (S_ISREG(st.st_mode)) {
            jw__states_consider_resume(best, dir, ent->d_name, stem);
        }
    }
    closedir(d);
}

bool jw_ra_find_resume_state(const char *states_dir, const char *rom_path,
                             int preferred_slot, int *out_slot,
                             char *out, size_t out_size) {
    if (!states_dir || !states_dir[0] || !rom_path || !rom_path[0] ||
        !out_slot || !out || out_size == 0) {
        return false;
    }

    char preferred[PATH_MAX];
    if (jw_ra_find_slot_state(states_dir, rom_path, preferred_slot,
                              preferred, sizeof(preferred))) {
        *out_slot = preferred_slot;
        snprintf(out, out_size, "%s", preferred);
        return true;
    }

    char stem[512];
    jw__states_rom_stem(rom_path, stem, sizeof(stem));
    jw__state_candidate best;
    memset(&best, 0, sizeof(best));
    best.slot = 0;

    jw__states_scan_resume_dir(&best, states_dir, stem, true);
    if (!best.found) {
        return false;
    }

    *out_slot = best.slot;
    snprintf(out, out_size, "%s", best.path);
    return true;
}

bool jw_ra_find_resume_state_for_core(const char *states_dir,
                                      const char *core_folder,
                                      const char *rom_path,
                                      int preferred_slot, int *out_slot,
                                      char *out, size_t out_size) {
    if (!states_dir || !states_dir[0] || !core_folder || !core_folder[0] ||
        !rom_path || !rom_path[0] ||
        !out_slot || !out || out_size == 0) {
        return false;
    }

    if (jw_ra_find_slot_state_for_core(states_dir, core_folder, rom_path,
                                       preferred_slot, out, out_size)) {
        *out_slot = preferred_slot;
        return true;
    }

    char core_dir[PATH_MAX];
    if (!jw_ra_core_states_dir(states_dir, core_folder,
                               core_dir, sizeof(core_dir))) {
        return false;
    }
    char stem[512];
    jw__states_rom_stem(rom_path, stem, sizeof(stem));
    jw__state_candidate best;
    memset(&best, 0, sizeof(best));
    best.slot = 0;
    jw__states_scan_resume_dir(&best, core_dir, stem, false);
    if (!best.found) {
        return false;
    }

    *out_slot = best.slot;
    snprintf(out, out_size, "%s", best.path);
    return true;
}

/* ── Thumbnail index ──────────────────────────────────────────────────────── */

typedef struct {
    char *path;
    time_t mtime;
} jw_state_thumb_entry;

struct jw_state_thumb_index {
    jw_state_thumb_entry *entries;
    int                   count;
    int                   cap;
};

/* Accept only savestate thumbnails: a name ending in ".png" that contains
   ".state" (so cover art / box art in the same tree is ignored). */
static bool jw__states_is_thumb_name(const char *name) {
    size_t n = strlen(name);
    if (n < sizeof(".state.png") - 1) {
        return false;
    }
    if (strcmp(name + n - 4, ".png") != 0) {
        return false;
    }
    return strstr(name, ".state") != NULL;
}

static void jw__index_add(jw_state_thumb_index *idx, const char *dir, const char *name) {
    if (!jw__states_is_thumb_name(name)) {
        return;
    }
    char full[PATH_MAX];
    struct stat st;
    if (!jw__states_try_stat(dir, name, full, sizeof(full), &st)) {
        return;
    }
    if (idx->count == idx->cap) {
        int ncap = idx->cap ? idx->cap * 2 : 32;
        jw_state_thumb_entry *np = realloc(idx->entries, (size_t)ncap * sizeof(*np));
        if (!np) {
            return;
        }
        idx->entries = np;
        idx->cap = ncap;
    }
    char *dup = strdup(full);
    if (dup) {
        idx->entries[idx->count].path = dup;
        idx->entries[idx->count].mtime = st.st_mtime;
        idx->count++;
    }
}

static void jw__index_scan_dir(jw_state_thumb_index *idx, const char *dir, bool recurse) {
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name) >= (int)sizeof(sub)) {
            continue;
        }
        struct stat st;
        if (stat(sub, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (recurse) {
                jw__index_scan_dir(idx, sub, false); /* one level of subfolders */
            }
        } else if (S_ISREG(st.st_mode)) {
            jw__index_add(idx, dir, ent->d_name);
        }
    }
    closedir(d);
}

jw_state_thumb_index *jw_state_thumb_index_build(const char *states_dir) {
    if (!states_dir || !states_dir[0]) {
        return NULL;
    }
    jw_state_thumb_index *idx = calloc(1, sizeof(*idx));
    if (!idx) {
        return NULL;
    }
    jw__index_scan_dir(idx, states_dir, true);
    return idx;
}

void jw_state_thumb_index_free(jw_state_thumb_index *idx) {
    if (!idx) {
        return;
    }
    for (int i = 0; i < idx->count; i++) {
        free(idx->entries[i].path);
    }
    free(idx->entries);
    free(idx);
}

bool jw_state_thumb_index_find(const jw_state_thumb_index *idx,
                               const char *rom_path, char *out, size_t out_size) {
    if (!idx || !rom_path || !rom_path[0] || !out || out_size == 0) {
        return false;
    }
    char stem[512];
    jw__states_rom_stem(rom_path, stem, sizeof(stem));

    const jw_state_thumb_entry *best = NULL;
    for (int i = 0; i < idx->count; i++) {
        const jw_state_thumb_entry *entry = &idx->entries[i];
        const char *name = jw__states_basename(entry->path);
        int slot = 0;
        if (!jw__states_match_slot_name(name, stem, true, &slot)) {
            continue;
        }
        if (slot == JW_RA_GAME_SWITCHER_STATE_SLOT) {
            best = entry;
            break;
        }
        if (!best || entry->mtime > best->mtime) {
            best = entry;
        }
    }
    if (!best) {
        return false;
    }
    snprintf(out, out_size, "%s", best->path);
    return true;
}

/* ── Slot enumeration + switcher-slot promotion ───────────────────────────── */

static void jw__states_collect_slot(jw_ra_slot_info *out, int max, int *count,
                                    const char *dir, const char *name,
                                    const char *stem) {
    int slot = 0;
    if (!jw__states_match_slot_name(name, stem, false, &slot)) {
        return;
    }
    char full[PATH_MAX];
    struct stat st;
    if (!jw__states_try_stat(dir, name, full, sizeof(full), &st)) {
        return;
    }
    /* Merge duplicates across flat + per-core subfolder: keep the newest mtime. */
    for (int i = 0; i < *count; i++) {
        if (out[i].slot == slot) {
            if ((long)st.st_mtime > out[i].mtime) {
                out[i].mtime = (long)st.st_mtime;
            }
            return;
        }
    }
    if (*count >= max) {
        return;
    }
    out[*count].slot = slot;
    out[*count].mtime = (long)st.st_mtime;
    (*count)++;
}

static void jw__states_scan_slots_dir(jw_ra_slot_info *out, int max, int *count,
                                      const char *dir, const char *stem,
                                      bool recurse) {
    DIR *d = opendir(dir);
    if (!d) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        char sub[PATH_MAX];
        if (snprintf(sub, sizeof(sub), "%s/%s", dir, ent->d_name) >=
                (int)sizeof(sub)) {
            continue;
        }
        struct stat st;
        if (stat(sub, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            if (recurse) {
                jw__states_scan_slots_dir(out, max, count, sub, stem, false);
            }
        } else if (S_ISREG(st.st_mode)) {
            jw__states_collect_slot(out, max, count, dir, ent->d_name, stem);
        }
    }
    closedir(d);
}

bool jw_ra_list_slots(const char *states_dir, const char *rom_path,
                      jw_ra_slot_info *out, int max, int *count) {
    if (!states_dir || !states_dir[0] || !rom_path || !rom_path[0] ||
        !out || max <= 0 || !count) {
        return false;
    }
    *count = 0;
    char stem[512];
    jw__states_rom_stem(rom_path, stem, sizeof(stem));
    jw__states_scan_slots_dir(out, max, count, states_dir, stem, true);
    return true;
}

static void jw__states_dirname(const char *path, char *out, size_t out_size) {
    const char *slash = strrchr(path, '/');
    if (!slash) {
        snprintf(out, out_size, ".");
        return;
    }
    size_t len = (size_t)(slash - path);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, path, len);
    out[len] = '\0';
}

static bool jw__copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb");
    if (!in) {
        return false;
    }
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[64 * 1024];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    if (ferror(in)) {
        ok = false;
    }
    fclose(in);
    if (fclose(out) != 0) {
        ok = false;
    }
    if (!ok) {
        remove(dst);
    }
    return ok;
}

bool jw_ra_promote_switcher_slot(const char *states_dir, const char *rom_path,
                                 int *out_slot) {
    if (!states_dir || !states_dir[0] || !rom_path || !rom_path[0]) {
        return false;
    }

    char src_state[PATH_MAX];
    if (!jw_ra_find_slot_state(states_dir, rom_path,
                               JW_RA_GAME_SWITCHER_STATE_SLOT,
                               src_state, sizeof(src_state))) {
        return false;
    }

    jw_ra_slot_info slots[64];
    int n = 0;
    jw_ra_list_slots(states_dir, rom_path, slots, 64, &n);

    /* Lowest free numbered slot 0..9; if all are taken, overwrite the oldest. */
    int dest = -1;
    for (int s = 0; s <= 9; s++) {
        bool used = false;
        for (int i = 0; i < n; i++) {
            if (slots[i].slot == s) {
                used = true;
                break;
            }
        }
        if (!used) {
            dest = s;
            break;
        }
    }
    if (dest < 0) {
        long oldest = LONG_MAX;
        int oldest_slot = 0;
        for (int i = 0; i < n; i++) {
            if (slots[i].slot >= 0 && slots[i].slot <= 9 &&
                slots[i].mtime < oldest) {
                oldest = slots[i].mtime;
                oldest_slot = slots[i].slot;
            }
        }
        dest = oldest_slot;
    }

    char dir[PATH_MAX];
    jw__states_dirname(src_state, dir, sizeof(dir));
    char stem[512];
    jw__states_rom_stem(rom_path, stem, sizeof(stem));

    char name[576];
    char dst_state[PATH_MAX];
    jw__states_slot_name(stem, dest, false, name, sizeof(name));
    if (snprintf(dst_state, sizeof(dst_state), "%s/%s", dir, name) >=
            (int)sizeof(dst_state)) {
        return false;
    }
    if (!jw__copy_file(src_state, dst_state)) {
        return false;
    }

    /* Best-effort thumbnail copy so the promoted slot keeps its screenshot. */
    char src_thumb[PATH_MAX];
    if (jw_ra_find_slot_thumb(states_dir, rom_path,
                              JW_RA_GAME_SWITCHER_STATE_SLOT,
                              src_thumb, sizeof(src_thumb))) {
        char dst_thumb[PATH_MAX];
        jw__states_slot_name(stem, dest, true, name, sizeof(name));
        if (snprintf(dst_thumb, sizeof(dst_thumb), "%s/%s", dir, name) <
                (int)sizeof(dst_thumb)) {
            jw__copy_file(src_thumb, dst_thumb);
        }
    }

    if (out_slot) {
        *out_slot = dest;
    }
    return true;
}
