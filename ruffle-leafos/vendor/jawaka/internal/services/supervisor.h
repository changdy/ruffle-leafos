#ifndef JW_SERVICES_SUPERVISOR_H
#define JW_SERVICES_SUPERVISOR_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#include "internal/services/backoff.h"
#include "internal/services/control_state.h"
#include "internal/services/legacy_ssh_migration.h"
#include "internal/services/manifest.h"
#include "internal/services/stop.h"

/* app-services-v1 (SVC-1) service supervisor: the piece that actually wires
 * the independent primitives into a working supervise-a-service loop.
 *
 * Normative source: plans/leaf-syncthing/contracts.md#svc-1--service-manifest
 * ("Supervision and process ownership", "Stop sequence", "When a stop cannot
 * be verified", "Restart policy") and #ctl-1--control-and-status-ipc.
 *
 * This module OWNS, per service id:
 *   - manifest revalidation from the on-disk pak.json (discovery);
 *   - the generation lease (internal/services/lease.c), held by the daemon
 *     for the whole generation;
 *   - process launch (internal/services/launch.c), the ownership
 *     reservation (internal/services/reservation.c), the stop sequence
 *     (internal/services/stop.c) over the group-absence check
 *     (internal/services/ownership.c), and the SVC-1 "do not reap the
 *     leader until the group is verified writer-free" rule;
 *   - restart backoff / circuit-breaker decisions
 *     (internal/services/backoff.c) and the persistent/session control
 *     state (internal/services/control_state.c).
 *
 * It deliberately does NOT do IPC framing or UI. The caller owns LIFE-1's
 * cooperative exchange; this layer owns the process-group stops, restart
 * latches, and daemon-wide start gate that make lifecycle decisions safe.
 *
 * REENTRANCY / THREADING: a jw_svc_supervisor is NOT thread-safe and every
 * public function must be called from the daemon's single main thread. The
 * one exception constraint flows the other way: jw_svc_supervisor_tick()
 * must run regularly (it polls child exit, lease re-acquisition once per
 * second while desired, backoff timers, and stop-sequence progress).
 *
 * BLOCKING: everything here is non-blocking EXCEPT the final stage of an
 * individual stop: after SIGKILL the group gets one synchronous bounded
 * verification of at most JW_SVC_STOP_KILL_WAIT_MS (2000 ms, the contract's
 * own worst case). jw_svc_supervisor_stop_service(),
 * jw_svc_supervisor_restart_service(), and jw_svc_supervisor_stop_all()
 * therefore return only after that bounded window; the daemon calls them
 * only where such a bounded wait is acceptable (shutdown, an explicit user
 * Stop/Restart, or a lifecycle trigger). Tick-driven stops
 * (leader-exit-with-live-descendants) escalate asynchronously so the main
 * loop never stalls.
 */

/* CTL-1 "Effective states" (contracts.md#ctl-1--control-and-status-ipc). */
typedef enum {
    JW_SVC_STATE_UNAVAILABLE = 0, /* pak missing, manifest invalid, duplicate
                                     id, or (MLP1) installed on a Secondary
                                     app root */
    JW_SVC_STATE_DISABLED,        /* valid, not enabled, not running */
    JW_SVC_STATE_STOPPED,         /* enabled, not running this session */
    JW_SVC_STATE_STALE_GENERATION,/* an earlier Jawaka generation still holds
                                     the service lease */
    JW_SVC_STATE_STARTING,        /* launched, not yet confirmed alive */
    JW_SVC_STATE_RUNNING,         /* process group alive */
    JW_SVC_STATE_STOPPING,        /* inside the stop sequence */
    JW_SVC_STATE_BACKOFF,         /* failed, waiting to retry */
    JW_SVC_STATE_FAILED,          /* circuit breaker open */
} jw_svc_effective_state;

