#include "internal/store/pakrat_themes.h"

#include "internal/launcher/user_themes.h"
#include "internal/platform/leaf_version.h"
#include "internal/store/pakrat_kind.h"
#include "internal/store/theme_package.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

int jw_pakrat_bundled_themes_path(const char *platform_root,
                                  const char *state_dir,
                                  char *out, size_t out_size) {
    if (!platform_root || !platform_root[0] || !state_dir || !state_dir[0] ||
        !out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    jw_installed_release release;
    int read_rc = jw_installed_release_read(state_dir, &release);
    if (read_rc > 0 || (read_rc == 0 && !release.release_id[0])) {
        return 1;
    }
    if (read_rc < 0 ||
        strchr(release.release_id, '/') || strcmp(release.release_id, ".") == 0 ||
        strcmp(release.release_id, "..") == 0) {
        return -1;
    }
    /* platform_root is <system>/platforms/<platform>; releases sit beside
       platforms/ under the same <system>. */
    char system[PATH_MAX];
    int n = snprintf(system, sizeof(system), "%s", platform_root);
    if (n < 0 || (size_t)n >= sizeof(system)) {
        return -1;
    }
    for (int strip = 0; strip < 2; strip++) {
        size_t len = strlen(system);
        while (len > 1 && system[len - 1] == '/') {
            system[--len] = '\0';
        }
        char *slash = strrchr(system, '/');
        if (!slash || slash == system) {
            return -1;
        }
        *slash = '\0';
    }
    n = snprintf(out, out_size, "%s/releases/%s/%s", system,
                 release.release_id, JW_PAKRAT_BUNDLED_THEMES_FILE);
    return n >= 0 && (size_t)n < out_size ? 0 : -1;
}

int jw_pakrat_theme_name_listed(const char *list_path, const char *name) {
    if (!list_path || !name || !name[0]) {
        return -1;
    }
    FILE *fp = fopen(list_path, "r");
    if (!fp) {
        return errno == ENOENT ? 0 : -1;
    }
    char line[512];
    int listed = 0;
    while (!listed && fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[--len] = '\0';
        }
        if (len == 0 || line[0] == '#') {
            continue;
        }
        listed = strcasecmp(line, name) == 0;
    }
    int failed = ferror(fp);
    fclose(fp);
    return failed ? -1 : listed;
}

int jw_pakrat_theme_name_reserved(const char *platform_root,
                                  const char *state_dir, const char *name) {
    if (!name || !name[0]) {
        return -1;
    }
    if (jw_theme_package_reserved_name(name)) {
        return 1;
    }
    char list[PATH_MAX];
    int path_rc = jw_pakrat_bundled_themes_path(platform_root, state_dir, list,
                                                sizeof(list));
    if (path_rc > 0) {
        return 0;
    }
    return path_rc < 0 ? -1 : jw_pakrat_theme_name_listed(list, name);
}

int jw_pakrat_theme_folder_count(const char *sdcard_root) {
    if (!sdcard_root || !sdcard_root[0]) {
        return -1;
    }
    char themes[PATH_MAX];
    int n = snprintf(themes, sizeof(themes), "%s/%s", sdcard_root,
                     JW_PAKRAT_THEMES_DIR);
    if (n < 0 || (size_t)n >= sizeof(themes)) {
        return -1;
    }
    DIR *dir = opendir(themes);
    if (!dir) {
        return errno == ENOENT ? 0 : -1;
    }
    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        /* The launcher's scan skips hidden names, which is also where Pak Rat's
           own transition siblings live. */
        if (entry->d_name[0] == '.') {
            continue;
        }
        char child[PATH_MAX];
        struct stat st;
        n = snprintf(child, sizeof(child), "%s/%s", themes, entry->d_name);
        if (n > 0 && (size_t)n < sizeof(child) && stat(child, &st) == 0 &&
            S_ISDIR(st.st_mode)) {
            count++;
        }
    }
    closedir(dir);
    return count;
}

bool jw_pakrat_theme_slots_full(int folder_count, bool replaces_existing) {
    return !replaces_existing && folder_count >= JW_USER_THEME_MAX;
}
