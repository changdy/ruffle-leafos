/* flock(), kill(), setpgid(), PATH_MAX, clock_gettime() are hidden by glibc
 * under a bare -std=c11 without a broader feature-test macro. Matches the
 * convention used by every other module in this directory. Must precede
 * every #include, including the paired header. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/supervisor.h"

#include "cJSON.h"
#include "internal/core/log.h"
#include "internal/services/dup_ids.h"
#include "internal/services/launch.h"
#include "internal/services/lease.h"
#include "internal/services/ownership.h"
#include "internal/services/reservation.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* How long a freshly launched leader must stay alive before "starting"
 * settles into "running". A command that exits immediately is either an
 * ordinary failure or (with live descendants) a foreground-contract
 * violation; both are caught by the leader-exit path, this window just
 * keeps CTL-1 from reporting a one-tick "running" flash for a process
 * that is already on its way out. */
#define JW_SVC_STARTING_SETTLE_MS 1000LL

/* SVC-1: "while desired enablement remains true, retry the non-blocking
 * acquisition once per second." */
#define JW_SVC_LEASE_RETRY_MS 1000LL

/* A just-reaped executable on the MLP1's removable filesystem can briefly
 * reject the immediate replacement exec. PKG-1 release is a synchronous
 * maintenance boundary, so make a small bounded retry rather than leaving a
 * desired service stopped. This is not a keep-alive loop: five attempts cap
 * the added latency at 200 ms and every failed child is already reaped by
 * launch.c before the next attempt. */
#define JW_SVC_PACKAGE_START_ATTEMPTS 5
#define JW_SVC_PACKAGE_START_RETRY_US 50000u

struct jw_svc_supervisor {
    char *runtime_dir;
    char *logs_dir;
    char *state_dir;
    char *userdata_root;
    /* Apps/ roots on the verified Primary storage source. Every one of these
     * is Primary: `Apps/<platform>` and `Apps/shared` are two directories of
     * one source. Secondary is a property of the SOURCE (PATH-2), which is
     * why the two lists are separate rather than "index 0 wins". */
    char **apps_scan_roots;      /* NULL-terminated, owned */
    int apps_scan_root_count;
    char **secondary_scan_roots; /* NULL-terminated, owned; may be empty */
    int secondary_scan_root_count;
    jw_svc_control_store *store;
    jw_svc_supervised *entries;  /* dynamic array, scan order */
    int count;
    int cap;
    /* A storage-policy stop cannot resume merely because the synchronous
     * stop sequence completed. Safe-unmount deliberately leaves the source
     * absent; a later mounted-and-rescanned edge releases this gate. */
    bool storage_restart_blocked;
    /* PKG-1 is a daemon-wide gate: Release A chooses the conservative scope
     * of every service for core-app replacement, so a newly installed service
     * discovered by the final scan is blocked too. */
    bool package_quiesce_active;
    char package_operation_id[JW_SVC_PACKAGE_OPERATION_ID_MAX + 1];
    bool mutation_active;
    char mutation_operation_id[JW_SVC_PACKAGE_OPERATION_ID_MAX + 1];
    char mutation_target[512];
    char mutation_package_id[JW_SVC_SUPERVISOR_ID_BUF];
    /* LIFE-1 authoritative active-launch gate. This is initialized from the
     * runtime active-game record before the first scan/autostart tick. */
    bool game_active;
    /* Set once the daemon reports child exits through
     * jw_svc_supervisor_note_child_exit(). Leader exits are then checked only
     * after such a report, not with a waitid per running service on every
     * tick. */
    bool exit_notify;
    bool child_exit_pending;
};

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static long long jw__mono_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return -1;
    }
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000LL);
}

/* Persistent CTL-1 timestamps must remain meaningful across daemon and device
 * restarts. CLOCK_MONOTONIC is reserved for in-process deadlines only. */
static long long jw__wall_us(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return -1;
    }
    return (long long)ts.tv_sec * 1000000LL + (long long)(ts.tv_nsec / 1000LL);
}

static long long jw__wall_ms(void) {
    long long now_us = jw__wall_us();
    return now_us < 0 ? now_us : now_us / 1000LL;
}

static void jw__set_reason(char *reason, size_t reason_size, const char *slug) {
    if (reason && reason_size > 0) {
        snprintf(reason, reason_size, "%s", slug ? slug : "unknown");
    }
}

static int jw__format(char *out, size_t out_size, const char *fmt, ...) {
    if (!out || out_size == 0) {
        return -1;
    }
    va_list args;
    va_start(args, fmt);
    int needed = vsnprintf(out, out_size, fmt, args);
    va_end(args);
    return needed >= 0 && (size_t)needed < out_size ? 0 : -1;
}

static bool jw__is_dir(const char *path) {
    struct stat st;
    return path && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *jw__strdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t n = strlen(s) + 1u;
    char *copy = malloc(n);
    if (copy) {
        memcpy(copy, s, n);
    }
    return copy;
}

bool jw_svc_supervisor_service_id_is_safe(const char *service_id) {
    return service_id && service_id[0] &&
           strlen(service_id) <= JW_SVC_ID_MAX &&
           strchr(service_id, '/') == NULL &&
           strcmp(service_id, ".") != 0 && strcmp(service_id, "..") != 0;
}

int jw_svc_supervisor_bound_log_tail(int requested) {
    if (requested <= 0) {
        return JW_SVC_LOG_TAIL_DEFAULT;
    }
    return requested > JW_SVC_LOG_TAIL_MAX ? JW_SVC_LOG_TAIL_MAX : requested;
}

bool jw_svc_supervisor_join_scan_root(char *out, size_t out_size,
                                      const char *apps_path,
                                      const char *root_name) {
    if (!out || out_size == 0 || !apps_path || !apps_path[0] ||
        !root_name || !root_name[0]) {
        return false;
    }
    int written = snprintf(out, out_size, "%s/%s", apps_path, root_name);
    return written >= 0 && (size_t)written < out_size;
}

bool jw_svc_supervisor_ctl_op_requires_id(const char *operation) {
    return operation &&
           (strcmp(operation, "status") == 0 ||
            strcmp(operation, "logs") == 0 ||
            strcmp(operation, "export-logs") == 0 ||
            strcmp(operation, "enable") == 0 ||
            strcmp(operation, "disable") == 0 ||
            strcmp(operation, "run") == 0 ||
            strcmp(operation, "stop") == 0 ||
            strcmp(operation, "restart") == 0);
}

/* Bounded string copy that never truncates without a NUL and never triggers
 * gcc's -Wformat-truncation (which rejects snprintf(dst, cap, "%s", src)
 * whenever src's provenance is an unbounded fixed array). */
