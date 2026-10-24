#include "internal/storage/sources.h"

#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#endif

static const char *jw__env_value(const char *name) {
    const char *value = getenv(name);
    return (value && value[0]) ? value : NULL;
}

static bool jw__format(char *out, size_t out_size, const char *fmt, ...) {
    if (!out || out_size == 0 || !fmt) {
        return false;
    }
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(out, out_size, fmt, args);
    va_end(args);
    return needed >= 0 && (size_t)needed < out_size;
}

static void jw__realpath_or_literal(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    out[0] = '\0';
    if (!path || !path[0]) {
        return;
    }
    char resolved[JW_STORAGE_PATH_MAX];
    if (realpath(path, resolved)) {
        snprintf(out, out_size, "%s", resolved);
    } else {
        size_t len = strlen(path);
        if (len >= out_size) len = out_size - 1;
        memmove(out, path, len);
        out[len] = '\0';
    }
}

static bool jw__path_is_within(const char *path, const char *root) {
    if (!path || !root || !path[0] || !root[0]) {
        return false;
    }
    size_t root_len = strlen(root);
    if (root_len == 1 && root[0] == '/') {
        return path[0] == '/';
    }
    return strncmp(path, root, root_len) == 0 &&
           (path[root_len] == '\0' || path[root_len] == '/');
}

static void jw__source_id_for_index(int index, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return;
    }
    if (index == 0) {
        snprintf(out, out_size, "%s", "primary");
    } else if (index == 1) {
        snprintf(out, out_size, "%s", "secondary_sd");
    } else {
        snprintf(out, out_size, "source%d", index + 1);
    }
}

static int jw__split_count(const char *list) {
    if (!list || !list[0]) {
        return 0;
    }
    int count = 1;
    for (const char *p = list; *p; p++) {
        if (*p == ':') {
            if (p == list || p[-1] == ':' || p[1] == '\0') {
                return -1;
            }
            count++;
        }
    }
    return count;
}

static int jw__split_nth(const char *list, int index, char *out, size_t out_size) {
    if (!list || index < 0 || !out || out_size == 0) {
        return -1;
    }
    const char *start = list;
    int current = 0;
    while (start && *start) {
        const char *end = strchr(start, ':');
        if (current == index) {
            size_t len = end ? (size_t)(end - start) : strlen(start);
            if (len == 0 || len >= out_size) {
                return -1;
            }
            memcpy(out, start, len);
            out[len] = '\0';
            return 0;
        }
        if (!end) {
            break;
        }
        start = end + 1;
        current++;
    }
    return -1;
}

static int jw__source_child_path(const char *plural_env,
                                 const char *singular_env,
                                 const char *root,
                                 const char *child,
                                 int index,
                                 char *out,
                                 size_t out_size) {
    const char *plural = jw__env_value(plural_env);
    if (plural && jw__split_nth(plural, index, out, out_size) == 0) {
        return 0;
    }
    if (index == 0) {
        const char *singular = jw__env_value(singular_env);
        if (singular) {
            return jw__format(out, out_size, "%s", singular) ? 0 : -1;
        }
    }
    return jw__format(out, out_size, "%s/%s", root, child) ? 0 : -1;
}

