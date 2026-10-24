#ifndef JW_STORAGE_HEALTH_H
#define JW_STORAGE_HEALTH_H

#include <stdbool.h>
#include <stddef.h>

#include "internal/storage/sources.h"

/* SD card health: is a mounted card still writable, and if not, why.

   The kernel flips a FAT card read-only after a filesystem error
   (errors=remount-ro). Every later write fails on its own with an unrelated
   message, so this module answers one question for every writer: may this
   path be written right now. The current mount table is authoritative; kernel
   log lines only ever improve the explanation.

   Everything that touches the system takes its paths from
   jw_storage_probe_env, so host tests can point it at fixture files. */

#define JW_STORAGE_MOUNT_MAX 128
#define JW_STORAGE_DEVICE_MAX 128
#define JW_STORAGE_FS_TYPE_MAX 32
#define JW_STORAGE_OPTIONS_MAX 512
#define JW_STORAGE_UUID_MAX 64
#define JW_STORAGE_KERNEL_MESSAGE_MAX 256
#define JW_STORAGE_REQUEST_ID_MAX 64
#define JW_STORAGE_REASON_MAX 64
#define JW_STORAGE_REPAIR_MIN_BATTERY_PERCENT 30

/* Internal-storage recovery root on MLP1. Deliberately separate from the
   SD-backed USERDATA_PATH and UMRK_INTERNAL_DATA_PATH: it must stay writable
   when every card is read-only. */
#define JW_STORAGE_MLP1_RECOVERY_ROOT "/userdata/umrk"
#define JW_STORAGE_MLP1_REPAIR_DIR JW_STORAGE_MLP1_RECOVERY_ROOT "/storage-repair"
#define JW_STORAGE_MLP1_LOG_DIR JW_STORAGE_MLP1_RECOVERY_ROOT "/logs"
#define JW_STORAGE_MLP1_REPAIR_TOOL "/usr/bin/umrk-storage-repair"

typedef enum {
    JW_STORAGE_ACCESS_UNKNOWN = 0,
    JW_STORAGE_ACCESS_READ_WRITE,
    JW_STORAGE_ACCESS_READ_ONLY,
} jw_storage_access;

typedef enum {
    JW_STORAGE_CAUSE_UNKNOWN = 0,
    JW_STORAGE_CAUSE_FILESYSTEM_ERROR,
    JW_STORAGE_CAUSE_WRITE_PROTECTED,
} jw_storage_cause;

typedef enum {
    JW_STORAGE_REPAIR_NONE = 0,
    JW_STORAGE_REPAIR_PENDING,
    JW_STORAGE_REPAIR_RUNNING,
    JW_STORAGE_REPAIR_FAILED,
} jw_storage_repair;

typedef struct {
    char device[JW_STORAGE_DEVICE_MAX];
    char mount_point[JW_STORAGE_PATH_MAX];
    char fs_type[JW_STORAGE_FS_TYPE_MAX];
    char mount_options[JW_STORAGE_OPTIONS_MAX];
    char super_options[JW_STORAGE_OPTIONS_MAX];
    unsigned major;
    unsigned minor;
    bool read_only;   /* either the mount or the superblock says ro */
} jw_storage_mount;

typedef struct {
    jw_storage_mount entries[JW_STORAGE_MOUNT_MAX];
    int count;
} jw_storage_mount_table;

typedef struct {
    const char *mountinfo_path;   /* default /proc/self/mountinfo */
    const char *by_uuid_dir;      /* default /dev/disk/by-uuid */
    const char *by_label_dir;     /* default /dev/disk/by-label */
    const char *sys_dev_block;    /* default /sys/dev/block */
    const char *dev_dir;          /* default /dev; fixtures point it at image files */
    const char *repair_dir;       /* NULL: no repair state on this platform */
    bool require_mounted_roots;   /* a card root that is not a mount is missing */
    bool skip_statvfs;            /* fixtures: trust the mount table alone */
} jw_storage_probe_env;

typedef struct {
    bool mount_seen;        /* a mount line for this device survives in the log */
    bool dirty_at_boot;     /* "Volume was not properly unmounted" (informational) */
    bool set_read_only;     /* the kernel reported flipping this mount read-only */
    char message[JW_STORAGE_KERNEL_MESSAGE_MAX]; /* the first error line after mount */
} jw_storage_kernel_evidence;

typedef struct {
    char request_id[JW_STORAGE_REQUEST_ID_MAX];
    char origin[32];
    char trigger[32];   /* why the hold existed, e.g. paused-shutdown; "" if none */
    char outcome[32];
    char mount_state[32];
    char mode[16];
    bool changes_complete;
    int reported_change_count;
    bool acknowledged;
    bool valid;
} jw_storage_repair_result;

typedef struct {
    char source_id[JW_STORAGE_SOURCE_ID_MAX];
    char root[JW_STORAGE_PATH_MAX];
    bool mounted;
    jw_storage_access access;
    jw_storage_cause cause;
    jw_storage_repair repair;
    char device[JW_STORAGE_DEVICE_MAX];
    char fs_type[JW_STORAGE_FS_TYPE_MAX];
    char uuid[JW_STORAGE_UUID_MAX];
    char label[JW_STORAGE_UUID_MAX];
    unsigned major;
    unsigned minor;
    bool block_write_protected;
    bool dirty_at_boot;
    char kernel_message[JW_STORAGE_KERNEL_MESSAGE_MAX];
    char repair_request_id[JW_STORAGE_REQUEST_ID_MAX];
} jw_storage_health;