static void jw__strcpy_bounded(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n > dst_size - 1u) {
        n = dst_size - 1u;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

const char *jw_svc_stop_reason_lifecycle_slug(jw_svc_stop_reason_kind kind) {
    switch (kind) {
    case JW_SVC_STOP_LIFECYCLE_GAME:    return "game";
    case JW_SVC_STOP_LIFECYCLE_SUSPEND: return "suspend";
    case JW_SVC_STOP_LIFECYCLE_STORAGE: return "storage";
    case JW_SVC_STOP_LIFECYCLE_PACKAGE: return "package";
    case JW_SVC_STOP_NONE:
    case JW_SVC_STOP_INTENTIONAL:
    case JW_SVC_STOP_LEADER_EXITED:
        break;
    }
    return NULL;
}

/* True when the stop was deliberate rather than a crash. SVC-1: "on-failure
 * never applies after an intentional stop, a lifecycle-policy stop, a package
 * operation, or shutdown" -- every lifecycle kind counts. */
static bool jw__stop_is_deliberate(jw_svc_stop_reason_kind kind) {
    return kind == JW_SVC_STOP_INTENTIONAL ||
           jw_svc_stop_reason_lifecycle_slug(kind) != NULL;
}

const char *jw_svc_effective_state_name(jw_svc_effective_state state) {
    switch (state) {
    case JW_SVC_STATE_UNAVAILABLE:      return "unavailable";
    case JW_SVC_STATE_DISABLED:         return "disabled";
    case JW_SVC_STATE_STOPPED:          return "stopped";
    case JW_SVC_STATE_STALE_GENERATION: return "stale-generation";
    case JW_SVC_STATE_STARTING:         return "starting";
    case JW_SVC_STATE_RUNNING:          return "running";
    case JW_SVC_STATE_STOPPING:         return "stopping";
    case JW_SVC_STATE_BACKOFF:          return "backoff";
    case JW_SVC_STATE_FAILED:           return "failed";
    }
    return "unavailable";
}

/* ------------------------------------------------------------------ */
/* entry lifecycle                                                     */
/* ------------------------------------------------------------------ */

static void jw__entry_release_runtime(jw_svc_supervised *e) {
    if (!e) {
        return;
    }
    /* A lease acquire may legitimately return descriptor 0 when the daemon
     * was started with stdin closed. Fresh entries are explicitly initialized
     * to -1, so every non-negative value is owned and must be closed. */
    if (e->lease_fd >= 0) {
        close(e->lease_fd);
    }
    e->lease_fd = -1;
    e->pgid = -1;
    e->launch_instant_us = 0;
}

static void jw__entry_initialize(jw_svc_supervised *e) {
    if (!e) {
        return;
    }
    /* `e` is newly obtained from realloc() and contains indeterminate bytes.
     * Never inspect manifest_loaded or lease_fd before initialization. */
    memset(e, 0, sizeof(*e));
    e->pgid = -1;
    e->lease_fd = -1;
}

static jw_svc_supervised *jw__find_mut(jw_svc_supervisor *sup,
                                       const char *service_id) {
    if (!sup || !service_id) {
        return NULL;
    }
    for (int i = 0; i < sup->count; i++) {
        if (strcmp(sup->entries[i].service_id, service_id) == 0) {
            return &sup->entries[i];
        }
    }
    return NULL;
}

const jw_svc_supervised *jw_svc_supervisor_find(const jw_svc_supervisor *sup,
                                                 const char *service_id) {
    if (!sup || !service_id) {
        return NULL;
    }
    for (int i = 0; i < sup->count; i++) {
        if (strcmp(sup->entries[i].service_id, service_id) == 0) {
            return &sup->entries[i];
        }
    }
    return NULL;
}

jw_svc_subscriber_auth_result jw_svc_supervisor_authenticate_subscriber(
    const jw_svc_supervisor *sup, const char *service_id, pid_t peer_pid,
    jw_svc_subscriber_binding *out_binding) {
    if (out_binding) {
        memset(out_binding, 0, sizeof(*out_binding));
    }
    if (peer_pid <= 0) {
        return JW_SVC_SUBSCRIBER_MISSING_CREDENTIAL;
    }
    const jw_svc_supervised *requested =
        jw_svc_supervisor_find(sup, service_id);
    if (!requested) {
        return JW_SVC_SUBSCRIBER_UNKNOWN_SERVICE;
    }
    if (requested->state == JW_SVC_STATE_STALE_GENERATION) {
        return JW_SVC_SUBSCRIBER_STALE_GENERATION;
    }
    if (requested->pgid > 0 && requested->lease_fd >= 0 &&
        requested->launch_instant_us > 0 &&
        jw_svc_process_is_live_group_member(peer_pid, requested->pgid)) {
        if (out_binding) {
            out_binding->peer_pid = peer_pid;
            out_binding->pgid = requested->pgid;
            out_binding->launch_instant_us = requested->launch_instant_us;
        }
        return JW_SVC_SUBSCRIBER_ACCEPTED;
    }

    int count = jw_svc_supervisor_count(sup);
    for (int i = 0; i < count; i++) {
        const jw_svc_supervised *other = jw_svc_supervisor_at(sup, i);
        if (!other || other == requested || other->pgid <= 0 ||
            other->lease_fd < 0 || other->launch_instant_us <= 0) {
            continue;
        }
        if (jw_svc_process_is_live_group_member(peer_pid, other->pgid)) {
            return JW_SVC_SUBSCRIBER_WRONG_GROUP;
        }
    }
    return JW_SVC_SUBSCRIBER_FOREGROUND;
}

bool jw_svc_supervisor_revalidate_subscriber(
    const jw_svc_supervisor *sup, const char *service_id,
    const jw_svc_subscriber_binding *binding) {
    if (!binding || binding->peer_pid <= 0 || binding->pgid <= 0 ||
        binding->launch_instant_us <= 0) {
        return false;
    }
    const jw_svc_supervised *entry =
        jw_svc_supervisor_find(sup, service_id);
    return entry && entry->pgid == binding->pgid && entry->lease_fd >= 0 &&
           entry->launch_instant_us == binding->launch_instant_us &&
           jw_svc_process_is_live_group_member(binding->peer_pid,
                                               binding->pgid);
}

int jw_svc_supervisor_count(const jw_svc_supervisor *sup) {
    return sup ? sup->count : 0;
}

const jw_svc_supervised *jw_svc_supervisor_at(const jw_svc_supervisor *sup,
                                              int index) {
    if (!sup || index < 0 || index >= sup->count) {
        return NULL;
    }
    return &sup->entries[index];
}

/* ------------------------------------------------------------------ */
/* open / close                                                        */
/* ------------------------------------------------------------------ */

void jw_svc_supervisor_close(jw_svc_supervisor *sup) {
    if (!sup) {
        return;
    }
    for (int i = 0; i < sup->count; i++) {
        if (sup->entries[i].manifest_loaded) {
            jw_service_manifest_destroy(&sup->entries[i].manifest);
        }
        if (sup->entries[i].lease_fd >= 0) {
            close(sup->entries[i].lease_fd);
        }
    }
    free(sup->entries);
    for (int i = 0; i < sup->apps_scan_root_count; i++) {
        free(sup->apps_scan_roots[i]);
    }
    free(sup->apps_scan_roots);
    for (int i = 0; i < sup->secondary_scan_root_count; i++) {
        free(sup->secondary_scan_roots[i]);
    }
    free(sup->secondary_scan_roots);
    free(sup->runtime_dir);
    free(sup->logs_dir);
    free(sup->state_dir);
    free(sup->userdata_root);
    if (sup->store) {
        jw_svc_control_store_close(sup->store);
    }
    free(sup);
}

/* Copies a NULL-terminated root list into an owned array. Returns false only
 * on allocation failure; a NULL or empty input is a legitimate empty list. */
static bool jw__copy_scan_roots(const char *const *src, char ***out,
                                int *out_count) {
    int n = 0;
    while (src && src[n]) {
        n++;
    }
    *out = calloc((size_t)n + 1u, sizeof(char *));
    if (!*out) {
        return false;
    }
    for (int i = 0; i < n; i++) {
        (*out)[i] = jw__strdup(src[i]);
        if (!(*out)[i]) {
            return false;
        }
        (*out_count)++;
    }
    return true;
}

jw_svc_supervisor *jw_svc_supervisor_open(
    const char *runtime_dir, const char *logs_dir, const char *state_dir,
    const char *const *apps_scan_roots,
    const char *const *secondary_apps_scan_roots,
    const char *userdata_root, char *reason, size_t reason_size) {
    if (!runtime_dir || !runtime_dir[0] || !state_dir || !state_dir[0] ||
        !apps_scan_roots) {
        jw__set_reason(reason, reason_size, "invalid-arguments");
        return NULL;
    }

    jw_svc_supervisor *sup = calloc(1, sizeof(*sup));
    if (!sup) {
        jw__set_reason(reason, reason_size, "out-of-memory");
        return NULL;
    }
    sup->runtime_dir = jw__strdup(runtime_dir);
    sup->logs_dir = jw__strdup(logs_dir ? logs_dir : "");
    sup->state_dir = jw__strdup(state_dir);
    sup->userdata_root = jw__strdup(userdata_root ? userdata_root : "");
    if (!sup->runtime_dir || !sup->logs_dir || !sup->state_dir ||
        !sup->userdata_root) {
        jw__set_reason(reason, reason_size, "out-of-memory");
        jw_svc_supervisor_close(sup);
        return NULL;
    }

    if (!jw__copy_scan_roots(apps_scan_roots, &sup->apps_scan_roots,
                             &sup->apps_scan_root_count) ||
        !jw__copy_scan_roots(secondary_apps_scan_roots,
                             &sup->secondary_scan_roots,
                             &sup->secondary_scan_root_count)) {
        jw__set_reason(reason, reason_size, "out-of-memory");
        jw_svc_supervisor_close(sup);
        return NULL;
    }

    char db_path[PATH_MAX];
    if (jw__format(db_path, sizeof(db_path), "%s/services-control.db",
                   state_dir) != 0) {
        jw__set_reason(reason, reason_size, "path-too-long");
        jw_svc_supervisor_close(sup);
        return NULL;
    }
    char store_reason[JW_SVC_REASON_BUF];
    if (!jw_svc_control_store_open(db_path, &sup->store,
                                   store_reason, sizeof(store_reason))) {
        jw__set_reason(reason, reason_size, "control-store-failed");
        jw_svc_supervisor_close(sup);
        return NULL;
    }
    /* SVC-1: session Run/Stop must not survive a daemon restart, even
     * though it shares a row with the persistent fields. */
    if (!jw_svc_control_store_clear_all_sessions(sup->store,
                                                 store_reason,
                                                 sizeof(store_reason))) {
        jw__set_reason(reason, reason_size, "control-store-failed");
        jw_svc_supervisor_close(sup);
        return NULL;
    }
    return sup;
}

bool jw_svc_supervisor_migrate_legacy_ssh_intent(
    jw_svc_supervisor *sup, const char *config_path,
    jw_svc_legacy_ssh_migration_report *out,
    char *reason, size_t reason_size) {
    if (!sup) {
        jw__set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    return jw_svc_migrate_legacy_ssh_intent(sup->store, config_path, out,
                                             reason, reason_size);
}

/* ------------------------------------------------------------------ */
/* persistence                                                         */
/* ------------------------------------------------------------------ */

static bool jw__persist(jw_svc_supervisor *sup, jw_svc_supervised *e,
                        const char *transition_reason) {
    e->control.last_transition_at_us = jw__wall_us();
    if (transition_reason) {
        snprintf(e->control.last_transition_reason,
                 sizeof(e->control.last_transition_reason), "%s",
                 transition_reason);
    }
    e->control.breaker_open = e->backoff.breaker_open;
    e->control.backoff_failure_count = e->backoff.count;
    for (int i = 0; i < e->backoff.count && i < JW_SVC_CONTROL_BACKOFF_TRACKED;
         i++) {
        e->control.backoff_failure_times_us[i] =
            e->backoff.failure_times_ms[i] * 1000LL;
    }
    char store_reason[JW_SVC_REASON_BUF];
    if (!jw_svc_control_store_put(sup->store, e->service_id, &e->control,
                                  store_reason, sizeof(store_reason))) {
        return false;
    }
    e->control_loaded = true;
    return true;
}

static void jw__load_control(jw_svc_supervisor *sup, jw_svc_supervised *e) {
    char store_reason[JW_SVC_REASON_BUF];
    if (!jw_svc_control_store_get(sup->store, e->service_id, &e->control,
                                  &e->control_loaded,
                                  store_reason, sizeof(store_reason))) {
        e->control_loaded = false;
        memset(&e->control, 0, sizeof(e->control));
    }
    e->desired_enabled = e->control.start_with_leaf;
    e->session_run = e->control.session_run; /* already cleared at open */
    /* A package-missing retained row has no current manifest scan to refill
     * this presentation field. Preserve the last installed identity from the
     * durable control record; a present package overwrites it during scan. */
    jw__strcpy_bounded(e->installed_package_version,
                       sizeof(e->installed_package_version),
                       e->control.installed_package_version);
    /* Persistent enablement is consumed once at daemon startup. It is not a
     * perpetual keep-alive bit: CTL-1 Stop and lifecycle stops must win for
     * the rest of this session. */
    e->autostart_pending = e->desired_enabled;
    e->backoff.breaker_open = e->control.breaker_open;
    e->backoff.count = e->control.backoff_failure_count;
    for (int i = 0; i < e->control.backoff_failure_count &&
                i < JW_SVC_BACKOFF_TRACKED; i++) {
        e->backoff.failure_times_ms[i] =
            e->control.backoff_failure_times_us[i] / 1000LL;
    }
}

static jw_svc_supervised *jw__ensure_entry(jw_svc_supervisor *sup,
                                           const char *service_id) {
    jw_svc_supervised *existing = jw__find_mut(sup, service_id);
    if (existing) {
        return existing;
    }
    if (sup->count == INT_MAX) {
        return NULL;
    }
    if (sup->count >= sup->cap) {
        if (sup->cap > INT_MAX / 2) {
            return NULL;
        }
        int new_cap = sup->cap > 0 ? sup->cap * 2 : 8;
        if (new_cap <= sup->cap ||
            (size_t)new_cap > SIZE_MAX / sizeof(*sup->entries)) {
            return NULL;
        }
        jw_svc_supervised *grown =
            realloc(sup->entries,
                    (size_t)new_cap * sizeof(*sup->entries));
        if (!grown) {
            return NULL;
        }
        sup->entries = grown;
        sup->cap = new_cap;
    }
    jw_svc_supervised *e = &sup->entries[sup->count++];
    jw__entry_initialize(e);
    jw__strcpy_bounded(e->service_id, sizeof(e->service_id), service_id);
    e->state = JW_SVC_STATE_UNAVAILABLE;
    jw__load_control(sup, e);
    return e;
}

/* ------------------------------------------------------------------ */
/* scan                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    char service_id[JW_SVC_SUPERVISOR_ID_BUF];
    char pak_root[PATH_MAX];
    bool on_secondary_root;
    bool valid;
    char reject_reason[JW_SVC_REASON_BUF];
    char package_version[32];
    jw_service_manifest manifest;
    bool manifest_loaded;
} jw__scan_candidate;

static bool jw__entry_available(const jw_svc_supervised *e) {
    return e && e->pak_present && e->manifest_valid &&
           !e->on_secondary_root;
}

static jw_svc_effective_state jw__entry_idle_state(
    const jw_svc_supervised *e) {
    if (!jw__entry_available(e)) {
        return JW_SVC_STATE_UNAVAILABLE;
    }
    return e->desired_enabled ? JW_SVC_STATE_STOPPED
                              : JW_SVC_STATE_DISABLED;
}

static void jw__load_stale_policy(jw_svc_supervisor *sup,
                                  jw_svc_supervised *e);

/* A generation lease is the authoritative cross-daemon survivor signal. A
 * crash can occur after the child and reservation exist but before the first
 * control-state write; if the pak is then absent, neither manifest discovery
 * nor retained control ids can name that writer. Enumerate existing lease
 * files so such a generation remains visible and non-overlappable. */
static void jw__discover_runtime_stale_entries(jw_svc_supervisor *sup) {
    char services_dir[PATH_MAX];
    if (jw__format(services_dir, sizeof(services_dir), "%s/services",
                   sup->runtime_dir) != 0) {
        return;
    }
    DIR *dir = opendir(services_dir);
    if (!dir) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (!jw_svc_supervisor_service_id_is_safe(de->d_name)) {
            continue;
        }
        jw_svc_supervised *known = jw__find_mut(sup, de->d_name);
        if (known && (known->pgid > 0 || known->lease_fd >= 0)) {
            continue;
        }
        char lease_path[PATH_MAX];
        if (jw__format(lease_path, sizeof(lease_path), "%s/%s/generation.lease",
                       services_dir, de->d_name) != 0) {
            continue;
        }
        struct stat st;
        if (lstat(lease_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }
        char lease_reason[JW_SVC_REASON_BUF];
        int lease_fd = jw_svc_lease_acquire(sup->runtime_dir, de->d_name,
                                            lease_reason,
                                            sizeof(lease_reason));
        if (lease_fd >= 0) {
            close(lease_fd);
            continue;
        }
        if (strcmp(lease_reason, "stale-generation") != 0) {
            continue;
        }
        jw_svc_supervised *e = jw__ensure_entry(sup, de->d_name);
        if (!e) {
            continue;
        }
        if (!e->pak_present) {
            e->manifest_valid = false;
            jw__strcpy_bounded(e->reject_reason, sizeof(e->reject_reason),
                               "package-missing");
        }
        jw__load_stale_policy(sup, e);
        e->state = JW_SVC_STATE_STALE_GENERATION;
        e->lease_retry_next_ms = jw__mono_ms() + JW_SVC_LEASE_RETRY_MS;
    }
    closedir(dir);
}

static void jw__candidate_free(jw__scan_candidate *c) {
    if (c && c->manifest_loaded) {
        jw_service_manifest_destroy(&c->manifest);
        c->manifest_loaded = false;
    }
}

/* Reads a pak's pak.json into a heap buffer. Returns NULL when the file is
 * absent, unreadable, or larger than 1 MiB (a pak.json has no business
 * being that large; refusing it keeps a hostile or corrupt manifest from
 * making the daemon allocate unbounded memory). */
static char *jw__read_pak_json(const char *pak_root) {
    char path[PATH_MAX];
    if (jw__format(path, sizeof(path), "%s/pak.json", pak_root) != 0) {
        return NULL;
    }
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    char *buf = NULL;
    if (fseek(fp, 0, SEEK_END) == 0) {
        long size = ftell(fp);
        if (size > 0 && size <= 1024L * 1024L &&
            fseek(fp, 0, SEEK_SET) == 0) {
            buf = malloc((size_t)size + 1u);
            if (buf) {
                size_t got = fread(buf, 1, (size_t)size, fp);
                buf[got] = '\0';
                if (got != (size_t)size) {
                    free(buf);
                    buf = NULL;
                }
            }
        }
    }
    fclose(fp);
    return buf;
}

/* Does this pak.json declare a service object at all? A pak without one is
 * an ordinary foreground app, not a service candidate, and must not be
 * flagged for service validation failures. */
static bool jw__pak_declares_service(const char *pak_json_text) {
    cJSON *root = cJSON_Parse(pak_json_text);
    if (!root) {
        return false;
    }
    cJSON *svc = cJSON_GetObjectItemCaseSensitive(root, "service");
    bool declares = svc != NULL;
    cJSON_Delete(root);
    return declares;
}

static void jw__scan_one_root(jw_svc_supervisor *sup, const char *root,
                              bool secondary, jw__scan_candidate **cands,
                              int *count, int *cap) {
    DIR *dir = opendir(root);
    if (!dir) {
        return;
    }
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') {
            continue;
        }
        char pak_root[PATH_MAX];
        if (jw__format(pak_root, sizeof(pak_root), "%s/%s", root,
                       de->d_name) != 0 || !jw__is_dir(pak_root)) {
            continue;
        }
        char *text = jw__read_pak_json(pak_root);
        if (!text) {
            continue;
        }
        bool declares = jw__pak_declares_service(text);
        if (!declares) {
            free(text);
            continue; /* ordinary foreground app pak */
        }
        if (*count >= *cap) {
            int new_cap = *cap > 0 ? *cap * 2 : 8;
            jw__scan_candidate *grown =
                realloc(*cands, (size_t)new_cap * sizeof(**cands));
            if (!grown) {
                free(text);
                continue;
            }
            *cands = grown;
            *cap = new_cap;
        }
        jw__scan_candidate *c = &(*cands)[*count];
        memset(c, 0, sizeof(*c));
        c->on_secondary_root = secondary;
        jw__strcpy_bounded(c->pak_root, sizeof(c->pak_root), pak_root);

        char reason[JW_SVC_REASON_BUF] = {0};
        const char *userdata = sup->userdata_root[0]
                                   ? sup->userdata_root : NULL;
        c->valid = jw_service_manifest_validate(text, pak_root, userdata,
                                                &c->manifest, reason,
                                                sizeof(reason));
        if (c->valid) {
            c->manifest_loaded = true;
            jw__strcpy_bounded(c->service_id, sizeof(c->service_id), c->manifest.id);
        } else {
            jw__strcpy_bounded(c->reject_reason, sizeof(c->reject_reason),
                          reason[0] ? reason : "invalid-manifest");
            /* An invalid service is still tracked (CTL-1 reports it as
             * "unavailable", it is not silently dropped), keyed by its
             * declared service.id when that is recoverable from the raw
             * JSON, else by the pak directory name trimmed of .pak as a
             * best-effort identity for logs. */
            const char *id_src = NULL;
            cJSON *raw = cJSON_Parse(text);
            if (raw) {
                cJSON *svc = cJSON_GetObjectItemCaseSensitive(raw, "service");
                cJSON *sid = svc ? cJSON_GetObjectItemCaseSensitive(svc, "id")
                                 : NULL;
                if (sid && cJSON_IsString(sid) && sid->valuestring &&
                    sid->valuestring[0]) {
                    id_src = sid->valuestring;
                }
            }
            if (id_src && jw_svc_supervisor_service_id_is_safe(id_src)) {
                jw__strcpy_bounded(c->service_id, sizeof(c->service_id), id_src);
            } else {
                jw__strcpy_bounded(c->service_id, sizeof(c->service_id), de->d_name);
                size_t len = strlen(c->service_id);
                if (len > 4 && strcmp(c->service_id + len - 4, ".pak") == 0) {
                    c->service_id[len - 4] = '\0';
                }
            }
            if (raw) {
                cJSON_Delete(raw);
            }
        }

        /* Best-effort package version for the installed-package-identity
         * control field. */
        cJSON *root_json = cJSON_Parse(text);
        if (root_json) {
            cJSON *v = cJSON_GetObjectItemCaseSensitive(root_json,
                                                        "pak_version");
            if (cJSON_IsString(v) && v->valuestring) {
                jw__strcpy_bounded(c->package_version, sizeof(c->package_version),
                          v->valuestring);
            }
            cJSON_Delete(root_json);
        }
        free(text);
        (*count)++;
    }
    closedir(dir);
    (void)sup;
}

