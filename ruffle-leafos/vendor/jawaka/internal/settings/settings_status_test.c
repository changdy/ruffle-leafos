#define CAT_IMPLEMENTATION
#include "catastrophe.h"
#define CAT_WIDGETS_IMPLEMENTATION
#include "catastrophe_widgets.h"

#include "internal/settings/settings.h"
#include "internal/settings/timezones.h"
#include "internal/i18n/i18n.h"
#include "internal/launcher/system_activity.h"

#include "internal/db/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int fail(const char *message) {
    fprintf(stderr, "settings-status-test: %s\n", message);
    return 1;
}

static int check_activity(void) {
    jw_system_activity activity = {0};
    if (jw_system_activity_text(&activity, 0, "Scanning")[0])
        return fail("idle System has a status line");
    activity.scan_running = true;
    snprintf(activity.scrape, sizeof(activity.scrape), "Scraping Genesis · 42/175");
    if (strcmp(jw_system_activity_text(&activity, 100, "Scanning"), activity.scrape))
        return fail("library scan hid scrape progress");
    jw_system_notice_set(&activity.feedback, "Saved", 100);
    if (strcmp(jw_system_activity_text(&activity, 6099, "Scanning"), "Saved"))
        return fail("feedback expired before six seconds");
    if (strcmp(jw_system_activity_text(&activity, 6100, "Scanning"), activity.scrape))
        return fail("feedback did not restore progress at six seconds");
    jw_system_notice_set(&activity.feedback, "Saved", 6200);
    if (strcmp(jw_system_activity_text(&activity, 12199, "Scanning"), "Saved"))
        return fail("repeated feedback did not restart its timer");
    activity.feedback.text[0] = '\0';  /* leave the originating page */
    snprintf(activity.scrape, sizeof(activity.scrape), "Scraping paused: quota");
    if (strcmp(jw_system_activity_text(&activity, 90000, "Scanning"), activity.scrape))
        return fail("paused activity expired or disappeared with page feedback");
    activity.scrape[0] = '\0';
    jw_system_notice_set(&activity.completion, "Scrape finished", 90000);
    if (strcmp(jw_system_activity_text(&activity, 90001, "Scanning"), "Scrape finished") ||
        strcmp(jw_system_activity_text(&activity, 96000, "Scanning"), "Scanning"))
        return fail("completion summary did not expire back to scan activity");
    activity.scan_running = false;
    if (jw_system_activity_text(&activity, 96000, "Scanning")[0])
        return fail("idle strip did not reclaim its space");
    jw_system_notice_set(&activity.feedback, "Saved", UINT32_MAX - 100);
    if (!jw_system_notice_remaining(&activity.feedback, 100) ||
        jw_system_notice_remaining(&activity.feedback, 6000))
        return fail("feedback expiry failed across clock wrap");
    return 0;
}

static int write_theme(const char *root, const char *dir, const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/Themes", root);
    mkdir(path, 0755);
    snprintf(path, sizeof(path), "%s/Themes/%s", root, dir);
    if (mkdir(path, 0755) != 0) return -1;
    snprintf(path, sizeof(path), "%s/Themes/%s/theme.json", root, dir);
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    fprintf(fp, "{ \"name\": \"%s\" }\n", name);
    return fclose(fp);
}

/* The Layout > Theme row and Pak Rat's Apply share one selection path, so a
   store Apply is exactly a Settings pick: same persisted key, same rebuild. */
static int check_theme_selection(void) {
    char root[] = "/tmp/settings-status-test.XXXXXX";
    if (!mkdtemp(root)) return fail("could not create a themes root");
    if (write_theme(root, "aurora", "Aurora") || write_theme(root, "neon-nights", "Neon Nights"))
        return fail("could not write test themes");
    jw_settings_ui ui = {0};
    ui.user_theme_index = -1;
    jw_settings_ui_set_themes_root(&ui, root);
    if (ui.user_themes.count != 2) return fail("test themes were not scanned");

    int neon = jw_user_themes_find(&ui.user_themes, "neon-nights");
    char status[128] = "stale";
    if (!jw_settings_ui_select_user_theme(&ui, neon, status, sizeof(status)) ||
        strcmp(ui.user_theme_dir, "neon-nights") != 0 ||
        jw_settings_user_theme_index(&ui) != neon)
        return fail("selecting a theme did not select it");
    if (jw_settings_ui_select_user_theme(&ui, neon, status, sizeof(status)))
        return fail("reselecting the current theme reported a change");
    if (jw_settings_ui_select_user_theme(&ui, 2, NULL, 0) ||
        jw_settings_ui_select_user_theme(&ui, -2, NULL, 0) ||
        strcmp(ui.user_theme_dir, "neon-nights") != 0)
        return fail("an out-of-range selection changed the theme");

    /* The Appearance > Theme row cycles through the same function and raises
       the flag. */
    ui.open = true;
    ui.screen = JW_SETTINGS_APPEARANCE;
    ui.appearance_list.cursor = JW_APPEAR_THEME;
    bool theme_changed = false;
    jw_settings_ui_handle_button(&ui, CAT_BTN_RIGHT, status, sizeof(status), &theme_changed);
    if (!theme_changed || ui.user_theme_dir[0] || jw_settings_user_theme_index(&ui) != -1)
        return fail("cycling past the last theme did not select None");
    if (status[0]) return fail("selecting None kept a theme's status");

    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) return fail("could not remove the themes root");
    return 0;
}

/* ─── Time Zone picker ──────────────────────────────────────────────────── */

static int tz_row(const char *tz) {
    for (int i = 0; i < kJawakaTimeZoneCount; ++i)
        if (strcmp(kJawakaTimeZones[i].tz, tz) == 0) return i;
    return -1;
}

/* The picker's offset column ("UTC", "UTC+0", "UTC-9:30") as seconds. */
#define JW_TZ_OFFSET_BAD 999999L
static long offset_seconds(const char *off) {
    if (!off || strncmp(off, "UTC", 3) != 0) return JW_TZ_OFFSET_BAD;
    if (!off[3]) return 0;
    int h = 0, m = 0;
    if (sscanf(off + 4, "%d:%d", &h, &m) < 1) return JW_TZ_OFFSET_BAD;
    long seconds = h * 3600L + m * 60L;
    return off[3] == '-' ? -seconds : seconds;
}

/* The rows this issue exists for, plus the fractional-offset regions that are
   the reason a whole-hour-only list is not enough. Leaf #72 asked for New
   Zealand; a picker that gained the row and lost Chatham or Auckland to a
   later edit is the regression worth catching by name. */
