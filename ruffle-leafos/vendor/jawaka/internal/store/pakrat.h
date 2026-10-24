#ifndef JW_STORE_PAKRAT_H
#define JW_STORE_PAKRAT_H

#include "internal/store/pakrat_kind.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>

typedef struct jw_pakrat_uninstall_info jw_pakrat_uninstall_info;

/* Why an install was refused before anything changed on the card, for refusals
   the store can explain better than a log line. */
typedef enum {
    JW_PAKRAT_REFUSED_NONE = 0,
    JW_PAKRAT_REFUSED_NEEDS_ADOPTION,  /* an unowned folder or pak is in the way */
    JW_PAKRAT_REFUSED_WITHDRAWN,       /* the catalog withdrew this theme */
    JW_PAKRAT_REFUSED_RESERVED_NAME,   /* a bundled theme owns this folder name */
    JW_PAKRAT_REFUSED_THEME_LIMIT,     /* Themes/ already holds JW_USER_THEME_MAX folders */
    JW_PAKRAT_REFUSED_INVALID_THEME,   /* the archive failed THEME-1: theme_reasons */
    JW_PAKRAT_REFUSED_THEME_NOT_LISTED,/* the launcher's own scan did not list it */
    JW_PAKRAT_REFUSED_CHECKSUM,        /* the download's size or SHA-256 is not the catalog's */
} jw_pakrat_refusal;

/* What a finished action means for the rest of the launcher. Pak Rat runs on
   a worker thread and never touches launcher state, so the render thread reads
   this after the job and acts on it: rescan user themes and drop theme art
   memos when themes_changed, and clear the in-memory selection when
   theme_selection_cleared (the persisted setting is already None). */
typedef struct {
    jw_pakrat_kind kind;
    jw_pakrat_refusal refusal;
    uint64_t theme_reasons;           /* THEME-1 reason mask (theme_package.h) */
    int themes_changed;               /* a folder under Themes/ was added, replaced or removed */
    int theme_updated;                /* an installed theme was replaced in place */
    int theme_selection_cleared;      /* the removed theme was selected; the setting is now None */
    char theme_dir[128];              /* the folder under Themes/ that changed */
} jw_pakrat_outcome;

typedef struct {
    char platform[64];
    char sdcard_root[PATH_MAX];
    char state_dir[PATH_MAX];
    char db_path[PATH_MAX];
    char platform_root[PATH_MAX];
    char runtime_dir[PATH_MAX];
    char socket_path[PATH_MAX];
    char *error_message;
    size_t error_message_size;
    jw_pakrat_outcome *outcome;       /* optional; zeroed when an action starts */
} jw_pakrat_context;

int jw_pakrat_rescan(const jw_pakrat_context *ctx);
/* Install (or update) a catalog pak or theme. allow_adopt != 0 permits
   replacing a pak or theme folder already on disk that Pak Rat does not own (a
   manual install); the caller is responsible for getting the user's consent
   first. A theme is held to THEME-1 before it is promoted, installs only to the
   primary card, and never takes a bundled theme's name. */
int jw_pakrat_install_app(const jw_pakrat_context *ctx, const char *store_id,
                          int allow_adopt);
/* Install the exact version selected by a prior catalog read. The installer
   re-runs normal compatibility selection and refuses if it no longer resolves
   to expected_version. */
int jw_pakrat_install_app_target(const jw_pakrat_context *ctx,
                                 const char *store_id,
                                 const char *expected_version,
                                 int allow_adopt);
/* Repair an owned install from its exact immutable historical version. The
   gate exception applies only when version equals the ownership record. */
int jw_pakrat_repair_app_version(const jw_pakrat_context *ctx,
                                 const char *store_id,
                                 const char *version);
/* Uninstalling the selected theme also sets the theme setting to None, in the
   same commit that drops the ownership row (outcome->theme_selection_cleared). */
int jw_pakrat_uninstall_app(const jw_pakrat_context *ctx, const char *store_id);
/* Read-only retained-data disclosure from Jawaka's cached, last-validated
   manifest metadata. Present and absent cards are both represented. */
int jw_pakrat_get_uninstall_info(const jw_pakrat_context *ctx,
                                 const char *store_id,
                                 jw_pakrat_uninstall_info *out);
void jw_pakrat_free_uninstall_info(jw_pakrat_uninstall_info *info);
/* Separate destructive action; never runs as an implicit part of uninstall. */
int jw_pakrat_remove_retained_data(const jw_pakrat_context *ctx,
                                   const char *store_id);

/* A theme's store preview is a catalog-hashed image, so the same bounds apply
   as to an artifact: the advertised size, exactly, and the advertised SHA-256.
   It is decoded whole on a 1 GB device, hence the byte and pixel caps. */
#define JW_PAKRAT_PREVIEW_MAX_BYTES (4LL * 1024LL * 1024LL)
#define JW_PAKRAT_PREVIEW_MAX_PX    2048
/* Previews kept on the card; the least recently used go first. */
#define JW_PAKRAT_PREVIEW_CACHE_MAX 64

/* Fetch a theme preview into <state_dir>/store/previews/<sha256>.<png|jpg>,
   or reuse the cached copy. A file only takes that name once its size and
   SHA-256 match and its header says PNG or JPEG within
   JW_PAKRAT_PREVIEW_MAX_PX per edge, so a cached name is always safe to decode.
   HTTP is allowed only when the catalog is a developer override, as for
   artifacts. Blocking network and file I/O: call it from a worker thread.
   Returns 0 with the path, -1 when the preview is unavailable or refused. */
int jw_pakrat_fetch_preview(const char *state_dir, const char *url,
                            const char *sha256, long long size,
                            char *out_path, size_t out_size);

#endif