/* Is there anything left worth remembering about this entry? An entry whose
 * package is gone, whose persistent and session intent are both false, and
 * which owns no live or stale generation is a spent record: keeping it would
 * pin the Settings -> Services screen visible forever after an uninstall and
 * grow the control store without bound, since nothing else ever deletes a
 * row. */
static bool jw__entry_is_exhausted(const jw_svc_supervised *e) {
    return !e->pak_present && !e->desired_enabled && !e->session_run &&
           e->pgid <= 0 && e->lease_fd < 0 && !e->reap_pending &&
           !e->stop_requested && !e->lifecycle_restart_pending &&
           !e->game_restart_pending &&
           !e->package_blocked && !e->package_snapshot_valid &&
           e->state != JW_SVC_STATE_STALE_GENERATION;
}

/* Drops spent records from both the control store and the in-memory set.
 * Callers are already told that entry pointers do not survive a scan. */
static void jw__shed_exhausted_entries(jw_svc_supervisor *sup) {
    int kept = 0;
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        if (!jw__entry_is_exhausted(e)) {
            if (kept != i) {
                sup->entries[kept] = *e;
            }
            kept++;
            continue;
        }
        char reason[JW_SVC_REASON_BUF];
        /* A failed delete is not fatal: the row simply survives to the next
         * scan. Never drop the in-memory entry in that case, or the record
         * would be invisible while still occupying the store. */
        if (!jw_svc_control_store_delete(sup->store, e->service_id, reason,
                                         sizeof(reason))) {
            if (kept != i) {
                sup->entries[kept] = *e;
            }
            kept++;
            continue;
        }
        if (e->manifest_loaded) {
            jw_service_manifest_destroy(&e->manifest);
        }
    }
    sup->count = kept;
}

int jw_svc_supervisor_scan(jw_svc_supervisor *sup) {
    if (!sup) {
        return -1;
    }

    jw__scan_candidate *cands = NULL;
    int cand_count = 0, cand_cap = 0;
    /* Every configured Primary-source root is Primary. `Apps/<platform>` and
     * `Apps/shared` are two directories of ONE storage source, so a service
     * pak in either is startable; only a pak on a Secondary SOURCE is
     * "reported and not startable" per SVC-1. */
    for (int i = 0; i < sup->apps_scan_root_count; i++) {
        jw__scan_one_root(sup, sup->apps_scan_roots[i], false,
                          &cands, &cand_count, &cand_cap);
    }
    for (int i = 0; i < sup->secondary_scan_root_count; i++) {
        jw__scan_one_root(sup, sup->secondary_scan_roots[i], true,
                          &cands, &cand_count, &cand_cap);
    }

    /* Cross-manifest duplicate service.id: "Duplicates make both
     * unavailable." Only validated manifests participate (an invalid one
     * already carries its own rejection). */
    if (cand_count > 1) {
        const char **ids = calloc((size_t)cand_count, sizeof(char *));
        bool *dup = calloc((size_t)cand_count, sizeof(bool));
        if (ids && dup) {
            for (int i = 0; i < cand_count; i++) {
                ids[i] = cands[i].valid ? cands[i].service_id : NULL;
            }
            if (jw_svc_find_duplicate_ids(ids, (size_t)cand_count, dup)) {
                for (int i = 0; i < cand_count; i++) {
                    if (dup[i] && cands[i].valid) {
                        jw_service_manifest_destroy(&cands[i].manifest);
                        cands[i].manifest_loaded = false;
                        cands[i].valid = false;
                        snprintf(cands[i].reject_reason,
                                 sizeof(cands[i].reject_reason), "%s",
                                 "duplicate-service-id");
                    }
                }
            }
        }
        free(ids);
        free(dup);
    }

    /* Reconcile against the current set: a running service whose pak
     * vanished or went invalid keeps its owned group but becomes
     * unavailable for new actions. */
    for (int i = 0; i < sup->count; i++) {
        sup->entries[i].pak_present = false;
    }
    for (int i = 0; i < cand_count; i++) {
        jw__scan_candidate *c = &cands[i];
        jw_svc_supervised *e = jw__find_mut(sup, c->service_id);
        /* A malformed manifest may still contain the exact id of a valid
         * service. It must not overwrite that valid entry merely because its
         * directory happened to sort later in readdir() order. */
        if (e && !c->valid && e->manifest_valid) {
            jw__candidate_free(c);
            continue;
        }
        if (!e) {
            e = jw__ensure_entry(sup, c->service_id);
            if (!e) {
                jw__candidate_free(c);
                continue;
            }
        } else if (e->manifest_loaded) {
            jw_service_manifest_destroy(&e->manifest);
            e->manifest_loaded = false;
        }
        e->pak_present = true;
        e->manifest_valid = c->valid;
        e->on_secondary_root = c->on_secondary_root;
        jw__strcpy_bounded(e->pak_root, sizeof(e->pak_root), c->pak_root);
        jw__strcpy_bounded(e->reject_reason, sizeof(e->reject_reason), c->reject_reason);
        jw__strcpy_bounded(e->installed_package_version,
                           sizeof(e->installed_package_version),
                           c->package_version);
        /* Bounded copies: a service.id is at most JW_SVC_ID_MAX (128) but
         * the control field caps at JW_SVC_CONTROL_PACKAGE_ID_MAX (127), so
         * copy with an explicit length bound instead of snprintf("%s") --
         * which gcc's -Wformat-truncation rejects as potentially lossy. */
        jw__strcpy_bounded(e->control.installed_package_id,
                           sizeof(e->control.installed_package_id),
                           c->service_id);
        jw__strcpy_bounded(e->control.installed_package_version,
                           sizeof(e->control.installed_package_version),
                           c->package_version);
        if (c->valid) {
            e->manifest = c->manifest;
            e->manifest_loaded = true;
            c->manifest_loaded = false; /* ownership moved */
        }
        if (!jw__entry_available(e)) {
            e->state = JW_SVC_STATE_UNAVAILABLE;
        } else if (e->state == JW_SVC_STATE_UNAVAILABLE) {
            if (e->pgid > 0) {
                e->state = (e->reap_pending || e->stop_kill_sent)
                               ? JW_SVC_STATE_STOPPING
                               : JW_SVC_STATE_RUNNING;
            } else {
                e->state = jw__entry_idle_state(e);
            }
        }
        jw__candidate_free(c);
    }

    /* A retained control-state row remains visible even after its package is
     * removed. This is the data source for CTL-1's package-missing
     * `unavailable` record and Settings -> Services' hidden-when-truly-empty
     * rule. */
    jw_svc_control_id *retained_ids = NULL;
    size_t retained_count = 0;
    char list_reason[JW_SVC_REASON_BUF];
    if (!jw_svc_control_store_list_ids(sup->store, &retained_ids,
                                       &retained_count, list_reason,
                                       sizeof(list_reason))) {
        for (int i = 0; i < cand_count; i++) {
            jw__candidate_free(&cands[i]);
        }
        free(cands);
        return -1;
    }
    for (size_t i = 0; i < retained_count; i++) {
        const char *id = retained_ids[i].service_id;
        if (!jw_svc_supervisor_service_id_is_safe(id)) {
            continue;
        }
        jw_svc_supervised *e = jw__ensure_entry(sup, id);
        if (!e) {
            continue;
        }
        if (!e->pak_present) {
            e->manifest_valid = false;
            jw__strcpy_bounded(e->reject_reason,
                               sizeof(e->reject_reason),
                               "package-missing");
            e->state = JW_SVC_STATE_UNAVAILABLE;
        }
    }
    jw_svc_control_store_free_ids(retained_ids);

    for (int i = 0; i < sup->count; i++) {
        if (!sup->entries[i].pak_present) {
            sup->entries[i].manifest_valid = false;
            jw__strcpy_bounded(sup->entries[i].reject_reason,
                          sizeof(sup->entries[i].reject_reason),
                          "package-missing");
            sup->entries[i].state = JW_SVC_STATE_UNAVAILABLE;
        }
    }
    jw__discover_runtime_stale_entries(sup);
    /* Detect survivors even when they were launched only for the old daemon's
     * session and persistent enablement is false. Without this probe a daemon
     * restart would clear session_run and silently forget a still-live writer
     * until somebody explicitly tried to Run it again. */
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        if (e->pgid > 0 ||
            !jw__entry_available(e) ||
            !jw_svc_supervisor_service_id_is_safe(e->service_id)) {
            continue;
        }
        char lease_reason[JW_SVC_REASON_BUF];
        int lease_fd = jw_svc_lease_acquire(sup->runtime_dir, e->service_id,
                                            lease_reason,
                                            sizeof(lease_reason));
        if (lease_fd >= 0) {
            close(lease_fd);
        } else if (strcmp(lease_reason, "stale-generation") == 0) {
            jw__load_stale_policy(sup, e);
            e->state = JW_SVC_STATE_STALE_GENERATION;
            e->lease_retry_next_ms =
                jw__mono_ms() + JW_SVC_LEASE_RETRY_MS;
        }
    }
    jw__shed_exhausted_entries(sup);
    free(cands);
    return sup->count;
}

bool jw_svc_supervisor_entry_is_listable(const jw_svc_supervised *e) {
    if (!e) {
        return false;
    }
    /* The wire id must satisfy the same reverse-DNS grammar every CTL-1 client
     * validates against. An invalid manifest can leave an entry keyed by a
     * best-effort fallback (a pak directory name), and emitting that would
     * make a strict client reject the whole `list` response rather than one
     * row. Keep such entries internal. */
    if (!jw_service_id_is_reverse_dns(e->service_id)) {
        return false;
    }
    /* CTL-1 lists discovered services, including a malformed service whose
     * canonical declared/fallback id can safely survive the wire. That is how
     * clients reach its `unavailable` status and specific rejection reason.
     * A spent package-missing row is still omitted unless intent/generation
     * state requires retaining it. */
    return e->pak_present || e->manifest_valid ||
           e->desired_enabled || e->session_run ||
           e->pgid > 0 || e->state == JW_SVC_STATE_STALE_GENERATION;
}

/* ------------------------------------------------------------------ */
/* launch / stop                                                       */
/* ------------------------------------------------------------------ */

/* Builds the small service-specific layer of the child environment as the
 * JSON object launch.c expects. The inherited daemon environment (Leaf
 * runtime paths) is snapshotted by launch.c itself; here we only add what
 * SVC-1 supervision step 3 names explicitly: the service runtime dir. */
static char *jw__build_env_json(jw_svc_supervisor *sup,
                                const jw_svc_supervised *e) {
    cJSON *env = cJSON_CreateObject();
    if (!env) {
        return NULL;
    }
    char svc_dir[PATH_MAX];
    if (jw__format(svc_dir, sizeof(svc_dir), "%s/services/%s",
                   sup->runtime_dir, e->service_id) != 0 ||
        !cJSON_AddStringToObject(env, "UMRK_SERVICE_RUNTIME_DIR", svc_dir) ||
        !cJSON_AddStringToObject(env, "UMRK_SERVICE_ID", e->service_id)) {
        cJSON_Delete(env);
        return NULL;
    }
    char *text = cJSON_PrintUnformatted(env);
    cJSON_Delete(env);
    return text;
}

/* Called only after jw_svc_group_absent() proved that no non-zombie group
 * member remains. Blocking here cannot wait on a live leader: the only child
 * left to collect is the deliberately held zombie reservation. */