static int check_timezone_catalog(void) {
    static const char *kRequired[] = {
        "Pacific/Auckland", "Pacific/Chatham",
        "Pacific/Marquesas", "America/St_Johns", "Asia/Kabul",
        "Asia/Kathmandu", "Asia/Yangon", "Australia/Eucla", "Australia/Adelaide",
        "Australia/Darwin", "Australia/Lord_Howe",
        /* The list this change grew out of must not lose its old rows either:
           an existing selection has to survive the upgrade. */
        "Pacific/Honolulu", "America/Anchorage", "America/Los_Angeles",
        "America/Denver", "America/Phoenix", "America/Chicago",
        "America/New_York", "America/Sao_Paulo", "UTC", "Europe/London",
        "Europe/Paris", "Europe/Athens", "Asia/Kolkata", "Asia/Shanghai",
        "Asia/Tokyo", "Australia/Sydney",
    };
    for (unsigned i = 0; i < sizeof(kRequired) / sizeof(kRequired[0]); ++i)
        if (tz_row(kRequired[i]) < 0) {
            fprintf(stderr, "settings-status-test: %s is missing from the picker\n",
                    kRequired[i]);
            return 1;
        }

    /* Ordered by standard offset. Rows are found by id, so a reorder is safe
       for saved settings, but an unordered picker is unusable at this length. */
    long previous = -13 * 3600;
    for (int i = 0; i < kJawakaTimeZoneCount; ++i) {
        long seconds = offset_seconds(kJawakaTimeZones[i].off);
        if (seconds == JW_TZ_OFFSET_BAD) {
            fprintf(stderr, "settings-status-test: unreadable offset \"%s\"\n",
                    kJawakaTimeZones[i].off);
            return 1;
        }
        if (seconds < previous) {
            fprintf(stderr, "settings-status-test: %s (%s) is out of offset order\n",
                    kJawakaTimeZones[i].tz, kJawakaTimeZones[i].off);
            return 1;
        }
        previous = seconds;
    }

    /* Every whole hour from -12 to +14 is reachable, and reachable by naming a
       place rather than an offset. The picker used to carry a parallel set of
       "UTC+N (fixed)" rows; they duplicated the offset column and, worse, opted
       the user out of the daylight-saving handling that is the reason to pick a
       region at all. Coverage is the part worth keeping. */
    for (int hour = -12; hour <= 14; ++hour) {
        bool found = false;
        for (int i = 0; i < kJawakaTimeZoneCount && !found; ++i)
            found = (offset_seconds(kJawakaTimeZones[i].off) == hour * 3600L);
        if (!found) {
            fprintf(stderr, "settings-status-test: no row offers UTC%+d\n", hour);
            return 1;
        }
    }

    /* Labels name places. Baker Island is the single exception allowed to carry
       an Etc/GMT id, because UTC-12 has no inhabited territory and therefore no
       IANA place id; its sign is reversed from the offset it produces, which is
       how the etcetera file defines it and is exactly the trap worth pinning. */
    for (int i = 0; i < kJawakaTimeZoneCount; ++i) {
        const char *tz = kJawakaTimeZones[i].tz;
        if (strncmp(tz, "Etc/", 4) != 0) continue;
        if (strcmp(kJawakaTimeZones[i].label, "Baker Island") != 0 ||
            strcmp(tz, "Etc/GMT+12") != 0 ||
            offset_seconds(kJawakaTimeZones[i].off) != -12 * 3600L) {
            fprintf(stderr, "settings-status-test: unexpected fixed-offset row "
                            "\"%s\" (%s)\n", kJawakaTimeZones[i].label, tz);
            return 1;
        }
    }
    for (int i = 0; i < kJawakaTimeZoneCount; ++i)
        if (strstr(kJawakaTimeZones[i].label, "(fixed)"))
            return fail("a fixed-offset row came back into the picker");

    /* Labels are drawn into a 48-byte buffer that also carries the "* " current
       marker, and they share a row with the offset column. */
    for (int i = 0; i < kJawakaTimeZoneCount; ++i) {
        if (strlen(kJawakaTimeZones[i].label) + 3 > 48)
            return fail("a picker label does not fit the row buffer");
        for (const char *c = kJawakaTimeZones[i].label; *c; ++c)
            if ((unsigned char)*c > 127)
                return fail("a picker label left ASCII (the font subset has no glyph)");
    }
    return 0;
}

/* Lookup is by id, not row index, which is what lets the table grow without a
   database migration. */
static int check_timezone_lookup(void) {
    if (strcmp(jw_timezone_label("Pacific/Auckland"), "New Zealand") != 0)
        return fail("Auckland did not resolve to its label");
    if (strcmp(jw_timezone_label(""), "System default") != 0)
        return fail("no selection did not read as the system default");
    if (strcmp(jw_timezone_label("Africa/Nairobi"), "Africa/Nairobi") != 0)
        return fail("an unknown saved id was not shown verbatim");
    if (jw_timezone_index_of("Pacific/Chatham") != tz_row("Pacific/Chatham"))
        return fail("Chatham's row lookup disagreed with the table");
    /* Row 0 is UTC-12. Opening the picker there with nothing saved would read
       as a default, so both the empty and the unknown case land on UTC. */
    if (jw_timezone_index_of("") != tz_row("UTC") ||
        jw_timezone_index_of("Africa/Nairobi") != tz_row("UTC"))
        return fail("the no-selection cursor did not fall back to UTC");
    if (tz_row("UTC") == 0)
        return fail("UTC is row 0, so the fallback check proves nothing");
    return 0;
}

/* The real button path: open the picker from the System row, move, select,
   and confirm what reaches the database. */
static int check_timezone_selection(void) {
    char db[] = "/tmp/settings-status-tz.XXXXXX";
    int fd = mkstemp(db);
    if (fd < 0) return fail("could not create a settings db");
    close(fd);
    unlink(db);   /* jw_db_set_setting creates it; an empty file is not a db */

    jw_settings_ui ui = {0};
    char status[128] = "";
    ui.open = true;
    snprintf(ui.db_path, sizeof(ui.db_path), "%s", db);
    cat_list_state_init(&ui.timezone_picker_list, 7);

    /* Nothing saved: the picker opens on UTC. Time Zone is the first System row
       when there is no Language row, which a zeroed ui has; if that stops being
       true, A below opens something else and this fails by name. */
    ui.screen = JW_SETTINGS_SYSTEM;
    ui.system_list.cursor = 0;
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_TIMEZONE_PICKER)
        return fail("A on the first System row did not open the Time Zone picker");
    if (ui.timezone_picker_list.cursor != tz_row("UTC"))
        return fail("an unset picker did not open on UTC");

    /* B leaves without saving: merely looking is not a change. */
    jw_settings_ui_handle_button(&ui, CAT_BTN_DOWN, status, sizeof(status), NULL);
    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_SYSTEM) return fail("B did not leave the picker");
    if (ui.timezone[0]) return fail("canceling the picker saved a zone");
    char saved[64] = "";
    if (jw_db_get_setting(db, "timezone", saved, sizeof(saved)) == 0 && saved[0])
        return fail("canceling the picker wrote to the database");

    /* Select New Zealand through A, then read it back out of the database. */
    static const char *kPicks[] = { "Pacific/Auckland", "Pacific/Chatham",
                                    "Pacific/Kiritimati", "Etc/GMT+12",
                                    "Europe/Paris" };
    for (unsigned i = 0; i < sizeof(kPicks) / sizeof(kPicks[0]); ++i) {
        ui.screen = JW_SETTINGS_TIMEZONE_PICKER;
        ui.timezone_picker_list.cursor = tz_row(kPicks[i]);
        status[0] = '\0';
        jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
        if (ui.screen != JW_SETTINGS_SYSTEM)
            return fail("selecting a zone did not return to System");
        if (strcmp(ui.timezone, kPicks[i]) != 0)
            return fail("the selected zone did not reach the settings state");
        saved[0] = '\0';
        if (jw_db_get_setting(db, "timezone", saved, sizeof(saved)) != 0 ||
            strcmp(saved, kPicks[i]) != 0) {
            fprintf(stderr, "settings-status-test: picked %s, database holds \"%s\"\n",
                    kPicks[i], saved);
            return 1;
        }
        /* The id is persisted, never the displayed offset: "UTC+12" handed to
           libc means the opposite of what the row promises. */
        if (strncmp(saved, "UTC", 3) == 0 && strcmp(saved, "UTC") != 0)
            return fail("a display string was persisted instead of a zone id");
        if (!status[0]) return fail("selecting a zone reported nothing");

        /* Reopening lands on the saved row, and it is inside the viewport --
           the case that matters now the list runs past one screen. */
        jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
        if (ui.screen != JW_SETTINGS_TIMEZONE_PICKER)
            return fail("reopening the picker failed");
        int cur = ui.timezone_picker_list.cursor;
        int top = ui.timezone_picker_list.scroll_offset;
        if (cur != tz_row(kPicks[i]))
            return fail("reopening did not land on the saved zone");
        if (cur < top || cur >= top + ui.timezone_picker_list.visible_rows) {
            fprintf(stderr, "settings-status-test: %s sits at row %d, outside rows "
                            "%d..%d\n", kPicks[i], cur, top,
                    top + ui.timezone_picker_list.visible_rows - 1);
            return 1;
        }
        jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    }

    /* Up from the first row wraps to the last, so the far end of a list this
       long is reachable without holding Down through fifty rows. */
    ui.screen = JW_SETTINGS_TIMEZONE_PICKER;
    ui.timezone_picker_list.cursor = 0;
    jw_settings_ui_handle_button(&ui, CAT_BTN_UP, status, sizeof(status), NULL);
    if (ui.timezone_picker_list.cursor != kJawakaTimeZoneCount - 1)
        return fail("Up from the first row did not reach the last");

    unlink(db);
    return 0;
}

