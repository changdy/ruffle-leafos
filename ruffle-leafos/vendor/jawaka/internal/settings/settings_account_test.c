/* Settings > Accounts, RetroAchievements row. Runs without a renderer, so it
   stays independent of the font and layout checks in settings-status-test. */
#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include "internal/settings/settings.h"
#include "internal/db/db.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(const char *message) {
    fprintf(stderr, "settings-account-test: %s\n", message);
    return 1;
}

/* Settings > Accounts reads the stored RetroAchievements account through the
   same validator as the launch handoff: a row the handoff would reject shows
   as not saved, never as a truncated "Saved:" name, and Y can clear it. */
static int ra_value_is(const char *db, const char *want, const char *label) {
    jw_settings_ui ui;
    memset(&ui, 0, sizeof(ui));
    snprintf(ui.db_path, sizeof(ui.db_path), "%s", db);
    jw_settings_ui_load_ra_account(&ui);
    char value[96];
    jw_settings_ra_account_value(&ui, value, sizeof(value));
    if (strcmp(value, want) != 0) {
        fprintf(stderr, "settings-account-test: %s: RetroAchievements row shows "
                        "\"%s\", want \"%s\"\n", label, value, want);
        return 1;
    }
    return 0;
}

static int check_ra_account_display(void) {
    char dir[] = "/tmp/settings-ra-account.XXXXXX";
    if (!mkdtemp(dir)) return fail("could not create an account store dir");
    char db[PATH_MAX];
    snprintf(db, sizeof(db), "%s/library.db", dir);
    long long revision = 0;

    if (jw_db_set_setting(db, "theme_name", "default") != 0)
        return fail("could not create the account store");
    if (ra_value_is(db, "Not signed in", "never configured")) return 1;

    if (jw_db_save_ra_account(db, "player-one", "correct horse", &revision) != 0)
        return fail("could not save a synthetic account");
    if (ra_value_is(db, "Saved: player-one", "configured")) return 1;

    /* A legacy username one byte over the 63-byte limit: the old loader
       showed its first 63 bytes as "Saved:". */
    char big[65];
    memset(big, 'u', 64);
    big[64] = '\0';
    if (jw_db_set_setting(db, "retroachievements_user", big) != 0)
        return fail("could not seed an oversized legacy name");
    if (ra_value_is(db, "Not saved - sign in again", "oversized legacy name"))
        return 1;

    if (jw_db_set_setting(db, "retroachievements_user", "player-one") != 0 ||
        jw_db_set_setting(db, "retroachievements_revision", "+7") != 0)
        return fail("could not seed a malformed revision");
    if (ra_value_is(db, "Not saved - sign in again", "malformed revision"))
        return 1;

    /* Y clears the unusable row through the checked sign-out. */
    jw_settings_ui ui;
    memset(&ui, 0, sizeof(ui));
    snprintf(ui.db_path, sizeof(ui.db_path), "%s", db);
    jw_settings_ui_load_ra_account(&ui);
    ui.open = true;
    ui.screen = JW_SETTINGS_ACCOUNTS;
    ui.accounts_list.cursor = JW_ACCOUNTS_RETROACHIEVEMENTS;
    char status[96] = "";
    jw_settings_ui_handle_button(&ui, CAT_BTN_Y, status, sizeof(status), NULL);
    if (strcmp(status, "Signed out of RetroAchievements") != 0 ||
        ui.ra_account_needs_repair)
        return fail("Y did not clear an invalid RetroAchievements row");
    if (ra_value_is(db, "Not signed in", "cleared invalid row")) return 1;

    unlink(db);
    rmdir(dir);
    return 0;
}

int main(void) {
    if (check_ra_account_display()) return 1;
    printf("PASS settings-account-test\n");
    return 0;
}