static bool jw__reap_verified_leader(pid_t leader, int *out_status) {
    int status = 0;
    pid_t reaped;
    do {
        reaped = waitpid(leader, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    if (reaped != leader) {
        return false;
    }
    if (out_status) {
        *out_status = status;
    }
    return true;
}

/* A stale generation is report-only and must never be signalled by this
 * daemon. Its last atomic policy snapshot still governs lifecycle decisions;
 * if the snapshot is missing/corrupt, SVC-1 requires the fail-safe "stop"
 * behavior. */
static void jw__load_stale_policy(jw_svc_supervisor *sup,
                                  jw_svc_supervised *e) {
    e->active_lifecycle_game = JW_SVC_LIFECYCLE_GAME_STOP;
    e->active_stop_on_storage_change = true;
    e->active_stop_on_suspend = true;
    jw_svc_reservation res;
    char reason[JW_SVC_REASON_BUF];
    if (!jw_svc_reservation_read(sup->runtime_dir, e->service_id, &res,
                                 reason, sizeof(reason))) {
        return;
    }
    if (res.game_policy == JW_SVC_RESERVATION_GAME_IGNORE) {
        e->active_lifecycle_game = JW_SVC_LIFECYCLE_GAME_IGNORE;
    } else if (res.game_policy == JW_SVC_RESERVATION_GAME_NOTIFY) {
        e->active_lifecycle_game = JW_SVC_LIFECYCLE_GAME_NOTIFY;
    }
    e->active_stop_on_storage_change = res.stop_on_storage_change;
    e->active_stop_on_suspend = res.stop_on_suspend;
}

/* One supervised launch attempt: acquire the generation lease, open the
 * rotating log, fork/exec, record the ownership reservation. On success the
 * entry owns a live pgid and the daemon-held lease fd. */
static bool jw__start_generation(jw_svc_supervisor *sup,
                                 jw_svc_supervised *e,
                                 char *reason, size_t reason_size) {
    char slug[JW_SVC_REASON_BUF];

    /* PKG-1: no generation may start between the verified stop barrier and
     * the post-replacement manifest scan. Check both the global latch (which
     * also covers newly discovered services) and the entry snapshot. */
    if (sup->package_quiesce_active || e->package_blocked) {
        jw__set_reason(reason, reason_size, "package-in-progress");
        return false;
    }
    if (sup->game_active &&
        e->manifest.lifecycle_game != JW_SVC_LIFECYCLE_GAME_IGNORE) {
        jw__set_reason(reason, reason_size, "lifecycle-in-progress");
        return false;
    }

    /* The lease is the no-overlap gate. Never start without it. */
    int lease_fd = jw_svc_lease_acquire(sup->runtime_dir, e->service_id,
                                        slug, sizeof(slug));
    if (lease_fd < 0) {
        if (strcmp(slug, "stale-generation") == 0) {
            jw__load_stale_policy(sup, e);
        }
        jw__set_reason(reason, reason_size,
                       strcmp(slug, "stale-generation") == 0
                           ? "stale-generation" : "lease-failed");
        return false;
    }

    int log_fd = -1;
    if (sup->logs_dir[0]) {
        log_fd = jw_svc_launch_open_log(sup->logs_dir, e->service_id,
                                        slug, sizeof(slug));
        /* A failed log open is not fatal: launch.c logs to /dev/null
         * instead rather than leaving the service on Jawaka's stdout. */
        if (log_fd < 0) {
            jw_log_warn("service logs unavailable service=%s reason=%s; "
                        "output goes to /dev/null",
                        e->service_id, slug);
        }
    }

    char run_abs[PATH_MAX];
    if (jw__format(run_abs, sizeof(run_abs), "%s/%s", e->pak_root,
                   e->manifest.run_path) != 0) {
        if (log_fd >= 0) close(log_fd);
        close(lease_fd);
        jw__set_reason(reason, reason_size, "path-too-long");
        return false;
    }

    const char *args[JW_SVC_MAX_ARGS];
    for (int i = 0; i < e->manifest.run_args_count; i++) {
        args[i] = e->manifest.run_args[i];
    }

    char *env_json = jw__build_env_json(sup, e);
    if (!env_json) {
        if (log_fd >= 0) close(log_fd);
        close(lease_fd);
        jw__set_reason(reason, reason_size, "launch-failed");
        return false;
    }
    jw_svc_launch_request req = {
        .run_path_abs = run_abs,
        .args = args,
        .args_count = e->manifest.run_args_count,
        .env_json = env_json,
        .lease_fd = lease_fd,
        .log_fd = log_fd,
    };
    char launch_reason[JW_SVC_LAUNCH_REASON_BUF];
    pid_t pid = jw_svc_launch(&req, launch_reason, sizeof(launch_reason));
    free(env_json);
    if (log_fd >= 0) {
        close(log_fd); /* the child holds its own duplicate */
    }
    if (pid < 0) {
        close(lease_fd);
        jw__set_reason(reason, reason_size, "launch-failed");
        return false;
    }

    /* Ownership reservation: leader pid == pgid, monotonic launch instant,
     * normalized lifecycle-policy snapshot. Written before the entry is
     * marked running so a crash between fork and here still leaves no
     * running group without a record. */
    long long launch_instant_us = jw_svc_reservation_now_us();
    jw_svc_reservation res = {
        .pgid = pid,
        .launch_instant_us = launch_instant_us,
        .game_policy =
            e->manifest.lifecycle_game == JW_SVC_LIFECYCLE_GAME_STOP
                ? JW_SVC_RESERVATION_GAME_STOP
                : e->manifest.lifecycle_game == JW_SVC_LIFECYCLE_GAME_NOTIFY
                      ? JW_SVC_RESERVATION_GAME_NOTIFY
                      : JW_SVC_RESERVATION_GAME_IGNORE,
        .stop_on_storage_change = e->manifest.stop_on_storage_change,
        .stop_on_suspend = e->manifest.stop_on_suspend,
    };
    if (!jw_svc_reservation_write(sup->runtime_dir, e->service_id, &res,
                                  slug, sizeof(slug))) {
        /* The group is live but unrecorded: stop it now rather than run
         * an untracked service, per SVC-1's "never an unreserved group". */
        jw_svc_stop_result stopped =
            jw_svc_stop_group(pid, e->manifest.stop_grace_ms,
                              jw_svc_group_absent);
        if (stopped.verified_absent &&
            jw__reap_verified_leader(pid, NULL)) {
            close(lease_fd);
        } else {
            /* Keep the exact child identity and lease in memory until later
             * ticks prove absence. Closing either here would lose the only
             * safe handle on a still-live writer after the disk write failed. */
            e->lease_fd = lease_fd;
            e->pgid = pid;
            e->launch_instant_us = launch_instant_us;
            e->active_stop_grace_ms = e->manifest.stop_grace_ms;
            e->active_restart_on_failure = false;
            e->active_lifecycle_game = e->manifest.lifecycle_game;
            e->active_stop_on_storage_change =
                e->manifest.stop_on_storage_change;
            e->active_stop_on_suspend = e->manifest.stop_on_suspend;
            e->pending_stop_reason = JW_SVC_STOP_INTENTIONAL;
            e->state = JW_SVC_STATE_STOPPING;
            e->stop_kill_sent = stopped.escalated_to_kill;
            e->stopping_since_ms = jw__mono_ms();
            e->reap_pending = stopped.verified_absent;
        }
        jw__set_reason(reason, reason_size, "reservation-failed");
        return false;
    }

    e->lease_fd = lease_fd;
    e->pgid = pid;
    e->launch_instant_us = launch_instant_us;
    e->active_stop_grace_ms = e->manifest.stop_grace_ms;
    e->active_restart_on_failure = e->manifest.restart_on_failure;
    e->active_lifecycle_game = e->manifest.lifecycle_game;
    e->active_stop_on_storage_change = e->manifest.stop_on_storage_change;
    e->active_stop_on_suspend = e->manifest.stop_on_suspend;
    e->autostart_pending = false;
    e->reap_pending = false;
    e->stop_requested = false;
    e->post_stop = JW_SVC_POST_STOP_NONE;
    e->stop_kill_sent = false;
    e->stop_unverified_logged = false;
    /* No stop has been requested for the fresh generation: if its leader
     * exits now that is a genuine crash, eligible for on-failure restart. */
    e->pending_stop_reason = JW_SVC_STOP_NONE;
    e->state = JW_SVC_STATE_STARTING;
    e->stopping_since_ms = jw__mono_ms();
    return true;
}

/* Initiates the stop sequence WITHOUT waiting for it: SIGTERM to the reserved
 * group, then tick owns the grace window, the SIGKILL escalation, verified
 * absence, the reap, and any post-stop action.
 *
 * This is what CTL-1 Stop/Disable/Restart use. Blocking the daemon's
 * single-threaded main loop for stop_grace_ms + 2000 ms (up to 17 s at the
 * contract ceiling) would freeze input, power handling, and every other
 * service's supervision -- the same reason SVC-1 forbids blocking the loop on
 * the lease, and the reason the leader-exit stop was already asynchronous.
 * "stopping" is a first-class CTL-1 effective state precisely so this
 * operation can be reported as in-progress rather than waited on. */
static void jw__begin_async_stop(jw_svc_supervisor *sup, jw_svc_supervised *e,
                                 jw_svc_stop_reason_kind kind,
                                 jw_svc_post_stop_action post) {
    if (!e || e->pgid <= 0) {
        return;
    }
    e->pending_stop_reason = kind;
    e->post_stop = post;
    e->autostart_pending = false;
    e->stop_unverified_logged = false;
    if (!e->stop_requested && !e->reap_pending) {
        /* First TERM for this stop: start the grace window. An already
         * in-flight stop keeps its original deadline so a second Stop press
         * cannot extend the group's life past the contract bound. */
        kill(-e->pgid, SIGTERM);
        e->stopping_since_ms = jw__mono_ms();
        e->stop_kill_sent = false;
    }
    e->stop_requested = true;
    e->state = JW_SVC_STATE_STOPPING;
    (void)sup;
}

/* Called once a group has been proven absent, reaped, and released. Runs the
 * tail of whatever operation initiated the stop. */
static void jw__on_group_released(jw_svc_supervisor *sup,
                                  jw_svc_supervised *e) {
    jw_svc_post_stop_action post = e->post_stop;
    e->post_stop = JW_SVC_POST_STOP_NONE;
    e->stop_requested = false;
    e->stop_kill_sent = false;
    e->stop_unverified_logged = false;
    e->stopping_since_ms = 0;
    if (post != JW_SVC_POST_STOP_RESTART) {
        return;
    }
    /* CTL-1 Restart: exactly one replacement generation, and only now that
     * the previous reserved group is verified gone. */
    if (!jw__entry_available(e)) {
        e->state = jw__entry_idle_state(e);
        jw__persist(sup, e, "restart-unavailable");
        return;
    }
    char reason[JW_SVC_REASON_BUF];
    if (jw__start_generation(sup, e, reason, sizeof(reason))) {
        e->control.restart_count++;
        jw__persist(sup, e, "restart");
        return;
    }
    if (strcmp(reason, "stale-generation") == 0) {
        e->state = JW_SVC_STATE_STALE_GENERATION;
        e->lease_retry_next_ms = jw__mono_ms() + JW_SVC_LEASE_RETRY_MS;
        return;
    }
    e->session_run = false;
    e->control.session_run = false;
    e->state = jw__entry_idle_state(e);
    jw__persist(sup, e, "restart-failed");
}

/* Runs the stop sequence against the entry's owned group, then reaps the
 * leader and releases the lease and reservation once absence is verified.
 * SVC-1's order is reservation -> verify -> reap, never reap -> verify.
 *
 * SYNCHRONOUS: blocks up to stop_grace_ms + JW_SVC_STOP_KILL_WAIT_MS. Only
 * for callers that genuinely cannot proceed without the guarantee (shutdown,
 * game launch, suspend, safe unmount). CTL-1 session ops use
 * jw__begin_async_stop() instead. */
static bool jw__stop_and_reap_coordinated(
    jw_svc_supervisor *sup, jw_svc_supervised *e, pid_t coordinator_pid,
    jw_svc_stop_result *stop_result, char *reason, size_t reason_size) {
    if (e->pgid <= 0) {
        jw__set_reason(reason, reason_size, "not-running");
        return false;
    }
    jw_svc_stop_result res = coordinator_pid > 0
        ? jw_svc_stop_group_coordinated(
              e->pgid, coordinator_pid, e->active_stop_grace_ms,
              jw_svc_group_absent)
        : jw_svc_stop_group(e->pgid, e->active_stop_grace_ms,
                            jw_svc_group_absent);
    if (stop_result) {
        *stop_result = res;
    }
    if (!res.verified_absent) {
        e->state = JW_SVC_STATE_STOPPING;
        e->stopping_since_ms = jw__mono_ms();
        e->stop_kill_sent = true;
        /* The synchronous TERM -> KILL -> 2 s sequence has already exhausted
         * its contract window. Remember that fact immediately (so a retried
         * safe-unmount cannot bypass it), and keep tick polling until the
         * survivor eventually disappears. */
        e->stop_requested = true;
        e->stop_unverified_logged = true;
        jw__set_reason(reason, reason_size, "stop-failed");
        return false;
    }
    /* Verified writer-free: now, and only now, reap the leader and release
     * the reservation. Reaping first would free the pgid for recycling. */
    int status = 0;
    if (!jw__reap_verified_leader(e->pgid, &status)) {
        e->state = JW_SVC_STATE_STOPPING;
        e->reap_pending = true;
        jw__set_reason(reason, reason_size, "stop-failed");
        return false;
    }
    e->reap_pending = false;
    jw__entry_release_runtime(e);
    e->state = jw__entry_idle_state(e);
    /* A synchronous stop supersedes any async stop that was already in
     * flight; run its tail (if any) now that the group is verified gone. */
    jw__on_group_released(sup, e);
    return true;
}

static bool jw__stop_and_reap(jw_svc_supervisor *sup, jw_svc_supervised *e,
                              char *reason, size_t reason_size) {
    return jw__stop_and_reap_coordinated(
        sup, e, 0, NULL, reason, reason_size);
}

/* ------------------------------------------------------------------ */
/* CTL-1 operations                                                    */
/* ------------------------------------------------------------------ */

static bool jw__require_available(const jw_svc_supervised *e,
                                  char *reason, size_t reason_size) {
    if (!e) {
        jw__set_reason(reason, reason_size, "unknown-service");
        return false;
    }
    if (!e->manifest_valid || e->on_secondary_root || !e->pak_present) {
        jw__set_reason(reason, reason_size, "unavailable");
        return false;
    }
    return true;
}

static bool jw__require_package_idle(const jw_svc_supervisor *sup,
                                     char *reason, size_t reason_size) {
    if (sup && sup->package_quiesce_active) {
        jw__set_reason(reason, reason_size, "package-in-progress");
        return false;
    }
    return true;
}

static bool jw__require_entry_package_idle(const jw_svc_supervised *e,
                                           char *reason,
                                           size_t reason_size) {
    if (e && e->package_blocked) {
        jw__set_reason(reason, reason_size, "package-in-progress");
        return false;
    }
    return true;
}

bool jw_svc_supervisor_enable(jw_svc_supervisor *sup, const char *service_id,
                              char *reason, size_t reason_size) {
    if (!jw__require_package_idle(sup, reason, reason_size)) {
        return false;
    }
    jw_svc_supervised *e = jw__find_mut(sup, service_id);
    if (!jw__require_entry_package_idle(e, reason, reason_size)) {
        return false;
    }
    if (!jw__require_available(e, reason, reason_size)) {
        return false;
    }
    bool old_desired = e->desired_enabled;
    bool old_control_desired = e->control.start_with_leaf;
    e->desired_enabled = true;
    e->control.start_with_leaf = true;
    if (!jw__persist(sup, e, "enable")) {
        e->desired_enabled = old_desired;
        e->control.start_with_leaf = old_control_desired;
        jw__set_reason(reason, reason_size, "store-failed");
        return false;
    }
    return true;
}

bool jw_svc_supervisor_disable(jw_svc_supervisor *sup, const char *service_id,
                               char *reason, size_t reason_size) {
    if (!jw__require_package_idle(sup, reason, reason_size)) {
        return false;
    }
    jw_svc_supervised *e = jw__find_mut(sup, service_id);
    if (!jw__require_entry_package_idle(e, reason, reason_size)) {
        return false;
    }
    if (!e) {
        jw__set_reason(reason, reason_size, "unknown-service");
        return false;
    }
    bool old_desired = e->desired_enabled;
    bool old_control_desired = e->control.start_with_leaf;
    bool old_session_run = e->session_run;
    bool old_control_session_run = e->control.session_run;
    bool old_autostart_pending = e->autostart_pending;
    bool old_lifecycle_restart_pending = e->lifecycle_restart_pending;
    bool old_game_restart_pending = e->game_restart_pending;
    e->desired_enabled = false;
    e->control.start_with_leaf = false;
    e->session_run = false;
    e->control.session_run = false;
    e->autostart_pending = false;
    e->lifecycle_restart_pending = false;
    e->game_restart_pending = false;
    /* Persist the disabled intent before attempting a potentially unverified
     * stop. Otherwise a stuck service would come back as desired after the
     * next daemon restart and could be relaunched when its stale lease clears. */
    if (!jw__persist(sup, e, "disable")) {
        e->desired_enabled = old_desired;
        e->control.start_with_leaf = old_control_desired;
        e->session_run = old_session_run;
        e->control.session_run = old_control_session_run;
        e->autostart_pending = old_autostart_pending;
        e->lifecycle_restart_pending = old_lifecycle_restart_pending;
        e->game_restart_pending = old_game_restart_pending;
        jw__set_reason(reason, reason_size, "store-failed");
        return false;
    }
    if (e->pgid > 0) {
        /* Non-blocking: the disabled intent is already durable above, so the
         * stop can complete under tick without holding the daemon loop. */
        jw__begin_async_stop(sup, e, JW_SVC_STOP_INTENTIONAL,
                             JW_SVC_POST_STOP_NONE);
        return true;
    }
    /* A stale generation is report-only but still positively alive by its
     * locked lease. Disabling withdraws every desired intent; it must not
     * erase STALE_GENERATION, because passive lease probing and lifecycle
     * safety checks both key off that state until acquisition proves absence. */
    if (e->state == JW_SVC_STATE_STALE_GENERATION) {
        return true;
    }
    if (e->state != JW_SVC_STATE_STOPPING) {
        e->state = jw__entry_idle_state(e);
    }
    return true;
}

bool jw_svc_supervisor_run(jw_svc_supervisor *sup, const char *service_id,
                           char *reason, size_t reason_size) {
    if (!jw__require_package_idle(sup, reason, reason_size)) {
        return false;
    }
    jw_svc_supervised *e = jw__find_mut(sup, service_id);
    if (!jw__require_entry_package_idle(e, reason, reason_size)) {
        return false;
    }
    if (!jw__require_available(e, reason, reason_size)) {
        return false;
    }
    if (sup->game_active &&
        e->manifest.lifecycle_game != JW_SVC_LIFECYCLE_GAME_IGNORE) {
        jw__set_reason(reason, reason_size, "lifecycle-in-progress");
        return false;
    }
    if (e->stop_requested || e->reap_pending ||
        e->state == JW_SVC_STATE_STOPPING) {
        /* A stop is still in flight. Starting now would overlap the old
         * reserved group, which SVC-1 forbids outright. */
        jw__set_reason(reason, reason_size, "stopping");
        return false;
    }
    if (e->pgid > 0 || e->state == JW_SVC_STATE_STARTING) {
        jw__set_reason(reason, reason_size, "already-running");
        return false;
    }
    /* A user-initiated Run clears the breaker (SVC-1 restart policy) and
     * triggers an immediate lease attempt even past the 1 s retry. */
    e->lifecycle_restart_pending = false;
    jw_svc_backoff_reset(&e->backoff);
    char start_reason[JW_SVC_REASON_BUF];
    if (!jw__start_generation(sup, e, start_reason, sizeof(start_reason))) {
        if (strcmp(start_reason, "stale-generation") == 0) {
            e->session_run = true;
            e->control.session_run = true;
            e->state = JW_SVC_STATE_STALE_GENERATION;
            e->lease_retry_next_ms = jw__mono_ms() + JW_SVC_LEASE_RETRY_MS;
            if (!jw__persist(sup, e, "run-stale-generation")) {
                e->session_run = false;
                e->control.session_run = false;
                snprintf(start_reason, sizeof(start_reason), "%s",
                         "store-failed");
            }
        }
        jw__set_reason(reason, reason_size, start_reason);
        return false;
    }
    e->session_run = true;
    e->control.session_run = true;
    if (!jw__persist(sup, e, "run")) {
        jw__set_reason(reason, reason_size, "store-failed");
        return false;
    }
    return true;
}

bool jw_svc_supervisor_stop(jw_svc_supervisor *sup, const char *service_id,
                            char *reason, size_t reason_size) {
    if (!jw__require_package_idle(sup, reason, reason_size)) {
        return false;
    }
    jw_svc_supervised *e = jw__find_mut(sup, service_id);
    if (!jw__require_entry_package_idle(e, reason, reason_size)) {
        return false;
    }
    if (!e) {
        jw__set_reason(reason, reason_size, "unknown-service");
        return false;
    }
    if (e->pgid <= 0) {
        jw__set_reason(reason, reason_size, "not-running");
        return false;
    }
    bool old_session_run = e->session_run;
    bool old_control_loaded = e->control_loaded;
    bool old_lifecycle_restart_pending = e->lifecycle_restart_pending;
    bool old_game_restart_pending = e->game_restart_pending;
    jw_svc_control_state old_control = e->control;
    e->lifecycle_restart_pending = false;
    e->game_restart_pending = false;
    /* Session intent is withdrawn immediately -- the user asked for the stop,
     * and it must survive even if the group turns out to be unstoppable. The
     * stop sequence itself then runs under tick. */
    e->session_run = false;
    e->control.session_run = false;
    if (!jw__persist(sup, e, "stop")) {
        e->session_run = old_session_run;
        e->control = old_control;
        e->control_loaded = old_control_loaded;
        e->lifecycle_restart_pending = old_lifecycle_restart_pending;
        e->game_restart_pending = old_game_restart_pending;
        jw__set_reason(reason, reason_size, "store-failed");
        return false;
    }
    /* Only signal after the control mutation was accepted. A caller receiving
     * store-failed must be able to rely on the old group still being untouched. */
    jw__begin_async_stop(sup, e, JW_SVC_STOP_INTENTIONAL,
                         JW_SVC_POST_STOP_NONE);
    return true;
}

bool jw_svc_supervisor_restart(jw_svc_supervisor *sup, const char *service_id,
                               char *reason, size_t reason_size) {
    if (!jw__require_package_idle(sup, reason, reason_size)) {
        return false;
    }
    jw_svc_supervised *e = jw__find_mut(sup, service_id);
    if (!jw__require_entry_package_idle(e, reason, reason_size)) {
        return false;
    }
    if (!jw__require_available(e, reason, reason_size)) {
        return false;
    }
    if (sup->game_active &&
        e->manifest.lifecycle_game != JW_SVC_LIFECYCLE_GAME_IGNORE) {
        jw__set_reason(reason, reason_size, "lifecycle-in-progress");
        return false;
    }
    jw_svc_backoff_state old_backoff = e->backoff;
    jw_svc_control_state old_control = e->control;
    bool old_control_loaded = e->control_loaded;
    bool old_session_run = e->session_run;
    bool old_lifecycle_restart_pending = e->lifecycle_restart_pending;
    jw_svc_backoff_reset(&e->backoff);
    e->lifecycle_restart_pending = false;
    e->session_run = true;
    e->control.session_run = true;
    /* Persist acceptance before touching the old process group or launching a
     * replacement. This gives store-failed a transactional meaning. */
    if (!jw__persist(sup, e, "restart-requested")) {
        e->backoff = old_backoff;
        e->control = old_control;
        e->control_loaded = old_control_loaded;
        e->session_run = old_session_run;
        e->lifecycle_restart_pending = old_lifecycle_restart_pending;
        jw__set_reason(reason, reason_size, "store-failed");
        return false;
    }
    if (e->pgid > 0) {
        /* Stop now, start later: tick launches the one replacement generation
         * once the previous reserved group is proven absent (SVC-1's "never
         * start a replacement over the old group"). Reported as `stopping`
         * until then rather than blocking the caller for the full sequence. */
        jw__begin_async_stop(sup, e, JW_SVC_STOP_INTENTIONAL,
                             JW_SVC_POST_STOP_RESTART);
        return true;
    }
    char start_reason[JW_SVC_REASON_BUF];
    if (!jw__start_generation(sup, e, start_reason, sizeof(start_reason))) {
        if (strcmp(start_reason, "stale-generation") == 0) {
            e->state = JW_SVC_STATE_STALE_GENERATION;
            e->lease_retry_next_ms = jw__mono_ms() + JW_SVC_LEASE_RETRY_MS;
            /* Acceptance is already durable. This write is diagnostic only;
             * failure must not turn an accepted queued restart into a reported
             * store failure after the fact. */
            (void)jw__persist(sup, e, "restart-stale-generation");
        } else {
            e->session_run = old_session_run;
            e->control.session_run = old_control.session_run;
            (void)jw__persist(sup, e, "restart-failed");
        }
        jw__set_reason(reason, reason_size, start_reason);
        return false;
    }
    /* The operation was accepted before launch. A later history-write failure
     * cannot honestly be reported as though no restart occurred. */
    (void)jw__persist(sup, e, "restart");
    return true;
}

/* ------------------------------------------------------------------ */
/* tick                                                                */
/* ------------------------------------------------------------------ */

/* Handles a leader that has exited: records the exit, decides whether the
 * failure is restartable (on-failure policy + backoff), and either starts
 * the stop sequence against the still-reserved group (live descendants) or
 * finalizes an already-absent group. */
static void jw__handle_leader_exit(jw_svc_supervisor *sup,
                                   jw_svc_supervised *e, int exit_code,
                                   bool failed) {
    e->control.has_last_exit = true;
    e->control.last_exit_code = exit_code;
    e->control.last_exit_at_us = jw__wall_us();

    /* Session Run is consumed by the exit: it is one-shot session control,
     * not a keep-alive. Only persistent "Start with Leaf"
     * (desired_enabled) or the on-failure restart policy may bring the
     * service back without a fresh user action.
     *
     * A CTL-1 Restart is the exception: this exit IS the stop half of that
     * operation, so the user's session intent carries into the replacement
     * generation rather than being consumed by the stop they asked for. */
    if (e->post_stop != JW_SVC_POST_STOP_RESTART) {
        e->session_run = false;
        e->control.session_run = false;
    }

    /* The leader is now a zombie holding the pgid reservation. If live
     * descendants remain the group is NOT absent and must be stopped and
     * verified before any restart -- never interpret leader exit as service
     * absence. */
    bool absent = jw_svc_group_absent(e->pgid);

    bool intentional = jw__stop_is_deliberate(e->pending_stop_reason);
    const char *transition_reason = "exited";
    if (!intentional && failed && e->active_restart_on_failure &&
        jw__entry_available(e)) {
        jw_svc_backoff_decision d =
            jw_svc_backoff_record_failure(&e->backoff, jw__wall_ms());
        if (d.breaker_open) {
            e->state = JW_SVC_STATE_FAILED;
            transition_reason = "circuit-breaker";
        } else {
            e->state = JW_SVC_STATE_BACKOFF;
            e->backoff_retry_at_ms = jw__mono_ms() + d.delay_ms;
            transition_reason = "failure";
        }
    } else {
        e->state = JW_SVC_STATE_STOPPING;
    }
    /* A foreground service that leaves a live descendant after its leader
     * exits has violated SVC-1 regardless of exit code or restart policy.
     * Preserve that stable diagnosis while the ordinary stop sequence
     * contains the still-reserved process group. */
    if (!intentional && !absent) {
        transition_reason = "foreground-contract-violation";
    }
    jw__persist(sup, e, transition_reason);

    if (absent) {
        /* Whole group is gone-or-zombie already: reap the leader and
         * release. The failure/backoff state decided above still stands. */
        if (!jw__reap_verified_leader(e->pgid, NULL)) {
            /* Keep the lease and ownership identity rather than freeing a
             * pgid whose leader was not successfully collected. */
            e->state = JW_SVC_STATE_STOPPING;
            e->reap_pending = true;
            return;
        }
        jw__entry_release_runtime(e);
        if (e->state != JW_SVC_STATE_BACKOFF &&
            e->state != JW_SVC_STATE_FAILED) {
            e->state = jw__entry_idle_state(e);
        }
        jw__on_group_released(sup, e);
    } else {
        /* Live descendants remain: begin the asynchronous stop sequence.
         * SIGTERM now, SIGKILL after the grace window, verified absence
         * before any restart. */
        kill(-e->pgid, SIGTERM);
        /* An async CTL-1 stop already opened a grace window; the leader
         * exiting inside it must not restart the clock, or a service could
         * outlive the contract's stop_grace_ms + 2000 ms bound. */
        if (!e->stop_requested) {
            e->stopping_since_ms = jw__mono_ms();
            e->stop_kill_sent = false;
            e->stop_unverified_logged = false;
        }
        e->reap_pending = true;
        if (!intentional) {
            e->pending_stop_reason = JW_SVC_STOP_LEADER_EXITED;
        }
        if (e->state != JW_SVC_STATE_BACKOFF &&
            e->state != JW_SVC_STATE_FAILED) {
            e->state = JW_SVC_STATE_STOPPING;
        }
    }
}

/* Observe child termination without consuming the zombie reservation.
 * waitpid(WNOHANG) is not a poll: once a child has exited it reaps it. WNOWAIT
 * is therefore mandatory until jw_svc_group_absent() has proved the group
 * writer-free. */
static bool jw__observe_leader_exit(pid_t leader, int *out_exit_code,
                                    bool *out_failed) {
    siginfo_t info;
    memset(&info, 0, sizeof(info));
    int rc;
    do {
        rc = waitid(P_PID, (id_t)leader, &info,
                    WEXITED | WNOHANG | WNOWAIT);
    } while (rc != 0 && errno == EINTR);
    if (rc != 0 || info.si_pid != leader) {
        return false;
    }
    if (info.si_code == CLD_EXITED) {
        *out_exit_code = info.si_status;
        *out_failed = info.si_status != 0;
    } else {
        *out_exit_code = 128 + info.si_status;
        *out_failed = true;
    }
    return true;
}

void jw_svc_supervisor_set_exit_notify(jw_svc_supervisor *sup, bool enabled) {
    if (!sup) {
        return;
    }
    sup->exit_notify = enabled;
    sup->child_exit_pending = true;   /* check every leader once on the switch */
}

void jw_svc_supervisor_note_child_exit(jw_svc_supervisor *sup) {
    if (sup) {
        sup->child_exit_pending = true;
    }
}

static void jw__deadline_min(long long *best, long long at) {
    if (*best < 0 || at < *best) {
        *best = at;
    }
}

/* Mirrors the tick below: which of its branches can act, and when. */
long long jw_svc_supervisor_next_deadline_ms(const jw_svc_supervisor *sup,
                                             long long now_ms) {
    if (!sup) {
        return -1;
    }
    long long best = -1;
    for (int i = 0; i < sup->count; i++) {
        const jw_svc_supervised *e = &sup->entries[i];
        if (e->pgid > 0 && (e->reap_pending || e->stop_requested)) {
            /* The stop sequence checks for the group to be gone each tick. */
            jw__deadline_min(&best, now_ms);
            continue;
        }
        if (e->pgid > 0) {
            if (e->state == JW_SVC_STATE_STARTING) {
                jw__deadline_min(&best, e->stopping_since_ms +
                                            JW_SVC_STARTING_SETTLE_MS);
            }
            continue;   /* running: only its exit matters */
        }
        if (sup->package_quiesce_active) {
            continue;
        }
        if (sup->game_active &&
            (e->manifest.lifecycle_game != JW_SVC_LIFECYCLE_GAME_IGNORE ||
             e->game_restart_pending)) {
            continue;
        }
        bool startable = e->state != JW_SVC_STATE_STALE_GENERATION &&
                         e->state != JW_SVC_STATE_STOPPING &&
                         jw__entry_available(e);
        if (e->game_restart_pending && startable) {
            jw__deadline_min(&best, now_ms);
        }
        if (e->lifecycle_restart_pending && startable &&
            !(sup->storage_restart_blocked &&
              e->pending_stop_reason == JW_SVC_STOP_LIFECYCLE_STORAGE)) {
            jw__deadline_min(&best, now_ms);
        }
        if (e->state == JW_SVC_STATE_BACKOFF) {
            jw__deadline_min(&best, e->backoff_retry_at_ms);
        }
        if (e->state == JW_SVC_STATE_STALE_GENERATION) {
            jw__deadline_min(&best, e->lease_retry_next_ms);
        }
        bool idle = e->state != JW_SVC_STATE_BACKOFF &&
                    e->state != JW_SVC_STATE_FAILED &&
                    e->state != JW_SVC_STATE_STOPPING &&
                    e->state != JW_SVC_STATE_STALE_GENERATION &&
                    e->state != JW_SVC_STATE_STARTING;
        if (idle && e->autostart_pending && e->manifest_valid &&
            e->pak_present && !e->on_secondary_root) {
            jw__deadline_min(&best, now_ms);
        }
    }
    return best;
}

int jw_svc_supervisor_tick(jw_svc_supervisor *sup) {
    if (!sup) {
        return -1;
    }
    int changes = 0;
    long long now = jw__mono_ms();
    bool observe_exits = !sup->exit_notify || sup->child_exit_pending;
    sup->child_exit_pending = false;

    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];

        /* 1. Poll the leader without reaping it early. */
        if (e->pgid > 0 && !e->reap_pending) {
            int exit_code = -1;
            bool failed = true;
            if (observe_exits &&
                jw__observe_leader_exit(e->pgid, &exit_code, &failed)) {
                jw__handle_leader_exit(sup, e, exit_code, failed);
                changes++;
                continue;
            }
            /* Settle "starting" into "running" after the leader has
             * survived its first window. */
            if (e->state == JW_SVC_STATE_STARTING &&
                now - e->stopping_since_ms >= JW_SVC_STARTING_SETTLE_MS) {
                e->state = JW_SVC_STATE_RUNNING;
                changes++;
            }
        }

        /* 2. Asynchronous stop sequence, driving both shapes: a group whose
         *    leader already exited leaving descendants (reap_pending), and a
         *    CTL-1 Stop/Disable/Restart whose leader may still be alive
         *    (stop_requested). Escalate to SIGKILL once the grace window
         *    elapses, then verify absence. */
        if (e->pgid > 0 && (e->reap_pending || e->stop_requested)) {
            long long grace = e->active_stop_grace_ms;
            if (!e->stop_kill_sent &&
                now - e->stopping_since_ms >= grace) {
                kill(-e->pgid, SIGKILL);
                e->stop_kill_sent = true;
                e->stopping_since_ms = now;
            }
            /* Reap as soon as the group is verified absent -- whether that
             * is inside the TERM grace window or after the KILL window.
             * Absence implies the leader is gone-or-zombie, so the blocking
             * waitpid below cannot wait on a live child. */
            bool wait_window_done =
                e->stop_kill_sent &&
                now - e->stopping_since_ms >= JW_SVC_STOP_KILL_WAIT_MS;
            if (jw_svc_group_absent(e->pgid)) {
                if (!jw__reap_verified_leader(e->pgid, NULL)) {
                    continue;
                }
                jw__entry_release_runtime(e);
                e->reap_pending = false;
                if (e->state != JW_SVC_STATE_BACKOFF &&
                    e->state != JW_SVC_STATE_FAILED) {
                    e->state = jw__entry_idle_state(e);
                }
                jw__on_group_released(sup, e);
                changes++;
            } else if (wait_window_done && !e->stop_unverified_logged) {
                /* TERM+KILL+2s and the group is STILL present: a known live
                 * writer. Leave it in "stopping", keep the lease, and do not
                 * reap -- SVC-1's unverified-stop table governs the
                 * surrounding operation. Record the transition once so CTL-1
                 * `status` can explain why the service is wedged, then keep
                 * polling so a later disappearance is still observed. */
                e->stop_unverified_logged = true;
                jw__persist(sup, e, "stop-unverified");
                changes++;
            }
        }

        /* PKG-1 keeps polling/reaping an already requested stop, but no
         * restart, stale-lease launch, or autostart may cross the package
         * replacement window. */
        if (sup->package_quiesce_active) {
            continue;
        }

        /* LIFE-1 keeps observing/reaping a group that is already stopping,
         * but every path that could create a new game-sensitive generation is
         * suppressed until the authoritative writer barrier clears. Ignore
         * services remain completely outside game coordination. */
        jw_svc_lifecycle_game game_policy = e->pgid > 0
            ? e->active_lifecycle_game : e->manifest.lifecycle_game;
        if (sup->game_active &&
            (game_policy != JW_SVC_LIFECYCLE_GAME_IGNORE ||
             e->game_restart_pending)) {
            continue;
        }

        /* 3. Backoff: retry a failed on-failure service once its delay
         *    elapses, only if the previous generation's group is gone. */
        bool started_this_tick = false;

        /* A mode-stop or fallback stop is resumed exactly once after the
         * active record was durably cleared. This is intentionally separate
         * from desired/session intent: it records the fact that Jawaka, not
         * the user, took down an effective service for this game. */
        if (e->game_restart_pending && e->pgid <= 0 &&
            e->state != JW_SVC_STATE_STALE_GENERATION &&
            e->state != JW_SVC_STATE_STOPPING &&
            jw__entry_available(e)) {
            char reason[JW_SVC_REASON_BUF];
            if (jw__start_generation(sup, e, reason, sizeof(reason))) {
                e->game_restart_pending = false;
                jw__persist(sup, e, "game-resume");
                started_this_tick = true;
                changes++;
            } else if (strcmp(reason, "stale-generation") == 0) {
                e->state = JW_SVC_STATE_STALE_GENERATION;
                e->lease_retry_next_ms = now + JW_SVC_LEASE_RETRY_MS;
                changes++;
            } else {
                e->game_restart_pending = false;
                e->state = jw__entry_idle_state(e);
                jw__persist(sup, e, "game-resume-failed");
                changes++;
            }
        }

        /* A service stopped for suspend/storage resumes exactly once. The
         * pending bit survives an unverified stop; pgid/state gates prevent a
         * replacement until the old reservation is proven absent. */
        if (!started_this_tick && e->lifecycle_restart_pending &&
            !(sup->storage_restart_blocked &&
              e->pending_stop_reason == JW_SVC_STOP_LIFECYCLE_STORAGE) &&
            e->pgid <= 0 &&
            e->state != JW_SVC_STATE_STALE_GENERATION &&
            e->state != JW_SVC_STATE_STOPPING &&
            jw__entry_available(e)) {
            char reason[JW_SVC_REASON_BUF];
            if (jw__start_generation(sup, e, reason, sizeof(reason))) {
                e->lifecycle_restart_pending = false;
                jw__persist(sup, e, "lifecycle-resume");
                started_this_tick = true;
                changes++;
            } else if (strcmp(reason, "stale-generation") == 0) {
                e->state = JW_SVC_STATE_STALE_GENERATION;
                e->lease_retry_next_ms = now + JW_SVC_LEASE_RETRY_MS;
                changes++;
            } else {
                e->lifecycle_restart_pending = false;
                e->state = jw__entry_idle_state(e);
                jw__persist(sup, e, "lifecycle-resume-failed");
                changes++;
            }
        }

        if (e->state == JW_SVC_STATE_BACKOFF && e->pgid <= 0 &&
            now >= e->backoff_retry_at_ms) {
            char reason[JW_SVC_REASON_BUF];
            if (jw__start_generation(sup, e, reason, sizeof(reason))) {
                e->control.restart_count++;
                jw__persist(sup, e, "backoff-retry");
                started_this_tick = true;
                changes++;
            } else if (strcmp(reason, "stale-generation") == 0) {
                e->state = JW_SVC_STATE_STALE_GENERATION;
                e->lease_retry_next_ms = now + JW_SVC_LEASE_RETRY_MS;
                changes++;
            } else {
                /* A failed retry is another failure for backoff purposes. */
                jw_svc_backoff_decision d =
                    jw_svc_backoff_record_failure(&e->backoff,
                                                  jw__wall_ms());
                if (d.breaker_open) {
                    e->state = JW_SVC_STATE_FAILED;
                } else {
                    e->backoff_retry_at_ms = now + d.delay_ms;
                }
                jw__persist(sup, e, "failure");
                changes++;
            }
        }

        /* 4. Stale generation: an old daemon generation still holds the
         *    lease. Retry the non-blocking acquisition once per second
         *    while desired enablement remains true. */
        if (!started_this_tick &&
            e->state == JW_SVC_STATE_STALE_GENERATION && e->pgid <= 0 &&
            (e->desired_enabled || e->session_run ||
             e->lifecycle_restart_pending || e->game_restart_pending) &&
            now >= e->lease_retry_next_ms) {
            char reason[JW_SVC_REASON_BUF];
            if (jw__start_generation(sup, e, reason, sizeof(reason))) {
                e->lifecycle_restart_pending = false;
                jw__persist(sup, e, "lease-acquired");
                started_this_tick = true;
                changes++;
            } else if (strcmp(reason, "stale-generation") == 0) {
                e->lease_retry_next_ms = now + JW_SVC_LEASE_RETRY_MS;
            } else {
                e->session_run = false;
                e->control.session_run = false;
                e->autostart_pending = false;
                if (e->pgid <= 0) {
                    e->state = jw__entry_idle_state(e);
                }
                jw__persist(sup, e, "stale-retry-failed");
                changes++;
            }
        }

        /* A report-only stale generation with no current desired intent still
         * needs passive liveness refresh. Acquire-and-close proves the old
         * holder is gone without launching a replacement. */
        if (!started_this_tick &&
            e->state == JW_SVC_STATE_STALE_GENERATION && e->pgid <= 0 &&
            !e->desired_enabled && !e->session_run &&
            !e->lifecycle_restart_pending && !e->game_restart_pending &&
            now >= e->lease_retry_next_ms) {
            char lease_reason[JW_SVC_REASON_BUF];
            int lease_fd = jw_svc_lease_acquire(
                sup->runtime_dir, e->service_id, lease_reason,
                sizeof(lease_reason));
            if (lease_fd >= 0) {
                close(lease_fd);
                e->state = jw__entry_idle_state(e);
                jw__persist(sup, e, "stale-generation-cleared");
                changes++;
            } else {
                e->lease_retry_next_ms = now + JW_SVC_LEASE_RETRY_MS;
            }
        }

        /* 5. Autostart: desired enablement with no running, no backoff,
         *    and no stale lease starts exactly one new generation. */
        bool idle = e->pgid <= 0 &&
                    e->state != JW_SVC_STATE_BACKOFF &&
                    e->state != JW_SVC_STATE_FAILED &&
                    e->state != JW_SVC_STATE_STOPPING &&
                    e->state != JW_SVC_STATE_STALE_GENERATION &&
                    e->state != JW_SVC_STATE_STARTING;
        if (idle && !started_this_tick && e->autostart_pending &&
            e->manifest_valid && e->pak_present && !e->on_secondary_root) {
            char reason[JW_SVC_REASON_BUF];
            if (jw__start_generation(sup, e, reason, sizeof(reason))) {
                jw__persist(sup, e, "autostart");
                changes++;
            } else if (strcmp(reason, "stale-generation") == 0) {
                e->state = JW_SVC_STATE_STALE_GENERATION;
                e->lease_retry_next_ms = now + JW_SVC_LEASE_RETRY_MS;
                changes++;
            } else if (e->pgid <= 0) {
                /* Consume this startup attempt. Retrying a fork/exec or
                 * reservation failure on every daemon tick is a process-spawn
                 * loop, not an autostart policy. A user Run can explicitly
                 * try again. */
                e->autostart_pending = false;
                e->state = jw__entry_idle_state(e);
                jw__persist(sup, e, "autostart-failed");
                changes++;
            }
        }
    }
    return changes;
}

