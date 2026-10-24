#include "internal/settings/appearance.h"
#include "internal/i18n/i18n.h"

#include "internal/db/db.h"
#include "internal/platform/bluetooth.h"
#include "internal/settings/theme_resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JW_COLOR_BUF_LEN 16

/* Mirror of Catastrophe's CAT_CORNER_* bits. catastrophe.h is not on the daemon
   include path (jawakad links appearance.c without it), so we can't reference
   the enum here. TL=1 TR=2 BL=4 BR=8. */
#define JW_CORNER_TL  0x1
#define JW_CORNER_TR  0x2
#define JW_CORNER_BL  0x4
#define JW_CORNER_BR  0x8
#define JW_CORNER_ALL (JW_CORNER_TL | JW_CORNER_TR | JW_CORNER_BL | JW_CORNER_BR)

const char *const kJawakaFontFamilyLabels[JW_APPEARANCE_FONT_FAMILY_COUNT] = {
    "Space Grotesk",
    "Inter",
    "Rounded M+",
    "Nunito",
    "Baloo 2",
    "Fredoka",
    "Lexend",
    "IBM Plex Sans",
    "Noto Sans",
};

const char *const kJawakaFontFamilyPaths[JW_APPEARANCE_FONT_FAMILY_COUNT] = {
    "fonts/SpaceGrotesk/SpaceGrotesk-Regular.ttf",
    "fonts/Inter/Inter.ttf",
    "fonts/MPlusRounded1c/MPLUSRounded1c-Bold.ttf",
    "fonts/Nunito/Nunito-Bold.ttf",
    "fonts/Baloo2/Baloo2-Bold.ttf",
    "fonts/Fredoka/Fredoka-Bold.ttf",
    "fonts/Lexend/Lexend-Bold.ttf",
    "fonts/IBMPlexSans/IBMPlexSans-Bold.ttf",
    "fonts/NotoSans/NotoSans-Bold.ttf",
};

const float kJawakaPillRadiusValues[JW_APPEARANCE_PILL_SHAPE_COUNT] = {
    1.0f,   /* Rounded */
    0.25f,  /* Soft */
    0.0f,   /* Square */
    1.0f,   /* Leaf: full radius on its two rounded corners */
};

/* Leaf rounds top-left + bottom-right only (the others stay sharp) for a
   directional highlight. */
const int kJawakaPillCornerMasks[JW_APPEARANCE_PILL_SHAPE_COUNT] = {
    JW_CORNER_ALL,
    JW_CORNER_ALL,
    JW_CORNER_ALL,
    JW_CORNER_TL | JW_CORNER_BR,
};

const int kJawakaFontSizeValues[JW_APPEARANCE_FONT_SIZE_COUNT] = {
    0,
    2,
    4,
    5,
};

static int jw__index_or(const char *val, int count, int fallback) {
    if (!val || !val[0] || count <= 0) return fallback;

    int idx = atoi(val);
    return (idx >= 0 && idx < count) ? idx : fallback;
}

static int jw__read_index(const char *db_path, const char *key, int count, int fallback) {
    if (!db_path || !db_path[0] || !key) return fallback;

    char val[32];
    if (jw_db_get_setting(db_path, key, val, sizeof(val)) != 0)
        return fallback;
    return jw__index_or(val, count, fallback);
}

static const char *jw__clock_token_for_index(int idx) {
    switch (idx) {
        case 0: return "hide";
        case 2: return "12";
        case 3: return "no-ampm";
        case 1:
        default: return "24";
    }
}

static int jw__bt_state_now(void) {
    if (!jw_bt_radio_is_on()) return 0;
    return (jw_bt_any_connected() == 1) ? 2 : 1;
}

int jw_appearance_font_family_index_from_db(const char *db_path) {
    return jw__read_index(db_path, "font_family_index",
                          JW_APPEARANCE_FONT_FAMILY_COUNT,
                          JW_APPEARANCE_FONT_FAMILY_DEFAULT);
}

const char *jw_appearance_font_path_for_index(int index) {
    if (index < 0 || index >= JW_APPEARANCE_FONT_FAMILY_COUNT)
        index = JW_APPEARANCE_FONT_FAMILY_DEFAULT;
    return kJawakaFontFamilyPaths[index];
}

