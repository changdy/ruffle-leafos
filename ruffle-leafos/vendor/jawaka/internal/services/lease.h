#ifndef JW_SERVICES_LEASE_H
#define JW_SERVICES_LEASE_H

#include <stddef.h>

/* SVC-1's generation lease (contracts.md#svc-1--service-manifest, "The
 * generation lease closes the daemon-restart overlap"). Attempts a
 * SINGLE non-blocking exclusive flock() acquisition on the stable
 * generation.lease file for `service_id` under `runtime_dir`
 * ($UMRK_RUNTIME_PATH).
 *
 * Creates runtime_dir/services/<service_id>/ (owner-only, 0700) and the
 * lease file within it if they do not already exist -- but NEVER
 * unlinks, truncates, or recreates an EXISTING lease file: its inode
 * identity is what makes an old generation's held lock observable to a
 * new Jawaka generation across a daemon restart. This function performs
 * exactly one non-blocking attempt; a caller that wants SVC-1's "retry
 * the non-blocking acquisition once per second while desired enablement
 * remains true" policy implements that retry loop itself around this
 * call -- that policy is daemon-scheduling logic, not part of the
 * primitive.
 *
 * Return value on success: an open, LOCKED, close-on-exec file descriptor
 * >= 0. The caller must keep it open for the lifetime of the service
 * generation it represents (per SVC-1's inherited-holder/guardian shapes -- e.g.
 * duplicating it to descriptor 3 in a forked child before exec, with
 * UMRK_SERVICE_LEASE_FD=3 exported and FD_CLOEXEC cleared there) and must
 * never call flock(fd, LOCK_UN) or close it while that generation is
 * still meant to be exclusive. There is deliberately no
 * jw_svc_lease_release(): closing the last open duplicate of the
 * returned descriptor -- which ordinary process exit does automatically
 * -- IS the release. An explicit unlock function would invite a caller
 * to "give back" a lease it still represents, defeating the mechanism by
 * letting a new generation start over one that has not actually gone
 * away.
 *
 * Return value on failure: -1, with a stable slug copied into `reason`
 * (may be NULL to discard it):
 *   "invalid-service-id"      service_id is empty, contains '/', or is
 *                             exactly "." or ".."
 *   "runtime-dir-unavailable" runtime_dir is empty, does not exist, or is
 *                             not a directory
 *   "path-too-long"           a constructed path exceeded PATH_MAX
 *   "mkdir-failed"            could not create/verify an owner-owned,
 *                             owner-only, non-symlink directory in the
 *                             services/<service_id>/ path
 *   "open-failed"             could not open/create a regular,
 *                             owner-owned, owner-only, non-symlink lease
 *                             file
 *   "lock-failed"             flock() failed for a reason other than the
 *                             lock already being held
 *   "stale-generation"        the lock is already held (EWOULDBLOCK): an
 *                             old cooperating generation still exists.
 *                             Per SVC-1, desired enablement is kept and
 *                             no replacement is started; this is a
 *                             positive, retryable condition, not a fault.
 */
int jw_svc_lease_acquire(const char *runtime_dir, const char *service_id,
                         char *reason, size_t reason_size);

#endif