/* Why a stop was initiated. Drives both the "on-failure never applies
 * after ..." restart-policy exclusion and the unverified-stop dispatch
 * table. */
typedef enum {
    JW_SVC_STOP_NONE = 0,         /* no stop requested: a leader exit here is
                                     a genuine crash, eligible for the
                                     on-failure restart policy */
    JW_SVC_STOP_INTENTIONAL,      /* CTL-1 Stop/Disable/Restart, shutdown,
                                     package operation: never a "failure" for
                                     backoff purposes */
    JW_SVC_STOP_LEADER_EXITED,    /* leader died but live descendants remain:
                                     the failure branch (backoff applies) */
    /* The declarative lifecycle-policy stops. CTL-1's `status` reports
     * these separately from an ordinary intentional stop -- contracts.md
     * requires "whether a lifecycle policy stop is in force (reason: game,
     * storage, suspend, package)" -- so they must stay distinguishable rather
     * than collapsing into JW_SVC_STOP_INTENTIONAL. All are equally
     * excluded from the on-failure restart policy. */
    JW_SVC_STOP_LIFECYCLE_GAME,     /* lifecycle.game == "stop" */
    JW_SVC_STOP_LIFECYCLE_SUSPEND,  /* lifecycle.stop_on_suspend */
    JW_SVC_STOP_LIFECYCLE_STORAGE,  /* lifecycle.stop_on_storage_change */
    JW_SVC_STOP_LIFECYCLE_PACKAGE,  /* PKG-1 package replacement barrier */
} jw_svc_stop_reason_kind;

/* CTL-1 `lifecycle_policy_stop.reason` slug for a stop kind, or NULL when the
 * kind is not a lifecycle-policy stop. */
const char *jw_svc_stop_reason_lifecycle_slug(jw_svc_stop_reason_kind kind);

/* What tick must do once an asynchronously stopped group is finally proven
 * absent. CTL-1 Stop/Disable/Restart no longer block the daemon loop for the
 * full stop sequence (see jw_svc_supervisor_stop() below), so the tail of
 * those operations runs here instead. */
typedef enum {
    JW_SVC_POST_STOP_NONE = 0,  /* nothing further: the entry goes idle */
    JW_SVC_POST_STOP_RESTART,   /* CTL-1 Restart: start exactly one new
                                   generation once absence is verified */
} jw_svc_post_stop_action;

/* Stable identifier for one supervised service (the service.id). */
#define JW_SVC_SUPERVISOR_ID_BUF JW_SVC_ID_BUF