/* Render at device size with a software renderer. Sentinel pixels catch text,
   sliders, swatches or highlights escaping the allocated page rectangle. */
/* Force Off Hold sits right after Auto Sleep (row 2 with no Language row).
   On the MLP1 it cycles 6/8/10/12 s, persists the seconds and names them in
   the status; everywhere else it is "Unavailable" and a press changes nothing. */
static int check_force_off_hold(void) {
    char db[] = "/tmp/settings-status-foh.XXXXXX";
    int fd = mkstemp(db);
    if (fd < 0) return fail("could not create a settings db");
    close(fd);
    unlink(db);

    jw_settings_ui ui = {0};
    char status[128] = "";
    char saved[32] = "";
    ui.open = true;
    snprintf(ui.db_path, sizeof(ui.db_path), "%s", db);
    ui.screen = JW_SETTINGS_SYSTEM;
    ui.system_list.cursor = 2;
    ui.force_off_hold_index = 2;   /* 10 s, the default */

#ifdef PLATFORM_MLP1
    jw_settings_ui_handle_button(&ui, CAT_BTN_RIGHT, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_SYSTEM) return fail("Right on Force Off Hold left System");
    if (ui.force_off_hold_index != 3) return fail("Right did not move 10 s to 12 s");
    if (jw_db_get_setting(db, "power_hard_cut_seconds", saved, sizeof(saved)) != 0 ||
        strcmp(saved, "12") != 0)
        return fail("12 s did not reach power_hard_cut_seconds");
    if (!strstr(status, "12 seconds")) return fail("status did not name the 12 s hold");

    jw_settings_ui_handle_button(&ui, CAT_BTN_RIGHT, status, sizeof(status), NULL);
    if (ui.force_off_hold_index != 0) return fail("Right past 12 s did not wrap to 6 s");
    saved[0] = '\0';
    if (jw_db_get_setting(db, "power_hard_cut_seconds", saved, sizeof(saved)) != 0 ||
        strcmp(saved, "6") != 0)
        return fail("6 s did not reach power_hard_cut_seconds");

    jw_settings_ui_handle_button(&ui, CAT_BTN_LEFT, status, sizeof(status), NULL);
    if (ui.force_off_hold_index != 3) return fail("Left from 6 s did not wrap to 12 s");
#else
    jw_settings_ui_handle_button(&ui, CAT_BTN_RIGHT, status, sizeof(status), NULL);
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_SYSTEM) return fail("Force Off Hold opened something");
    if (ui.force_off_hold_index != 2) return fail("an unavailable Force Off Hold cycled");
    if (status[0]) return fail("an unavailable Force Off Hold set a status");
    if (jw_db_get_setting(db, "power_hard_cut_seconds", saved, sizeof(saved)) == 0 && saved[0])
        return fail("an unavailable Force Off Hold wrote to the database");
#endif
    unlink(db);
    return 0;
}

/* Save Before Power Off and Resume Game on Boot sit on the Games page after
   Game Performance. Resume switches exactly where the save row does, persists
   resume_game_on_boot, and says when the save setting has to come first.
   Neither is a System row any more. */
static int check_boot_resume(void) {
    char db[] = "/tmp/settings-status-resume.XXXXXX";
    int fd = mkstemp(db);
    if (fd < 0) return fail("could not create a settings db");
    close(fd);
    unlink(db);

    jw_settings_ui ui = {0};
    char status[256] = "";
    char saved[32] = "";
    ui.open = true;
    snprintf(ui.db_path, sizeof(ui.db_path), "%s", db);
    ui.screen = JW_SETTINGS_GAMES;
    ui.power_hold_save_supported = true;   /* as on the MLP1 */

    ui.games_list.cursor = JW_GAMES_POWER_HOLD_SAVE;
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (!ui.power_hold_save_enabled || ui.boot_resume_enabled)
        return fail("the Games row did not switch Save Before Power Off");
    if (jw_db_get_setting(db, "save_state_on_power_hold", saved, sizeof(saved)) != 0 ||
        strcmp(saved, "1") != 0)
        return fail("On did not reach save_state_on_power_hold");
    if (strcmp(status, "Hold power to shut down, then release to save") != 0)
        return fail("Save Before Power Off status is wrong");
    jw_settings_ui_handle_button(&ui, CAT_BTN_LEFT, status, sizeof(status), NULL);
    if (ui.power_hold_save_enabled) return fail("Left did not switch the save row off");

    ui.games_list.cursor = JW_GAMES_BOOT_RESUME;
    status[0] = '\0';
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_GAMES) return fail("Resume Game on Boot left Games");
    if (!ui.boot_resume_enabled) return fail("A did not switch Resume Game on Boot on");
    if (jw_db_get_setting(db, "resume_game_on_boot", saved, sizeof(saved)) != 0 ||
        strcmp(saved, "1") != 0)
        return fail("On did not reach resume_game_on_boot");
    if (strcmp(status, "Turn on Save Before Power Off first") != 0)
        return fail("On with the save setting Off did not ask for it first");

    jw_settings_ui_handle_button(&ui, CAT_BTN_RIGHT, status, sizeof(status), NULL);
    if (ui.boot_resume_enabled) return fail("Right did not switch Resume Game on Boot off");
    saved[0] = '\0';
    if (jw_db_get_setting(db, "resume_game_on_boot", saved, sizeof(saved)) != 0 ||
        strcmp(saved, "0") != 0)
        return fail("Off did not reach resume_game_on_boot");
    if (strcmp(status, "Your game will not resume at startup") != 0)
        return fail("Off status is wrong");

    ui.power_hold_save_enabled = true;
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (!ui.boot_resume_enabled) return fail("A did not switch it back on");
    if (strcmp(status, "Continue your game when you turn on your console after saving "
                       "with the power button. Hold B during startup to skip.") != 0)
        return fail("On status is not the plan's text");

    /* Wherever Save Before Power Off is unavailable, so is this. */
    ui.power_hold_save_supported = false;
    ui.boot_resume_enabled = false;
    status[0] = '\0';
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    jw_settings_ui_handle_button(&ui, CAT_BTN_RIGHT, status, sizeof(status), NULL);
    if (ui.boot_resume_enabled) return fail("an unavailable Resume Game on Boot switched");
    if (status[0]) return fail("an unavailable Resume Game on Boot set a status");
    saved[0] = '\0';
    if (jw_db_get_setting(db, "resume_game_on_boot", saved, sizeof(saved)) != 0 ||
        strcmp(saved, "1") != 0)
        return fail("an unavailable Resume Game on Boot wrote to the database");

    /* The System page no longer has them: walking every row there changes
       neither key. */
    ui.power_hold_save_supported = true;
    ui.power_hold_save_enabled = false;
    ui.boot_resume_enabled = false;
    ui.screen = JW_SETTINGS_SYSTEM;
    for (int row = 0; row < 8; row++) {
        ui.system_list.cursor = row;
        jw_settings_ui_handle_button(&ui, CAT_BTN_RIGHT, status, sizeof(status), NULL);
        ui.screen = JW_SETTINGS_SYSTEM;
    }
    if (ui.power_hold_save_enabled || ui.boot_resume_enabled)
        return fail("a System row still switches a power-off setting");
    unlink(db);
    return 0;
}

