#ifndef JW_CATALOG_EFFECTIVE_H
#define JW_CATALOG_EFFECTIVE_H

#include <stdbool.h>
#include <stddef.h>

/* CAT-1, the effective catalog. Normative text:
   umrk-workspace/plans/pak-rat/content-paks-contract.md#cat-1
   Schemas and fixtures: leaf-contracts/contracts/leaf-content/

   The catalog a reader loads is a content-addressed immutable generation
   directory, selected by a one-line file that is replaced atomically:

     $UMRK_INTERNAL_DATA_PATH/catalog/
       current              "gen-<64 hex>\n", replaced by rename()
       diagnostics.json     OUTSIDE any generation, so a failed compile
                            still leaves a readable explanation
       gen-<digest>/        written once, never mutated
         systems.json  cores.json  info/  stamp.json

   A directory cannot replace a non-empty directory via rename() -- that
   fails ENOTEMPTY -- which is why publication swaps a small file and never
   the directory itself.

   Producer and readers do deliberately different amounts of work:

     jawakad   full provenance validation (rehash every input) at startup
               and before a ROM scan, then recompile and republish
     readers   structural validation only, per catalog load

   Readers never rehash a core binary on a request path; freshness is the
   producer's job. There is no "serve the last good generation" branch
   anywhere: a stale catalog silently pins a user to a pre-OTA system list,
   which is worse than not shipping the feature. */

/* "gen-" + 64 hex + NUL. */
#define JW_CAT_GEN_NAME_MAX 69
#define JW_CAT_REASON_MAX 64

typedef enum {
    /* out_dir holds a generation directory the caller should load. */
    JW_CAT_EFFECTIVE = 0,
    /* Fall back to the release defaults; reason says why. */
    JW_CAT_RELEASE_DEFAULTS
} jw_catalog_resolution;

/* $UMRK_INTERNAL_DATA_PATH/catalog, else <sdcard_root>/.umrk/<platform>/catalog.
   Resolves only; creates nothing. Returns 0 on success. */
int jw_catalog_dir(const char *sdcard_root, char *out, size_t out_size);

/* Structural validation -- what EVERY reader does, per catalog load.
   Parses the selector, requires that generation and its stamp, and checks
   the stamp's schema, platform, and installed release_id. Never hashes a
   contributor binary. */
jw_catalog_resolution jw_catalog_effective_dir(const char *sdcard_root,
                                               char *out_dir,
                                               size_t out_dir_size,
                                               char *reason,
                                               size_t reason_size);

/* Full provenance validation, then recompile and republish if anything
   moved. jawakad only -- at startup and before a ROM scan.

   On a mismatch the selector is unlinked and the catalog directory fsynced
   BEFORE recompiling, so every new reader falls back to release defaults
   during the gap and a crash mid-compile leaves no selector rather than one
   pointing at unverified output.

   Returns 0 when an effective generation is published (its name in
   `generation`), 1 when the caller must run on release defaults (`reason`
   says why), -1 on a hard error. */
int jw_catalog_refresh(const char *sdcard_root,
                       const char *release_defaults_dir,
                       char *generation,
                       size_t generation_size,
                       char *reason,
                       size_t reason_size);

/* Producer entry point. `contributors` is a sorted array of validated
   {provider,source_id,pak_version,pak_dir,provides} objects, optionally with
   a validated content_scrape companion. `diagnostics` is the enumeration
   validator's entries; merge diagnostics are appended. */
typedef struct cJSON cJSON;
int jw_catalog_refresh_with_contributors(const char *sdcard_root,
                                         const char *release_defaults_dir,
                                         const cJSON *contributors,
                                         const cJSON *diagnostics,
                                         char *generation,
                                         size_t generation_size,
                                         char *reason,
                                         size_t reason_size);

/* Fail closed and leave a CAT-1 diagnostic outside every generation. This is
   also used by discovery failures that happen before the compiler is entered,
   so "compile-failed" never becomes a log-only explanation. */
int jw_catalog_record_failure(const char *sdcard_root,
                              const char *reason,
                              const char *detail);

/* Remove abandoned tmp-* directories. Never a finalized generation: a
   CentralScrutinizer process can outlive a jawakad crash, so even daemon
   startup is not a reader-free window. Content addressing already keeps
   no-op recompiles from growing the directory. */
int jw_catalog_cleanup_temps(const char *sdcard_root, size_t *out_removed);

/* --- CAT-1 primitives, exposed for the fixture-driven tests --- */

/* Exactly "gen-" + 64 lowercase hex + one '\n'. Strict on purpose: this
   value names a directory a reader is about to open. */
bool jw_catalog_parse_selector(const char *raw, char *out, size_t out_size);

/* For each regular file under `dir`, ascending by relative path in byte
   order: rel_path || 0x00 || uint64_be(len) || bytes.

   The length prefix is not decoration. Without it {"a":"x","b":"yz"} and
   {"a":"xy","b":"z"} hash identically, and an info directory could be
   rewritten across a file boundary while keeping a valid stamp. A missing
   directory hashes as empty -- neither it nor an empty one can contribute
   an .info file, so neither should change the generation identity. */
int jw_catalog_tree_sha256(const char *dir, char out_hex[65]);

#endif /* JW_CATALOG_EFFECTIVE_H */
