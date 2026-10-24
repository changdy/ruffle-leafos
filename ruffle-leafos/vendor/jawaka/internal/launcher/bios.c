#include "internal/launcher/bios.h"

#include "internal/i18n/i18n.h"   /* JW_UI: extraction marker only; T() is called at the draw site */

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static void jw__bios_copy(char *out, size_t out_size, const char *value) {
    if (!out || out_size == 0) {
        return;
    }
    snprintf(out, out_size, "%s", value ? value : "");
}

static bool jw__bios_saturn_rel_path_valid(const char *rel_path) {
    static const char prefix[] = JW_BIOS_SATURN_SUBDIR "/";
    return jw_storage_relative_path_valid(rel_path) &&
           strncasecmp(rel_path, prefix, sizeof(prefix) - 1u) == 0 &&
           rel_path[sizeof(prefix) - 1u] != '\0';
}

void jw_bios_choice_parse(const char *value, jw_bios_choice *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (!value || !value[0]) {
        return;
    }
    if (strcmp(value, "hle") == 0) {
        out->kind = JW_BIOS_CHOICE_HLE;
        return;
    }
    if (strncmp(value, "file:", 5) != 0) {
        return;
    }
    const char *source = value + 5;
    const char *separator = strchr(source, ':');
    if (!separator) {
        return;
    }
    size_t source_len = (size_t)(separator - source);
    if (source_len == 0 || source_len >= sizeof(out->source_id)) {
        return;
    }
    const char *rel = separator + 1;
    if (!jw__bios_saturn_rel_path_valid(rel) ||
        strlen(rel) >= sizeof(out->rel_path)) {
        return;
    }
    memcpy(out->source_id, source, source_len);
    out->source_id[source_len] = '\0';
    jw__bios_copy(out->rel_path, sizeof(out->rel_path), rel);
    out->kind = JW_BIOS_CHOICE_FILE;
}

bool jw_bios_choice_format(const jw_bios_choice *choice, char *out, size_t out_size) {
    if (!choice || !out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    switch (choice->kind) {
        case JW_BIOS_CHOICE_HLE:
            return snprintf(out, out_size, "hle") < (int)out_size;
        case JW_BIOS_CHOICE_FILE: {
            if (!choice->source_id[0] || strchr(choice->source_id, ':') ||
                !jw__bios_saturn_rel_path_valid(choice->rel_path)) {
                return false;
            }
            int needed = snprintf(out, out_size, "file:%s:%s",
                                  choice->source_id, choice->rel_path);
            if (needed < 0 || (size_t)needed >= out_size) {
                out[0] = '\0';
                return false;
            }
            return true;
        }
        case JW_BIOS_CHOICE_DEFAULT:
        default:
            return false;
    }
}

bool jw_bios_choice_equal(const jw_bios_choice *a, const jw_bios_choice *b) {
    if (!a || !b || a->kind != b->kind) {
        return false;
    }
    if (a->kind != JW_BIOS_CHOICE_FILE) {
        return true;
    }
    return strcmp(a->source_id, b->source_id) == 0 &&
           strcmp(a->rel_path, b->rel_path) == 0;
}

void jw_bios_resolve(const char *game_value, const char *system_value,
                     jw_bios_resolution *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));

    jw_bios_choice choice;
    jw_bios_choice_parse(game_value, &choice);
    if (choice.kind != JW_BIOS_CHOICE_DEFAULT) {
        out->choice = choice;
        out->origin = JW_BIOS_ORIGIN_GAME;
        return;
    }
    jw_bios_choice_parse(system_value, &choice);
    if (choice.kind != JW_BIOS_CHOICE_DEFAULT) {
        out->choice = choice;
        out->origin = JW_BIOS_ORIGIN_SYSTEM;
        return;
    }
    /* Nothing chosen anywhere: the emulator's own default, which is HLE. */
    out->choice.kind = JW_BIOS_CHOICE_HLE;
    out->origin = JW_BIOS_ORIGIN_DEFAULT;
}

