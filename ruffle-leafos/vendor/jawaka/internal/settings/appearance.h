#ifndef JW_SETTINGS_APPEARANCE_H
#define JW_SETTINGS_APPEARANCE_H

#include <stddef.h>

#include "internal/i18n/i18n.h"

#define JW_APPEARANCE_FONT_FAMILY_COUNT 9
#define JW_APPEARANCE_FONT_FAMILY_DEFAULT 3   /* Nunito (index into kJawakaFontFamily*) */

extern const char *const kJawakaFontFamilyLabels[JW_APPEARANCE_FONT_FAMILY_COUNT];
extern const char *const kJawakaFontFamilyPaths[JW_APPEARANCE_FONT_FAMILY_COUNT];

/* Canonical appearance value tables. Defined in appearance.c — the one TU
   linked into both jawakad and the UI binaries — so the daemon's env export and
   the settings UI share a single source of truth. settings.c owns the matching
   display labels and static-asserts its row counts against these. */
#define JW_APPEARANCE_PILL_SHAPE_COUNT 4
/* Default list/pill style when nothing is persisted — "Leaf" (index 3). Must
   match settings.c's JW_SETTINGS_PILL_SHAPE_DEFAULT so the launcher and the
   appearance env exported to apps agree on a fresh install. */
#define JW_APPEARANCE_PILL_SHAPE_DEFAULT 3
#define JW_APPEARANCE_FONT_SIZE_COUNT  4

extern const float kJawakaPillRadiusValues[JW_APPEARANCE_PILL_SHAPE_COUNT];
extern const int   kJawakaPillCornerMasks[JW_APPEARANCE_PILL_SHAPE_COUNT];
extern const int   kJawakaFontSizeValues[JW_APPEARANCE_FONT_SIZE_COUNT];

int jw_appearance_font_family_index_from_db(const char *db_path);
/* The CJK face, shared by every theme. Kept here rather than read from the
   stylesheet because this path is also baked into the child environment at
   fork() time, where reading a stylesheet is not available. */
#define JW_APPEARANCE_CJK_FONT_PATH "fonts/SourceHanSansCN/SourceHanSansCN-Regular.otf"
/* Japanese glyph forms for the same codepoints (compare 直 or 骨). It covers
   every character the Chinese face does, so Chinese titles still render. */
#define JW_APPEARANCE_JA_FONT_PATH  "fonts/LeafHanSansJP/LeafHanSansJP-Regular.otf"

const char *jw_appearance_font_path_for_index(int index);

/* Resolves to the CJK face for any language that needs it, ignoring `index`.
   Use this anywhere the UI font is chosen; the plain _for_index form is for
   the font picker itself, which must keep showing the themed families. */
const char *jw_appearance_font_path_for_language(int index, const char *lang);

/* False when the themed family at `index` lacks letters `lang` needs (Fredoka
   has no Vietnamese; four families have no Cyrillic). The _for_language path
   above already steps around it; the font picker uses these to skip it and to
   name the face actually drawn. */
bool jw_appearance_font_covers_language(int index, const char *lang);
int  jw_appearance_font_index_for_language(int index, const char *lang);

/* The face Catastrophe substitutes for CJK strings, exported as
   CAT_CJK_FONT_PATH. "" leaves the theme's cjk_font in charge; only Japanese
   needs different glyph forms. */
const char *jw_appearance_cjk_font_path_for_language(const char *lang);

/* The name the Font row shows while a CJK language pins the face. */
const char *jw_appearance_cjk_font_label(const char *lang);

/* Pre-resolved appearance environment, holding the final CAT_* env values as
 * ready-to-export strings. Filled by jw_appearance_resolve() (which touches the
 * SQLite DB) and consumed by jw_appearance_apply_env() (which only calls
 * setenv). The split exists so the DB read happens in the parent before fork(),
 * never between fork() and execv() — opening SQLite there is not fork-safe on
 * macOS (it reaches into libsystem_trace/os_log and intermittently crashes the
 * forked child). See the spawn helpers in cmd/jawakad/main.c. */
typedef struct jw_appearance_env {
    char theme_name[256];
    const char *font_path;        /* points into static kJawakaFontFamilyPaths */
    char font_bump[16];
    char pill_radius_ratio[16];
    char pill_corner_mask[16];
    char accent[16];
    char bg[16];
    char text[16];
    char hint[16];
    char highlight[16];
    char button_label[16];
    char button_glyph_bg[16];
    char show_hints[8];           /* "0"/"1" — exported as CAT_SHOW_HINTS */
    char status_show_wifi[8];     /* CAT_STATUS_SHOW_WIFI */
    char status_show_battery[8];  /* CAT_STATUS_SHOW_BATTERY */
    char status_show_battery_level[8]; /* CAT_STATUS_SHOW_BATTERY_LEVEL */
    char status_show_bluetooth[8];/* CAT_STATUS_SHOW_BLUETOOTH */
    char status_clock[16];        /* CAT_STATUS_CLOCK: hide / 12 / 24 / no-ampm */
    char status_bt_state[8];      /* CAT_STATUS_BT_STATE: 0 off, 1 on, 2 connected */
    char timezone[64];            /* TZ override for launched apps; empty = inherit */
    /* Persisted language code, "en" when unset. Drives the font choice above and
       is handed to the child so it loads the matching string table. Sized from
       the same constant as every other language code, so it cannot truncate one
       the scanner accepted. */
    char language[JW_I18N_CODE_MAX];
} jw_appearance_env;

/* Parent-side: read the DB (and env) and resolve every appearance value into
 * `out`. Always fully populates `out` (falling back to defaults). Safe to call
 * anywhere a normal SQLite open is safe — but NOT between fork() and execv(). */
void jw_appearance_resolve(const char *db_path, jw_appearance_env *out);

/* jw_appearance_resolve() without the Bluetooth snapshot: status_bt_state is
 * left "0". The snapshot shells out to bluetoothctl, which on jawakad's main
 * loop stalls input forwarding; use this for a child that draws no status bar
 * (the OSD). Reads the DB exactly once. Same fork() rule as above. */
void jw_appearance_resolve_settings(const char *db_path, jw_appearance_env *out);

/* Child-side: export a resolved appearance into the environment with setenv
 * only — no allocation beyond setenv's own, no DB/SQLite. Safe to call in a
 * forked child before execv(). Returns 0 on success, -1 if any setenv failed. */
int jw_appearance_apply_env(const jw_appearance_env *env);

/* Convenience: resolve + apply in one call. Equivalent to the original export
 * path; only use where fork()/execv() is not involved. */
int jw_appearance_export_env(const char *db_path);

#endif