/* None of the nine themed families carry a single CJK glyph, so a translated UI
   has to move off the user's chosen font entirely. This is a whole-UI swap
   rather than a per-string fallback: with a partial translation a per-string
   choice would stack two typefaces in one list, which reads as a rendering bug
   rather than a missing string. Source Han Sans covers Latin well, so English
   that has not been translated yet still looks deliberate.

   The font picker must reflect this while a CJK language is active -- silently
   ignoring the user's choice is worse than showing them why it does not apply. */
/* True for `code` itself or `code` with a region ("vi", "vi_VN"), not for a
   longer code that merely starts with it ("vix"). */
static bool jw__language_is(const char *lang, const char *code) {
    size_t n = strlen(code);
    return lang && strncmp(lang, code, n) == 0 && (lang[n] == '\0' || lang[n] == '_');
}

static bool jw__language_is_japanese(const char *lang) {
    return jw__language_is(lang, "ja");
}

/* Themed families missing letters a translation needs, so text in them would
   drop letters mid-word. Fredoka has no Vietnamese (ă, đ, ơ, ư and most of the
   stacked-accent forms); Space Grotesk, Lexend, Baloo 2 and Fredoka have no
   Cyrillic at all. Every other pairing covers the whole translation. Unlike
   CJK this swaps only the listed family, for the default (Nunito covers both),
   and the saved choice is kept for when the language changes back. */
static const struct {
    const char *lang;
    const char *family;     /* a kJawakaFontFamilyLabels entry */
} kFontGaps[] = {
    { "vi", "Fredoka" },
    { "ru", "Space Grotesk" },
    { "ru", "Lexend" },
    { "ru", "Baloo 2" },
    { "ru", "Fredoka" },
};

bool jw_appearance_font_covers_language(int index, const char *lang) {
    if (index < 0 || index >= JW_APPEARANCE_FONT_FAMILY_COUNT)
        return true;
    for (size_t i = 0; i < sizeof(kFontGaps) / sizeof(kFontGaps[0]); i++)
        if (jw__language_is(lang, kFontGaps[i].lang) &&
            strcmp(kJawakaFontFamilyLabels[index], kFontGaps[i].family) == 0)
            return false;
    return true;
}

int jw_appearance_font_index_for_language(int index, const char *lang) {
    if (index < 0 || index >= JW_APPEARANCE_FONT_FAMILY_COUNT)
        index = JW_APPEARANCE_FONT_FAMILY_DEFAULT;
    return jw_appearance_font_covers_language(index, lang)
               ? index
               : JW_APPEARANCE_FONT_FAMILY_DEFAULT;
}

const char *jw_appearance_font_path_for_language(int index, const char *lang) {
    if (jw__language_is_japanese(lang))
        return JW_APPEARANCE_JA_FONT_PATH;
    if (jw_i18n_language_is_cjk(lang))
        return JW_APPEARANCE_CJK_FONT_PATH;
    return jw_appearance_font_path_for_index(jw_appearance_font_index_for_language(index, lang));
}

/* The UI face alone is not enough for Japanese: Catastrophe moves every CJK
   string onto a separate CJK face, so without this override the translated
   strings would still come out in Chinese forms and only the Latin text would
   change. */
const char *jw_appearance_cjk_font_path_for_language(const char *lang) {
    return jw__language_is_japanese(lang) ? JW_APPEARANCE_JA_FONT_PATH : "";
}

const char *jw_appearance_cjk_font_label(const char *lang) {
    return jw__language_is_japanese(lang) ? "Leaf Han Sans JP" : "Source Han Sans";
}

/* A persisted key, where its value lands, and what stands in when it is
   missing or empty. */
typedef struct {
    const char *key;
    char       *out;
    size_t      out_size;
    const char *fallback;
} jw__appearance_key;