/* ------------------------------------------------------------------ */
/* package replacement                                                 */
/* ------------------------------------------------------------------ */

static bool jw__package_operation_id_valid(const char *operation_id) {
    if (!operation_id || !operation_id[0]) {
        return false;
    }
    size_t len = strlen(operation_id);
    if (len > JW_SVC_PACKAGE_OPERATION_ID_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)operation_id[i];
        bool alpha = (c >= (unsigned char)'A' && c <= (unsigned char)'Z') ||
                     (c >= (unsigned char)'a' && c <= (unsigned char)'z');
        bool digit = c >= (unsigned char)'0' && c <= (unsigned char)'9';
        if (!alpha && !digit && c != (unsigned char)'.' &&
            c != (unsigned char)'_' && c != (unsigned char)'-') {
            return false;
        }
    }
    return true;
}

bool jw_svc_supervisor_package_active(const jw_svc_supervisor *sup) {
    return sup && sup->package_quiesce_active;
}

bool jw_svc_supervisor_package_begin(jw_svc_supervisor *sup,
                                     const char *operation_id,
                                     char *out_stuck_id,
                                     size_t stuck_id_size,
                                     char *reason, size_t reason_size) {
    if (out_stuck_id && stuck_id_size > 0) {
        out_stuck_id[0] = '\0';
    }
    if (!sup || !jw__package_operation_id_valid(operation_id)) {
        jw__set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    if (sup->package_quiesce_active || sup->mutation_active) {
        jw__set_reason(reason, reason_size, "package-in-progress");
        return false;
    }
    if (sup->game_active) {
        jw__set_reason(reason, reason_size, "lifecycle-in-progress");
        return false;
    }
    if (sup->storage_restart_blocked) {
        jw__set_reason(reason, reason_size, "lifecycle-in-progress");
        return false;
    }

    /* Complete the preflight before delivering a signal. A known stale
     * generation or stop owned by another transition means no package byte
     * may change and no otherwise healthy service should be bounced. */
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        bool stale = e->state == JW_SVC_STATE_STALE_GENERATION;
        bool transition = e->lifecycle_restart_pending ||
                          e->post_stop != JW_SVC_POST_STOP_NONE ||
                          e->stop_requested || e->reap_pending ||
                          e->stop_unverified_logged ||
                          e->state == JW_SVC_STATE_STOPPING ||
                          (e->pgid > 0 &&
                           e->pending_stop_reason != JW_SVC_STOP_NONE);
        if (!stale && !transition) {
            continue;
        }
        if (out_stuck_id && stuck_id_size > 0) {
            snprintf(out_stuck_id, stuck_id_size, "%s", e->service_id);
        }
        jw__set_reason(reason, reason_size,
                       stale ? "stale-generation" :
                               "lifecycle-in-progress");
        return false;
    }

    sup->package_quiesce_active = true;
    snprintf(sup->package_operation_id, sizeof(sup->package_operation_id),
             "%s", operation_id);
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        e->package_blocked = true;
        e->package_snapshot_valid = true;
        e->package_restore_desired = e->desired_enabled;
        e->package_was_effective =
            e->pgid > 0 || e->state == JW_SVC_STATE_STARTING ||
            e->state == JW_SVC_STATE_RUNNING;
        e->autostart_pending = false;
        e->lifecycle_restart_pending = false;
        e->game_restart_pending = false;
        e->post_stop = JW_SVC_POST_STOP_NONE;
        e->session_run = false;
        e->control.session_run = false;
    }

    bool verified = true;
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        e->pending_stop_reason = JW_SVC_STOP_LIFECYCLE_PACKAGE;
        if (e->pgid > 0) {
            char stop_reason[JW_SVC_REASON_BUF];
            if (!jw__stop_and_reap(sup, e, stop_reason,
                                   sizeof(stop_reason))) {
                if (verified && out_stuck_id && stuck_id_size > 0) {
                    snprintf(out_stuck_id, stuck_id_size, "%s",
                             e->service_id);
                }
                verified = false;
                (void)jw__persist(sup, e, "package-unverified");
                continue;
            }
        }
        (void)jw__persist(sup, e, "package-quiesce");
    }
    if (!verified) {
        /* Fail closed. The caller did not receive permission to mutate and
         * the global latch prevents a replacement generation from racing the
         * survivor. A later matching end() releases only after tick proves
         * every group absent. */
        jw__set_reason(reason, reason_size, "stop-failed");
        return false;
    }
    return true;
}