typedef struct {
    char service_id[JW_SVC_SUPERVISOR_ID_BUF];

    /* Discovery outcome for the current scan. */
    bool pak_present;           /* a pak with this service id was found */
    bool manifest_valid;        /* pak.json passed SVC-1 validation */
    bool on_secondary_root;     /* discovered on a non-Primary app root */
    char reject_reason[JW_SVC_REASON_BUF]; /* manifest slug, when invalid */
    char pak_root[4096];        /* absolute pak directory, when present */
    char installed_package_version[32];

    /* Runtime supervision state. */
    jw_svc_effective_state state;
    bool desired_enabled;       /* persistent "Start with Leaf" */
    bool session_run;           /* session Run (cleared at daemon start) */
    bool autostart_pending;     /* startup-only consumption of persistent
                                   enablement; explicit Stop must not be
                                   undone by the next tick */
    pid_t pgid;                 /* >0 while a generation is owned */
    int lease_fd;               /* >=0 while the daemon holds the lease */
    long long launch_instant_us;/* ownership-reservation identity */
    long long lease_retry_next_ms; /* stale-generation: next acquire try */
    long long backoff_retry_at_ms; /* backoff: earliest next restart */
    long long stopping_since_ms;   /* stop sequence started (KILL pending) */
    bool stop_kill_sent;        /* SIGKILL already delivered this stop */
    jw_svc_stop_reason_kind pending_stop_reason;
    bool reap_pending;          /* leader exited, group not yet verified */
    /* An asynchronous stop is in flight: SIGTERM has been delivered but the
     * leader may still be alive. Distinct from reap_pending (which means the
     * leader has already exited) so tick keeps observing the real child exit
     * and still records last_exit. Tick owns the escalation and the reap. */
    bool stop_requested;
    jw_svc_post_stop_action post_stop;
    /* One-shot latch so the "survived TERM+KILL+2s" transition is recorded
     * once per stop rather than on every subsequent tick. */
    bool stop_unverified_logged;

    /* Immutable launch-time policy snapshot. A package rescan may replace
     * `manifest` while this generation is still alive; stop/restart/lifecycle
     * decisions for the owned group must continue using the policy that was
     * atomically reserved when that group launched. */
    int active_stop_grace_ms;
    bool active_restart_on_failure;
    jw_svc_lifecycle_game active_lifecycle_game;
    bool active_stop_on_storage_change;
    bool active_stop_on_suspend;
    /* A policy-triggered stop of a group that was running before suspend or
     * storage change is restarted after the transition. This remains set
     * across an unverified stop so tick can never overlap the old group. */
    bool lifecycle_restart_pending;
    /* LIFE-1 mode-stop/fallback restart latch. Set only when Jawaka stopped
     * an effective service for the current game. It is session-scoped: the
     * active-game record is durable in runtime tmpfs, this was-running fact is
     * deliberately lost across a daemon restart. */
    bool game_restart_pending;

    /* PKG-1 snapshot. A package operation blocks every service while its
     * caller replaces bytes. Persistent desired state is restored after the
     * post-replacement scan; a session-only Run is intentionally not. */
    bool package_blocked;
    bool package_snapshot_valid;
    bool package_restore_desired;
    bool package_was_effective;

    jw_svc_backoff_state backoff;
    jw_svc_control_state control;   /* persisted row snapshot (owned) */
    bool control_loaded;            /* a row existed in the store */
    jw_service_manifest manifest;   /* owned; destroy on removal/rescan */
    bool manifest_loaded;
} jw_svc_supervised;

typedef struct jw_svc_supervisor jw_svc_supervisor;

typedef enum {
    JW_SVC_SUBSCRIBER_ACCEPTED = 0,
    JW_SVC_SUBSCRIBER_MISSING_CREDENTIAL,
    JW_SVC_SUBSCRIBER_UNKNOWN_SERVICE,
    JW_SVC_SUBSCRIBER_STALE_GENERATION,
    JW_SVC_SUBSCRIBER_WRONG_GROUP,
    JW_SVC_SUBSCRIBER_FOREGROUND,
} jw_svc_subscriber_auth_result;

typedef struct {
    pid_t peer_pid;
    pid_t pgid;
    long long launch_instant_us;
} jw_svc_subscriber_binding;

/* Opens the supervisor: resolves the control-state SQLite store at
 * <state_dir>/services-control.db, clears every row's session Run flag
 * (SVC-1: session control never survives a daemon restart), and prepares
 * an empty service set. Call jw_svc_supervisor_scan() next to populate it.
 *
 * runtime_dir  = $UMRK_RUNTIME_PATH (lease + reservation live in tmpfs)
 * logs_dir     = $LOGS_PATH
 * state_dir    = durable directory for the control-state db
 * apps_scan_roots = NULL-terminated array of Apps/ roots on the verified
 *                   PRIMARY storage source. Every entry here is Primary --
 *                   `Apps/<platform>` and `Apps/shared` are two directories
 *                   of one source, not two sources, so both are startable.
 * secondary_apps_scan_roots = NULL-terminated array of Apps/ roots on
 *                   Secondary storage sources, or NULL when none are known.
 *                   SVC-1: a service discovered here is reported and NOT
 *                   startable. Primary/Secondary is a property of the storage
 *                   source (PATH-2 `SDCARD_PATHS` index 0 is Primary), never
 *                   of which Apps/ subdirectory a pak happens to sit in.
 * userdata_root   = $USERDATA_PATH (state.root confinement checks)
 *
 * Returns NULL on failure, copying a slug into `reason` when non-NULL:
 *   "invalid-arguments" / "control-store-failed"
 * A supervisor that cannot open its store fails closed (no service is
 * supervised without durable intent) rather than supervising with
 * forgettable state. */