jw_bios_file_status jw_bios_resolve_file(const jw_storage_source_list *sources,
                                         const jw_bios_choice *choice,
                                         char *out_abs, size_t out_abs_size) {
    if (out_abs && out_abs_size > 0) {
        out_abs[0] = '\0';
    }
    if (!choice || choice->kind != JW_BIOS_CHOICE_FILE) {
        return JW_BIOS_FILE_NO_CHOICE;
    }
    if (!jw__bios_saturn_rel_path_valid(choice->rel_path)) {
        return JW_BIOS_FILE_INVALID_PATH;
    }
    const jw_storage_source *source =
        jw_storage_sources_find_by_id(sources, choice->source_id);
    if (!source || !source->configured || !source->available || !source->bios_path[0]) {
        return JW_BIOS_FILE_SOURCE_UNAVAILABLE;
    }

    char resolved[JW_STORAGE_PATH_MAX];
    if (jw_storage_resolve_bios(source, choice->rel_path,
                                resolved, sizeof(resolved)) != 0) {
        /* Distinguish "the file is gone" from "the path leaves the BIOS root":
           the first is the ordinary recoverable case the menu explains, the
           second is a selection that must never be launched. */
        char candidate[JW_STORAGE_PATH_MAX];
        struct stat st;
        if (snprintf(candidate, sizeof(candidate), "%s/%s",
                     source->bios_path, choice->rel_path) >= (int)sizeof(candidate) ||
            lstat(candidate, &st) != 0) {
            return JW_BIOS_FILE_MISSING;
        }
        return JW_BIOS_FILE_OUTSIDE_ROOT;
    }

    struct stat st;
    if (stat(resolved, &st) != 0) {
        return JW_BIOS_FILE_MISSING;
    }
    if (!S_ISREG(st.st_mode)) {
        return JW_BIOS_FILE_NOT_REGULAR;
    }
    if (access(resolved, R_OK) != 0) {
        return JW_BIOS_FILE_UNREADABLE;
    }
    if (st.st_size != (off_t)JW_BIOS_SATURN_IMAGE_BYTES) {
        return JW_BIOS_FILE_WRONG_SIZE;
    }
    if (out_abs && out_abs_size > 0 &&
        snprintf(out_abs, out_abs_size, "%s", resolved) >= (int)out_abs_size) {
        out_abs[0] = '\0';
        return JW_BIOS_FILE_INVALID_PATH;
    }
    return JW_BIOS_FILE_OK;
}

/* One key per outcome. The daemon returns these verbatim over IPC and logs
   them; the launcher runs the same pointer through T() before drawing, which
   is why they are marked for extraction here rather than wrapped. */
const char *jw_bios_file_status_text(jw_bios_file_status status) {
    switch (status) {
        case JW_BIOS_FILE_OK:
            return JW_UI("BIOS file ready");
        case JW_BIOS_FILE_NO_CHOICE:
            return JW_UI("No BIOS file selected");
        case JW_BIOS_FILE_INVALID_PATH:
            return JW_UI("Selected BIOS path is not usable");
        case JW_BIOS_FILE_SOURCE_UNAVAILABLE:
            return JW_UI("Selected BIOS card is not mounted");
        case JW_BIOS_FILE_MISSING:
            return JW_UI("Selected BIOS file is missing");
        case JW_BIOS_FILE_OUTSIDE_ROOT:
            return JW_UI("Selected BIOS file left the BIOS folder");
        case JW_BIOS_FILE_NOT_REGULAR:
            return JW_UI("Selected BIOS file is not a file");
        case JW_BIOS_FILE_UNREADABLE:
            return JW_UI("Selected BIOS file cannot be read");
        case JW_BIOS_FILE_WRONG_SIZE:
            return JW_UI("Selected BIOS file is not a 512 KiB image");
    }
    return JW_UI("Selected BIOS file is unavailable");
}

