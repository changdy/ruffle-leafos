#ifndef JW_STORE_PAKRAT_STATE_H
#define JW_STORE_PAKRAT_STATE_H

#include "internal/store/pakrat.h"
#include "internal/store/pakrat_catalog.h"

typedef enum {
    JW_PAKRAT_APP_AVAILABLE = 0,
    JW_PAKRAT_APP_INSTALLED,
    JW_PAKRAT_APP_UPDATE_AVAILABLE,
    JW_PAKRAT_APP_STALE,
    JW_PAKRAT_APP_UNMANAGED      /* present on disk but not installed by Pak Rat */
} jw_pakrat_app_status;

typedef struct {
    jw_pakrat_catalog_package package;
    jw_pakrat_app_status status;
    int managed;
    int installed_owned;
    int app_present;
    int primary_action_allowed;
    int installed_version_missing_from_history;
    int action_uses_history;
    char action_version[64];
    char installed_version[64];
    char installed_at[64];
    char app_name[256];
    char app_pak_dir[512];
    char gated_version[64];
    char gated_min_leaf_version[64];
    /* CONTENT-1 facts read from the INSTALLED pak, not from the storefront
       lane -- so a sideloaded pak in no lane at all still gets them right. */
    /* Which storefront lane listed this package. Presentation only: it says
       what the store can tell the user BEFORE anything is installed. It never
       decides Open or install health -- those read the installed pak. */
    jw_pakrat_catalog_lane lane;
    int content_provides;   /* the installed pak declares a `provides` block */
    int content_only;       /* ...and ships no executable launch.sh */
    int open_allowed;       /* installed pak has an executable launch.sh */
    char provides_summary[128];  /* e.g. "SCUMMVM" or "SNES +1 core" */
    char content_diagnostic[192];/* newest diagnostics.json entry for this pak */
    /* Themes (package.kind == JW_PAKRAT_KIND_THEME). app_present says the
       folder exists; a theme is never Stale and never opens. A withdrawn theme
       is listed only while it is installed, with no install action. */
    int theme_name_reserved;     /* a bundled theme owns the folder; also sets managed */
    int theme_slots_full;        /* not installed, and Themes/ has no room for it */
} jw_pakrat_app_state;

const char *jw_pakrat_app_status_name(jw_pakrat_app_status status);

/* Returns 0 on success, 1 when no catalog is configured or no matching package
   exists, JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF when the schema is too new,
   and -1 on invalid catalog data or I/O failure. */
int jw_pakrat_find_catalog_package(const jw_pakrat_context *ctx,
                                   const char *store_id,
                                   jw_pakrat_catalog_package *out,
                                   int *out_is_dev_override);

/* Resolve an exact historical package without applying its Leaf gate. The
   caller must only use this for repair of the same recorded owned version. */
int jw_pakrat_find_catalog_package_version(
    const jw_pakrat_context *ctx,
    const char *store_id,
    const char *version,
    jw_pakrat_catalog_package *out,
    int *out_is_dev_override);

/* Builds the read-only app-store state the UI needs: catalog package data joined
   to Pak Rat ownership rows and scanned app presence, for apps, content paks
   and themes alike (package.kind tells them apart). Returns 0 on success, 1
   when no catalog is configured, JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF when
   the schema is too new, and -1 on other errors. */
int jw_pakrat_list_app_states(const jw_pakrat_context *ctx,
                              jw_pakrat_app_state *out,
                              int max_count,
                              int *out_count);

#endif