jw_svc_supervisor *jw_svc_supervisor_open(
    const char *runtime_dir, const char *logs_dir, const char *state_dir,
    const char *const *apps_scan_roots,
    const char *const *secondary_apps_scan_roots,
    const char *userdata_root, char *reason, size_t reason_size);

/* Runs A5's one-time legacy SSH enablement migration against this
 * supervisor's durable control store. Call before the first scan so a migrated
 * enabled intent participates in normal autostart reconciliation. */
bool jw_svc_supervisor_migrate_legacy_ssh_intent(
    jw_svc_supervisor *sup, const char *config_path,
    jw_svc_legacy_ssh_migration_report *out,
    char *reason, size_t reason_size);

void jw_svc_supervisor_close(jw_svc_supervisor *sup);

/* Rescans every configured Apps/ root for service-bearing paks: validates
 * each pak.json against SVC-1 (internal/services/manifest.c), flags
 * cross-manifest duplicate service.ids (internal/services/dup_ids.c), and
 * reconciles the result with the running set. A running service whose pak
 * disappears or becomes invalid is left running (its processes still exist
 * and still owe the contract's stop guarantees); its state flips to
 * UNAVAILABLE for new actions but the group is still owned. Call at
 * startup and whenever the caller notices an app install/remove. */
int jw_svc_supervisor_scan(jw_svc_supervisor *sup);

/* Main-loop tick. Call every iteration; it keeps its own timers (the settle
 * window, backoff, the once-per-second lease retry), so an extra call is
 * harmless. Returns the number of state changes applied (informational). */
int jw_svc_supervisor_tick(jw_svc_supervisor *sup);

/* The next monotonic ms at which jw_svc_supervisor_tick() has work: now_ms
 * while a stop sequence or a pending start needs every tick, the settle,
 * backoff or lease-retry time otherwise, or -1 when every service is simply
 * running or idle (only a child exit can change that). */
long long jw_svc_supervisor_next_deadline_ms(const jw_svc_supervisor *sup,
                                             long long now_ms);

/* Opt in to exit notification: after this, the tick checks service leaders
 * for exit only once jw_svc_supervisor_note_child_exit() has been called
 * (from a SIGCHLD), instead of on every tick. Without it the tick checks
 * every time, as before. */
void jw_svc_supervisor_set_exit_notify(jw_svc_supervisor *sup, bool enabled);
void jw_svc_supervisor_note_child_exit(jw_svc_supervisor *sup);

/* Looks up a supervised entry by service id, or NULL. The pointer is
 * invalidated by the next scan; do not retain it across one. */
const jw_svc_supervised *jw_svc_supervisor_find(const jw_svc_supervisor *sup,
                                                const char *service_id);

/* Number of entries (for CTL-1 list). */
int jw_svc_supervisor_count(const jw_svc_supervisor *sup);

/* Should this entry appear in the canonical CTL-1 `list`? True for every
 * discovered service with a wire-safe id (including malformed/unavailable
 * manifests), a retained desired-state record, and any live or stale
 * generation; false for a spent package-missing record and for any entry whose
 * id would not survive a client's reverse-DNS validation. This is the single
 * predicate behind both the IPC response and Settings -> Services'
 * "absent on a clean system" rule -- they must not drift apart. */
bool jw_svc_supervisor_entry_is_listable(const jw_svc_supervised *e);