static int check_color_temperature(SDL_Renderer *renderer, uint32_t *pixels,
                                   const char *platform) {
    setenv("UMRK_PLATFORM_PATH", platform, 1);
    if (!jw_i18n_load("ru_RU")) return fail("could not load the Russian translation");
    static const struct {
        bool supported, hdmi;
        int kelvin;
        const char *value;
    } cases[] = {
        { true,  false, JW_PLATFORM_COLOR_TEMP_NEUTRAL_K, "Neutral" },
        { false, false, JW_PLATFORM_COLOR_TEMP_NEUTRAL_K, "Unavailable" },
        { true,  true,  JW_PLATFORM_COLOR_TEMP_NEUTRAL_K, "Panel only" },
        { true,  false, JW_PLATFORM_COLOR_TEMP_MAX_K,     "10000 K" },
    };
    for (int bump = 0; bump <= CAT_FONT_BUMP_MAX; ++bump) {
        if (cat_set_font_bump(bump) != CAT_OK) return fail("font bump failed");
        for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            jw_settings_ui ui = {0};
            ui.open = true;
            ui.screen = JW_SETTINGS_DISPLAY;
            ui.color_temp_supported = cases[i].supported;
            ui.color_temp_kelvin = cases[i].kelvin;
            ui.hdmi_supported = true; /* No unrelated "Unavailable" in the HDMI row. */
            ui.hdmi_connected = cases[i].hdmi;
            ui.hdmi_output_mode = cases[i].hdmi ? 1 : 0;
            cat_list_state_init(&ui.display_list, JW_DISPLAY_ROW_COUNT);
            ui.display_list.cursor = JW_DISPLAY_COLOR_TEMP;
            cat__text_cache_clear();
            SDL_SetRenderDrawColor(renderer, 13, 29, 47, 255);
            SDL_RenderClear(renderer);
            jw_settings_ui_render(&ui, 12, 60, 936, 620);

            /* Measurement does not populate this cache: the full value must
               reach the renderer, rather than a byte-truncated UTF-8 prefix. */
            const char *expected = T(cases[i].value);
            bool rendered = false;
            for (int j = 0; j < cat__g.text_cache.count; ++j)
                if (!strcmp(cat__g.text_cache.entries[j].text, expected))
                    rendered = true;
            if (!rendered) {
                fprintf(stderr, "settings-status-test: color temperature lost %s "
                                "at font bump %d\n", expected, bump);
                return 1;
            }
            if (SDL_RenderIsClipEnabled(renderer)) return fail("display page leaked its clip");
            if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, pixels,
                                     960 * sizeof(*pixels)) != 0)
                return fail("could not inspect the display page");
            for (int y = 0; y < 720; ++y)
                for (int x = 0; x < 960; ++x)
                    if ((x < 12 || x >= 948 || y < 60 || y >= 680) &&
                        pixels[y * 960 + x] != 0xff0d1d2f)
                        return fail("Russian display page drew outside its viewport");
        }
    }
    jw_i18n_shutdown();
    unsetenv("UMRK_PLATFORM_PATH");
    return 0;
}

static int check_layout_viewport(const char *platform) {
    SDL_setenv("SDL_VIDEODRIVER", "dummy", 1);
    SDL_setenv("CAT_WINDOW_WIDTH", "960", 1);
    SDL_setenv("CAT_WINDOW_HEIGHT", "720", 1);
    char font[4096];
    const char *fonts = getenv("CAT_FONTS_DIR");
    snprintf(font, sizeof(font), "%s/fonts/Nunito/Nunito-Bold.ttf", fonts ? fonts : "res");
    cat_config config = { .start_hidden = true, .defer_input_init = true,
                          .disable_background = true, .disable_font_bump = true,
                          .font_path = font };
    if (cat_init(&config) != CAT_OK) return fail("could not initialize render check");
    SDL_Renderer *renderer = cat_get_renderer();
    uint32_t *pixels = malloc(960 * 720 * sizeof(*pixels));
    if (!pixels) return fail("pixel allocation failed");
    jw_settings_ui ui = {0};
    ui.open = true;
    ui.screen = JW_SETTINGS_HOME_SCREEN;
    ui.user_theme_index = -1;
    cat_list_state_init(&ui.home_screen_list, JW_HOMESCREEN_ROW_COUNT);
    ui.home_screen_list.cursor = JW_HOMESCREEN_TABS;
    for (int bump = 2; bump <= 5; bump += 3) {
        if (cat_set_font_bump(bump) != CAT_OK) return fail("font bump failed");
        /* Expanded/shrunk/restored: hints and activity taking or releasing space. */
        const int heights[] = { 620, 500, 450, 620 };
        for (unsigned i = 0; i < sizeof(heights) / sizeof(heights[0]); ++i) {
            SDL_SetRenderDrawColor(renderer, 13, 29, 47, 255);
            SDL_RenderClear(renderer);
            jw_settings_ui_render(&ui, 12, 60, 936, heights[i]);
            if (ui.home_screen_list.cursor != JW_HOMESCREEN_TABS ||
                ui.home_screen_list.cursor < ui.home_screen_list.scroll_offset ||
                ui.home_screen_list.cursor >= ui.home_screen_list.scroll_offset + ui.home_screen_list.visible_rows)
                return fail("viewport resize lost the selected Home Tabs row");
            int row_h = TTF_FontHeight(cat_get_font(CAT_FONT_MEDIUM)) + cat_scale(12);
            int header_h = TTF_FontHeight(cat_get_font(CAT_FONT_LARGE)) + cat_scale(10);
            if (ui.home_screen_list.visible_rows * row_h > heights[i] - header_h)
                return fail("visible rows exceed the available content height");
            if (SDL_RenderIsClipEnabled(renderer)) return fail("page leaked its clip");
            if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, pixels,
                                     960 * sizeof(*pixels)) != 0)
                return fail("could not inspect rendered pixels");
            ap_color text = cat_get_theme()->highlighted_text;
            uint32_t ink = 0xff000000u | (uint32_t)text.r << 16 |
                           (uint32_t)text.g << 8 | text.b;
            int selected_y = 60 + header_h +
                (ui.home_screen_list.cursor - ui.home_screen_list.scroll_offset) * row_h;
            bool label_visible = false;
            for (int y = selected_y + 6; y < selected_y + row_h - 6; ++y)
                for (int x = 48; x < 240; ++x)
                    if (pixels[y * 960 + x] == ink) label_visible = true;
            if (!label_visible) return fail("selected row label did not scroll with its highlight");
            for (int y = 0; y < 720; ++y)
                for (int x = 0; x < 960; ++x)
                    if ((x < 12 || x >= 948 || y < 60 || y >= 60 + heights[i]) &&
                        pixels[y * 960 + x] != 0xff0d1d2f)
                        return fail("settings drew outside its viewport");
            cat_list_state_move(&ui.home_screen_list, -1, JW_HOMESCREEN_ROW_COUNT);
            cat_list_state_move(&ui.home_screen_list, 1, JW_HOMESCREEN_ROW_COUNT);
        }
    }

    /* The Time Zone picker at the same sizes. It is now the longest list in
       Settings by a wide margin, and its subheader is the one line of copy that
       explains what the offset column means -- a subheader clipped to
       "Offsets are standard ti..." would leave the column unexplained, which is
       what the sentence exists to prevent. The last row is the interesting
       scroll position: that is where a list this long overruns its pane. */
    jw_settings_ui tz = {0};
    tz.open = true;
    tz.screen = JW_SETTINGS_TIMEZONE_PICKER;
    tz.user_theme_index = -1;
    cat_list_state_init(&tz.timezone_picker_list, 7);
    snprintf(tz.timezone, sizeof(tz.timezone), "%s", "Pacific/Auckland");
    for (int bump = 2; bump <= 5; bump += 3) {
        if (cat_set_font_bump(bump) != CAT_OK) return fail("font bump failed");
        const int heights[] = { 620, 450 };
        for (unsigned i = 0; i < sizeof(heights) / sizeof(heights[0]); ++i) {
            for (int pass = 0; pass < 2; ++pass) {
                /* First at the saved row, then at the very end of the list. */
                cat_list_state_jump(&tz.timezone_picker_list,
                                    pass ? kJawakaTimeZoneCount - 1
                                         : jw_timezone_index_of(tz.timezone),
                                    kJawakaTimeZoneCount);
                SDL_SetRenderDrawColor(renderer, 13, 29, 47, 255);
                SDL_RenderClear(renderer);
                jw_settings_ui_render(&tz, 12, 60, 936, heights[i]);
                if (SDL_RenderIsClipEnabled(renderer)) return fail("the picker leaked its clip");
                int cur = tz.timezone_picker_list.cursor;
                int top = tz.timezone_picker_list.scroll_offset;
                if (cur < top || cur >= top + tz.timezone_picker_list.visible_rows)
                    return fail("the picker scrolled its cursor out of view");
                if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, pixels,
                                         960 * sizeof(*pixels)) != 0)
                    return fail("could not inspect the rendered picker");
                for (int y = 0; y < 720; ++y)
                    for (int x = 0; x < 960; ++x)
                        if ((x < 12 || x >= 948 || y < 60 || y >= 60 + heights[i]) &&
                            pixels[y * 960 + x] != 0xff0d1d2f)
                            return fail("the picker drew outside its viewport");
            }
        }
        /* The subheader has to fit the pane, not just stay inside the window:
           cat_draw_text_ellipsized would silently cut it otherwise. Measured
           against the same width the renderer passes. */
        const char *sub = T("Offsets are standard time; regions adjust for DST");
        if (cat_measure_text(cat_get_font(CAT_FONT_SMALL), sub) > 936 - cat_scale(24)) {
            fprintf(stderr, "settings-status-test: the picker subheader is clipped at "
                            "font bump %d\n", bump);
            return 1;
        }
    }
    jw_settings_ui hidden = {0};
    hidden.open = true;
    hidden.screen = JW_SETTINGS_HIDDEN_GAMES;
    hidden.hidden_games_loaded = true;
    hidden.hidden_games_count = 80;
    hidden.hidden_games = calloc(80, sizeof(*hidden.hidden_games));
    if (!hidden.hidden_games) return fail("hidden game render allocation failed");
    cat_list_state_init(&hidden.hidden_games_list, 7);
    for (int i = 0; i < hidden.hidden_games_count; ++i) {
        snprintf(hidden.hidden_games[i].rom.game.source_id, sizeof(hidden.hidden_games[i].rom.game.source_id),
                 "%s", "secondary_sd");
        snprintf(hidden.hidden_games[i].rom.game.rom_relpath, sizeof(hidden.hidden_games[i].rom.game.rom_relpath),
                 "SFC/Hidden game %03d.sfc", i);
    }
    for (int bump = 2; bump <= 5; bump += 3) {
        if (cat_set_font_bump(bump) != CAT_OK) return fail("font bump failed");
        cat_list_state_jump(&hidden.hidden_games_list, 79, 80);
        SDL_SetRenderDrawColor(renderer, 13, 29, 47, 255);
        SDL_RenderClear(renderer);
        jw_settings_ui_render(&hidden, 12, 60, 936, 450);
        if (hidden.hidden_games_list.cursor != 79 ||
            79 < hidden.hidden_games_list.scroll_offset ||
            79 >= hidden.hidden_games_list.scroll_offset + hidden.hidden_games_list.visible_rows)
            return fail("last hidden game was not visible");
        if (SDL_RenderIsClipEnabled(renderer)) return fail("hidden games leaked its clip");
        if (SDL_RenderReadPixels(renderer, NULL, SDL_PIXELFORMAT_ARGB8888, pixels,
                                 960 * sizeof(*pixels)) != 0)
            return fail("could not inspect hidden games");
        for (int y = 0; y < 720; ++y)
            for (int x = 0; x < 960; ++x)
                if ((x < 12 || x >= 948 || y < 60 || y >= 510) &&
                    pixels[y * 960 + x] != 0xff0d1d2f)
                    return fail("hidden games drew outside its viewport");
    }
    jw_settings_ui_close(&hidden);
    if (check_color_temperature(renderer, pixels, platform)) return 1;
    free(pixels);
    cat_quit();
    return 0;
}

