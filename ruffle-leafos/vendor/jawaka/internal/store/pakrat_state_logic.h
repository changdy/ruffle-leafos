#ifndef JW_STORE_PAKRAT_STATE_LOGIC_H
#define JW_STORE_PAKRAT_STATE_LOGIC_H

#include "internal/store/pakrat_state.h"

/* Keep the primary-action label and button dispatch in sync: an available
   update takes priority over opening the installed app. */
int jw_pakrat_primary_action_opens(const jw_pakrat_app_state *app);

/* Resolve an owned install without ever treating an older catalog selection as
   an update. Invalid versions fail closed.

   `app_present` is whether the install is listed in Apps. `content_only` is
   whether the INSTALLED pak declares `provides` and ships no executable
   launch.sh -- in which case its absence from Apps is the contract working,
   not damage, and reporting it as Stale would offer the user a repair for a
   pak that is perfectly installed. Both come from the installed pak, never
   from which storefront lane the package was listed in. */
jw_pakrat_app_status jw_pakrat_resolve_owned_state(
    const char *selected_version,
    const char *installed_version,
    int app_present,
    int content_only,
    int *out_primary_action_allowed);

/* A theme has no Apps listing to go missing from, so it is never Stale:
   installed, or an update is available. */
jw_pakrat_app_status jw_pakrat_resolve_owned_theme_state(
    const char *selected_version,
    const char *installed_version,
    int *out_primary_action_allowed);

/* The store shows apps and themes as separate sections of one listing. Fills
   out_index with the positions in `apps` whose package kind is `kind`, in
   listing order, and returns how many there are (at most max_index). */
int jw_pakrat_store_section(const jw_pakrat_app_state *apps, int count,
                            jw_pakrat_kind kind, int *out_index, int max_index);

/* The section row showing store_id, or -1. Keeps the cursor on the same
   package when a reload moves it (an uninstalled withdrawn theme drops out). */
int jw_pakrat_store_section_find(const jw_pakrat_app_state *apps,
                                 const int *index, int index_count,
                                 const char *store_id);

/* What A does on a theme's details. A theme never opens; once it is on the
   card and the launcher lists it, the useful thing is to wear it. */
typedef enum {
    JW_PAKRAT_THEME_ACTION_NONE = 0,  /* reserved name, withdrawn, gated, or no history */
    JW_PAKRAT_THEME_ACTION_INSTALL,
    JW_PAKRAT_THEME_ACTION_ADOPT,     /* a hand-made folder has the name: needs consent */
    JW_PAKRAT_THEME_ACTION_UPDATE,
    JW_PAKRAT_THEME_ACTION_REINSTALL, /* owned, but the folder is gone or unlisted */
    JW_PAKRAT_THEME_ACTION_APPLY,
    JW_PAKRAT_THEME_ACTION_APPLIED,   /* already the selected theme */
} jw_pakrat_theme_action;

/* `listed` is whether the launcher's theme scan lists the install folder, and
   `applied` whether it is the selected theme. An update wins over Apply, as it
   wins over Open for an app; a withdrawn theme keeps Apply and loses the rest. */
jw_pakrat_theme_action jw_pakrat_theme_primary_action(
    const jw_pakrat_app_state *app, int listed, int applied);

/* The refusal the installer would give before downloading anything, from the
   facts the listing already holds, so the store can explain it without a
   round trip. JW_PAKRAT_REFUSED_NONE when an install may proceed. */
jw_pakrat_refusal jw_pakrat_theme_install_precheck(const jw_pakrat_app_state *app);

#endif
