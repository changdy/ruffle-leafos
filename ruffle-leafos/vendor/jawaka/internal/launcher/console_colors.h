#ifndef JW_LAUNCHER_CONSOLE_COLORS_H
#define JW_LAUNCHER_CONSOLE_COLORS_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL.h>

/* Per-console background-color lookup, populated from the active theme's
 * stylesheet.json under the `launcher.console_colors` key:
 *
 *   "launcher": {
 *     "console_colors": {
 *       "GB":  "#9BBC0F",
 *       "SFC": "#8A7FB8",
 *       ...
 *     }
 *   }
 *
 * Used by Jawaka-Horizontal to paint each parallelogram tile with a curated
 * identity color. Missing or unmapped systems fall back to the carousel's
 * existing hash-derived color.
 *
 * Backwards compatible: if the stylesheet has no `console_colors` block, the
 * table loads empty and every lookup returns false.
 */

#define JW_CONSOLE_COLOR_MAX 64

typedef struct {
    char     code[32];   /* matches jw_system_entry.name */
    uint32_t rgb;        /* 0xRRGGBB; alpha applied by caller */
} jw_console_color;

typedef struct {
    jw_console_color entries[JW_CONSOLE_COLOR_MAX];
    int              count;
} jw_console_color_table;

/* Clear the table back to empty. */
void jw_console_colors_clear(jw_console_color_table *t);

/* Open <theme_dir>/<theme_name>/stylesheet.json and populate `out` from
 * launcher.console_colors. Returns 0 on success (table may be empty if the
 * key is missing), or -1 on file/parse error. Existing entries in `out` are
 * cleared first. */
int jw_console_colors_load(jw_console_color_table *out,
                           const char *theme_dir,
                           const char *theme_name);

/* Look up `code` (case-sensitive). On hit, writes the color into `out_color`
 * with alpha 255 and returns true. On miss returns false and leaves out_color
 * unchanged. */
bool jw_console_colors_lookup(const jw_console_color_table *t,
                              const char *code,
                              SDL_Color *out_color);

#endif /* JW_LAUNCHER_CONSOLE_COLORS_H */
