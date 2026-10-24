#ifndef JW_RETROARCH_SHADER_CATALOG_H
#define JW_RETROARCH_SHADER_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

/* The in-game picker's list of Leaf recommendations for one system.
 *
 * Built from the release-validated shader manifest, but parsed as if it were
 * hostile: the file lives on a FAT card a user can edit, and a malformed one
 * must never stop the in-game menu from opening. Every failure yields an empty
 * catalog plus a diagnostic, never a partial or a crash. */

#define JW_SHADER_CATALOG_MAX_ROWS 64u
#define JW_SHADER_CATALOG_MAX_BYTES (512u * 1024u)
#define JW_SHADER_CATALOG_MAX_CONSTRAINTS 6u

/* Only schema 2 is understood. A newer manifest is refused rather than guessed
 * at, because a field this code does not know about may change what a row
 * means. */
#define JW_SHADER_CATALOG_SCHEMA 2

typedef struct {
    char *path;          /* manifest-relative, e.g. leaf-recommended/x.glslp */
    char *display_name;
    char *description;
    char *constraints[JW_SHADER_CATALOG_MAX_CONSTRAINTS];
    size_t constraint_count;
} jw_shader_catalog_row;

typedef struct {
    jw_shader_catalog_row *rows;
    size_t count;
    /* Human-readable reason the catalog is empty or truncated. Always set when
     * count is 0 so the UI can say something better than "no shaders". */
    char diagnostic[192];
} jw_shader_catalog;

/* Load recommendations for `system_id` from `manifest_path`.
 *
 * Returns true when the manifest parsed; a true return with count 0 is a valid
 * "nothing for this system". Returns false only when the manifest could not be
 * used at all, with the reason in `out->diagnostic`. Either way `out` is safe
 * to pass to jw_shader_catalog_free(). */
bool jw_shader_catalog_load(const char *manifest_path, const char *system_id,
                            jw_shader_catalog *out);

void jw_shader_catalog_free(jw_shader_catalog *catalog);

/* True when `path` is a manifest-relative recommendation path: below
 * leaf-recommended/, ends in .glslp, no absolute prefix, no "..", no backslash.
 * Exposed for tests. */
bool jw_shader_catalog_path_ok(const char *path);

/* Resolve the single #reference in a RetroArch automatic preset. RetroArch
 * reports the automatic game/folder/core preset as active, while the picker
 * needs to map that wrapper back to the recommendation it references. This is
 * intentionally one hop: recommendation presets may themselves reference
 * bundled shader implementations, and those are still distinct picker rows. */
bool jw_shader_catalog_reference_target(const char *preset_path,
                                        char *out, size_t out_size);

#endif
