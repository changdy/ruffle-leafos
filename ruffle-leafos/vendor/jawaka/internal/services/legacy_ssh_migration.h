#ifndef JW_SERVICES_LEGACY_SSH_MIGRATION_H
#define JW_SERVICES_LEGACY_SSH_MIGRATION_H

#include "internal/services/control_state.h"

#include <stdbool.h>
#include <stddef.h>

#define JW_SVC_LEGACY_SSH_SERVICE_ID "org.umrk.sshserver"
#define JW_SVC_LEGACY_SSH_MIGRATION_ID "release-a-ssh-intent-v1"

typedef struct {
    bool applied;
    bool enabled;
    bool config_present;
    bool config_valid;
} jw_svc_legacy_ssh_migration_report;

/* Converts the old hook's implicit "configured means autostart" behavior into
 * one persistent SVC-1 intent decision. A missing or structurally invalid
 * config completes the migration disabled. An I/O failure does not write the
 * marker and is retried on the next daemon start. */
bool jw_svc_migrate_legacy_ssh_intent(
    jw_svc_control_store *store, const char *config_path,
    jw_svc_legacy_ssh_migration_report *out,
    char *reason, size_t reason_size);

#endif
