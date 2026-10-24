#ifndef JW_RETROARCH_CATALOG_H
#define JW_RETROARCH_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

/* The only frontend protocol a content pak can declare (leaf-contracts
   docs/leaf-retroarch-frontend.md). */
#define JW_RA_FRONTEND_PROTOCOL "leaf-ra-1"

typedef struct {
    char **items;
    size_t count;
} jw_ra_string_list;

typedef enum {
    JW_RA_SCRAPE_NAME_FILENAME = 0,
    JW_RA_SCRAPE_NAME_DESCRIPTOR = 1,
} jw_ra_scrape_name_source;

typedef struct {
    char *id;
    char *display_name;
    char *type;
    char *libretro_name;
    char *file_name;
    char *config_folder;
    char *info_name;
    char *path;
    /* Content-pak provenance. provider is Apps-relative (for example
       "mlp1/ScummVM.pak"), never an absolute mount path. core_root_rel is
       derived from file_name/path when the row is loaded. */
    char *core_root_rel;
    char *provider;
    char *source_id;
    /* LEAF-RA-1 frontend: a pak-supplied RetroArch launched in place of
       Leaf's own for this core. Pak-relative like file_name; NULL for every
       release core and for pak cores that run in Leaf's RetroArch. */
    char *frontend_path;
    bool supports_menu;
    bool supports_savestate;
    bool supports_disk_control;
    bool needs_swap;
    bool requires_direct_drm;
    char *status;
} jw_ra_core;

typedef struct {
    char *id;
    char *name;
    jw_ra_string_list patterns;
    jw_ra_string_list extensions;
    jw_ra_string_list archive_extensions;
    jw_ra_string_list archive_inner_extensions;
    char *archive_mode;
    jw_ra_string_list file_names;
    jw_ra_string_list ignore_file_names;
    jw_ra_string_list playlist_extensions;
    char *m3u_generation;
    bool name_map;
    char *default_core;
    /* Optional owner of save/state files from RetroArch's historical flat
       layout. Legacy files may only be recovered for this core. */
    char *legacy_flat_core;
    jw_ra_string_list alternate_cores;
    char *rom_root;
    char *image_root;
    char *group;
    char *bios_directory;
    int screenscraper_platform_ids[8];
    size_t screenscraper_platform_id_count;
    jw_ra_scrape_name_source screenscraper_name_source;
    char screenscraper_lookup_extension[17];
    /* Content-pak artwork provenance. Paths stay pak-relative in the catalog;
       provider is Apps-relative and source_id names the owning card. */
    char *icon_flat;
    char *icon_photographic;
    char *wordmark;
    char *wordmark_provider;
    char *grid_icon;
    char *grid_icon_provider;
    char *wordmark_color;
    char *wordmark_color_provider;
    char *provider;
    char *source_id;
} jw_ra_system;

typedef struct {
    jw_ra_core *cores;
    size_t core_count;
    jw_ra_system *systems;
    size_t system_count;
    char *sdcard_root;
    char *info_dir;
} jw_ra_catalog;

typedef struct {
    char id[64];
    char display_name[128];
    char type[32];
    char file_name[256];
    char config_folder[256];
    char path[256];
    /* Empty for a release-owned core. Apps-relative providers are at most
       133 bytes ("<platform>/<Name>.pak"). */
    char provider[136];
    bool supports_menu;
    bool supports_savestate;
    bool supports_disk_control;
    bool needs_swap;
    bool requires_direct_drm;
    bool is_default;
} jw_ra_core_choice;

jw_ra_catalog *jw_ra_catalog_load(const char *sdcard_root, char *error, size_t error_size);
const jw_ra_catalog *jw_ra_catalog_get(const char *sdcard_root, char *error, size_t error_size);

/* Two explicit seams, never one shared "defaults dir".

   jw_ra_release_defaults_dir() -- the RELEASE-owned directory. Besides
   cores.json/systems.json it holds arcade_names.txt, retroarch.cfg,
   retroarch-record.cfg, and pulse-default.pa. Callers that want
   release-owned data must name this one: repointing a shared accessor at
   the effective catalog would have silently dropped arcade titles.

   jw_ra_effective_catalog_dir() -- where a catalog READER loads
   systems.json and cores.json from: the selected effective generation when
   structural validation passes, otherwise the release defaults. Returns 1
   for a generation, 0 for release defaults, -1 when neither resolved;
   `reason` receives "effective" or the CAT-1 fallback reason.

   Both honor the UMRK_PLATFORM_PATH / SYSTEM_PATH overrides, otherwise
   derive <sdcard_root>/<.system|UMRK>/<platform_id>/defaults. Returns 0 on
   success (out holds a NUL-terminated path), -1 on error. */