/* Languages after English come out in display-name order however the files were
   written. The list used to be raw readdir() order, which on FAT32 tracks write
   order: writing the files in scrambled order here is what makes this a test of
   the sort rather than of the directory happening to agree with it. */
static int check_language_order(void) {
    char root[] = "/tmp/settings-lang-order.XXXXXX";
    if (!mkdtemp(root)) return fail("could not create a language root");
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/i18n", root);
    if (mkdir(dir, 0755) != 0) return fail("could not create i18n dir");
    const char *scrambled[] = { "zh_CN", "fr_FR", "ja_JP", "es_MX" };
    for (unsigned i = 0; i < sizeof(scrambled) / sizeof(scrambled[0]); ++i) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s.tsv", dir, scrambled[i]);
        FILE *fp = fopen(path, "w");
        if (!fp) return fail("could not write a language table");
        fputs("Hello\tHello\n", fp);
        fclose(fp);
    }
    setenv("UMRK_INTERNAL_DATA_PATH", root, 1);
    unsetenv("UMRK_PLATFORM_PATH");

    jw_settings_ui ui;
    memset(&ui, 0, sizeof(ui));
    jw_settings_ui_init(&ui, "", "Jawaka-Tabs", "");
    static const char *expect[] = { "en", "es_MX", "fr_FR", "zh_CN", "ja_JP" };
    int want = (int)(sizeof(expect) / sizeof(expect[0]));
    if (ui.language_count != want) {
        fprintf(stderr, "settings-status-test: %d languages, expected %d\n",
                ui.language_count, want);
        return 1;
    }
    for (int i = 0; i < want; ++i) {
        if (strcmp(ui.languages[i], expect[i]) != 0) {
            fprintf(stderr, "settings-status-test: language %d is %s, expected %s\n",
                    i, ui.languages[i], expect[i]);
            return 1;
        }
    }
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) return fail("could not remove the language root");
    unsetenv("UMRK_INTERNAL_DATA_PATH");
    return 0;
}

/* Every language the scanner returns reaches the Settings list, English
   included. The scanner used to keep 8 while this list kept 7 after English, so
   the last language vanished; filling the scanner to its limit is what catches
   that class of mismatch. */
static int check_language_capacity(void) {
    char root[] = "/tmp/settings-lang-cap.XXXXXX";
    if (!mkdtemp(root)) return fail("could not create a language root");
    char dir[PATH_MAX];
    snprintf(dir, sizeof(dir), "%s/i18n", root);
    if (mkdir(dir, 0755) != 0) return fail("could not create i18n dir");
    for (int i = 0; i < JW_I18N_MAX_LANGUAGES; i++) {
        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/l%02d.tsv", dir, i);
        FILE *fp = fopen(path, "w");
        if (!fp) return fail("could not write a language table");
        fputs("Hello\tHello\n", fp);
        fclose(fp);
    }
    setenv("UMRK_INTERNAL_DATA_PATH", root, 1);
    unsetenv("UMRK_PLATFORM_PATH");
    jw_settings_ui ui;
    memset(&ui, 0, sizeof(ui));
    jw_settings_ui_init(&ui, "", "Jawaka-Tabs", "");
    if (ui.language_count != JW_I18N_MAX_LANGUAGES + 1) {
        fprintf(stderr, "settings-status-test: %d languages listed, expected %d "
                "(English + %d)\n", ui.language_count, JW_I18N_MAX_LANGUAGES + 1,
                JW_I18N_MAX_LANGUAGES);
        return 1;
    }
    if (strcmp(ui.languages[0], "en") != 0) return fail("English is not first");
    char cmd[PATH_MAX + 16];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", root);
    if (system(cmd) != 0) return fail("could not remove the language root");
    unsetenv("UMRK_INTERNAL_DATA_PATH");
    return 0;
}

/* Every child page hands B back to the page that opens it. This is the check
   that reorganizing Settings needs: moving a page under a new parent means
   retargeting its B, and a stale target strands the user on an unrelated screen
   with no sign anything is wrong. (Bluetooth did exactly that, landing on Wi-Fi
   after it stopped being a row there.) */
