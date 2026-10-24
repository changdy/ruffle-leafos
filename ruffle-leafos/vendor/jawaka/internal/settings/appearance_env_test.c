/* Narrow appearance/language environment test.
 *
 * jw_appearance_apply_env() is the fork()-child half of the appearance
 * handoff: it runs between fork() and execv() for every ordinary app launch,
 * so what it exports is the only language input an app can rely on. This
 * checks the per-launch language contract from
 * umrk-workspace/docs/runtime-paths.md:
 *
 *   - UMRK_LANGUAGE (canonical) and JAWAKA_LANGUAGE (alias) are exported
 *     together and equal, for "en" and for "zh_CN";
 *   - export uses overwrite semantics, so a stale language inherited from an
 *     earlier game launch is replaced, in both directions;
 *   - an empty/missing resolved language exports "en" (absence means English);
 *   - a CJK language drives the CJK font path.
 *
 * It also covers the TZ half of the same handoff. A launched app reads its
 * local time from TZ, never from the settings database, so the zone a user
 * picks reaches an app only if this export replaces whatever the daemon
 * environment still holds from an earlier launch. Getting that wrong is
 * invisible in the launcher, whose own clock is corrected in-process, and shows
 * up only inside apps.
 */

#include "internal/settings/appearance.h"
#include "internal/settings/timezones.h"
#include "internal/db/db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail(const char *message) {
    fprintf(stderr, "appearance-env-test: %s\n", message);
    return 1;
}

static const char *env_or(const char *name, const char *fallback) {
    const char *v = getenv(name);
    return (v && v[0]) ? v : fallback;
}

static int env_is(const char *name, const char *wanted) {
    const char *v = getenv(name);
    return v && strcmp(v, wanted) == 0;
}

/* These ids have to be real picker rows, or the export check below is
   exercising a zone no user can actually choose. */
static int in_picker(const char *tz) {
    for (int i = 0; i < kJawakaTimeZoneCount; ++i)
        if (strcmp(kJawakaTimeZones[i].tz, tz) == 0) return 1;
    return 0;
}

static void fill_minimal(jw_appearance_env *env, const char *language) {
    memset(env, 0, sizeof(*env));
    snprintf(env->language, sizeof(env->language), "%s", language);
}