bool jw_storage_source_paths_v2_valid(void) {
    static const struct {
        const char *plural;
        const char *singular;
    } required[] = {
        {"SDCARD_PATHS", "SDCARD_PATH"},
        {"USERDATA_PATHS", "USERDATA_PATH"},
        {"SHARED_USERDATA_PATHS", "SHARED_USERDATA_PATH"},
        {"SAVES_PATHS", "SAVES_PATH"},
        {"STATES_PATHS", "STATES_PATH"},
    };
    const char *version = jw__env_value("UMRK_ENV_VERSION");
    const char *roots = jw__env_value("SDCARD_PATHS");
    int count = jw__split_count(roots);
    if (!version || strcmp(version, "2") != 0 || count <= 0 ||
        count > JW_STORAGE_MAX_SOURCES) {
        return false;
    }

    char values[sizeof(required) / sizeof(required[0])]
               [JW_STORAGE_MAX_SOURCES][JW_STORAGE_PATH_MAX];
    memset(values, 0, sizeof(values));
    for (size_t list = 0; list < sizeof(required) / sizeof(required[0]); list++) {
        const char *plural = jw__env_value(required[list].plural);
        const char *singular = jw__env_value(required[list].singular);
        if (!plural || !singular || jw__split_count(plural) != count) {
            return false;
        }
        for (int index = 0; index < count; index++) {
            char *value = values[list][index];
            if (jw__split_nth(plural, index, value, JW_STORAGE_PATH_MAX) != 0 ||
                value[0] != '/' || strstr(value, "//") || strstr(value, "/./") ||
                strstr(value, "/../")) {
                return false;
            }
            size_t len = strlen(value);
            if ((len > 1 && value[len - 1] == '/') ||
                (len >= 2 && strcmp(value + len - 2, "/.") == 0) ||
                (len >= 3 && strcmp(value + len - 3, "/..") == 0)) {
                return false;
            }
            for (int prior = 0; prior < index; prior++) {
                if (strcmp(value, values[list][prior]) == 0) {
                    return false;
                }
            }
        }
        if (strcmp(values[list][0], singular) != 0) {
            return false;
        }
    }
    for (int index = 0; index < count; index++) {
        const char *root = values[0][index];
        for (size_t list = 1; list < sizeof(required) / sizeof(required[0]); list++) {
            if (!jw__path_is_within(values[list][index], root)) {
                return false;
            }
        }
    }
    return true;
}