static int check_back_targets(void) {
    static const struct {
        jw_settings_screen screen;
        jw_settings_screen parent;
    } kBack[] = {
        { JW_SETTINGS_COLORS,              JW_SETTINGS_APPEARANCE  },
        { JW_SETTINGS_STATUS_BAR,          JW_SETTINGS_APPEARANCE  },
        { JW_SETTINGS_APPEARANCE,          JW_SETTINGS_HOME        },
        { JW_SETTINGS_HOME_SCREEN,         JW_SETTINGS_HOME        },
        { JW_SETTINGS_HOME_TABS,           JW_SETTINGS_HOME_SCREEN },
        { JW_SETTINGS_DISPLAY,             JW_SETTINGS_HOME        },
        { JW_SETTINGS_LIGHTING,            JW_SETTINGS_HOME        },
        { JW_SETTINGS_WIFI,                JW_SETTINGS_HOME        },
        { JW_SETTINGS_BLUETOOTH,           JW_SETTINGS_HOME        },
        { JW_SETTINGS_GAMES,               JW_SETTINGS_HOME        },
        { JW_SETTINGS_ACCOUNTS,            JW_SETTINGS_GAMES       },
        { JW_SETTINGS_HIDDEN_GAMES,        JW_SETTINGS_GAMES       },
        { JW_SETTINGS_SCRAPE_PRIORITY,     JW_SETTINGS_GAMES       },
        { JW_SETTINGS_SCRAPE_QUEUE,        JW_SETTINGS_GAMES       },
        { JW_SETTINGS_SCRAPE_DOWNLOAD,     JW_SETTINGS_GAMES       },
        { JW_SETTINGS_SCRAPE_QUEUE_DETAIL, JW_SETTINGS_SCRAPE_QUEUE },
        { JW_SETTINGS_CONTROLS,            JW_SETTINGS_HOME        },
        { JW_SETTINGS_SYSTEM,              JW_SETTINGS_HOME        },
        { JW_SETTINGS_TIMEZONE_PICKER,     JW_SETTINGS_SYSTEM      },
        { JW_SETTINGS_SERVICES,            JW_SETTINGS_SYSTEM      },
        { JW_SETTINGS_UPDATE,              JW_SETTINGS_HOME        },
        { JW_SETTINGS_UPDATE_PICKER,       JW_SETTINGS_UPDATE      },
    };
    for (unsigned i = 0; i < sizeof(kBack) / sizeof(kBack[0]); ++i) {
        jw_settings_ui ui = {0};
        char status[64] = "";
        ui.open = true;
        ui.screen = kBack[i].screen;
        jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
        if (ui.screen != kBack[i].parent) {
            fprintf(stderr, "settings-status-test: screen %d went back to %d, "
                            "expected %d\n", (int)kBack[i].screen,
                    (int)ui.screen, (int)kBack[i].parent);
            return 1;
        }
    }
    return 0;
}

static int check_hidden_games(void) {
    char path[] = "/tmp/settings-status-hidden.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) return fail("could not create hidden-games db");
    close(fd);
    sqlite3 *db = NULL;
    if (jw_db_open(path, &db) || jw_db_apply_schema(db) || jw_db_scan_begin(db))
        return fail("could not initialize hidden-games db");
    if (jw_db_insert_game_stable(db, "SFC", "A hidden game", "secondary_sd",
            "SFC/Game000.sfc", "/test/secondary/Roms/SFC/Game000.sfc", NULL, NULL, NULL))
        return fail("could not add a hidden game");
    for (int i = 0; i < 80; ++i) {
        char sql[160];
        snprintf(sql, sizeof(sql), "INSERT INTO hidden_roms(source_id,rom_relpath) "
                 "VALUES('primary','SFC/Game%03d.sfc');", i);
        if (sqlite3_exec(db, sql, NULL, NULL, NULL) != SQLITE_OK)
            return fail("could not add orphan hidden preferences");
    }
    if (sqlite3_exec(db, "INSERT INTO hidden_roms(source_id,rom_relpath) "
            "VALUES('secondary_sd','SFC/Game000.sfc');", NULL, NULL, NULL) != SQLITE_OK)
        return fail("could not hide the secondary game");

    jw_settings_ui ui = {0};
    ui.open = true;
    ui.screen = JW_SETTINGS_GAMES;
    ui.games_list.cursor = JW_GAMES_HIDDEN_GAMES;
    snprintf(ui.db_path, sizeof(ui.db_path), "%s", path);
    char status[128] = "";
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_HIDDEN_GAMES || !ui.hidden_games_loaded ||
        ui.hidden_games_count != 81 || ui.hidden_games[0].rom.game.id <= 0)
        return fail("Hidden Games did not open with every hidden preference");
    /* Before renderer init, the unavailable-daemon dialog returns immediately.
       URI mode uses SQLite's real read-only result without chmod assumptions. */
    snprintf(ui.db_path, sizeof(ui.db_path), "file:%s?mode=ro", path);
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (ui.visibility_changed || ui.hidden_games_count != 81 ||
        !strstr(status, "read-only") || !strstr(status, "still hidden"))
        return fail("read-only Unhide changed visibility or missed the card warning");
    snprintf(ui.db_path, sizeof(ui.db_path), "%s", path);
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    int count = 0;
    if (!ui.visibility_changed || ui.hidden_games_count != 80 ||
        jw_db_count_hidden_games(path, &count) || count != 80 ||
        strcmp(ui.hidden_games[0].rom.game.source_id, "primary") || ui.hidden_games[0].rom.game.id != 0)
        return fail("Unhide removed the wrong source or kept the selected row");

    ui.visibility_changed = false;
    jw_settings_ui_handle_button(&ui, CAT_BTN_UP, status, sizeof(status), NULL);
    if (ui.hidden_games_list.cursor != 79)
        return fail("the final hidden preference was unreachable");
    if (sqlite3_exec(db, "CREATE TRIGGER reject_unhide BEFORE DELETE ON hidden_roms "
            "BEGIN SELECT RAISE(ABORT,'test write failure'); END;", NULL, NULL, NULL) != SQLITE_OK)
        return fail("could not inject unhide failure");
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (ui.visibility_changed || ui.hidden_games_count != 80 || !status[0])
        return fail("failed Unhide changed visibility or lost its error");
    if (sqlite3_exec(db, "DROP TRIGGER reject_unhide;", NULL, NULL, NULL) != SQLITE_OK)
        return fail("could not clear unhide failure");
    for (int i = 80; i > 0; --i) {
        jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
        if (ui.hidden_games_count != i - 1 ||
            (i > 1 && ui.hidden_games_list.cursor >= i - 1))
            return fail("Unhide lost a row or left the cursor out of bounds");
    }
    ui.visibility_changed = false;
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (ui.visibility_changed || jw_db_count_hidden_games(path, &count) || count)
        return fail("empty Hidden Games retained preferences or handled A");
    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_GAMES || ui.hidden_games || status[0] ||
        ui.games_list.cursor != JW_GAMES_HIDDEN_GAMES)
        return fail("Hidden Games did not return to its Games row cleanly");
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (!ui.hidden_games_loaded || ui.hidden_games_count)
        return fail("empty Hidden Games did not reopen");
    jw_settings_ui_close(&ui);
    jw_db_close(db);
    unlink(path);
    return 0;
}

static int hidden_row(const jw_settings_ui *ui, const char *source,
                      const char *path, const char *member) {
    for (int i = 0; i < ui->hidden_games_count; ++i) {
        const jw_hidden_rom_entry *row = &ui->hidden_games[i].rom;
        if (!strcmp(row->game.source_id, source) && !strcmp(row->game.rom_relpath, path) &&
            !strcmp(row->member, member)) return i;
    }
    return -1;
}

/* Disc and archive-member keys written by Manage Discs in the v0.13.0 betas
   still list by path and clear independently from Hidden Games. */