int jw_bios_entry_compare(const jw_bios_entry *a, const jw_bios_entry *b) {
    if (a->is_dir != b->is_dir) {
        return a->is_dir ? -1 : 1;
    }
    return strcmp(a->name, b->name);
}

int jw_bios_list_dir(const char *dir_abs, const jw_bios_entry *after,
                     jw_bios_entry *out, int max_out, int *out_count,
                     jw_bios_cancel_fn cancel, void *cancel_ctx,
                     jw_bios_list_result *result) {
    jw_bios_list_result local;
    memset(&local, 0, sizeof(local));
    if (out_count) {
        *out_count = 0;
    }
    if (!dir_abs || !dir_abs[0] || !out || max_out <= 0 || !out_count) {
        local.failed = true;
        if (result) *result = local;
        return -1;
    }

    DIR *dir = opendir(dir_abs);
    if (!dir) {
        local.failed = true;
        if (result) *result = local;
        return -1;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (local.examined > 0 && local.examined % JW_BIOS_SCAN_BATCH == 0 &&
            cancel && cancel(cancel_ctx)) {
            local.cancelled = true;
            break;
        }
        local.examined++;

        const char *name = ent->d_name;
        if (name[0] == '.' &&
            (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
            continue;
        }
        if (strlen(name) >= JW_BIOS_NAME_MAX) {
            continue;
        }

        char child[JW_STORAGE_PATH_MAX];
        if (snprintf(child, sizeof(child), "%s/%s", dir_abs, name) >= (int)sizeof(child)) {
            continue;
        }
        /* lstat, not stat: a symlink is never followed, in the listing or at
           launch, so no entry can point outside the BIOS root. */
        struct stat st;
        if (lstat(child, &st) != 0) {
            continue;
        }

        jw_bios_entry candidate;
        memset(&candidate, 0, sizeof(candidate));
        snprintf(candidate.name, sizeof(candidate.name), "%s", name);
        if (S_ISDIR(st.st_mode)) {
            candidate.is_dir = true;
        } else if (S_ISREG(st.st_mode) &&
                   st.st_size == (off_t)JW_BIOS_SATURN_IMAGE_BYTES) {
            candidate.is_dir = false;
        } else {
            continue;
        }

        if (after && jw_bios_entry_compare(&candidate, after) <= 0) {
            continue;
        }
        if (count == max_out &&
            jw_bios_entry_compare(&candidate, &out[count - 1]) >= 0) {
            local.has_more = true;
            continue;
        }

        int at = count;
        while (at > 0 && jw_bios_entry_compare(&candidate, &out[at - 1]) < 0) {
            at--;
        }
        int last = count < max_out ? count : max_out - 1;
        if (count == max_out) {
            local.has_more = true;
        }
        for (int i = last; i > at; i--) {
            out[i] = out[i - 1];
        }
        out[at] = candidate;
        if (count < max_out) {
            count++;
        }
    }
    closedir(dir);

    *out_count = count;
    if (result) {
        *result = local;
    }
    return 0;
}

bool jw_bios_rel_join(const char *rel_dir, const char *name,
                      char *out, size_t out_size) {
    if (!name || !name[0] || !out || out_size == 0) {
        return false;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0 || strchr(name, '/')) {
        return false;
    }
    int needed = (rel_dir && rel_dir[0])
        ? snprintf(out, out_size, "%s/%s", rel_dir, name)
        : snprintf(out, out_size, "%s", name);
    if (needed < 0 || (size_t)needed >= out_size) {
        out[0] = '\0';
        return false;
    }
    if (!jw_storage_relative_path_valid(out)) {
        out[0] = '\0';
        return false;
    }
    return true;
}

void jw_bios_rel_parent(const char *rel_dir, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!rel_dir || !rel_dir[0]) {
        return;
    }
    const char *slash = strrchr(rel_dir, '/');
    if (!slash) {
        return;
    }
    size_t len = (size_t)(slash - rel_dir);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, rel_dir, len);
    out[len] = '\0';
}