void jw_appearance_resolve_settings(const char *db_path, jw_appearance_env *out) {
    if (!out) return;

    char font_family[16], font_size[16], pill_shape[16], clock_style[16];
    char theme[sizeof(out->theme_name)];
    const jw__appearance_key keys[] = {
        { "font_family_index", font_family, sizeof(font_family), "" },
        { "font_size_index",   font_size,   sizeof(font_size),   "" },
        { "pill_shape_index",  pill_shape,  sizeof(pill_shape),  "" },
        { "clock_style_index", clock_style, sizeof(clock_style), "" },
        { "theme_name",        theme,       sizeof(theme),       "" },
        /* Resolved here, in the parent, because jw__spawn_child re-runs this on
           every launcher spawn -- so a language change applies by simply
           respawning the launcher, with no separate plumbing to push a font
           down to the child. */
        { "language", out->language, sizeof(out->language), "en" },
        /* Defaults mirror Settings' Leaf scheme so apps inherit the identity
           theme even before the first settings session persists color rows. */
        { "accent_color",          out->accent,          sizeof(out->accent),          "#1E331E" },
        { "bg_color",              out->bg,              sizeof(out->bg),              "#0F160E" },
        { "text_color",            out->text,            sizeof(out->text),            "#E8F1E3" },
        { "hint_color",            out->hint,            sizeof(out->hint),            "#7E9579" },
        { "highlight_color",       out->highlight,       sizeof(out->highlight),       "#7FB069" },
        { "button_label_color",    out->button_label,    sizeof(out->button_label),    "#0F160E" },
        { "button_glyph_bg_color", out->button_glyph_bg, sizeof(out->button_glyph_bg), "#7FB069" },
        /* Button-hints visibility ("0"/"1"), so apps hide their footers when
           the user turned hints off in the launcher. Default on. */
        { "show_hints", out->show_hints, sizeof(out->show_hints), "1" },
        /* Status-bar visibility mirrors the launcher so native apps wear the
           same chrome without opening the settings DB themselves. */
        { "show_wifi",          out->status_show_wifi,          sizeof(out->status_show_wifi),          "1" },
        { "show_battery",       out->status_show_battery,       sizeof(out->status_show_battery),       "1" },
        { "show_battery_level", out->status_show_battery_level, sizeof(out->status_show_battery_level), "0" },
        { "show_bluetooth",     out->status_show_bluetooth,     sizeof(out->status_show_bluetooth),     "1" },
        { "timezone",           out->timezone,                  sizeof(out->timezone),                  "" },
    };
    const size_t key_count = sizeof(keys) / sizeof(keys[0]);

    /* One DB open for every key. jawakad resolves on its main loop, which is
       also the loop that forwards the D-pad, and each separate read reopened
       the database and re-checked its schema: twenty of them per resolve held
       input back long enough for a tap to turn into a hold. A read that fails
       partway keeps what it got; the rest fall back below. */
    jw_db_setting_query queries[sizeof(keys) / sizeof(keys[0])];
    for (size_t i = 0; i < key_count; i++) {
        keys[i].out[0] = '\0';
        queries[i] = (jw_db_setting_query){ keys[i].key, keys[i].out, keys[i].out_size, 0 };
    }
    if (db_path && db_path[0])
        (void)jw_db_get_settings(db_path, queries, (int)key_count);
    for (size_t i = 0; i < key_count; i++) {
        if (!keys[i].out[0])
            snprintf(keys[i].out, keys[i].out_size, "%s", keys[i].fallback);
    }

    int font_idx = jw__index_or(font_family, JW_APPEARANCE_FONT_FAMILY_COUNT,
                                JW_APPEARANCE_FONT_FAMILY_DEFAULT);
    int font_size_idx = jw__index_or(font_size, JW_APPEARANCE_FONT_SIZE_COUNT, 1);
    int pill_idx = jw__index_or(pill_shape, JW_APPEARANCE_PILL_SHAPE_COUNT,
                                JW_APPEARANCE_PILL_SHAPE_DEFAULT);

    jw_resolve_theme_name_value(theme, out->theme_name, sizeof(out->theme_name));

    /* The font path tables are static const, so the pointer stays valid across a
       later fork()/execv() in the child. A CJK language ignores the chosen
       family entirely: none of the themed ones have the glyphs. */
    out->font_path = jw_appearance_font_path_for_language(font_idx, out->language);

    /* Pre-format the numeric values here in the parent so the child only has to
       call setenv (no vsnprintf) after fork(). */
    snprintf(out->font_bump, sizeof(out->font_bump), "%d", kJawakaFontSizeValues[font_size_idx]);
    snprintf(out->pill_radius_ratio, sizeof(out->pill_radius_ratio), "%.2f", kJawakaPillRadiusValues[pill_idx]);
    snprintf(out->pill_corner_mask, sizeof(out->pill_corner_mask), "%d", kJawakaPillCornerMasks[pill_idx]);

    snprintf(out->status_clock, sizeof(out->status_clock), "%s",
             jw__clock_token_for_index(jw__index_or(clock_style, 4, 1)));
    snprintf(out->status_bt_state, sizeof(out->status_bt_state), "%s", "0");
}

