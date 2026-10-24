#include "internal/store/pakrat_state_logic.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static jw_pakrat_app_state theme_state(jw_pakrat_app_status status, int owned) {
    jw_pakrat_app_state app;
    memset(&app, 0, sizeof(app));
    app.package.kind = JW_PAKRAT_KIND_THEME;
    app.status = status;
    app.installed_owned = owned;
    app.app_present = owned;
    app.primary_action_allowed = 1;
    return app;
}

static void check_store_sections(void) {
    jw_pakrat_app_state apps[5];
    memset(apps, 0, sizeof(apps));
    const jw_pakrat_kind kinds[5] = {
        JW_PAKRAT_KIND_APP, JW_PAKRAT_KIND_THEME, JW_PAKRAT_KIND_APP,
        JW_PAKRAT_KIND_THEME, JW_PAKRAT_KIND_THEME,
    };
    for (int i = 0; i < 5; i++) {
        apps[i].package.kind = kinds[i];
        snprintf(apps[i].package.id, sizeof(apps[i].package.id), "pkg-%d", i);
    }
    int index[5];
    /* Apps keep their listing order and never see a theme. */
    assert(jw_pakrat_store_section(apps, 5, JW_PAKRAT_KIND_APP, index, 5) == 2);
    assert(index[0] == 0 && index[1] == 2);
    int themes = jw_pakrat_store_section(apps, 5, JW_PAKRAT_KIND_THEME, index, 5);
    assert(themes == 3 && index[0] == 1 && index[1] == 3 && index[2] == 4);
    assert(jw_pakrat_store_section(apps, 5, JW_PAKRAT_KIND_THEME, index, 2) == 2);
    assert(jw_pakrat_store_section(apps, 0, JW_PAKRAT_KIND_THEME, index, 5) == 0);

    /* A reload keeps the cursor on the package, not on the row number. */
    themes = jw_pakrat_store_section(apps, 5, JW_PAKRAT_KIND_THEME, index, 5);
    assert(jw_pakrat_store_section_find(apps, index, themes, "pkg-3") == 1);
    assert(jw_pakrat_store_section_find(apps, index, themes, "pkg-2") == -1);
    assert(jw_pakrat_store_section_find(apps, index, themes, "") == -1);
    assert(jw_pakrat_store_section_find(apps, index, themes, NULL) == -1);
}

static void check_theme_actions(void) {
    jw_pakrat_app_state app = theme_state(JW_PAKRAT_APP_AVAILABLE, 0);
    assert(jw_pakrat_theme_primary_action(&app, 0, 0) == JW_PAKRAT_THEME_ACTION_INSTALL);
    assert(jw_pakrat_theme_install_precheck(&app) == JW_PAKRAT_REFUSED_NONE);

    /* Full Themes/ still reads as Install; the precheck explains the refusal
       before a download is wasted. */
    app.theme_slots_full = 1;
    assert(jw_pakrat_theme_primary_action(&app, 0, 0) == JW_PAKRAT_THEME_ACTION_INSTALL);
    assert(jw_pakrat_theme_install_precheck(&app) == JW_PAKRAT_REFUSED_THEME_LIMIT);

    /* A bundled theme owns the name: nothing to do, and the reason is the name. */
    app = theme_state(JW_PAKRAT_APP_AVAILABLE, 0);
    app.theme_name_reserved = 1;
    app.managed = 1;
    assert(jw_pakrat_theme_primary_action(&app, 0, 0) == JW_PAKRAT_THEME_ACTION_NONE);
    assert(jw_pakrat_theme_install_precheck(&app) == JW_PAKRAT_REFUSED_RESERVED_NAME);

    app = theme_state(JW_PAKRAT_APP_UNMANAGED, 0);
    app.app_present = 1;
    assert(jw_pakrat_theme_primary_action(&app, 1, 0) == JW_PAKRAT_THEME_ACTION_ADOPT);

    app = theme_state(JW_PAKRAT_APP_AVAILABLE, 0);
    app.primary_action_allowed = 0;   /* gated on a newer Leaf */
    assert(jw_pakrat_theme_primary_action(&app, 0, 0) == JW_PAKRAT_THEME_ACTION_NONE);

    /* Installed and listed: Apply, or already applied. */
    app = theme_state(JW_PAKRAT_APP_INSTALLED, 1);
    assert(jw_pakrat_theme_primary_action(&app, 1, 0) == JW_PAKRAT_THEME_ACTION_APPLY);
    assert(jw_pakrat_theme_primary_action(&app, 1, 1) == JW_PAKRAT_THEME_ACTION_APPLIED);
    /* Owned but the folder is gone, or the scan does not list it: repair. */
    assert(jw_pakrat_theme_primary_action(&app, 0, 0) == JW_PAKRAT_THEME_ACTION_REINSTALL);
    app.app_present = 0;
    assert(jw_pakrat_theme_primary_action(&app, 1, 0) == JW_PAKRAT_THEME_ACTION_REINSTALL);
    app.primary_action_allowed = 0;   /* installed version missing from history */
    assert(jw_pakrat_theme_primary_action(&app, 1, 0) == JW_PAKRAT_THEME_ACTION_NONE);

    /* An update wins over Apply. */
    app = theme_state(JW_PAKRAT_APP_UPDATE_AVAILABLE, 1);
    assert(jw_pakrat_theme_primary_action(&app, 1, 1) == JW_PAKRAT_THEME_ACTION_UPDATE);

    /* Withdrawn: an installed copy keeps working and can still be applied, but
       nothing is installed, updated or repaired from the store. */
    app.package.withdrawn = 1;
    assert(jw_pakrat_theme_primary_action(&app, 1, 0) == JW_PAKRAT_THEME_ACTION_APPLY);
    app.app_present = 0;
    assert(jw_pakrat_theme_primary_action(&app, 1, 0) == JW_PAKRAT_THEME_ACTION_NONE);
    assert(jw_pakrat_theme_install_precheck(&app) == JW_PAKRAT_REFUSED_WITHDRAWN);
    app = theme_state(JW_PAKRAT_APP_AVAILABLE, 0);
    app.package.withdrawn = 1;
    assert(jw_pakrat_theme_primary_action(&app, 0, 0) == JW_PAKRAT_THEME_ACTION_NONE);

    /* Apps never get theme actions. */
    app.package.kind = JW_PAKRAT_KIND_APP;
    assert(jw_pakrat_theme_primary_action(&app, 1, 0) == JW_PAKRAT_THEME_ACTION_NONE);
    assert(jw_pakrat_theme_install_precheck(&app) == JW_PAKRAT_REFUSED_NONE);
    assert(jw_pakrat_theme_primary_action(NULL, 1, 0) == JW_PAKRAT_THEME_ACTION_NONE);
}