/* The entry at `index` in scan order, or NULL. */
const jw_svc_supervised *jw_svc_supervisor_at(const jw_svc_supervisor *sup,
                                              int index);

/* LIFE-1 peer authentication. The declared service id is only a lookup key;
 * the kernel-derived pid must be a live member of that service's exact
 * currently reserved generation. The returned binding is revalidated before
 * lifecycle replies are trusted. */
jw_svc_subscriber_auth_result jw_svc_supervisor_authenticate_subscriber(
    const jw_svc_supervisor *sup, const char *service_id, pid_t peer_pid,
    jw_svc_subscriber_binding *out_binding);
bool jw_svc_supervisor_revalidate_subscriber(
    const jw_svc_supervisor *sup, const char *service_id,
    const jw_svc_subscriber_binding *binding);

/* CTL-1 mutating operations. Each returns true on success; on failure
 * returns false with a stable slug in `reason` (may be NULL):
 *   "unknown-service" / "unavailable" / "stale-generation" /
 *   "already-running" / "not-running" / "lease-failed" /
 *   "launch-failed" / "stop-failed" / "store-failed"
 *
 * enable/disable write the persistent Start with Leaf flag. disable also
 * stops a running service (intentional stop). enable does NOT by itself
 * start the service this session (that is run()); it only records intent.
 * run/stop/restart are session control and never touch persistent intent.
 * run/restart clear the circuit breaker (SVC-1 restart policy).
 *
 * NON-BLOCKING: stop/disable/restart INITIATE the stop sequence (SIGTERM to
 * the reserved group, desired intent persisted) and return immediately with
 * the entry in JW_SVC_STATE_STOPPING. Tick owns the rest -- grace window,
 * SIGKILL escalation, verified absence, reap, and (for restart) starting the
 * one replacement generation. These calls must never block the daemon's
 * single-threaded main loop for stop_grace_ms + 2000 ms: a caller that needs
 * a *completed* stop before proceeding uses stop_all()/game_launch_begin()/
 * suspend_begin()/storage_change_begin() instead, which are deliberately
 * synchronous because their callers cannot continue without the guarantee. */
bool jw_svc_supervisor_enable(jw_svc_supervisor *sup, const char *service_id,
                              char *reason, size_t reason_size);
bool jw_svc_supervisor_disable(jw_svc_supervisor *sup, const char *service_id,
                               char *reason, size_t reason_size);
bool jw_svc_supervisor_run(jw_svc_supervisor *sup, const char *service_id,
                           char *reason, size_t reason_size);
bool jw_svc_supervisor_stop(jw_svc_supervisor *sup, const char *service_id,
                            char *reason, size_t reason_size);
bool jw_svc_supervisor_restart(jw_svc_supervisor *sup, const char *service_id,
                               char *reason, size_t reason_size);

/* PKG-1 core-app package barrier. begin() snapshots every service, blocks all
 * new service generations, and synchronously proves every owned group absent.
 * Only a successful begin permits the caller to replace package bytes.
 *
 * A stale generation, an in-flight stop owned by another operation, or a
 * TERM+KILL survivor fails begin without authorizing mutation. The barrier
 * remains active after a stop failure so no start can race the survivor;
 * callers must retry end() after absence can be verified.
 *
 * end() rescans package manifests while starts remain blocked, restores the
 * snapshotted persistent desired state, releases the barrier, and starts each
 * available desired-enabled service. operation_id is an ASCII token of at
 * most JW_SVC_PACKAGE_OPERATION_ID_MAX bytes and must match at end(). */
#define JW_SVC_PACKAGE_OPERATION_ID_MAX 63
bool jw_svc_supervisor_package_begin(jw_svc_supervisor *sup,
                                     const char *operation_id,
                                     char *out_stuck_id,
                                     size_t stuck_id_size,
                                     char *reason, size_t reason_size);
bool jw_svc_supervisor_package_end(jw_svc_supervisor *sup,
                                   const char *operation_id,
                                   char *reason, size_t reason_size);