bool jw_svc_supervisor_package_end(jw_svc_supervisor *sup,
                                   const char *operation_id,
                                   char *reason, size_t reason_size) {
    if (!sup || !jw__package_operation_id_valid(operation_id)) {
        jw__set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    if (!sup->package_quiesce_active) {
        jw__set_reason(reason, reason_size, "no-package-operation");
        return false;
    }
    if (strcmp(operation_id, sup->package_operation_id) != 0) {
        jw__set_reason(reason, reason_size, "operation-mismatch");
        return false;
    }
    for (int i = 0; i < sup->count; i++) {
        const jw_svc_supervised *e = &sup->entries[i];
        if (!e->package_blocked) {
            continue;
        }
        if (e->pgid > 0 || e->state == JW_SVC_STATE_STALE_GENERATION ||
            e->stop_requested || e->reap_pending) {
            jw__set_reason(reason, reason_size, "stop-failed");
            return false;
        }
    }

    /* Scan the replacement while the global start latch is still held. The
     * scan reconciles manifest ownership in-place, so snapshot fields on
     * pre-existing service ids survive. */
    if (jw_svc_supervisor_scan(sup) < 0) {
        jw__set_reason(reason, reason_size, "rescan-failed");
        return false;
    }

    sup->package_quiesce_active = false;
    sup->package_operation_id[0] = '\0';
    bool restored = true;
    char failure_reason[JW_SVC_REASON_BUF] = "restore-failed";
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        bool desired = e->package_snapshot_valid
                           ? e->package_restore_desired
                           : e->desired_enabled;
        e->desired_enabled = desired;
        e->control.start_with_leaf = desired;
        e->session_run = false;
        e->control.session_run = false;
        e->autostart_pending = false;
        e->lifecycle_restart_pending = false;
        e->package_blocked = false;
        e->package_snapshot_valid = false;
        e->package_restore_desired = false;
        e->package_was_effective = false;
        if (e->pending_stop_reason == JW_SVC_STOP_LIFECYCLE_PACKAGE) {
            e->pending_stop_reason = JW_SVC_STOP_NONE;
        }
        if (e->pgid <= 0 &&
            e->state != JW_SVC_STATE_STALE_GENERATION) {
            e->state = jw__entry_idle_state(e);
        }
        if (!jw__persist(sup, e, "package-rescan")) {
            restored = false;
            snprintf(failure_reason, sizeof(failure_reason), "%s",
                     "store-failed");
        }
        if (!desired) {
            continue;
        }
        if (!jw__entry_available(e)) {
            restored = false;
            snprintf(failure_reason, sizeof(failure_reason), "%s",
                     "restore-unavailable");
            (void)jw__persist(sup, e, "package-resume-unavailable");
            continue;
        }
        jw_svc_backoff_reset(&e->backoff);
        char start_reason[JW_SVC_REASON_BUF] = {0};
        bool started = false;
        for (int attempt = 0; attempt < JW_SVC_PACKAGE_START_ATTEMPTS;
             attempt++) {
            if (jw__start_generation(sup, e, start_reason,
                                     sizeof(start_reason))) {
                started = true;
                break;
            }
            if (strcmp(start_reason, "launch-failed") != 0 ||
                e->pgid > 0 ||
                attempt + 1 >= JW_SVC_PACKAGE_START_ATTEMPTS) {
                break;
            }
            usleep(JW_SVC_PACKAGE_START_RETRY_US);
        }
        if (!started) {
            restored = false;
            snprintf(failure_reason, sizeof(failure_reason), "%s",
                     start_reason[0] ? start_reason : "restore-failed");
            if (strcmp(start_reason, "stale-generation") == 0) {
                e->state = JW_SVC_STATE_STALE_GENERATION;
                e->lease_retry_next_ms =
                    jw__mono_ms() + JW_SVC_LEASE_RETRY_MS;
            }
            char transition[JW_SVC_REASON_BUF];
            if (jw__format(transition, sizeof(transition),
                           "package-resume-%s",
                           start_reason[0] ? start_reason : "failed") == 0) {
                (void)jw__persist(sup, e, transition);
            }
            continue;
        }
        (void)jw__persist(sup, e, "package-resume");
    }
    if (!restored) {
        jw__set_reason(reason, reason_size, failure_reason);
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Pak Rat target mutation                                             */
/* ------------------------------------------------------------------ */

static bool jw__mutation_target_valid(const char *target) {
    if (!target || !target[0] || strlen(target) >= 512u ||
        strncmp(target, "Apps/", 5) == 0 || target[0] == '/' ||
        strchr(target, '\\')) {
        return false;
    }
    const char *slash = strchr(target, '/');
    if (!slash || slash == target || !slash[1] || strchr(slash + 1, '/')) {
        return false;
    }
    return strcmp(slash + 1, ".") != 0 && strcmp(slash + 1, "..") != 0;
}

static bool jw__entry_target_matches(const jw_svc_supervisor *sup,
                                     const jw_svc_supervised *e,
                                     const char *target,
                                     const char *package_id) {
    if (!sup || !e || !target || !package_id ||
        strcmp(e->service_id, package_id) != 0) {
        return false;
    }
    if (!e->pak_root[0]) {
        /* A retained stale-generation row can outlive package discovery. The
         * stable package/service id is sufficient because SVC-1 makes it
         * globally unique and TXN-1 must not bypass its locked lease. */
        return e->state == JW_SVC_STATE_STALE_GENERATION || !e->pak_present;
    }
    const char *slash = strchr(target, '/');
    if (!slash) {
        return false;
    }
    size_t namespace_len = (size_t)(slash - target);
    const char *leaf = slash + 1;
    for (int group = 0; group < 2; group++) {
        char *const *roots = group == 0 ? sup->apps_scan_roots
                                        : sup->secondary_scan_roots;
        int count = group == 0 ? sup->apps_scan_root_count
                               : sup->secondary_scan_root_count;
        for (int i = 0; i < count; i++) {
            const char *root = roots[i];
            const char *base = strrchr(root, '/');
            base = base ? base + 1 : root;
            if (strlen(base) != namespace_len ||
                strncmp(base, target, namespace_len) != 0) {
                continue;
            }
            char expected[PATH_MAX];
            if (jw__format(expected, sizeof(expected), "%s/%s", root,
                           leaf) == 0 && strcmp(expected, e->pak_root) == 0) {
                return true;
            }
        }
    }
    return false;
}

bool jw_svc_supervisor_mutation_active(const jw_svc_supervisor *sup) {
    return sup && sup->mutation_active;
}

bool jw_svc_supervisor_mutation_target_blocked(
    const jw_svc_supervisor *sup, const char *apps_relative_path) {
    if (!sup || !sup->mutation_active || !apps_relative_path) {
        return false;
    }
    const char *path = strncmp(apps_relative_path, "Apps/", 5) == 0
                           ? apps_relative_path + 5
                           : apps_relative_path;
    return strcmp(path, sup->mutation_target) == 0;
}

bool jw_svc_supervisor_mutation_info(
    const jw_svc_supervisor *sup,
    char *operation_id, size_t operation_id_size,
    char *target_path, size_t target_path_size,
    char *package_id, size_t package_id_size) {
    if (!sup || !sup->mutation_active) {
        return false;
    }
    if (operation_id && operation_id_size > 0) {
        snprintf(operation_id, operation_id_size, "%s",
                 sup->mutation_operation_id);
    }
    if (target_path && target_path_size > 0) {
        snprintf(target_path, target_path_size, "%s", sup->mutation_target);
    }
    if (package_id && package_id_size > 0) {
        snprintf(package_id, package_id_size, "%s",
                 sup->mutation_package_id);
    }
    return true;
}

bool jw_svc_supervisor_mutation_begin(
    jw_svc_supervisor *sup, const char *operation_id,
    const char *target_path, const char *package_id,
    char *out_stuck_id, size_t stuck_id_size,
    char *reason, size_t reason_size) {
    if (out_stuck_id && stuck_id_size > 0) {
        out_stuck_id[0] = '\0';
    }
    if (!sup || !jw__package_operation_id_valid(operation_id) ||
        !jw__mutation_target_valid(target_path) ||
        !jw_service_id_is_reverse_dns(package_id)) {
        jw__set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    if (sup->package_quiesce_active || sup->mutation_active) {
        jw__set_reason(reason, reason_size, "package-in-progress");
        return false;
    }
    if (sup->game_active || sup->storage_restart_blocked) {
        jw__set_reason(reason, reason_size, "lifecycle-in-progress");
        return false;
    }

    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        if (!jw__entry_target_matches(sup, e, target_path, package_id)) {
            continue;
        }
        bool stale = e->state == JW_SVC_STATE_STALE_GENERATION;
        bool transition = e->lifecycle_restart_pending ||
                          e->post_stop != JW_SVC_POST_STOP_NONE ||
                          e->stop_requested || e->reap_pending ||
                          e->stop_unverified_logged ||
                          e->state == JW_SVC_STATE_STOPPING ||
                          (e->pgid > 0 &&
                           e->pending_stop_reason != JW_SVC_STOP_NONE);
        if (!stale && !transition) {
            continue;
        }
        if (out_stuck_id && stuck_id_size > 0) {
            snprintf(out_stuck_id, stuck_id_size, "%s", e->service_id);
        }
        jw__set_reason(reason, reason_size,
                       stale ? "stale-generation" : "lifecycle-in-progress");
        return false;
    }

    sup->mutation_active = true;
    snprintf(sup->mutation_operation_id,
             sizeof(sup->mutation_operation_id), "%s", operation_id);
    snprintf(sup->mutation_target, sizeof(sup->mutation_target), "%s",
             target_path);
    snprintf(sup->mutation_package_id,
             sizeof(sup->mutation_package_id), "%s", package_id);
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        if (!jw__entry_target_matches(sup, e, target_path, package_id)) {
            continue;
        }
        e->package_blocked = true;
        e->package_snapshot_valid = true;
        e->package_restore_desired = e->desired_enabled;
        e->package_was_effective =
            e->pgid > 0 || e->state == JW_SVC_STATE_STARTING ||
            e->state == JW_SVC_STATE_RUNNING;
        e->autostart_pending = false;
        e->lifecycle_restart_pending = false;
        e->game_restart_pending = false;
        e->post_stop = JW_SVC_POST_STOP_NONE;
        e->session_run = false;
        e->control.session_run = false;
        e->pending_stop_reason = JW_SVC_STOP_LIFECYCLE_PACKAGE;
        if (e->pgid > 0) {
            char stop_reason[JW_SVC_REASON_BUF];
            if (!jw__stop_and_reap(sup, e, stop_reason,
                                   sizeof(stop_reason))) {
                if (out_stuck_id && stuck_id_size > 0) {
                    snprintf(out_stuck_id, stuck_id_size, "%s",
                             e->service_id);
                }
                (void)jw__persist(sup, e, "mutation-unverified");
                jw__set_reason(reason, reason_size, "stop-failed");
                return false;
            }
        }
        if (!jw__persist(sup, e, "mutation-quiesce")) {
            jw__set_reason(reason, reason_size, "store-failed");
            return false;
        }
    }
    return true;
}

bool jw_svc_supervisor_mutation_end(jw_svc_supervisor *sup,
                                    const char *operation_id,
                                    bool installed_has_service,
                                    char *reason, size_t reason_size) {
    if (!sup || !jw__package_operation_id_valid(operation_id)) {
        jw__set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    if (!sup->mutation_active) {
        jw__set_reason(reason, reason_size, "no-package-operation");
        return false;
    }
    if (strcmp(operation_id, sup->mutation_operation_id) != 0) {
        jw__set_reason(reason, reason_size, "operation-mismatch");
        return false;
    }
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        if (e->package_blocked &&
            (e->pgid > 0 || e->state == JW_SVC_STATE_STALE_GENERATION ||
             e->stop_requested || e->reap_pending)) {
            jw__set_reason(reason, reason_size, "stop-failed");
            return false;
        }
    }
    if (jw_svc_supervisor_scan(sup) < 0) {
        jw__set_reason(reason, reason_size, "rescan-failed");
        return false;
    }

    bool ok = true;
    bool found_installed_target = false;
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        bool target_entry = jw__entry_target_matches(
            sup, e, sup->mutation_target, sup->mutation_package_id);
        if (target_entry) {
            found_installed_target = true;
        }
        if (!e->package_blocked && !e->package_snapshot_valid &&
            !target_entry) {
            continue;
        }
        e->package_blocked = true;
        bool desired = installed_has_service &&
                       e->package_snapshot_valid &&
                       e->package_restore_desired;
        bool restart = installed_has_service &&
                       e->package_snapshot_valid &&
                       e->package_was_effective;
        e->desired_enabled = desired;
        e->session_run = restart && !desired;
        e->control.start_with_leaf = desired;
        e->control.session_run = e->session_run;
        e->autostart_pending = false;
        e->lifecycle_restart_pending = false;
        e->game_restart_pending = false;
        if (e->pending_stop_reason == JW_SVC_STOP_LIFECYCLE_PACKAGE) {
            e->pending_stop_reason = JW_SVC_STOP_NONE;
        }
        e->state = jw__entry_idle_state(e);
        if (!installed_has_service) {
            memset(&e->control, 0, sizeof(e->control));
            char store_reason[JW_SVC_REASON_BUF];
            if (!jw_svc_control_store_delete(sup->store, e->service_id,
                                             store_reason,
                                             sizeof(store_reason))) {
                ok = false;
            } else {
                e->control_loaded = false;
            }
            continue;
        }
        if (!target_entry || !jw__entry_available(e)) {
            ok = false;
            continue;
        }
        if (!jw__persist(sup, e, "mutation-rescan")) {
            ok = false;
            continue;
        }
        if (restart) {
            e->package_blocked = false;
            char start_reason[JW_SVC_REASON_BUF];
            if (!jw__start_generation(sup, e, start_reason,
                                      sizeof(start_reason)) ||
                !jw__persist(sup, e, "mutation-resume")) {
                if (e->pgid > 0) {
                    e->pending_stop_reason = JW_SVC_STOP_LIFECYCLE_PACKAGE;
                    char stop_reason[JW_SVC_REASON_BUF];
                    (void)jw__stop_and_reap(sup, e, stop_reason,
                                            sizeof(stop_reason));
                }
                e->package_blocked = true;
                ok = false;
            }
        }
    }
    if (installed_has_service && !found_installed_target) {
        ok = false;
    }
    if (!ok) {
        jw__set_reason(reason, reason_size, "restore-failed");
        return false;
    }
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        bool target_entry = jw__entry_target_matches(
            sup, e, sup->mutation_target, sup->mutation_package_id);
        if (!e->package_blocked && !e->package_snapshot_valid &&
            !target_entry) {
            continue;
        }
        e->package_blocked = false;
        e->package_snapshot_valid = false;
        e->package_restore_desired = false;
        e->package_was_effective = false;
    }
    sup->mutation_active = false;
    sup->mutation_operation_id[0] = '\0';
    sup->mutation_target[0] = '\0';
    sup->mutation_package_id[0] = '\0';
    return true;
}

