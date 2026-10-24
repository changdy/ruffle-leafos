#ifndef JW_RETROARCH_LEGACY_MIGRATION_H
#define JW_RETROARCH_LEGACY_MIGRATION_H

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    JW_RA_LEGACY_MIGRATION_NOT_APPLICABLE = 0,
    JW_RA_LEGACY_MIGRATION_NO_FILES,
    JW_RA_LEGACY_MIGRATION_COPIED,
    JW_RA_LEGACY_MIGRATION_AMBIGUOUS,
    JW_RA_LEGACY_MIGRATION_FAILED
} jw_ra_legacy_migration_result;

typedef struct {
    int copied;
    int existing;
    int failed;
    char detail[1024];
} jw_ra_legacy_migration_report;

/* Cheap, read-only preflight used before DB ambiguity work. Returns true only
   when a recognized flat source has no scoped destination yet. Retained flat
   sources therefore cost no database scan on subsequent launches. */
bool jw_ra_legacy_flat_files_need_recovery(const char *source_root,
                                           const char *rom_path,
                                           const char *core_config_folder);

/* Recover legacy flat RetroArch saves/states into one explicitly declared core
   namespace. Sources are retained and existing destinations always win. */
jw_ra_legacy_migration_result jw_ra_migrate_legacy_flat_files(
    const char *source_root,
    const char *rom_path,
    const char *effective_core_id,
    const char *core_config_folder,
    const char *legacy_flat_core,
    bool ambiguous,
    jw_ra_legacy_migration_report *out);

#endif