bool jw_svc_supervisor_package_active(const jw_svc_supervisor *sup);

/* TXN-1 Pak Rat mutation bracket. Unlike PKG-1 this is keyed to one canonical
 * Apps-relative target plus its stable package id and quiesces only the
 * service owned by that package. The caller owns the process-lifetime flock;
 * Jawaka owns the start/launch gate and the SVC-1 absence proof. */
bool jw_svc_supervisor_mutation_begin(
    jw_svc_supervisor *sup, const char *operation_id,
    const char *target_path, const char *package_id,
    char *out_stuck_id, size_t stuck_id_size,
    char *reason, size_t reason_size);
bool jw_svc_supervisor_mutation_end(jw_svc_supervisor *sup,
                                    const char *operation_id,
                                    bool installed_has_service,
                                    char *reason, size_t reason_size);
bool jw_svc_supervisor_mutation_active(const jw_svc_supervisor *sup);
bool jw_svc_supervisor_mutation_target_blocked(
    const jw_svc_supervisor *sup, const char *apps_relative_path);
bool jw_svc_supervisor_mutation_info(
    const jw_svc_supervisor *sup,
    char *operation_id, size_t operation_id_size,
    char *target_path, size_t target_path_size,
    char *package_id, size_t package_id_size);

/* Daemon shutdown: stops every running service with the intentional-stop
 * reason and verifies each group absent, blocking per service by at most
 * its stop_grace_ms + JW_SVC_STOP_KILL_WAIT_MS. Per SVC-1's "when a stop
 * cannot be verified" table, shutdown CONTINUES past an unverified stop;
 * the stuck service's id is logged and the function returns the count of
 * services whose absence could NOT be verified (0 = clean shutdown). */
int jw_svc_supervisor_stop_all(jw_svc_supervisor *sup);

/* After jw_svc_supervisor_stop_all(): one line per service whose absence was
 * not verified, "service=<id> pgid=<n> lease=<path>\n" (pgid 0 for a stale
 * generation this daemon cannot signal). The generation lease stays locked
 * while any group member lives, so an acquirable lease proves absence to a
 * process outside Jawaka without signalling anything. Returns the number of
 * lines written; a line that does not fit is omitted, never truncated. */
int jw_svc_supervisor_describe_unverified(const jw_svc_supervisor *sup,
                                          char *out, size_t out_size);

/* Compatibility bulk hook at the SVC-1/LIFE-1 boundary. Stops every live
 * mode-stop generation and marks every stale non-ignore generation stuck.
 * Live notify exchanges and their fallbacks are selected by the daemon.
 * Returns the number stopped; the first unverified/stale service is reported
 * through `out_stuck_id` and must never be treated as safe to launch past. */
int jw_svc_supervisor_game_launch_begin(jw_svc_supervisor *sup,
                                        char *out_stuck_id,
                                        size_t stuck_id_size);

/* LIFE-1's daemon-wide start gate. Set active immediately after the durable
 * active-game record commit and before any service decision or game writer.
 * While set, no new generation whose manifest policy is stop/notify can
 * start (Run, Restart, autostart, backoff, or stale-lease recovery).
 *
 * game_stop_service synchronously stops one selected live generation and
 * remembers that it was running so game_finish can restart it after the
 * writer-exit barrier. A proven-absent service succeeds without a restart
 * latch; stale/non-owned generations and overlapping transitions fail.
 * game_finish only releases the gate; tick performs remembered starts. */
void jw_svc_supervisor_game_set_active(jw_svc_supervisor *sup, bool active);
bool jw_svc_supervisor_game_active(const jw_svc_supervisor *sup);
/* True while a non-ignore service is either still running for the current
 * game or waiting to restart after Jawaka stopped it for that game. */
