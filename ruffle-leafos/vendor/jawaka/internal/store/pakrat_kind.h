/* What a Pak Rat package installs, and under which root.
 *
 * Apps and content paks install to Apps/<platform>/<Name>.pak on whichever
 * card owns them. Themes install to Themes/<id>/ on the primary card, the only
 * place the launcher discovers user themes.
 *
 * The root travels inside install_path so that every record, marker and lock
 * that stores only an install_path still resolves to the right place: an app's
 * install_path is Apps-relative ("mlp1/Name.pak"), as it always has been, and a
 * theme's names its root ("Themes/neon-nights"). No platform is called
 * "Themes", and the catalog refuses one. pakrat_installs.kind records the same
 * fact explicitly and is checked against the path. */
#ifndef JW_STORE_PAKRAT_KIND_H
#define JW_STORE_PAKRAT_KIND_H

#include <stdbool.h>
#include <string.h>

typedef enum {
    JW_PAKRAT_KIND_APP = 0,
    JW_PAKRAT_KIND_THEME = 1,
} jw_pakrat_kind;

#define JW_PAKRAT_APPS_DIR         "Apps"
#define JW_PAKRAT_THEMES_DIR       "Themes"
#define JW_PAKRAT_THEME_PATH_PREFIX JW_PAKRAT_THEMES_DIR "/"
/* A theme has no platform, but pakrat_installs.platform is required. */
#define JW_PAKRAT_THEME_PLATFORM   "any"
#define JW_PAKRAT_THEME_MANIFEST   "theme.json"

static inline jw_pakrat_kind jw_pakrat_install_path_kind(const char *install_path) {
    return install_path &&
                   strncmp(install_path, JW_PAKRAT_THEME_PATH_PREFIX,
                           sizeof(JW_PAKRAT_THEME_PATH_PREFIX) - 1) == 0
               ? JW_PAKRAT_KIND_THEME
               : JW_PAKRAT_KIND_APP;
}

/* The pakrat_installs.kind value. */
static inline const char *jw_pakrat_kind_name(jw_pakrat_kind kind) {
    return kind == JW_PAKRAT_KIND_THEME ? "theme" : "app";
}

static inline bool jw_pakrat_kind_from_name(const char *name, jw_pakrat_kind *out) {
    if (name && strcmp(name, "theme") == 0) {
        *out = JW_PAKRAT_KIND_THEME;
        return true;
    }
    if (name && strcmp(name, "app") == 0) {
        *out = JW_PAKRAT_KIND_APP;
        return true;
    }
    return false;
}

/* Prefix that turns an install_path into a card-root-relative path, for logs
   and user-facing text: "Apps/" for an app, nothing for a theme. */
static inline const char *jw_pakrat_install_path_display_prefix(const char *install_path) {
    return jw_pakrat_install_path_kind(install_path) == JW_PAKRAT_KIND_THEME ? "" : "Apps/";
}

#endif