/* ------------------------------------------------------------------ */
/* shutdown / lifecycle                                                */
/* ------------------------------------------------------------------ */

int jw_svc_supervisor_stop_all(jw_svc_supervisor *sup) {
    if (!sup) {
        return 0;
    }
    int unverified = 0;
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        if (e->state == JW_SVC_STATE_STALE_GENERATION) {
            /* The old generation is deliberately not signallable, but its
             * locked lease is positive evidence that shutdown cannot verify
             * it stopped. Shutdown continues and reports the warning. */
            e->pending_stop_reason = JW_SVC_STOP_INTENTIONAL;
            jw__persist(sup, e, "shutdown-unverified-stale");
            unverified++;
            continue;
        }
        if (e->pgid <= 0) {
            continue;
        }
        e->pending_stop_reason = JW_SVC_STOP_INTENTIONAL;
        e->lifecycle_restart_pending = false;
        /* Discard any in-flight CTL-1 Restart. Without this the post-stop
         * action would fire from inside jw__stop_and_reap() and launch a
         * fresh generation while the daemon is shutting down -- a service
         * started with nothing left to supervise it. */
        e->post_stop = JW_SVC_POST_STOP_NONE;
        char reason[JW_SVC_REASON_BUF];
        if (!jw__stop_and_reap(sup, e, reason, sizeof(reason))) {
            /* SVC-1: shutdown continues with a warning; the stuck service
             * is recorded, not allowed to wedge the platform transition. */
            unverified++;
            continue;
        }
        e->session_run = false;
        e->control.session_run = false;
        jw__persist(sup, e, "shutdown");
    }
    return unverified;
}