static void expect_content(const char *selected, const char *installed,
                           int present, int content_only,
                           jw_pakrat_app_status expected_status,
                           int expected_action) {
    int action = -1;
    assert(jw_pakrat_resolve_owned_state(selected, installed, present,
                                         content_only, &action)
           == expected_status);
    assert(action == expected_action);
}

static void expect(const char *selected, const char *installed, int present,
                   jw_pakrat_app_status expected_status,
                   int expected_action) {
    expect_content(selected, installed, present, 0, expected_status,
                   expected_action);
}

int main(void) {
    jw_pakrat_app_state app = {
        .status = JW_PAKRAT_APP_INSTALLED,
        .installed_owned = 1,
        .open_allowed = 1,
        .primary_action_allowed = 1,
    };
    assert(jw_pakrat_primary_action_opens(&app));
    app.status = JW_PAKRAT_APP_UPDATE_AVAILABLE;
    assert(!jw_pakrat_primary_action_opens(&app));
    app.status = JW_PAKRAT_APP_INSTALLED;
    app.primary_action_allowed = 0; /* Catalog gates must not block Open. */
    assert(jw_pakrat_primary_action_opens(&app));
    app.open_allowed = 0; /* Content-only paks have no app to open. */
    assert(!jw_pakrat_primary_action_opens(&app));
    app.open_allowed = 1;
    app.managed = 1;
    assert(!jw_pakrat_primary_action_opens(&app));
    app.managed = 0;
    app.installed_owned = 0;
    assert(!jw_pakrat_primary_action_opens(&app));
    assert(!jw_pakrat_primary_action_opens(NULL));

    expect("0.2.0", "0.2.0", 1, JW_PAKRAT_APP_INSTALLED, 1);
    expect("0.3.0", "0.2.0", 1, JW_PAKRAT_APP_UPDATE_AVAILABLE, 1);
    expect("0.1.2", "0.2.0", 1, JW_PAKRAT_APP_INSTALLED, 0);
    expect("0.2.0", "v0.2.0", 1, JW_PAKRAT_APP_INSTALLED, 0);
    expect("0.2.0", "0.2.0", 0, JW_PAKRAT_APP_STALE, 1);
    expect("0.1.2", "0.2.0", 0, JW_PAKRAT_APP_STALE, 0);

    /* CONTENT-1: a pure content pak is absent from Apps BY DESIGN. Reporting
       it as Stale would offer a repair for a pak that is installed correctly,
       so absence only means damage when the pak was supposed to be listed. */
    expect_content("0.2.0", "0.2.0", 0, 1, JW_PAKRAT_APP_INSTALLED, 1);
    expect_content("0.3.0", "0.2.0", 0, 1, JW_PAKRAT_APP_UPDATE_AVAILABLE, 1);
    expect_content("0.1.2", "0.2.0", 0, 1, JW_PAKRAT_APP_INSTALLED, 0);

    /* A hybrid is listed in Apps, so it behaves exactly like an app: if it
       goes missing from Apps, that IS damage. */
    expect_content("0.2.0", "0.2.0", 1, 0, JW_PAKRAT_APP_INSTALLED, 1);
    expect_content("0.2.0", "0.2.0", 0, 0, JW_PAKRAT_APP_STALE, 1);

    /* A theme has no Apps listing, so a missing listing is never damage. */
    {
        int action = -1;
        assert(jw_pakrat_resolve_owned_theme_state("1.0.0", "1.0.0", &action) ==
               JW_PAKRAT_APP_INSTALLED && action == 1);
        assert(jw_pakrat_resolve_owned_theme_state("1.1.0", "1.0.0", &action) ==
               JW_PAKRAT_APP_UPDATE_AVAILABLE && action == 1);
        assert(jw_pakrat_resolve_owned_theme_state("1.0.0", "1.1.0", &action) ==
               JW_PAKRAT_APP_INSTALLED && action == 0);
    }

    check_store_sections();
    check_theme_actions();

    puts("PASS pakrat-state-logic-test");
    return 0;
}
