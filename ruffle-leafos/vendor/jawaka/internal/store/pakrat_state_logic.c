#include "internal/store/pakrat_state_logic.h"

#include "internal/platform/leaf_version.h"

#include <string.h>

int jw_pakrat_primary_action_opens(const jw_pakrat_app_state *app) {
    return app && !app->managed && app->installed_owned && app->open_allowed &&
           app->status != JW_PAKRAT_APP_UPDATE_AVAILABLE;
}

jw_pakrat_app_status jw_pakrat_resolve_owned_state(
    const char *selected_version,
    const char *installed_version,
    int app_present,
    int content_only,
    int *out_primary_action_allowed) {
    int selected[3];
    int installed[3];
    int valid = selected_version && installed_version &&
                jw_pak_version_parse(selected_version, selected) == 0 &&
                jw_pak_version_parse(installed_version, installed) == 0;
    int cmp = valid ? jw_version_cmp(selected, installed) : 0;

    if (out_primary_action_allowed) {
        *out_primary_action_allowed = 0;
    }
    /* A content-only pak is DELIBERATELY absent from Apps (CONTENT-1 D6), so
       only a pak that should have been listed and is not counts as stale. */
    if (!app_present && !content_only) {
        if (out_primary_action_allowed) {
            *out_primary_action_allowed = valid && cmp == 0;
        }
        return JW_PAKRAT_APP_STALE;
    }
    if (valid && cmp > 0) {
        if (out_primary_action_allowed) {
            *out_primary_action_allowed = 1;
        }
        return JW_PAKRAT_APP_UPDATE_AVAILABLE;
    }
    if (out_primary_action_allowed) {
        *out_primary_action_allowed = valid && cmp == 0;
    }
    return JW_PAKRAT_APP_INSTALLED;
}

jw_pakrat_app_status jw_pakrat_resolve_owned_theme_state(
    const char *selected_version,
    const char *installed_version,
    int *out_primary_action_allowed) {
    return jw_pakrat_resolve_owned_state(selected_version, installed_version,
                                         1, 0, out_primary_action_allowed);
}

int jw_pakrat_store_section(const jw_pakrat_app_state *apps, int count,
                            jw_pakrat_kind kind, int *out_index, int max_index) {
    int n = 0;
    for (int i = 0; apps && out_index && i < count && n < max_index; i++) {
        if (apps[i].package.kind == kind) {
            out_index[n++] = i;
        }
    }
    return n;
}

int jw_pakrat_store_section_find(const jw_pakrat_app_state *apps,
                                 const int *index, int index_count,
                                 const char *store_id) {
    for (int i = 0; apps && index && store_id && store_id[0] && i < index_count; i++) {
        if (strcmp(apps[index[i]].package.id, store_id) == 0) {
            return i;
        }
    }
    return -1;
}

jw_pakrat_theme_action jw_pakrat_theme_primary_action(
    const jw_pakrat_app_state *app, int listed, int applied) {
    if (!app || app->package.kind != JW_PAKRAT_KIND_THEME || app->managed) {
        return JW_PAKRAT_THEME_ACTION_NONE;
    }
    if (app->installed_owned) {
        if (app->status == JW_PAKRAT_APP_UPDATE_AVAILABLE &&
            app->primary_action_allowed && !app->package.withdrawn) {
            return JW_PAKRAT_THEME_ACTION_UPDATE;
        }
        if (app->app_present && listed) {
            return applied ? JW_PAKRAT_THEME_ACTION_APPLIED
                           : JW_PAKRAT_THEME_ACTION_APPLY;
        }
        return app->primary_action_allowed && !app->package.withdrawn
                   ? JW_PAKRAT_THEME_ACTION_REINSTALL
                   : JW_PAKRAT_THEME_ACTION_NONE;
    }
    if (app->package.withdrawn || !app->primary_action_allowed) {
        return JW_PAKRAT_THEME_ACTION_NONE;
    }
    return app->status == JW_PAKRAT_APP_UNMANAGED
               ? JW_PAKRAT_THEME_ACTION_ADOPT
               : JW_PAKRAT_THEME_ACTION_INSTALL;
}

jw_pakrat_refusal jw_pakrat_theme_install_precheck(const jw_pakrat_app_state *app) {
    if (!app || app->package.kind != JW_PAKRAT_KIND_THEME) {
        return JW_PAKRAT_REFUSED_NONE;
    }
    if (app->theme_name_reserved) {
        return JW_PAKRAT_REFUSED_RESERVED_NAME;
    }
    if (app->package.withdrawn) {
        return JW_PAKRAT_REFUSED_WITHDRAWN;
    }
    if (app->theme_slots_full) {
        return JW_PAKRAT_REFUSED_THEME_LIMIT;
    }
    return JW_PAKRAT_REFUSED_NONE;
}
