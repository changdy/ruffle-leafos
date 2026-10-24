#ifndef JW_SERVICES_LAUNCH_H
#define JW_SERVICES_LAUNCH_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

/* app-services-v1 (SVC-1) service launch: the fork()/exec() half of
 * supervision step 3
 * (plans/leaf-syncthing/contracts.md#svc-1--service-manifest,
 * "Supervision and process ownership").
 *
 * This module turns one already-validated manifest plus an already-held
 * generation-lease descriptor into a running supervised process group.
 * It is deliberately NOT the supervisor: it does not validate the
 * manifest (internal/services/manifest.c), acquire the lease
 * (internal/services/lease.c), record the ownership reservation
 * (internal/services/reservation.c), decide restart/backoff
 * (internal/services/backoff.c), or stop the group
 * (internal/services/stop.c). It only LAUNCHES, exactly once per call,
 * and reports the new leader's pid so the caller can persist the
 * reservation.
 *
 * What the child does, in order, before exec:
 *
 *   1. setpgid(0, 0)        -- a NEW process group whose pgid equals the
 *                              child pid. This is the group every later
 *                              SVC-1 operation (stop, absence check,
 *                              signalling) targets, and the reason the
 *                              caller must not reap the leader early
 *                              (see below).
 *   2. dup2(lease_fd, 3)    -- first relocate the caller's lease/log fds
 *                              off all reserved descriptors, then place the
 *                              inherited generation lease on descriptor 3
 *                              with FD_CLOEXEC CLEARED so it survives exec.
 *                              UMRK_SERVICE_LEASE_FD=3 was prepared in the
 *                              parent-built child environment. Per SVC-1
 *                              the lock rides the open file description
 *                              across fork+exec and is released only when
 *                              the last duplicate closes (ordinary exit).
 *   3. PR_SET_PDEATHSIG     -- Linux only (MLP1): ask the kernel to send
 *                              the service SIGTERM when its parent
 *                              (Jawaka) dies, closing the set-then-
 *                              recheck race per SVC-1's "Descendant
 *                              survival" note. There is no macOS
 *                              analogue (this dev machine's 26.x SDK has
 *                              no pdwaitpid), so on macOS this step is a
 *                              documented no-op and supervisor-death
 *                              cleanup is exercised on Linux instead.
 *   4. stdout/stderr        -- redirected into the service's rotating
 *                              bounded log (see jw_svc_launch_open_log)
 *                              per supervision step 5.
 *   5. exec                 -- run.path resolved against the pak root,
 *                              with run.args, in the caller-supplied
 *                              environment.
 *
 * THE CALLER OWNS THE RESERVATION RULE. On success this returns the
 * leader's pid (== its pgid). SVC-1 requires Jawaka, as the leader's
 * parent, to NOT waitpid() it until the whole group is confirmed
 * writer-free: an unreaped leader keeps its pid -- and therefore the
 * pgid number -- allocated, so a recycled pgid can never be mistaken for
 * this service. Reaping happens only after jw_svc_group_absent()
 * reports the group gone, never before. This module does not enforce
 * that ordering; it cannot. It only hands back the pid.
 */

#define JW_SVC_LAUNCH_LEASE_FD 3
#define JW_SVC_LAUNCH_LEASE_ENV "UMRK_SERVICE_LEASE_FD"

/* Rotating log caps, SVC-1 supervision step 5: "capped at 5 files x
 * 256 KiB" under $LOGS_PATH/services/<service-id>/. The current file is
 * <id>.log; rotated generations are <id>.log.1 (newest) .. .4 (oldest),
 * so at most JW_SVC_LOG_MAX_FILES managed files exist and the oldest is
 * dropped on rotation. Rotation is deliberately checked only when a new
 * generation opens the log: an active current file can temporarily grow
 * beyond the cap, but it is truncated to JW_SVC_LOG_MAX_BYTES before it
 * becomes an archived generation. */