static int check_leftover_disc_keys(void) {
    char root[] = "/tmp/settings-hidden-discs.XXXXXX";
    if (!mkdtemp(root)) return fail("could not create hidden-disc fixtures");
    const char *dirs[] = { "first", "first/Roms", "first/Roms/PS",
                          "second", "second/Roms", "second/Roms/PS" };
    char path[512], primary[512], secondary[512], roots[1024], roms[1040], db_path[512];
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
        if (mkdir(path, 0755)) return fail("could not create hidden-disc folders");
    }
    struct { const char *path, *text; } files[] = {
        { "first/Roms/PS/Adventure.m3u", "disc.chd|Opening\r\narchive.zip#First|Bonus\r\n"
          "archive.zip#Second|Bonus\r\nmissing.chd\r\n#SAVEDISK:Save Disk\r\n" },
        { "first/Roms/PS/disc.chd", "synthetic disc" },
        { "first/Roms/PS/archive.zip", "synthetic archive" },
        { "second/Roms/PS/Adventure.m3u", "disc.chd|Other card\n" },
        { "second/Roms/PS/disc.chd", "other synthetic disc" },
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, files[i].path);
        FILE *fp = fopen(path, "wb");
        if (!fp) return fail("could not write hidden-disc fixture");
        if (fputs(files[i].text, fp) < 0 || fclose(fp))
            return fail("could not finish hidden-disc fixture");
    }
    snprintf(primary, sizeof(primary), "%s/first", root);
    snprintf(secondary, sizeof(secondary), "%s/second", root);
    snprintf(roots, sizeof(roots), "%s:%s", primary, secondary);
    snprintf(roms, sizeof(roms), "%s/Roms:%s/Roms", primary, secondary);
    setenv("SDCARD_PATH", primary, 1);
    setenv("SDCARD_PATHS", roots, 1);
    setenv("ROMS_PATHS", roms, 1);
    snprintf(db_path, sizeof(db_path), "%s/library.db", root);
    sqlite3 *db = NULL;
    if (jw_db_open(db_path, &db) || jw_db_apply_schema(db) || jw_db_scan_begin(db))
        return fail("could not initialize hidden-disc library");
    const char *sources[] = { "primary", "secondary_sd" };
    for (int i = 0; i < 2; ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, files[i ? 3 : 0].path);
        if (jw_db_insert_game_stable(db, "PS", "Adventure", sources[i],
                "PS/Adventure.m3u", path, NULL, NULL, NULL))
            return fail("could not index parent playlist");
    }
    snprintf(path, sizeof(path), "%s/%s", root, files[1].path);
    if (jw_db_insert_game_stable(db, "PS", "Standalone disc title", "primary",
            "PS/disc.chd", path, NULL, NULL, NULL))
        return fail("could not index standalone disc");
    struct { const char *source, *path, *member; } keys[] = {
        { "primary", "PS/Adventure.m3u", "" },
        { "primary", "PS/disc.chd", "" },
        { "secondary_sd", "PS/disc.chd", "" },
        { "primary", "PS/archive.zip", "First" },
        { "primary", "PS/archive.zip", "Second" },
        { "primary", "PS/missing.chd", "" },
        { "primary", "PS/orphan.zip", "Gone" },
        { "primary", "PS/orphan.zip", "Keep" },
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (jw_db_set_rom_hidden(db_path, keys[i].source, keys[i].path, keys[i].member, 1))
            return fail("could not hide a disc fixture");

    jw_settings_ui ui = {0};
    ui.open = true;
    ui.screen = JW_SETTINGS_GAMES;
    ui.games_list.cursor = JW_GAMES_HIDDEN_GAMES;
    snprintf(ui.db_path, sizeof(ui.db_path), "%s", db_path);
    char status[128] = "";
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (!ui.hidden_games_loaded || ui.hidden_games_count != 8)
        return fail("leftover disc keys did not load");
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i)
        if (hidden_row(&ui, keys[i].source, keys[i].path, keys[i].member) < 0)
            return fail("a leftover disc key is missing from Hidden Games");
    int row = hidden_row(&ui, "primary", "PS/missing.chd", "");
    if (ui.hidden_games[row].rom.game.id != 0 ||
        strcmp(ui.hidden_games[row].rom.game.rom_relpath, "PS/missing.chd"))
        return fail("an unindexed disc key did not fall back to its path");

    ui.hidden_games_list.cursor = hidden_row(&ui, "primary", "PS/archive.zip", "First");
    snprintf(ui.db_path, sizeof(ui.db_path), "file:%s?mode=ro", db_path);
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (ui.visibility_changed || ui.hidden_games_count != 8 || !strstr(status, "read-only"))
        return fail("read-only disc Unhide changed a preference");
    snprintf(ui.db_path, sizeof(ui.db_path), "%s", db_path);
    ui.hidden_games_list.cursor = hidden_row(&ui, "primary", "PS/archive.zip", "First");
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    int hidden = 0;
    if (!ui.visibility_changed || ui.hidden_games_count != 7 || strcmp(status, "Game unhidden") ||
        jw_db_is_rom_hidden(db_path, "primary", "PS/archive.zip", "First", &hidden) || hidden ||
        jw_db_is_rom_hidden(db_path, "primary", "PS/archive.zip", "Second", &hidden) || !hidden ||
        jw_db_is_rom_hidden(db_path, "primary", "PS/Adventure.m3u", "", &hidden) || !hidden)
        return fail("Unhide changed another archive selector or its parent visibility");
    ui.hidden_games_list.cursor = hidden_row(&ui, "primary", "PS/Adventure.m3u", "");
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (ui.hidden_games_count != 6 ||
        jw_db_is_rom_hidden(db_path, "primary", "PS/disc.chd", "", &hidden) || !hidden)
        return fail("unhiding a parent cleared its disc preference");
    ui.hidden_games_list.cursor = hidden_row(&ui, "primary", "PS/orphan.zip", "Gone");
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (ui.hidden_games_count != 5 ||
        jw_db_is_rom_hidden(db_path, "primary", "PS/orphan.zip", "Gone", &hidden) || hidden ||
        jw_db_is_rom_hidden(db_path, "primary", "PS/orphan.zip", "Keep", &hidden) || !hidden)
        return fail("orphan archive member preference was not independently clearable");
    ui.hidden_games_list.cursor = hidden_row(&ui, "primary", "PS/disc.chd", "");
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (ui.hidden_games_count != 4 ||
        jw_db_is_rom_hidden(db_path, "secondary_sd", "PS/disc.chd", "", &hidden) || !hidden)
        return fail("Unhide removed the same-named disc on another source");

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, files[i].path);
        FILE *fp = fopen(path, "rb");
        char bytes[256] = "";
        if (!fp) return fail("disc visibility removed a content file");
        size_t size = fread(bytes, 1, sizeof(bytes), fp);
        fclose(fp);
        if (size != strlen(files[i].text) || memcmp(bytes, files[i].text, size))
            return fail("disc visibility edited a content file");
    }
    snprintf(path, sizeof(path), "%s/%s", root, files[0].path);
    if (unlink(path)) return fail("could not remove fixture playlist");
    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    row = hidden_row(&ui, "primary", "PS/archive.zip", "Second");
    if (!ui.hidden_games_loaded || row < 0)
        return fail("a removed playlist hid a leftover disc key");
    ui.hidden_games_list.cursor = row;
    jw_settings_ui_handle_button(&ui, CAT_BTN_A, status, sizeof(status), NULL);
    if (jw_db_is_rom_hidden(db_path, "primary", "PS/archive.zip", "Second", &hidden) || hidden)
        return fail("unavailable parent prevented clearing its disc preference");
    jw_settings_ui_close(&ui);
    jw_db_close(db);
    unlink(db_path);
    for (size_t i = 1; i < sizeof(files) / sizeof(files[0]); ++i) {
        snprintf(path, sizeof(path), "%s/%s", root, files[i].path);
        unlink(path);
    }
    for (int i = (int)(sizeof(dirs) / sizeof(dirs[0])) - 1; i >= 0; --i) {
        snprintf(path, sizeof(path), "%s/%s", root, dirs[i]);
        rmdir(path);
    }
    rmdir(root);
    unsetenv("SDCARD_PATH");
    unsetenv("SDCARD_PATHS");
    unsetenv("ROMS_PATHS");
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) return fail("expected the compiled translation directory");
    if (sqlite3_config(SQLITE_CONFIG_URI, 1) != SQLITE_OK)
        return fail("could not enable the read-only database fixture");
    jw_settings_ui ui = {0};
    char status[64] = "Saved scrape order";

    if (check_back_targets()) return 1;
    if (check_hidden_games()) return 1;
    if (check_leftover_disc_keys()) return 1;
    if (check_language_order()) return 1;
    if (check_language_capacity()) return 1;
    if (check_force_off_hold()) return 1;
    if (check_boot_resume()) return 1;

    ui.open = true;
    ui.screen = JW_SETTINGS_SCRAPE_PRIORITY;
    ui.scrape_edit_grabbed = true;
    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_SCRAPE_PRIORITY || ui.scrape_edit_grabbed ||
        strcmp(status, "Saved scrape order") != 0)
        return fail("canceling a scrape grab cleared status or changed page");

    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_GAMES || status[0] != '\0')
        return fail("leaving scrape priority did not clear status");

    memset(&ui, 0, sizeof(ui));
    snprintf(status, sizeof(status), "%s", "Saved home tabs");
    ui.open = true;
    ui.screen = JW_SETTINGS_HOME_TABS;
    ui.home_tabs_grabbed = true;
    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_HOME_TABS || ui.home_tabs_grabbed ||
        strcmp(status, "Saved home tabs") != 0)
        return fail("canceling a home-tab grab cleared status or changed page");

    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_HOME_SCREEN || status[0] != '\0')
        return fail("leaving home tabs did not clear status");

    /* System Icons cycles the three packs and rides the theme-changed flag --
       that flag is what makes the launcher clear its memoized icon paths, so a
       pack change with the flag down would keep drawing the old artwork. */
    memset(&ui, 0, sizeof(ui));
    ui.open = true;
    ui.screen = JW_SETTINGS_HOME_SCREEN;
    ui.home_screen_list.cursor = JW_HOMESCREEN_SYSTEM_ICONS;
    ui.system_icon_pack_index = JW_SYSTEM_ICON_PACK_AUTO;
    for (int i = 1; i <= JW_SYSTEM_ICON_PACK_COUNT; i++) {
        bool theme_changed = false;
        jw_settings_ui_handle_button(&ui, CAT_BTN_RIGHT, status, sizeof(status),
                                     &theme_changed);
        if (ui.system_icon_pack_index != i % JW_SYSTEM_ICON_PACK_COUNT)
            return fail("System Icons did not cycle to the next pack");
        if (!theme_changed)
            return fail("System Icons change did not raise the rebuild flag");
    }

    bool theme_changed = false;
    jw_settings_ui_handle_button(&ui, CAT_BTN_LEFT, status, sizeof(status),
                                 &theme_changed);
    if (ui.system_icon_pack_index != JW_SYSTEM_ICON_PACK_COUNT - 1 || !theme_changed)
        return fail("System Icons did not cycle backwards");

    /* Hotkeys & Rumble keeps all seven rows off MLP1: the capture toggles
       stay on the parent page because there is no Hotkeys child to
       move them into. On MLP1 this is four, and the shortcut page's own logic
       is covered by input-shortcuts-test -- the UI cannot be built in MLP1
       shape on this host, because Catastrophe's MLP1 paths need <linux/input.h>
       and poll(). */