typedef enum {
    JW_STORAGE_WRITE_OK = 0,
    JW_STORAGE_WRITE_MISSING,
    JW_STORAGE_WRITE_READ_ONLY,
    JW_STORAGE_WRITE_REPAIR_HOLD,
    JW_STORAGE_WRITE_UNKNOWN,
} jw_storage_write_verdict;

void jw_storage_probe_env_default(jw_storage_probe_env *env);

int jw_storage_mount_table_parse(const char *mountinfo_text,
                                 jw_storage_mount_table *out);
int jw_storage_mount_table_read(const char *mountinfo_path,
                                jw_storage_mount_table *out);
const jw_storage_mount *jw_storage_mount_at(const jw_storage_mount_table *table,
                                            const char *mount_point);
/* Longest mount-point prefix containing abs_path. */
const jw_storage_mount *jw_storage_mount_for_path(const jw_storage_mount_table *table,
                                                  const char *abs_path);

/* Scan kernel log text for evidence about one block device ("mmcblk1").
   Evidence resets at each mount of that device, so an error from an earlier
   card or an earlier mount lifetime is never attached to the current one. */
void jw_storage_kernel_scan(const char *log_text, const char *device_name,
                            jw_storage_kernel_evidence *out);
/* Bounded read of the kernel ring buffer. Returns a malloc'd NUL-terminated
   buffer or NULL when unavailable (no permission, not Linux). */
char *jw_storage_kernel_log_read(void);

bool jw_storage_uuid_valid(const char *uuid);
/* Resolve "/dev/mmcblk1" to its UUID/label through the by-uuid/by-label links. */
bool jw_storage_device_uuid(const jw_storage_probe_env *env, const char *device,
                            char *uuid, size_t uuid_size);
bool jw_storage_device_label(const jw_storage_probe_env *env, const char *device,
                             char *label, size_t label_size);

/* Repair hold recorded by the boot repair runner for this volume UUID. */
jw_storage_repair jw_storage_repair_hold_for_uuid(const jw_storage_probe_env *env,
                                                  const char *uuid,
                                                  char *request_id,
                                                  size_t request_id_size);
/* The most recent completed result for this UUID. Legacy global pointers are
   accepted only when the summary names this exact UUID. */
/* Why this card is held ("paused-shutdown" when a shutdown could not prove it
   closed). Empty and false when there is no hold or it names no trigger. The
   hold's protection does not depend on this; it only selects wording. */
bool jw_storage_repair_hold_trigger(const jw_storage_probe_env *env, const char *uuid,
                                    char *out, size_t out_size);
bool jw_storage_repair_last_result(const jw_storage_probe_env *env, const char *uuid,
                                   jw_storage_repair_result *out);

/* Probe one configured card root. kernel_log may be NULL; the cause then
   stays unknown unless block write protection is visible. */
void jw_storage_health_probe(const jw_storage_probe_env *env,
                             const char *source_id, const char *root,
                             const char *kernel_log, jw_storage_health *out);

/* Fill identity into an unmounted probe result from an inserted card that is
   held for repair (hotplug refuses to mount it), so a check can still be
   requested. Only a FAT or exFAT volume that is not mounted anywhere counts.
   Returns false, leaving out unchanged, when there is none. */
bool jw_storage_health_probe_unmounted_hold(const jw_storage_probe_env *env,
                                            jw_storage_health *out);

const char *jw_storage_access_name(jw_storage_access access);
const char *jw_storage_cause_name(jw_storage_cause cause);
const char *jw_storage_repair_name(jw_storage_repair repair);
/* Stable, untranslated reason key for a denied write. */
const char *jw_storage_write_verdict_reason(jw_storage_write_verdict verdict);

/* Writer gate. Resolves path (or its nearest existing parent, for a file not
   yet created) to its mounted card and denies missing, read-only, unknown and
   repair-held cards. Advisory only: every write must still handle failure.
   reason receives a stable key suitable for mapping to user text. */
jw_storage_write_verdict jw_storage_path_check(const char *path,
                                               char *reason, size_t reason_size);
bool jw_storage_path_writable(const char *path, char *reason, size_t reason_size);
/* Same, against an explicit environment and card root list (host tests). */
jw_storage_write_verdict jw_storage_path_check_env(const jw_storage_probe_env *env,
                                                   const char *const *roots,
                                                   int root_count,
                                                   const char *path,
                                                   char *reason, size_t reason_size);

/* A writer saw errno err while writing path. EROFS (and EIO on a card)
   requests an immediate health refresh from the owning daemon. */
void jw_storage_report_write_error(const char *path, int err);
bool jw_storage_take_refresh_request(void);
bool jw_storage_errno_is_storage_failure(int err);

/* Per-source transition tracker kept by the daemon. */
typedef struct {
    jw_storage_health health;
    bool valid;
    bool kernel_checked;   /* evidence already read for this identity */
} jw_storage_health_slot;

typedef struct {
    jw_storage_health_slot slots[JW_STORAGE_MAX_SOURCES];
    int count;
    unsigned generation;
} jw_storage_health_monitor;

/* Fold a fresh probe into slot index. Returns true when anything a user or
   writer can observe changed; bumps generation and logs the transition once.
   *newly_read_only is set when this update is a new read-only observation for
   the card identity (the moment to read kernel evidence and warn). */
bool jw_storage_health_monitor_update(jw_storage_health_monitor *monitor,
                                      int index, const jw_storage_health *probe,
                                      bool *newly_read_only);

#endif /* JW_STORAGE_HEALTH_H */