int jw_ra_release_defaults_dir(const char *sdcard_root, char *out, size_t out_size);
int jw_ra_effective_catalog_dir(const char *sdcard_root, char *out, size_t out_size,
                                char *reason, size_t reason_size);

/* Producer side (jawakad only): full provenance validation, then recompile
   and republish the effective catalog if any release input moved. Returns 0
   when a generation is published (`reason` is "published" or "reused"), 1
   when the caller must run on release defaults (`reason` says why), -1 on a
   hard error. */
int jw_ra_catalog_refresh(const char *sdcard_root, char *generation,
                          size_t generation_size, char *reason, size_t reason_size);
void jw_ra_catalog_free(jw_ra_catalog *catalog);

const jw_ra_system *jw_ra_catalog_find_system(const jw_ra_catalog *catalog, const char *system_id);
const jw_ra_system *jw_ra_catalog_match_system_folder(const jw_ra_catalog *catalog, const char *folder);
const jw_ra_core *jw_ra_catalog_find_core(const jw_ra_catalog *catalog, const char *core_id);

bool jw_ra_string_list_contains(const jw_ra_string_list *list, const char *value);
bool jw_ra_string_list_contains_casefold(const jw_ra_string_list *list, const char *value);
bool jw_ra_core_is_packaged_retroarch(const jw_ra_core *core);
/* RetroArch uses library_name verbatim as a FAT32 directory component. */
bool jw_ra_core_folder_is_safe(const char *folder);

/* One exact catalog core is launchable right now: a packaged RetroArch core
   whose file exists, or a packaged path core whose launcher is executable.
   Checks only this core, never a default or alternate in its place. */
bool jw_ra_catalog_core_available(const jw_ra_catalog *catalog,
                                  const jw_ra_core *core,
                                  const char *core_dir,
                                  const char *platform_dir);

/* Resolve one core against its live owner. Release cores use core_dir (or
   platform_dir for type:path); content cores use APPS_PATH/provider and keep
   only pak-relative paths in catalog data. require_executable applies to
   standalone path cores. */
/* The absolute path of a pak core's LEAF-RA-1 frontend, checked to be an
   executable regular file. -1 when the core has none or it is missing. */
int jw_ra_catalog_resolve_frontend_path(const jw_ra_catalog *catalog,
                                        const jw_ra_core *core,
                                        char *out,
                                        size_t out_size);
int jw_ra_catalog_resolve_core_path(const jw_ra_catalog *catalog,
                                    const jw_ra_core *core,
                                    const char *core_dir,
                                    const char *platform_dir,
                                    bool require_executable,
                                    char *out,
                                    size_t out_size);

/* CONTENT-ART slots resolved against their own provider's live Apps root.
   The resolved file must be a regular file inside that pak (symlinks are
   followed, then contained). Returns 0, or -1 with out[0] cleared. */
int jw_ra_catalog_resolve_system_wordmark_path(const jw_ra_catalog *catalog,
                                               const jw_ra_system *system,
                                               char *out, size_t out_size);

int jw_ra_catalog_resolve_system_grid_icon_path(const jw_ra_catalog *catalog,
                                               const jw_ra_system *system,
                                               char *out, size_t out_size);

int jw_ra_catalog_resolve_system_wordmark_color_path(const jw_ra_catalog *catalog,
                                                     const jw_ra_system *system,
                                                     char *out, size_t out_size);

/* The merged info directory belonging to this exact catalog snapshot. */
int jw_ra_catalog_info_dir(const jw_ra_catalog *catalog,
                           char *out,
                           size_t out_size);

/* Resolve one exact content-pak icon against its live Apps root. This only
   builds the path; the launcher owns candidate fallback and image decoding. */
int jw_ra_catalog_resolve_system_icon_path(const jw_ra_catalog *catalog,
                                           const jw_ra_system *system,
                                           bool photographic,
                                           char *out,
                                           size_t out_size);

int jw_ra_catalog_list_system_cores(const jw_ra_catalog *catalog,
                                    const char *system_id,
                                    const char *core_dir,
                                    const char *platform_dir,
                                    jw_ra_core_choice *out,
                                    size_t max_count,
                                    size_t *out_count);

int jw_ra_catalog_resolve_core_file(const jw_ra_catalog *catalog,
                                    const char *system_id,
                                    const char *core_dir,
                                    char *core_file,
                                    size_t core_file_size,
                                    char *core_id,
                                    size_t core_id_size,
                                    char *diagnostic,
                                    size_t diagnostic_size);

int jw_ra_catalog_resolve_core_file_for_choice(const jw_ra_catalog *catalog,
                                               const char *system_id,
                                               const char *preferred_core_id,
                                               const char *core_dir,
                                               char *core_file,
                                               size_t core_file_size,
                                               char *core_id,
                                               size_t core_id_size,
                                               char *diagnostic,
                                               size_t diagnostic_size);

#endif