void jw_appearance_resolve(const char *db_path, jw_appearance_env *out) {
    if (!out) return;

    jw_appearance_resolve_settings(db_path, out);
    snprintf(out->status_bt_state, sizeof(out->status_bt_state), "%d",
             jw__bt_state_now());
}

int jw_appearance_apply_env(const jw_appearance_env *env) {
    if (!env) return -1;

    int rc = 0;
    rc |= setenv("CAT_THEME_NAME", env->theme_name, 1);
    rc |= setenv("CAT_FONT_PATH", env->font_path ? env->font_path : "", 1);
    /* Always written, so a language change replaces whatever the daemon's
       environment inherited from an earlier spawn. */
    rc |= setenv("CAT_CJK_FONT_PATH",
                 jw_appearance_cjk_font_path_for_language(env->language), 1);
    rc |= setenv("CAT_FONT_BUMP", env->font_bump, 1);
    rc |= setenv("CAT_PILL_RADIUS_RATIO", env->pill_radius_ratio, 1);
    rc |= setenv("CAT_PILL_CORNER_MASK", env->pill_corner_mask, 1);
    rc |= setenv("CAT_COLOR_ACCENT", env->accent, 1);
    rc |= setenv("CAT_COLOR_BACKGROUND", env->bg, 1);
    rc |= setenv("CAT_COLOR_TEXT", env->text, 1);
    rc |= setenv("CAT_COLOR_HINT", env->hint, 1);
    rc |= setenv("CAT_COLOR_HIGHLIGHT", env->highlight, 1);
    rc |= setenv("CAT_COLOR_BUTTON_LABEL", env->button_label, 1);
    rc |= setenv("CAT_COLOR_BUTTON_GLYPH_BG", env->button_glyph_bg, 1);
    rc |= setenv("CAT_SHOW_HINTS", env->show_hints[0] ? env->show_hints : "1", 1);
    rc |= setenv("CAT_STATUS_SHOW_WIFI",
                 env->status_show_wifi[0] ? env->status_show_wifi : "1", 1);
    rc |= setenv("CAT_STATUS_SHOW_BATTERY",
                 env->status_show_battery[0] ? env->status_show_battery : "1", 1);
    rc |= setenv("CAT_STATUS_SHOW_BATTERY_LEVEL",
                 env->status_show_battery_level[0] ? env->status_show_battery_level : "0", 1);
    rc |= setenv("CAT_STATUS_SHOW_BLUETOOTH",
                 env->status_show_bluetooth[0] ? env->status_show_bluetooth : "1", 1);
    rc |= setenv("CAT_STATUS_CLOCK", env->status_clock[0] ? env->status_clock : "24", 1);
    rc |= setenv("CAT_STATUS_BT_STATE",
                 env->status_bt_state[0] ? env->status_bt_state : "0", 1);
    /* Canonical per-launch language, plus its indefinite compatibility alias.
       Both always carry the same value here; this child-side export runs after
       fork() with overwrite semantics, so it replaces any stale language the
       daemon environment still holds from an earlier game launch. */
    rc |= setenv("UMRK_LANGUAGE",
                  env->language[0] ? env->language : "en", 1);
    rc |= setenv("JAWAKA_LANGUAGE",
                  env->language[0] ? env->language : "en", 1);
    if (env->timezone[0]) {
        rc |= setenv("TZ", env->timezone, 1);
    }

    return rc == 0 ? 0 : -1;
}

int jw_appearance_export_env(const char *db_path) {
    jw_appearance_env env;
    jw_appearance_resolve(db_path, &env);
    return jw_appearance_apply_env(&env);
}
