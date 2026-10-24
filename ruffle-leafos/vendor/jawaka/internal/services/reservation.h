#ifndef JW_SERVICES_RESERVATION_H
#define JW_SERVICES_RESERVATION_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* SVC-1's ownership reservation (contracts.md#svc-1--service-manifest,
 * "Supervision and process ownership" step 4): "atomically records the
 * group's ownership reservation in runtime tmpfs: the leader's pid (which
 * is the pgid), monotonic launch instant, and a normalized snapshot of
 * the validated lifecycle policy... A missing/corrupt policy snapshot
 * fails safe after a daemon restart."
 *
 * This module is the record's storage alone: atomic write (unique temp file in
 * the same directory, full write, fsync, rename, then a parent-directory
 * fsync -- the exact recipe contracts.md's LIFE-1 section spells out for
 * its own active-launch record, reused verbatim here since it is the
 * same durability requirement) and a read that distinguishes "no record"
 * from "a record exists but could not be trusted" -- SVC-1 requires a
 * caller to react differently to each (a missing record for a service
 * that was never running is unremarkable; a corrupt one after a crash
 * must fail safe as though every stop trigger were enabled).
 *
 * This module does not create or harden the per-service directory it
 * writes into -- that is internal/services/lease.c's job
 * (jw_svc_lease_acquire()), which SVC-1's own ordering runs first. A
 * write here assumes that directory already exists.
 */

/* Distinct name from manifest.h's jw_svc_lifecycle_game (internal/
 * services/manifest.h, a separately-reviewed, independently-branched
 * module) even though the three values mean the same thing -- these
 * primitives are deliberately independent of each other's headers. */
typedef enum {
    JW_SVC_RESERVATION_GAME_IGNORE = 0,
    JW_SVC_RESERVATION_GAME_STOP,
    JW_SVC_RESERVATION_GAME_NOTIFY,
} jw_svc_reservation_game_policy;

typedef struct {
    /* The leader's pid, which SVC-1 states IS the pgid (the leader calls
     * setpgid(0, 0) before exec). Must be > 0. */
    pid_t pgid;
    /* CLOCK_MONOTONIC microseconds -- meaningful only within the current
     * boot, which is exactly the lifetime of the runtime tmpfs this
     * record lives in. Use jw_svc_reservation_now_us() to produce it. */
    long long launch_instant_us;
    jw_svc_reservation_game_policy game_policy;
    bool stop_on_storage_change;
    bool stop_on_suspend;
} jw_svc_reservation;

/* CLOCK_MONOTONIC now, in microseconds, for populating launch_instant_us.
 * Returns -1 if the clock cannot be read or represented. */
long long jw_svc_reservation_now_us(void);

/* Atomically writes `reservation` as the ownership record for
 * `service_id` under `runtime_dir` (services/<service_id>/reservation),
 * replacing any existing record. Returns true on success.
 *
 * The JSON object contains exactly the five keys `pgid`,
 * `launch_instant_us`, `game`, `stop_on_storage_change`, and
 * `stop_on_suspend`.
 *
 * Returns false with a stable slug in `reason` (may be NULL to discard):
 *   "invalid-arguments"       service_id or reservation is invalid
 *                             (empty/unsafe service id, non-positive or
 *                             unrepresentable pgid, launch instant outside
 *                             the lossless JSON integer range, or unknown
 *                             game policy)
 *   "path-too-long"           a constructed path exceeded PATH_MAX
 *   "service-dir-unavailable" services/<service_id>/ does not exist (or
 *                             is not a directory) -- acquire the
 *                             generation lease first
 *   "out-of-memory"           could not build the record's JSON encoding
 *   "write-failed"            the temp-write/fsync/rename/dirfsync
 *                             sequence failed at some step
 */
bool jw_svc_reservation_write(const char *runtime_dir, const char *service_id,
                              const jw_svc_reservation *reservation,
                              char *reason, size_t reason_size);

/* Reads the ownership record for `service_id` under `runtime_dir` into
 * *out. Returns true and fills *out only for a fully well-formed record.
 *
 * Returns false with a stable slug in `reason` (may be NULL to discard),
 * *out left zeroed:
 *   "invalid-arguments"    service_id or out is invalid
 *   "path-too-long"        a constructed path exceeded PATH_MAX
 *   "reservation-missing"  no record file exists -- distinct from
 *                          "corrupt" because SVC-1 treats these
 *                          differently: missing is unremarkable for a
 *                          service that was never running; a record that
 *                          exists but cannot be trusted must fail safe.
 *   "reservation-corrupt"  a record file exists but could not be read
 *                          fully, parsed, or type-checked field by field
 *                          (unknown/duplicate keys, non-integral or
 *                          out-of-range numbers, embedded NULs, and
 *                          non-regular/symlinked records are also corrupt)
 */
bool jw_svc_reservation_read(const char *runtime_dir, const char *service_id,
                             jw_svc_reservation *out,
                             char *reason, size_t reason_size);

#endif