bool jw_svc_supervisor_game_has_participant(const jw_svc_supervisor *sup);
bool jw_svc_supervisor_game_stop_service(jw_svc_supervisor *sup,
                                         const char *service_id,
                                         const jw_svc_subscriber_binding *coordinator,
                                         jw_svc_stop_result *stop_result,
                                         char *reason, size_t reason_size);
void jw_svc_supervisor_game_finish(jw_svc_supervisor *sup);

/* Declarative lifecycle stops. Both stop every currently owned generation
 * whose immutable launch-time policy enables the trigger and remember it for
 * one restart after the transition. A locked stale generation is report-only:
 * it is never signalled and is returned through out_stuck_id. Suspend callers
 * continue with a warning; safe-unmount callers fail their operation when the
 * id is non-empty. For an external hotplug that already happened, callers
 * continue with a warning. Tick performs the remembered restart only after
 * verified absence (and, for suspend, naturally only after the blocking sleep
 * returns because the daemon loop is paused while suspended).
 *
 * Return value is the number of groups verified stopped. out_stuck_id receives
 * the first unverified/stale service id, or an empty string. */
int jw_svc_supervisor_suspend_begin(jw_svc_supervisor *sup,
                                    char *out_stuck_id,
                                    size_t stuck_id_size);
int jw_svc_supervisor_storage_change_begin(jw_svc_supervisor *sup,
                                           char *out_stuck_id,
                                           size_t stuck_id_size);

/* Release storage-policy restarts only after the platform reports the source
 * mounted and the daemon has rescanned it, or when a requested unmount failed
 * and the original mounted topology never changed. */
void jw_svc_supervisor_storage_change_resume(jw_svc_supervisor *sup);

/* Side-effect-free pre-check for the safe-unmount caller. Reports the first
 * storage-sensitive service whose absence cannot presently be verified -- a
 * locked stale generation this daemon may not signal, a survivor that
 * outlived a previous TERM+KILL+2s, or an owned group already inside a stop
 * sequence whose original intent/restart action must not be overwritten.
 *
 * SVC-1 makes safe unmount the one caller that must "fail the unmount" when
 * absence cannot be verified. Refusing after having already stopped every
 * OTHER storage-sensitive service would bounce healthy services for an
 * operation that never happened, so the caller checks first and only calls
 * storage_change_begin() when this returns false.
 *
 * Returns true when a blocker exists, copying its id into out_stuck_id. */
bool jw_svc_supervisor_storage_change_blocked(const jw_svc_supervisor *sup,
                                              char *out_stuck_id,
                                              size_t stuck_id_size);

/* Daemon-side safe-unmount bookkeeping: suppress the follow-up storage tick
 * only when this request actually verified at least one policy stop AND the
 * platform unmount succeeded. A refused/failed request must never consume a
 * later real removal event. Kept here as a pure helper so the outcome matrix
 * is covered without coupling tests to jawakad's large main translation unit. */
bool jw_svc_storage_should_suppress_followup_tick(int verified_stopped,
                                                  bool unmount_succeeded);

/* The effective-state slug used on CTL-1 ("unavailable", "disabled", ...). */
const char *jw_svc_effective_state_name(jw_svc_effective_state state);

/* CTL-1 helpers shared with jawakad's IPC layer. Service ids used as log-path
 * components are independently checked even when they came from a retained
 * invalid-manifest record, and hostile tail counts are bounded before any
 * allocation. */
#define JW_SVC_LOG_TAIL_DEFAULT 100
#define JW_SVC_LOG_TAIL_MAX 1000
bool jw_svc_supervisor_service_id_is_safe(const char *service_id);
int jw_svc_supervisor_bound_log_tail(int requested);
bool jw_svc_supervisor_join_scan_root(char *out, size_t out_size,
                                      const char *apps_path,
                                      const char *root_name);
bool jw_svc_supervisor_ctl_op_requires_id(const char *operation);

#endif /* JW_SERVICES_SUPERVISOR_H */