int main(void) {
    jw_appearance_env env;

    /* "en": both variables exported, equal. */
    fill_minimal(&env, "en");
    if (jw_appearance_apply_env(&env) != 0)
        return fail("apply_env failed for en");
    if (!env_is("UMRK_LANGUAGE", "en") || !env_is("JAWAKA_LANGUAGE", "en"))
        return fail("en: canonical/alias not both exported as en");
    if (strcmp(env_or("UMRK_LANGUAGE", ""), env_or("JAWAKA_LANGUAGE", "")) != 0)
        return fail("en: canonical and alias differ");

    /* Stale inherited value must be replaced (zh_CN -> en). */
    setenv("UMRK_LANGUAGE", "zh_CN", 1);
    setenv("JAWAKA_LANGUAGE", "zh_CN", 1);
    fill_minimal(&env, "en");
    jw_appearance_apply_env(&env);
    if (!env_is("UMRK_LANGUAGE", "en") || !env_is("JAWAKA_LANGUAGE", "en"))
        return fail("stale zh_CN was not replaced by en");

    /* "zh_CN": both exported, alias equal, CJK font selected. */
    setenv("UMRK_LANGUAGE", "fr", 1);   /* stale unknown value */
    fill_minimal(&env, "zh_CN");
    if (jw_appearance_apply_env(&env) != 0)
        return fail("apply_env failed for zh_CN");
    if (!env_is("UMRK_LANGUAGE", "zh_CN") || !env_is("JAWAKA_LANGUAGE", "zh_CN"))
        return fail("zh_CN: canonical/alias not both exported");
    if (strcmp(env_or("UMRK_LANGUAGE", ""), env_or("JAWAKA_LANGUAGE", "")) != 0)
        return fail("zh_CN: canonical and alias differ");
    if (strcmp(jw_appearance_font_path_for_language(3, "zh_CN"),
               JW_APPEARANCE_CJK_FONT_PATH) != 0)
        return fail("zh_CN did not select the CJK font");

    /* Stale inherited value must be replaced (en -> zh_CN). */
    setenv("UMRK_LANGUAGE", "en", 1);
    setenv("JAWAKA_LANGUAGE", "en", 1);
    fill_minimal(&env, "zh_CN");
    if (jw_appearance_apply_env(&env) != 0)
        return fail("apply_env failed for stale en -> zh_CN");
    if (!env_is("UMRK_LANGUAGE", "zh_CN") || !env_is("JAWAKA_LANGUAGE", "zh_CN"))
        return fail("stale en was not replaced by zh_CN");

    /* Japanese needs its own glyph forms in both slots: the UI face, and the
       CJK override Catastrophe substitutes for every Han/kana string. Chinese
       leaves the override empty so the theme's cjk_font applies. */
    if (strcmp(jw_appearance_font_path_for_language(3, "ja_JP"),
               JW_APPEARANCE_JA_FONT_PATH) != 0)
        return fail("ja_JP did not select the Japanese UI face");
    fill_minimal(&env, "ja_JP");
    if (jw_appearance_apply_env(&env) != 0)
        return fail("apply_env failed for ja_JP");
    if (!env_is("CAT_CJK_FONT_PATH", JW_APPEARANCE_JA_FONT_PATH))
        return fail("ja_JP did not export the Japanese CJK face");
    fill_minimal(&env, "zh_CN");
    jw_appearance_apply_env(&env);
    if (!env_is("CAT_CJK_FONT_PATH", ""))
        return fail("stale Japanese CJK face survived a switch to zh_CN");
    if (strcmp(jw_appearance_font_path_for_language(3, "jam"),
               JW_APPEARANCE_JA_FONT_PATH) == 0)
        return fail("a code merely starting with ja chose the Japanese face");

    /* Fredoka has no Vietnamese letters, so Vietnamese draws in the default
       family instead. Every other family, and Fredoka in other languages,
       is left alone. */
    int fredoka = -1;
    for (int i = 0; i < JW_APPEARANCE_FONT_FAMILY_COUNT; i++)
        if (strcmp(kJawakaFontFamilyLabels[i], "Fredoka") == 0) fredoka = i;
    if (fredoka < 0)
        return fail("Fredoka is not in the font list");
    if (strcmp(jw_appearance_font_path_for_language(fredoka, "vi_VN"),
               jw_appearance_font_path_for_index(JW_APPEARANCE_FONT_FAMILY_DEFAULT)) != 0)
        return fail("vi_VN kept Fredoka, which cannot draw Vietnamese");
    if (strcmp(jw_appearance_font_path_for_language(fredoka, "en"),
               jw_appearance_font_path_for_index(fredoka)) != 0)
        return fail("Fredoka was replaced in English");
    for (int i = 0; i < JW_APPEARANCE_FONT_FAMILY_COUNT; i++)
        if (i != fredoka && jw_appearance_font_index_for_language(i, "vi_VN") != i)
            return fail("vi_VN replaced a family that covers Vietnamese");
    if (!jw_appearance_font_covers_language(fredoka, "vix"))
        return fail("a code merely starting with vi lost Fredoka");

    /* Space Grotesk, Lexend, Baloo 2 and Fredoka have no Cyrillic, so Russian
       draws each of them in the default family. The rest are left alone, and
       English keeps every family. */
    static const char *const no_cyrillic[] = { "Space Grotesk", "Lexend", "Baloo 2", "Fredoka" };
    int replaced = 0;
    for (int i = 0; i < JW_APPEARANCE_FONT_FAMILY_COUNT; i++) {
        int gap = 0;
        for (size_t j = 0; j < sizeof(no_cyrillic) / sizeof(no_cyrillic[0]); j++)
            if (strcmp(kJawakaFontFamilyLabels[i], no_cyrillic[j]) == 0) gap = 1;
        replaced += gap;
        int want = gap ? JW_APPEARANCE_FONT_FAMILY_DEFAULT : i;
        if (jw_appearance_font_index_for_language(i, "ru_RU") != want)
            return fail(gap ? "ru_RU kept a family with no Cyrillic"
                            : "ru_RU replaced a family that covers Cyrillic");
        if (jw_appearance_font_index_for_language(i, "en") != i)
            return fail("a family was replaced in English");
    }
    if (replaced != (int)(sizeof(no_cyrillic) / sizeof(no_cyrillic[0])))
        return fail("a family with no Cyrillic is not in the font list");
    if (!jw_appearance_font_covers_language(JW_APPEARANCE_FONT_FAMILY_DEFAULT, "ru_RU") ||
        !jw_appearance_font_covers_language(JW_APPEARANCE_FONT_FAMILY_DEFAULT, "vi_VN"))
        return fail("the default family cannot draw a language it stands in for");
    if (!jw_appearance_font_covers_language(0, "rus"))
        return fail("a code merely starting with ru lost a family");

    /* Missing settings: jw_appearance_resolve() must fully populate the env,
       defaulting the language to "en", and export must publish that. */
    unsetenv("UMRK_LANGUAGE");
    unsetenv("JAWAKA_LANGUAGE");
    jw_appearance_env resolved;
    jw_appearance_resolve(NULL, &resolved);
    if (strcmp(resolved.language, "en") != 0)
        return fail("resolve(NULL) did not default language to en");
    jw_appearance_apply_env(&resolved);
    if (!env_is("UMRK_LANGUAGE", "en") || !env_is("JAWAKA_LANGUAGE", "en"))
        return fail("missing settings did not export en");

    /* Persisted settings come back from the single batched read, and a key
       that is missing, empty or out of range falls back per key. */
    char db_path[] = "/tmp/appearance-env-test-XXXXXX";
    int db_fd = mkstemp(db_path);
    if (db_fd < 0)
        return fail("could not create a temp database");
    close(db_fd);
    unlink(db_path);   /* let jw_db create it with the schema */
    if (jw_db_set_setting(db_path, "font_family_index", "7") != 0 ||
        jw_db_set_setting(db_path, "font_size_index", "2") != 0 ||
        jw_db_set_setting(db_path, "pill_shape_index", "99") != 0 ||
        jw_db_set_setting(db_path, "theme_name", "Jawaka-Grid") != 0 ||
        jw_db_set_setting(db_path, "accent_color", "#123456") != 0 ||
        jw_db_set_setting(db_path, "bg_color", "") != 0 ||
        jw_db_set_setting(db_path, "clock_style_index", "2") != 0 ||
        jw_db_set_setting(db_path, "show_wifi", "0") != 0 ||
        jw_db_set_setting(db_path, "timezone", "Europe/Paris") != 0)
        return fail("could not seed the temp database");
    unsetenv("JAWAKA_THEME");
    jw_appearance_resolve_settings(db_path, &resolved);
    if (strcmp(resolved.font_path, kJawakaFontFamilyPaths[7]) != 0)
        return fail("resolve did not read font_family_index");
    if (strcmp(resolved.font_bump, "4") != 0)
        return fail("resolve did not read font_size_index");
    if (strcmp(resolved.pill_corner_mask, "9") != 0)
        return fail("an out-of-range pill_shape_index did not fall back to Leaf");
    if (strcmp(resolved.theme_name, "Jawaka-Grid") != 0)
        return fail("resolve did not read theme_name");
    if (strcmp(resolved.accent, "#123456") != 0)
        return fail("resolve did not read accent_color");
    if (strcmp(resolved.bg, "#0F160E") != 0)
        return fail("an empty bg_color did not fall back to the Leaf default");
    if (strcmp(resolved.text, "#E8F1E3") != 0)
        return fail("a missing text_color did not fall back to the Leaf default");
    if (strcmp(resolved.language, "en") != 0)
        return fail("a missing language did not fall back to en");
    if (strcmp(resolved.status_clock, "12") != 0)
        return fail("resolve did not read clock_style_index");
    if (strcmp(resolved.status_show_wifi, "0") != 0 ||
        strcmp(resolved.status_show_battery, "1") != 0)
        return fail("status-bar visibility did not read or fall back per key");
    if (strcmp(resolved.timezone, "Europe/Paris") != 0)
        return fail("resolve did not read timezone");
    if (strcmp(resolved.status_bt_state, "0") != 0)
        return fail("resolve_settings did not leave the Bluetooth state at 0");
    setenv("JAWAKA_THEME", "Jawaka-Vertical", 1);
    jw_appearance_resolve_settings(db_path, &resolved);
    unsetenv("JAWAKA_THEME");
    if (strcmp(resolved.theme_name, "Jawaka-Vertical") != 0)
        return fail("JAWAKA_THEME did not override the persisted theme");
    unlink(db_path);

    /* Empty resolved language exports "en" (absence means English). */
    fill_minimal(&env, "");
    jw_appearance_apply_env(&env);
    if (!env_is("UMRK_LANGUAGE", "en") || !env_is("JAWAKA_LANGUAGE", "en"))
        return fail("empty language did not export en");

    /* TZ: a saved zone must overwrite an inherited one, in both directions.
       Europe/Paris standing in for the stale value an earlier launch left
       behind, and back again, because a user switching away from New Zealand
       has exactly the same problem in reverse. Chatham and the Line Islands
       ride along: their fractional and far-east offsets are the ids most likely
       to be mangled by a well-meaning "normalize the offset" change somewhere
       in the path. */
    static const char *kZones[] = { "Pacific/Auckland", "Europe/Paris",
                                    "Pacific/Chatham", "Pacific/Kiritimati",
                                    "Pacific/Auckland" };
    for (unsigned i = 0; i < sizeof(kZones) / sizeof(kZones[0]); ++i) {
        if (!in_picker(kZones[i]))
            return fail("a zone used here is not actually in the picker");
        fill_minimal(&env, "en");
        snprintf(env.timezone, sizeof(env.timezone), "%s", kZones[i]);
        if (jw_appearance_apply_env(&env) != 0)
            return fail("apply_env failed with a timezone set");
        if (!env_is("TZ", kZones[i])) {
            fprintf(stderr, "appearance-env-test: exported TZ=%s, expected %s\n",
                    env_or("TZ", "(unset)"), kZones[i]);
            return 1;
        }
    }

    /* No saved zone leaves the inherited one alone: an empty setting means
       "follow the system", not "reset to UTC". */
    setenv("TZ", "Europe/Paris", 1);
    fill_minimal(&env, "en");
    if (jw_appearance_apply_env(&env) != 0)
        return fail("apply_env failed with no timezone");
    if (!env_is("TZ", "Europe/Paris"))
        return fail("an empty timezone setting overwrote the inherited TZ");

    puts("PASS appearance-env-test");
    return 0;
}
