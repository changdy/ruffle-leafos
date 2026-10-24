#ifndef JW_LAUNCHER_BIOS_H
#define JW_LAUNCHER_BIOS_H

/* Saturn BIOS selection for the YabaSanshiro standalone emulator.

   The launcher persists a LOGICAL choice (source id + BIOS-root-relative path
   below the dedicated SATURN folder)
   and never an absolute mount path: the two MLP1 SD mounts swap across reboots,
   so an absolute path recorded today names a different card tomorrow. The
   absolute path exists only inside one resolved launch.

   Everything here is pure enough to test natively: parsing, precedence,
   validation and directory listing take paths, not a running UI. */

#include "internal/storage/sources.h"

#include <stdbool.h>
#include <stddef.h>

/* Game- and system-scoped content setting key (jw_db_*_setting). */
#define JW_CONTENT_SETTING_SATURN_BIOS "saturn_bios"

/* Canonical child of every source's general BIOS root. RetroArch and the
   standalone picker both use this folder for Saturn firmware. */
#define JW_BIOS_SATURN_SUBDIR "SATURN"

/* Every Saturn BIOS image is a raw 512 KiB dump: the emulator allocates
   0x80000 bytes and fills them with T123Load(BiosRom, 0x80000, 2, file).
   A file of another size cannot be one, so it is not offered and not accepted.
   The converse does not hold -- a 512 KiB file may belong to another console,
   or be no firmware at all -- so nothing here calls a candidate verified. */
#define JW_BIOS_SATURN_IMAGE_BYTES 524288

#define JW_BIOS_NAME_MAX 256
#define JW_BIOS_REL_PATH_MAX 512
/* "file:" + source id + ":" + relative path. */
#define JW_BIOS_VALUE_MAX (JW_STORAGE_SOURCE_ID_MAX + JW_BIOS_REL_PATH_MAX + 8)

typedef enum {
    JW_BIOS_CHOICE_DEFAULT = 0, /* no override at this scope */
    JW_BIOS_CHOICE_HLE,         /* explicit "no BIOS file" */
    JW_BIOS_CHOICE_FILE,        /* one deliberate staged image */
} jw_bios_choice_kind;

typedef struct {
    jw_bios_choice_kind kind;
    char source_id[JW_STORAGE_SOURCE_ID_MAX];
    char rel_path[JW_BIOS_REL_PATH_MAX];
} jw_bios_choice;

typedef enum {
    JW_BIOS_ORIGIN_DEFAULT = 0,
    JW_BIOS_ORIGIN_SYSTEM,
    JW_BIOS_ORIGIN_GAME,
} jw_bios_origin;

typedef struct {
    /* Never JW_BIOS_CHOICE_DEFAULT: an absent override resolves to HLE, which
       is what the emulator has always done. */
    jw_bios_choice choice;
    jw_bios_origin origin;
} jw_bios_resolution;

/* Parse a stored content-setting value. An empty or unparsable value is
   JW_BIOS_CHOICE_DEFAULT, so a setting written by a newer build degrades to
   today's behavior instead of selecting some other file. */
void jw_bios_choice_parse(const char *value, jw_bios_choice *out);
/* Format for storage. JW_BIOS_CHOICE_DEFAULT has no representation -- delete
   the setting instead -- so it returns false. */
bool jw_bios_choice_format(const jw_bios_choice *choice, char *out, size_t out_size);
bool jw_bios_choice_equal(const jw_bios_choice *a, const jw_bios_choice *b);

/* Game override, then system override, then HLE. */
void jw_bios_resolve(const char *game_value, const char *system_value,
                     jw_bios_resolution *out);

typedef enum {
    JW_BIOS_FILE_OK = 0,
    JW_BIOS_FILE_NO_CHOICE,          /* not a file choice */
    JW_BIOS_FILE_INVALID_PATH,
    JW_BIOS_FILE_SOURCE_UNAVAILABLE, /* card absent or not configured */
    JW_BIOS_FILE_MISSING,            /* moved, renamed or deleted */
    JW_BIOS_FILE_OUTSIDE_ROOT,
    JW_BIOS_FILE_NOT_REGULAR,
    JW_BIOS_FILE_UNREADABLE,
    JW_BIOS_FILE_WRONG_SIZE,
} jw_bios_file_status;

/* Re-resolve a stored choice against the sources mounted right now. Succeeds
   only for a readable regular file of exactly JW_BIOS_SATURN_IMAGE_BYTES that
   lives inside its own source's BIOS/SATURN folder. */
jw_bios_file_status jw_bios_resolve_file(const jw_storage_source_list *sources,
                                         const jw_bios_choice *choice,
                                         char *out_abs, size_t out_abs_size);
/* Stable English one-liner; the UI translates it, the daemon logs it. */
const char *jw_bios_file_status_text(jw_bios_file_status status);

/* ── Bounded folder listing ─────────────────────────────────────────────────
   The picker enumerates only the folder the user is standing in, one bounded
   page at a time. Rows are ordered subfolders-then-files, byte-ascending by
   name, so paging is stable and a page can be revisited by re-enumeration
   without a persistent index. */

#define JW_BIOS_PAGE_ROWS 256   /* eligible rows retained per page */
#define JW_BIOS_SCAN_BATCH 128  /* entries examined between cancellation checks */

typedef struct {
    bool is_dir;
    char name[JW_BIOS_NAME_MAX];
} jw_bios_entry;

typedef bool (*jw_bios_cancel_fn)(void *ctx);

typedef struct {
    int examined;      /* directory entries looked at */
    bool cancelled;    /* the caller asked to stop; rows are incomplete */
    bool has_more;     /* eligible rows exist after the last returned row */
    bool failed;       /* the directory could not be read */
} jw_bios_list_result;

/* List one page of `dir_abs`. `after` is the last row of the previous page
   (NULL for the first page); rows strictly after it are returned. Symlinks are
   skipped rather than followed. Returns 0 on success. */
int jw_bios_list_dir(const char *dir_abs, const jw_bios_entry *after,
                     jw_bios_entry *out, int max_out, int *out_count,
                     jw_bios_cancel_fn cancel, void *cancel_ctx,
                     jw_bios_list_result *result);

/* Row ordering: subfolders first, then byte-ascending name. */
int jw_bios_entry_compare(const jw_bios_entry *a, const jw_bios_entry *b);

/* Append `name` to `rel_dir` ("" for the BIOS root). Rejects anything that
   would leave the root. */
bool jw_bios_rel_join(const char *rel_dir, const char *name,
                      char *out, size_t out_size);
/* Strip the last component of `rel_dir`; "" stays "". */
void jw_bios_rel_parent(const char *rel_dir, char *out, size_t out_size);

#endif /* JW_LAUNCHER_BIOS_H */
