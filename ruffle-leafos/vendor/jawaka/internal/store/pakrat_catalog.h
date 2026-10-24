#ifndef JW_STORE_PAKRAT_CATALOG_H
#define JW_STORE_PAKRAT_CATALOG_H

#include "internal/store/pakrat_kind.h"

#define JW_PAKRAT_CATALOG_SCHEMA_MAX 1
#define JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF (-2)

typedef struct {
    jw_pakrat_kind kind;
    char id[128];
    char name[256];
    char summary[512];
    char version[64];
    char min_leaf_version[64];
    char platform[64];
    char install_name[256];
    char install_path[512];
    char runtime_manifest_path[256];
    char artifact_url[1024];
    char artifact_name[256];
    char artifact_archive[16];
    char artifact_sha256[80];
    long long artifact_size;
    long long artifact_installed_size;
    /* themes[] only. A theme's install_name is its id, with no .pak suffix,
       and its install_path is "Themes/<install_name>". */
    char author[128];
    char description[1280];
    char license[32];
    char preview_url[1024];
    char preview_sha256[80];
    long long preview_size;
    long long owner_github_id;   /* 0 when the catalog does not say */
    int withdrawn;               /* hidden from browsing; new installs refused */
} jw_pakrat_catalog_package;

/* STORE-CONTENT-1. A package that declares `provides` lives in `content[]`,
   never in `apps[]`, because it is gated on the content-pak contract by
   construction and `apps[]` is the gate-unaware lane. A client that predates
   this contract parses `apps[]`, ignores the unknown `content` key, and never
   learns a content pak exists -- which is the whole reason the lane is a new
   key rather than a field on an existing one.

   The lane is metadata for what the store SHOWS. It never decides whether
   Pak Rat offers "Open": that comes from the installed pak's launch.sh, so
   the answer stays right for a hybrid, for a pure content pak, and for a
   sideloaded pak that is in no lane at all. */
typedef enum {
    JW_PAKRAT_LANE_APPS = 0,
    JW_PAKRAT_LANE_CONTENT = 1,
    JW_PAKRAT_LANE_THEMES = 2
} jw_pakrat_catalog_lane;

/* themes[] (Pak Rat themes plan, "Catalog: the themes lane"). A flat entry per
   theme -- no packages[], no platform, no runtime -- because one THEME-1 zip
   installs on every Leaf device. Versions follow the content[] rules: every
   version is gated (THEME-1 requires min_leaf_version), and the legacy fields
   mirror the newest entry. A launcher that predates themes ignores the key,
   so the storefront schema stays 1.

   Ids are unique across apps[], content[] and themes[]. `withdrawn` is the one
   change a published entry may receive besides new versions; a withdrawn theme
   is still selected here so an installed copy can be shown as no longer
   available. */

typedef struct {
    jw_pakrat_catalog_package package;
    char gated_version[64];
    char gated_min_leaf_version[64];
    jw_pakrat_catalog_lane lane;
} jw_pakrat_catalog_selection;

/* Parse one storefront and select the newest compatible version of every
   package for `platform`, and of every theme regardless of platform. device_leaf_version may be empty/unknown; that fails
   gated versions closed unless is_dev_override is nonzero.

   Returns 0 on success, JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF when the
   storefront schema is newer than this client, and -1 for malformed or
   internally inconsistent catalog data. */
int jw_pakrat_catalog_parse_and_select(
    const char *json,
    const char *platform,
    const char *device_leaf_version,
    int is_dev_override,
    jw_pakrat_catalog_selection *out,
    int max_count,
    int *out_count);

/* Resolve one exact immutable historical version. Gates are intentionally not
   applied here; callers must restrict this to repair of an owned install whose
   recorded version exactly matches requested_version.

   Themes are searched too, without a platform.

   Returns 0 on success, 1 when the app/platform/version is not present,
   JW_PAKRAT_CATALOG_REQUIRES_NEWER_LEAF for a newer schema, and -1 for
   malformed or internally inconsistent data. */
int jw_pakrat_catalog_find_exact(
    const char *json,
    const char *platform,
    const char *store_id,
    const char *requested_version,
    jw_pakrat_catalog_package *out);

#endif