#if defined(PLATFORM_MLP1) && defined(__linux__)
static uint32_t jw__le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void jw__filesystem_fingerprint(unsigned major_id, unsigned minor_id,
                                       const char *filesystem,
                                       const char *device,
                                       char *out, size_t out_size) {
    DIR *uuid_dir = opendir("/dev/disk/by-uuid");
    if (uuid_dir) {
        struct dirent *entry = NULL;
        while ((entry = readdir(uuid_dir)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            char path[JW_STORAGE_PATH_MAX];
            struct stat st;
            if (jw__format(path, sizeof(path), "/dev/disk/by-uuid/%s", entry->d_name) &&
                stat(path, &st) == 0 && S_ISBLK(st.st_mode) &&
                major(st.st_rdev) == major_id && minor(st.st_rdev) == minor_id) {
                jw__format(out, out_size, "uuid:%s", entry->d_name);
                break;
            }
        }
        closedir(uuid_dir);
        if (out[0]) return;
    }
    if (filesystem && device && device[0] == '/' &&
        (strcmp(filesystem, "vfat") == 0 || strcmp(filesystem, "msdos") == 0 ||
         strcmp(filesystem, "fat") == 0)) {
        unsigned char sector[512];
        int fd = open(device, O_RDONLY | O_CLOEXEC);
        ssize_t got = fd >= 0 ? pread(fd, sector, sizeof(sector), 0) : -1;
        if (fd >= 0) close(fd);
        if (got == (ssize_t)sizeof(sector) &&
            sector[510] == 0x55 && sector[511] == 0xaa) {
            size_t offset = (sector[17] == 0 && sector[18] == 0 &&
                             sector[22] == 0 && sector[23] == 0) ? 67 : 39;
            uint32_t serial = jw__le32(&sector[offset]);
            if (serial) jw__format(out, out_size, "fat:%08x", serial);
        }
    }
}

static void jw__mountinfo_unescape(char *path) {
    if (!path) return;
    char *read = path;
    char *write = path;
    while (*read) {
        if (read[0] == '\\' &&
            ((read[1] == '0' && read[2] == '4' && read[3] == '0') ||
             (read[1] == '0' && read[2] == '1' && read[3] == '1') ||
             (read[1] == '0' && read[2] == '1' && read[3] == '2') ||
             (read[1] == '1' && read[2] == '3' && read[3] == '4'))) {
            int value = (read[1] - '0') * 64 +
                        (read[2] - '0') * 8 +
                        (read[3] - '0');
            *write++ = (char)value;
            read += 4;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}
#endif

static int jw__source_availability(jw_storage_source *source) {
    struct stat st;
    if (!source || stat(source->root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        return 0;
    }
    source->device_id = (unsigned long long)st.st_dev;
    if (stat(source->roms_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        source->roms_device_id = (unsigned long long)st.st_dev;
    }
    if (stat(source->images_path, &st) == 0 && S_ISDIR(st.st_mode)) {
        source->images_device_id = (unsigned long long)st.st_dev;
    }
#if defined(PLATFORM_MLP1) && defined(__linux__)
    FILE *mounts = fopen("/proc/self/mountinfo", "r");
    if (!mounts) {
        return 0;
    }
    char line[8192];
    size_t roms_mount_len = 0;
    size_t images_mount_len = 0;
    char roms_abs[JW_STORAGE_PATH_MAX];
    char images_abs[JW_STORAGE_PATH_MAX];
    jw__realpath_or_literal(source->roms_path, roms_abs, sizeof(roms_abs));
    jw__realpath_or_literal(source->images_path, images_abs, sizeof(images_abs));
    while (fgets(line, sizeof(line), mounts)) {
        int mount_id = 0;
        int parent_id = 0;
        unsigned major = 0;
        unsigned minor = 0;
        char filesystem[64] = "";
        char device[JW_STORAGE_PATH_MAX] = "";
        char mountpoint[JW_STORAGE_PATH_MAX];
        mountpoint[0] = '\0';
        if (sscanf(line, "%d %d %u:%u %*s %4095s",
                   &mount_id, &parent_id, &major, &minor, mountpoint) == 5) {
            jw__mountinfo_unescape(mountpoint);
        }
        char *separator = strstr(line, " - ");
        if (separator) {
            (void)sscanf(separator + 3, "%63s %4095s", filesystem, device);
            jw__mountinfo_unescape(device);
        }
        size_t mount_len = strlen(mountpoint);
        if (mountpoint[0] && strcmp(source->root_abs, mountpoint) == 0) {
            source->mount_id = mount_id;
            jw__filesystem_fingerprint(major, minor, filesystem, device,
                                       source->filesystem_fingerprint,
                                       sizeof(source->filesystem_fingerprint));
        }
        if (mountpoint[0] && mount_len > roms_mount_len &&
            jw__path_is_within(roms_abs, mountpoint)) {
            source->roms_mount_id = mount_id;
            roms_mount_len = mount_len;
            source->roms_filesystem_fingerprint[0] = '\0';
            jw__filesystem_fingerprint(major, minor, filesystem, device,
                                       source->roms_filesystem_fingerprint,
                                       sizeof(source->roms_filesystem_fingerprint));
        }
        if (mountpoint[0] && mount_len > images_mount_len &&
            jw__path_is_within(images_abs, mountpoint)) {
            source->images_mount_id = mount_id;
            images_mount_len = mount_len;
            source->images_filesystem_fingerprint[0] = '\0';
            jw__filesystem_fingerprint(major, minor, filesystem, device,
                                       source->images_filesystem_fingerprint,
                                       sizeof(source->images_filesystem_fingerprint));
        }
    }
    fclose(mounts);
    source->available = source->mount_id > 0;
#else
    source->available = true;
#endif
    return 0;
}

static bool jw__same_path(const char *a, const char *b) {
    char a_abs[JW_STORAGE_PATH_MAX];
    char b_abs[JW_STORAGE_PATH_MAX];
    jw__realpath_or_literal(a, a_abs, sizeof(a_abs));
    jw__realpath_or_literal(b, b_abs, sizeof(b_abs));
    return a_abs[0] && b_abs[0] && strcmp(a_abs, b_abs) == 0;
}

int jw_storage_sources_resolve(const char *primary_root, jw_storage_source_list *out) {
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    const char *root_list = jw__env_value("SDCARD_PATHS");
    char fallback[JW_STORAGE_PATH_MAX * 2];
    if (!root_list) {
        const char *env_primary = jw__env_value("SDCARD_PATH");
        if (!env_primary) {
            env_primary = jw__env_value("JAWAKA_SDCARD_ROOT");
        }
        if (!env_primary) {
            env_primary = primary_root;
        }
        if (!env_primary || !env_primary[0]) {
            env_primary = "./mock-sdcard";
        }
        const char *secondary = jw__env_value("UMRK_SECONDARY_SDCARD_PATH");
        if (!secondary) {
            secondary = jw__env_value("SECONDARY_SDCARD_PATH");
        }
        if (!jw__format(fallback, sizeof(fallback), secondary ? "%s:%s" : "%s",
                        env_primary, secondary)) {
            return -1;
        }
        root_list = fallback;
    }

    int root_count = jw__split_count(root_list);
    if (root_count <= 0 || root_count > JW_STORAGE_MAX_SOURCES) {
        return -1;
    }
    static const char *aligned_lists[] = {
        "USERDATA_PATHS", "SHARED_USERDATA_PATHS", "ROMS_PATHS",
        "IMAGES_PATHS", "MUSIC_PATHS", "VIDEO_PATHS", "APPS_PATHS", "BIOS_PATHS",
        "SAVES_PATHS", "STATES_PATHS", "CHEATS_PATHS",
    };
    for (size_t i = 0; i < sizeof(aligned_lists) / sizeof(aligned_lists[0]); i++) {
        const char *value = jw__env_value(aligned_lists[i]);
        if (value && jw__split_count(value) != root_count) {
            return -1;
        }
    }

    const char *declared_primary = jw__env_value("SDCARD_PATH");
    if (!declared_primary) {
        declared_primary = primary_root;
    }
    char first_root[JW_STORAGE_PATH_MAX];
    if (declared_primary &&
        (jw__split_nth(root_list, 0, first_root, sizeof(first_root)) != 0 ||
         !jw__same_path(first_root, declared_primary))) {
        return -1;
    }

    const char *cursor = root_list;
    while (cursor && *cursor && out->count < JW_STORAGE_MAX_SOURCES) {
        const char *end = strchr(cursor, ':');
        size_t len = end ? (size_t)(end - cursor) : strlen(cursor);
        if (len > 0) {
            jw_storage_source *source = &out->sources[out->count];
            if (len >= sizeof(source->root)) {
                return -1;
            }
            memcpy(source->root, cursor, len);
            source->root[len] = '\0';
            source->primary = out->count == 0;
            source->configured = true;
            jw__source_id_for_index(out->count, source->id, sizeof(source->id));
            jw__realpath_or_literal(source->root, source->root_abs,
                                    sizeof(source->root_abs));
            for (int prior = 0; prior < out->count; prior++) {
                if (strcmp(source->root_abs, out->sources[prior].root_abs) == 0) {
                    return -1;
                }
            }

            int index = out->count;
            char userdata_child[128];
            const char *platform = jw__env_value("PLATFORM");
            if (!platform) {
                platform = "mac";
            }
            if (!jw__format(userdata_child, sizeof(userdata_child),
                            ".userdata/%s", platform)) {
                return -1;
            }
            if (jw__source_child_path("USERDATA_PATHS", "USERDATA_PATH",
                                      source->root, userdata_child, index,
                                      source->userdata_path,
                                      sizeof(source->userdata_path)) != 0 ||
                jw__source_child_path("SHARED_USERDATA_PATHS",
                                      "SHARED_USERDATA_PATH", source->root,
                                      ".userdata/shared", index,
                                      source->shared_userdata_path,
                                      sizeof(source->shared_userdata_path)) != 0 ||
                jw__source_child_path("ROMS_PATHS", "ROMS_PATH", source->root,
                                      "Roms", index, source->roms_path,
                                      sizeof(source->roms_path)) != 0 ||
                jw__source_child_path("IMAGES_PATHS", "IMAGES_PATH", source->root,
                                      "Images", index, source->images_path,
                                      sizeof(source->images_path)) != 0 ||
                jw__source_child_path("MUSIC_PATHS", "MUSIC_PATH", source->root,
                                      "Music", index, source->music_path,
                                      sizeof(source->music_path)) != 0 ||
                jw__source_child_path("VIDEO_PATHS", "VIDEO_PATH", source->root,
                                      "Videos", index, source->video_path,
                                      sizeof(source->video_path)) != 0 ||
                jw__source_child_path("APPS_PATHS", "APPS_PATH", source->root,
                                      "Apps", index, source->apps_path,
                                      sizeof(source->apps_path)) != 0 ||
                jw__source_child_path("BIOS_PATHS", "BIOS_PATH", source->root,
                                      "BIOS", index, source->bios_path,
                                      sizeof(source->bios_path)) != 0 ||
                jw__source_child_path("SAVES_PATHS", "SAVES_PATH", source->root,
                                      "Saves", index, source->saves_path,
                                      sizeof(source->saves_path)) != 0 ||
                jw__source_child_path("STATES_PATHS", "STATES_PATH", source->root,
                                      "States", index, source->states_path,
                                      sizeof(source->states_path)) != 0 ||
                jw__source_child_path("CHEATS_PATHS", "CHEATS_PATH", source->root,
                                      "Cheats", index, source->cheats_path,
                                      sizeof(source->cheats_path)) != 0) {
                return -1;
            }
            jw__source_availability(source);
            out->count++;
        }
        if (!end) {
            break;
        }
        cursor = end + 1;
    }

    return out->count > 0 ? 0 : -1;
}

const jw_storage_source *jw_storage_sources_primary(const jw_storage_source_list *list) {
    return (list && list->count > 0) ? &list->sources[0] : NULL;
}

const jw_storage_source *jw_storage_sources_find_by_id(const jw_storage_source_list *list,
                                                       const char *id) {
    if (!list || !id || !id[0]) {
        return NULL;
    }
    for (int i = 0; i < list->count; i++) {
        if (strcmp(list->sources[i].id, id) == 0) {
            return &list->sources[i];
        }
    }
    return NULL;
}

const jw_storage_source *jw_storage_sources_find_for_path(const jw_storage_source_list *list,
                                                          const char *path) {
    if (!list || list->count <= 0 || !path || !path[0]) {
        return NULL;
    }
    if (path[0] != '/') {
        return jw_storage_sources_primary(list);
    }

    char resolved[JW_STORAGE_PATH_MAX];
    jw__realpath_or_literal(path, resolved, sizeof(resolved));

    const jw_storage_source *best = NULL;
    size_t best_len = 0;
    for (int i = 0; i < list->count; i++) {
        const jw_storage_source *source = &list->sources[i];
        const char *root = source->root_abs[0] ? source->root_abs : source->root;
        size_t len = strlen(root);
        if (len > best_len && jw__path_is_within(resolved, root)) {
            best = source;
            best_len = len;
        }
    }
    return best ? best : jw_storage_sources_primary(list);
}

int jw_storage_resolve_path(const jw_storage_source_list *list, const char *path,
                            char *out, size_t out_size) {
    if (!list || list->count <= 0 || !path || !path[0] || !out || out_size == 0) {
        return -1;
    }
    int needed = 0;
    if (path[0] == '/') {
        needed = snprintf(out, out_size, "%s", path);
    } else {
        const jw_storage_source *primary = jw_storage_sources_primary(list);
        needed = snprintf(out, out_size, "%s/%s", primary->root, path);
    }
    return needed >= 0 && (size_t)needed < out_size ? 0 : -1;
}

int jw_storage_db_path_for_source(const jw_storage_source *source,
                                  const char *relative_path,
                                  const char *absolute_path,
                                  char *out, size_t out_size) {
    if (!source || !out || out_size == 0) {
        return -1;
    }
    const char *value = source->primary ? relative_path : absolute_path;
    if (!value || !value[0]) {
        return -1;
    }
    char temp[JW_STORAGE_PATH_MAX];
    int needed = snprintf(temp, sizeof(temp), "%s", value);
    if (needed < 0 || (size_t)needed >= sizeof(temp)) {
        return -1;
    }
    needed = snprintf(out, out_size, "%s", temp);
    return needed >= 0 && (size_t)needed < out_size ? 0 : -1;
}

bool jw_storage_relative_path_valid(const char *path) {
    if (!path || !path[0] || path[0] == '/') {
        return false;
    }
    const unsigned char *p = (const unsigned char *)path;
    const unsigned char *component = p;
    for (;;) {
        if ((*p != '\0' && *p < 0x20) || *p == 0x7f || *p == '\\') {
            return false;
        }
        if (*p == '/' || *p == '\0') {
            size_t len = (size_t)(p - component);
            if (len == 0 || (len == 1 && component[0] == '.') ||
                (len == 2 && component[0] == '.' && component[1] == '.')) {
                return false;
            }
            if (*p == '\0') {
                return true;
            }
            component = p + 1;
        }
        p++;
    }
}

int jw_storage_resolve_bios(const jw_storage_source *source,
                            const char *bios_relpath,
                            char *out, size_t out_size) {
    if (!source || !source->configured || !source->available ||
        !source->bios_path[0] ||
        !jw_storage_relative_path_valid(bios_relpath) || !out || out_size == 0) {
        return -1;
    }
    char candidate[JW_STORAGE_PATH_MAX];
    if (!jw__format(candidate, sizeof(candidate), "%s/%s",
                    source->bios_path, bios_relpath)) {
        return -1;
    }
    char resolved[JW_STORAGE_PATH_MAX];
    if (!realpath(candidate, resolved)) {
        return -1;
    }
    char bios_root[JW_STORAGE_PATH_MAX];
    jw__realpath_or_literal(source->bios_path, bios_root, sizeof(bios_root));
    if (!jw__path_is_within(resolved, bios_root)) {
        return -1;
    }
    return jw__format(out, out_size, "%s", resolved) ? 0 : -1;
}

int jw_storage_resolve_rom(const jw_storage_source *source,
                           const char *rom_relpath,
                           bool require_regular_file,
                           char *out, size_t out_size) {
    if (!source || !source->configured || !source->available ||
        !jw_storage_relative_path_valid(rom_relpath) || !out || out_size == 0) {
        return -1;
    }
    char candidate[JW_STORAGE_PATH_MAX];
    if (!jw__format(candidate, sizeof(candidate), "%s/%s",
                    source->roms_path, rom_relpath)) {
        return -1;
    }
    char resolved[JW_STORAGE_PATH_MAX];
    if (!realpath(candidate, resolved)) {
        return -1;
    }
    char roms_root[JW_STORAGE_PATH_MAX];
    jw__realpath_or_literal(source->roms_path, roms_root, sizeof(roms_root));
    if (!jw__path_is_within(resolved, roms_root)) {
        return -1;
    }
    if (require_regular_file) {
        struct stat st;
        if (stat(resolved, &st) != 0 || !S_ISREG(st.st_mode)) {
            return -1;
        }
    }
    return jw__format(out, out_size, "%s", resolved) ? 0 : -1;
}