#define JW_SVC_LOG_MAX_FILES 5
#define JW_SVC_LOG_MAX_BYTES (256 * 1024)

#define JW_SVC_LAUNCH_REASON_BUF 64

typedef struct {
    /* Absolute path to the service executable, already validated by
     * jw_service_manifest_validate() to be confined to the pak, regular,
     * and executable. Copied into the child before fork. */
    const char *run_path_abs;
    /* argv[1..]; argv[0] is always run_path_abs. At most
     * JW_SVC_LAUNCH_MAX_ARGS entries, each already length-checked by
     * manifest validation. May be empty (args_count == 0). */
    const char *const *args;
    int args_count;

    /* The Leaf runtime environment for the child, as a JSON object
     * serialized with cJSON_PrintUnformatted(): {"NAME":"value", ...}.
     * Parsed into a complete child-only environment in the PARENT before
     * fork (failures fail the launch, not the child), with
     * UMRK_SERVICE_LEASE_FD=3 forced on top. Passing it as text -- rather
     * than mutating the real environ -- keeps the parent's environment
     * untouched and lets the child call async-signal-safe execve()
     * directly. May be NULL/empty for no extra variables. */
    const char *env_json;

    /* An open, locked descriptor from jw_svc_lease_acquire(). dup2()'d
     * to descriptor 3 in the child with FD_CLOEXEC cleared. Must be >=
     * 0; the launch is refused otherwise (a service must never start
     * without its no-overlap gate held). */
    int lease_fd;

    /* Open log descriptor from jw_svc_launch_open_log(), or -1 to log
     * the service to /dev/null instead. The child always gets a valid
     * stdout/stderr target; a missing log is never a reason to leave the
     * service attached to Jawaka's own stdout. */
    int log_fd;
} jw_svc_launch_request;

#define JW_SVC_LAUNCH_MAX_ARGS 16

/* Opens (creating if needed) the current rotating log for `service_id`
 * under `logs_dir` ($LOGS_PATH), applying the cap above: when the
 * current file has reached JW_SVC_LOG_MAX_BYTES it is first bounded and
 * rotated to <id>.log.1 (shifting .1->.2 ... .4 dropped), and a fresh
 * current file is opened. Creates logs_dir/services/<service_id>/ as
 * owner-owned and not group/world-writable. Returns an O_APPEND|O_CLOEXEC
 * descriptor for the child to inherit explicitly, or -1 with a slug in
 * `reason` (may be NULL):
 *   "invalid-service-id" / "logs-dir-unavailable" / "path-too-long"
 *   "mkdir-failed" / "rotate-failed" / "open-failed"
 * The caller passes the result as jw_svc_launch_request.log_fd and
 * closes its own copy after the launch (the child has its own). */
int jw_svc_launch_open_log(const char *logs_dir, const char *service_id,
                           char *reason, size_t reason_size);

/* Launches one service process group per the sequence above.
 *
 * Returns the leader's pid (> 0, == its pgid) on success. The caller
 * must then record the ownership reservation for that pgid and must NOT
 * reap the leader until the group is verified absent.
 *
 * Returns -1 on failure with a stable slug in `reason` (may be NULL):
 *   "invalid-request"   run_path_abs missing, args out of range, or
 *                       lease_fd < 0, log_fd < -1, or the same descriptor
 *                       supplied for both the lease and log
 *   "env-parse-failed"  env_json is not a flat JSON string object
 *   "open-log-failed"   a valid log_fd could not be used (see open_log)
 *   "fork-failed"       parent-side launch setup, fork(), child setup, or
 *                       execve() failed; no launched child remains
 * Child setup/exec failures are reported synchronously over an internal
 * CLOEXEC pipe, reaped here, and returned as -1. A returned pid therefore
 * means execve() closed that pipe and the launched leader is now the
 * caller's reservation responsibility. */
pid_t jw_svc_launch(const jw_svc_launch_request *req,
                    char *reason, size_t reason_size);

#endif
