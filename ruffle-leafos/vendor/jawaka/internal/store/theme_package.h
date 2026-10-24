/* THEME-1: the Leaf theme package (leaf-contracts docs/themes.md).
 *
 * The install-time half of the contract. It must agree reason for reason with
 * the store's reference validator (contracts/leaf-themes/scripts/theme_model.py)
 * and runs the same stages in the same order:
 *
 *   1-3  container, declared sizes, integrity  -- read from the zip itself
 *   4    entry names, types and the allowlist  -- read from the zip itself
 *   5    theme.json and image headers          -- read from the extracted tree
 *
 * Only allowlisted files are ever written to disk, so stage 5 judges exactly
 * the files a device would keep. `make theme-package-test` runs this module
 * over every contract fixture. */
#ifndef JW_STORE_THEME_PACKAGE_H
#define JW_STORE_THEME_PACKAGE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Contract order (theme_model.REASONS). Bit n of a reason mask is reason n. */
typedef enum {
    JW_THEME_ARCHIVE_TOO_LARGE = 0,
    JW_THEME_MALFORMED_ARCHIVE,
    JW_THEME_TOO_MANY_ENTRIES,
    JW_THEME_UNSUPPORTED_COMPRESSION,
    JW_THEME_UNCOMPRESSED_TOO_LARGE,
    JW_THEME_COMPRESSION_RATIO,
    JW_THEME_ENTRY_NAME_ENCODING,
    JW_THEME_ABSOLUTE_PATH,
    JW_THEME_BACKSLASH_PATH,
    JW_THEME_PATH_TRAVERSAL,
    JW_THEME_HIDDEN_FILE,
    JW_THEME_SYMLINK,
    JW_THEME_SPECIAL_FILE,
    JW_THEME_DUPLICATE_ENTRY,
    JW_THEME_NOT_SINGLE_FOLDER,
    JW_THEME_UNKNOWN_FILE,
    JW_THEME_SYSTEM_ID_INVALID,
    JW_THEME_RESERVED_SYSTEM_ID,
    JW_THEME_MULTIPLE_WALLPAPERS,
    JW_THEME_MISSING_MANIFEST,
    JW_THEME_MANIFEST_TOO_LARGE,
    JW_THEME_MALFORMED_MANIFEST,
    JW_THEME_MISSING_PREVIEW,
    JW_THEME_ID_MISMATCH,
    JW_THEME_RESERVED_NAME,
    JW_THEME_UNSUPPORTED_IMAGE,
    JW_THEME_IMAGE_DIMENSIONS,
    JW_THEME_UNKNOWN_SCHEMA,
    JW_THEME_UNKNOWN_FIELD,
    JW_THEME_ID_INVALID,
    JW_THEME_NAME_INVALID,
    JW_THEME_AUTHOR_INVALID,
    JW_THEME_VERSION_INVALID,
    JW_THEME_MIN_LEAF_VERSION,
    JW_THEME_UNKNOWN_LICENSE,
    JW_THEME_DESCRIPTION_INVALID,
    JW_THEME_GRID_INVALID,
    JW_THEME_COLOR_INVALID,
    JW_THEME_COLOR_LEVEL_INVALID,
    JW_THEME_STATUS_STYLE_INVALID,
    JW_THEME_REASON_COUNT
} jw_theme_reason;

typedef enum {
    JW_THEME_WARN_ICON_OFF_SIZE = 0,
    JW_THEME_WARN_NO_ART,
    JW_THEME_WARNING_COUNT
} jw_theme_warning;

/* Limits a device needs outside the validator too. */
#define JW_THEME_MAX_ARCHIVE_BYTES (10L * 1024L * 1024L)
#define JW_THEME_ID_MAX 40

typedef struct {
    uint64_t reasons;       /* bit (1 << jw_theme_reason) per reason */
    uint32_t warnings;      /* bit (1 << jw_theme_warning) per warning */
    char root[256];         /* the single top-level folder, when there is one */
    /* theme.json fields, copied when each one is valid under THEME-1. */
    char id[JW_THEME_ID_MAX + 1];
    char name[96];
    char author[64];
    char version[16];
    char min_leaf_version[16];
    char license[32];
} jw_theme_package_result;

const char *jw_theme_package_reason_slug(jw_theme_reason reason);
const char *jw_theme_package_warning_slug(jw_theme_warning warning);

/* True when `id` is one of THEME-1's reserved install names, ignoring case. */
bool jw_theme_package_reserved_name(const char *id);

/* The first reason in contract order, or -1 when the package is accepted. */
int jw_theme_package_first_reason(const jw_theme_package_result *result);

/* Validate the theme archive at zip_path. When validation reaches stage 5, the
   allowlisted files are extracted to <extract_dir>/<root>/ (extract_dir must
   exist) and judged there. Returns 0 when validation reached a verdict --
   `out->reasons` says whether it was accepted -- and -1 on an I/O or memory
   failure that left no verdict. The caller removes extract_dir on refusal. */
int jw_theme_package_validate_zip(const char *zip_path, const char *extract_dir,
                                  jw_theme_package_result *out);

/* THEME-1's theme.json rules on raw bytes: the "Reading theme.json" rules and
   every field rule (theme_model.parse_manifest_bytes + validate_manifest).
   Fills out's reason mask and manifest fields; archive reasons are left
   untouched. */
void jw_theme_package_check_manifest(const unsigned char *data, size_t len,
                                     jw_theme_package_result *out);

#endif