#ifndef PLATFORM_MLP1
    if (JW_CONTROLS_ROW_COUNT != 7)
        return fail("non-MLP1 Controls page changed row count");
    if (JW_CONTROLS_REC_KEEP != JW_CONTROLS_ROW_COUNT - 1)
        return fail("non-MLP1 Controls rows are no longer contiguous");

    memset(&ui, 0, sizeof(ui));
    ui.open = true;
    ui.screen = JW_SETTINGS_CONTROLS;
    ui.controls_list.cursor = JW_CONTROLS_REC_KEEP;
    status[0] = '\0';
    /* cat_list_state_move wraps rather than clamps, so the last row leads back
       to the first. The point of the check is that the page's row count is the
       seven-row one, not that the cursor stops. */
    jw_settings_ui_handle_button(&ui, CAT_BTN_DOWN, status, sizeof(status), NULL);
    if (ui.controls_list.cursor != JW_CONTROLS_UI_RUMBLE)
        return fail("Controls cursor did not wrap from the last row to the first");
    jw_settings_ui_handle_button(&ui, CAT_BTN_UP, status, sizeof(status), NULL);
    if (ui.controls_list.cursor != JW_CONTROLS_REC_KEEP)
        return fail("Controls cursor did not wrap back to the last row");
    jw_settings_ui_handle_button(&ui, CAT_BTN_B, status, sizeof(status), NULL);
    if (ui.screen != JW_SETTINGS_HOME)
        return fail("B on Controls did not return to the settings home");
#endif


    /* Home Tabs: switching a tab off leaves it where it sits. The list used to
       be partitioned into shown-then-hidden, so hiding a row moved it under the
       cursor; only X plus Up/Down reorders now, hidden rows included. */
    {
        jw_settings_ui tabs = {0};
        tabs.open = true;
        tabs.screen = JW_SETTINGS_HOME_TABS;
        for (int i = 0; i < JW_HOME_TABS_COUNT; i++) tabs.home_tab_order[i] = i;
        tabs.home_tab_visible = JW_HOME_TABS_COUNT;
        tabs.home_tabs_list.cursor = 1;

        jw_settings_ui_handle_button(&tabs, CAT_BTN_A, status, sizeof(status), NULL);
        if (!tabs.home_tab_hidden[1] || tabs.home_tab_visible != JW_HOME_TABS_COUNT - 1)
            return fail("A did not hide the tab under the cursor");
        if (tabs.home_tabs_list.cursor != 1)
            return fail("hiding a tab moved the cursor");
        for (int i = 0; i < JW_HOME_TABS_COUNT; i++)
            if (tabs.home_tab_order[i] != i)
                return fail("hiding a tab reordered the list");

        jw_settings_ui_handle_button(&tabs, CAT_BTN_A, status, sizeof(status), NULL);
        if (tabs.home_tab_hidden[1] || tabs.home_tab_visible != JW_HOME_TABS_COUNT)
            return fail("A did not show the tab again");

        /* A hidden row moves like any other: where it sits is where it returns. */
        tabs.home_tabs_list.cursor = 0;
        jw_settings_ui_handle_button(&tabs, CAT_BTN_A, status, sizeof(status), NULL);
        jw_settings_ui_handle_button(&tabs, CAT_BTN_X, status, sizeof(status), NULL);
        if (!tabs.home_tabs_grabbed)
            return fail("X did not grab a hidden row");
        jw_settings_ui_handle_button(&tabs, CAT_BTN_DOWN, status, sizeof(status), NULL);
        if (tabs.home_tab_order[0] != 1 || tabs.home_tab_order[1] != 0 ||
            tabs.home_tabs_list.cursor != 1)
            return fail("a grabbed hidden row did not move down");
        jw_settings_ui_handle_button(&tabs, CAT_BTN_X, status, sizeof(status), NULL);

        /* The last visible tab cannot be switched off. */
        for (int i = 0; i < JW_HOME_TABS_COUNT; i++) {
            tabs.home_tab_hidden[i] = i != 2;
        }
        tabs.home_tab_visible = 1;
        for (int i = 0; i < JW_HOME_TABS_COUNT; i++) {
            if (tabs.home_tab_order[i] != 2) continue;
            tabs.home_tabs_list.cursor = i;
            break;
        }
        jw_settings_ui_handle_button(&tabs, CAT_BTN_A, status, sizeof(status), NULL);
        if (tabs.home_tab_hidden[2] || tabs.home_tab_visible != 1)
            return fail("the last visible tab was switched off");
    }

    if (check_timezone_catalog() || check_timezone_lookup() ||
        check_timezone_selection()) return 1;
    if (check_activity() || check_theme_selection() || check_layout_viewport(argv[1])) return 1;
    puts("PASS settings-status-test");
    return 0;
}