int jw_svc_supervisor_describe_unverified(const jw_svc_supervisor *sup,
                                          char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    if (!sup) {
        return 0;
    }
    size_t used = 0;
    int lines = 0;
    for (int i = 0; i < sup->count; i++) {
        const jw_svc_supervised *e = &sup->entries[i];
        bool stale = e->state == JW_SVC_STATE_STALE_GENERATION;
        bool stuck = e->state == JW_SVC_STATE_STOPPING && e->pgid > 0;
        if (!stale && !stuck) {
            continue;
        }
        int n = snprintf(out + used, out_size - used,
                         "service=%s pgid=%d lease=%s/services/%s/generation.lease\n",
                         e->service_id, stale ? 0 : (int)e->pgid,
                         sup->runtime_dir, e->service_id);
        if (n < 0 || (size_t)n >= out_size - used) {
            out[used] = '\0';
            continue;
        }
        used += (size_t)n;
        lines++;
    }
    return lines;
}

int jw_svc_supervisor_game_launch_begin(jw_svc_supervisor *sup,
                                        char *out_stuck_id,
                                        size_t stuck_id_size) {
    if (out_stuck_id && stuck_id_size > 0) {
        out_stuck_id[0] = '\0';
    }
    if (!sup) {
        return 0;
    }
    sup->game_active = true;
    int stopped = 0;
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        if (e->state == JW_SVC_STATE_STALE_GENERATION &&
            e->active_lifecycle_game != JW_SVC_LIFECYCLE_GAME_IGNORE) {
            e->lifecycle_restart_pending = false;
            if (out_stuck_id && stuck_id_size > 0 &&
                out_stuck_id[0] == '\0') {
                snprintf(out_stuck_id, stuck_id_size, "%s", e->service_id);
            }
            continue;
        }
        if (e->pgid <= 0) {
            continue;
        }
        if (e->active_lifecycle_game != JW_SVC_LIFECYCLE_GAME_STOP) {
            continue; /* live notify is handled by the daemon exchange */
        }
        e->lifecycle_restart_pending = false;
        e->game_restart_pending = true;
        /* A game-launch stop supersedes a pending CTL-1 Restart: the whole
         * point is that nothing runs over the game. */
        e->post_stop = JW_SVC_POST_STOP_NONE;
        e->pending_stop_reason = JW_SVC_STOP_LIFECYCLE_GAME;
        e->autostart_pending = false;
        char reason[JW_SVC_REASON_BUF];
        if (!jw__stop_and_reap(sup, e, reason, sizeof(reason))) {
            if (out_stuck_id && stuck_id_size > 0 &&
                out_stuck_id[0] == '\0') {
                snprintf(out_stuck_id, stuck_id_size, "%s", e->service_id);
            }
            continue;
        }
        jw__persist(sup, e, "game-launch");
        stopped++;
    }
    return stopped;
}

void jw_svc_supervisor_game_set_active(jw_svc_supervisor *sup, bool active) {
    if (sup) {
        sup->game_active = active;
    }
}

bool jw_svc_supervisor_game_active(const jw_svc_supervisor *sup) {
    return sup && sup->game_active;
}

bool jw_svc_supervisor_game_has_participant(const jw_svc_supervisor *sup) {
    if (!sup) {
        return false;
    }
    for (int i = 0; i < sup->count; i++) {
        const jw_svc_supervised *e = &sup->entries[i];
        if (e->game_restart_pending ||
            (e->pgid > 0 && e->active_lifecycle_game !=
                                JW_SVC_LIFECYCLE_GAME_IGNORE)) {
            return true;
        }
    }
    return false;
}

bool jw_svc_supervisor_game_stop_service(jw_svc_supervisor *sup,
                                         const char *service_id,
                                         const jw_svc_subscriber_binding *coordinator,
                                         jw_svc_stop_result *stop_result,
                                         char *reason, size_t reason_size) {
    if (stop_result) {
        *stop_result = (jw_svc_stop_result){0};
    }
    if (!sup || !service_id || !service_id[0]) {
        jw__set_reason(reason, reason_size, "invalid-arguments");
        return false;
    }
    jw_svc_supervised *e = jw__find_mut(sup, service_id);
    if (!e) {
        jw__set_reason(reason, reason_size, "unknown-service");
        return false;
    }
    sup->game_active = true;
    if (e->state == JW_SVC_STATE_STALE_GENERATION) {
        jw__set_reason(reason, reason_size, "stale-generation");
        return false;
    }
    if (e->pgid <= 0) {
        jw__set_reason(reason, reason_size, "ok");
        return true;
    }
    if (e->stop_requested || e->reap_pending ||
        e->state == JW_SVC_STATE_STOPPING ||
        e->pending_stop_reason != JW_SVC_STOP_NONE) {
        jw__set_reason(reason, reason_size, "lifecycle-in-progress");
        return false;
    }
    e->lifecycle_restart_pending = false;
    e->game_restart_pending = true;
    e->post_stop = JW_SVC_POST_STOP_NONE;
    e->pending_stop_reason = JW_SVC_STOP_LIFECYCLE_GAME;
    e->autostart_pending = false;
    pid_t coordinator_pid = 0;
    if (coordinator && coordinator->peer_pid == e->pgid &&
        jw_svc_supervisor_revalidate_subscriber(
            sup, service_id, coordinator)) {
        coordinator_pid = coordinator->peer_pid;
    }
    if (!jw__stop_and_reap_coordinated(
            sup, e, coordinator_pid, stop_result, reason, reason_size)) {
        (void)jw__persist(sup, e, "game-unverified");
        return false;
    }
    (void)jw__persist(sup, e, "game-launch");
    jw__set_reason(reason, reason_size, "ok");
    return true;
}

void jw_svc_supervisor_game_finish(jw_svc_supervisor *sup) {
    if (sup) {
        sup->game_active = false;
    }
}

typedef enum {
    JW__SVC_LIFECYCLE_SUSPEND = 0,
    JW__SVC_LIFECYCLE_STORAGE_CHANGE,
} jw__svc_lifecycle_trigger;

static int jw__lifecycle_stop_begin(jw_svc_supervisor *sup,
                                    jw__svc_lifecycle_trigger trigger,
                                    char *out_stuck_id,
                                    size_t stuck_id_size) {
    if (out_stuck_id && stuck_id_size > 0) {
        out_stuck_id[0] = '\0';
    }
    if (!sup) {
        return 0;
    }
    const char *transition =
        trigger == JW__SVC_LIFECYCLE_SUSPEND ? "suspend" : "storage-change";
    int stopped = 0;
    for (int i = 0; i < sup->count; i++) {
        jw_svc_supervised *e = &sup->entries[i];
        bool enabled = trigger == JW__SVC_LIFECYCLE_SUSPEND
                           ? e->active_stop_on_suspend
                           : e->active_stop_on_storage_change;
        if (!enabled) {
            continue;
        }
        if (e->state == JW_SVC_STATE_STALE_GENERATION) {
            if (out_stuck_id && stuck_id_size > 0 &&
                out_stuck_id[0] == '\0') {
                snprintf(out_stuck_id, stuck_id_size, "%s", e->service_id);
            }
            jw__persist(sup, e,
                        trigger == JW__SVC_LIFECYCLE_SUSPEND
                            ? "suspend-unverified-stale"
                            : "storage-change-unverified-stale");
            continue;
        }
        if (e->pgid <= 0) {
            continue;
        }
        /* A user request, shutdown, game stop, or crash-descendant cleanup
         * already owns this generation's stop. Do not replace its reason or
         * turn it into a lifecycle resume that could undo that intent. */
        if (e->pending_stop_reason != JW_SVC_STOP_NONE) {
            /* Preserve the owner and reason of the existing stop, but never
             * mistake "already stopping" for "verified absent". Suspend may
             * continue with this warning; safe-unmount must refuse. */
            if (out_stuck_id && stuck_id_size > 0 &&
                out_stuck_id[0] == '\0') {
                snprintf(out_stuck_id, stuck_id_size, "%s", e->service_id);
            }
            continue;
        }
        /* Recorded as the specific lifecycle-policy stop, not as a generic
         * intentional stop: CTL-1 `status` has to be able to tell a user that
         * their service went down for suspend/storage rather than because
         * somebody pressed Stop. Both are equally excluded from on-failure. */
        e->pending_stop_reason = trigger == JW__SVC_LIFECYCLE_SUSPEND
                                     ? JW_SVC_STOP_LIFECYCLE_SUSPEND
                                     : JW_SVC_STOP_LIFECYCLE_STORAGE;
        e->autostart_pending = false;
        /* lifecycle_restart_pending is the resume mechanism for this stop, so
         * a pending CTL-1 Restart is folded into it rather than firing twice
         * (which would race two generations at the same reservation). */
        e->post_stop = JW_SVC_POST_STOP_NONE;
        e->lifecycle_restart_pending = true;
        char reason[JW_SVC_REASON_BUF];
        if (!jw__stop_and_reap(sup, e, reason, sizeof(reason))) {
            if (out_stuck_id && stuck_id_size > 0 &&
                out_stuck_id[0] == '\0') {
                snprintf(out_stuck_id, stuck_id_size, "%s", e->service_id);
            }
            jw__persist(sup, e,
                        trigger == JW__SVC_LIFECYCLE_SUSPEND
                            ? "suspend-unverified"
                            : "storage-change-unverified");
            continue;
        }
        jw__persist(sup, e, transition);
        stopped++;
    }
    return stopped;
}

int jw_svc_supervisor_suspend_begin(jw_svc_supervisor *sup,
                                    char *out_stuck_id,
                                    size_t stuck_id_size) {
    return jw__lifecycle_stop_begin(sup, JW__SVC_LIFECYCLE_SUSPEND,
                                    out_stuck_id, stuck_id_size);
}

int jw_svc_supervisor_storage_change_begin(jw_svc_supervisor *sup,
                                           char *out_stuck_id,
                                           size_t stuck_id_size) {
    if (sup) {
        sup->storage_restart_blocked = true;
    }
    return jw__lifecycle_stop_begin(
        sup, JW__SVC_LIFECYCLE_STORAGE_CHANGE, out_stuck_id, stuck_id_size);
}

void jw_svc_supervisor_storage_change_resume(jw_svc_supervisor *sup) {
    if (sup) {
        sup->storage_restart_blocked = false;
    }
}

bool jw_svc_supervisor_storage_change_blocked(const jw_svc_supervisor *sup,
                                              char *out_stuck_id,
                                              size_t stuck_id_size) {
    if (out_stuck_id && stuck_id_size > 0) {
        out_stuck_id[0] = '\0';
    }
    if (!sup) {
        return false;
    }
    for (int i = 0; i < sup->count; i++) {
        const jw_svc_supervised *e = &sup->entries[i];
        if (!e->active_stop_on_storage_change) {
            continue;
        }
        /* A locked stale generation is never signallable by this daemon, so
         * its stop can never be verified -- known before any signal. */
        bool stale = e->state == JW_SVC_STATE_STALE_GENERATION;
        /* Any owned group whose stop is already in flight remains a live
         * writer until absence is positively verified. Starting another
         * lifecycle stop would overwrite its owner/restart action; allowing
         * the unmount would violate SVC-1. */
        bool in_flight = e->pgid > 0 &&
            (e->pending_stop_reason != JW_SVC_STOP_NONE ||
             e->stop_requested || e->reap_pending ||
             e->stop_unverified_logged ||
             e->state == JW_SVC_STATE_STOPPING);
        if (!stale && !in_flight) {
            continue;
        }
        if (out_stuck_id && stuck_id_size > 0) {
            snprintf(out_stuck_id, stuck_id_size, "%s", e->service_id);
        }
        return true;
    }
    return false;
}

bool jw_svc_storage_should_suppress_followup_tick(int verified_stopped,
                                                  bool unmount_succeeded) {
    return verified_stopped > 0 && unmount_succeeded;
}
