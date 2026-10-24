#ifndef JW_SUSPEND_INHIBIT_H
#define JW_SUSPEND_INHIBIT_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

#define JW_SUSPEND_INHIBIT_MAX_LEASES 32
#define JW_SUSPEND_INHIBIT_TOKEN_LEN 32

/* Lease scopes. Both defer deep suspend (stage 2); only the screen scope also
   defers the stage-1 backlight blank, for foreground work the user is watching
   rather than waiting on. */
#define JW_SUSPEND_SCOPE_SUSPEND "block-suspend"
#define JW_SUSPEND_SCOPE_SCREEN  "block-screen"

typedef struct {
    bool active;
    char token[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    pid_t pid;
    unsigned long long process_start;
    long long acquired_ms;
    char scope[32];
    char reason[64];
} jw_suspend_lease;

typedef struct {
    char token[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    pid_t pid;
} jw_suspend_release_tombstone;

typedef struct {
    jw_suspend_lease leases[JW_SUSPEND_INHIBIT_MAX_LEASES];
    jw_suspend_release_tombstone released[JW_SUSPEND_INHIBIT_MAX_LEASES];
    unsigned int released_cursor;
    unsigned long long nonce;
} jw_suspend_inhibitor;

typedef enum {
    JW_SUSPEND_LEASE_OK = 0,
    JW_SUSPEND_LEASE_INVALID,
    JW_SUSPEND_LEASE_FULL,
    JW_SUSPEND_LEASE_NOT_FOUND,
    JW_SUSPEND_LEASE_WRONG_OWNER
} jw_suspend_lease_result;

typedef enum {
    JW_SUSPEND_PENDING_NONE = 0,
    JW_SUSPEND_PENDING_AUTO,
    JW_SUSPEND_PENDING_EXPLICIT
} jw_suspend_pending;

typedef enum {
    JW_SUSPEND_DECISION_NONE = 0,
    JW_SUSPEND_DECISION_SCREEN_OFF,
    JW_SUSPEND_DECISION_DEEP_SLEEP,
    JW_SUSPEND_DECISION_POWEROFF
} jw_suspend_decision;

typedef struct {
    jw_suspend_pending pending;
} jw_suspend_policy;

void jw_suspend_inhibitor_init(jw_suspend_inhibitor *inhibitor);
jw_suspend_lease_result jw_suspend_inhibitor_acquire(
    jw_suspend_inhibitor *inhibitor, pid_t pid, const char *scope,
    const char *reason, long long acquired_ms,
    char token[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1]);
jw_suspend_lease_result jw_suspend_inhibitor_release(
    jw_suspend_inhibitor *inhibitor, pid_t pid, const char *token,
    bool *released_active);
int jw_suspend_inhibitor_reap(jw_suspend_inhibitor *inhibitor);
int jw_suspend_inhibitor_count(const jw_suspend_inhibitor *inhibitor);
/* Active leases holding exactly `scope`. Use JW_SUSPEND_SCOPE_SCREEN to decide
   whether the stage-1 backlight blank must be deferred. */
int jw_suspend_inhibitor_count_scope(const jw_suspend_inhibitor *inhibitor,
                                     const char *scope);
void jw_suspend_inhibitor_clear(jw_suspend_inhibitor *inhibitor);
const char *jw_suspend_lease_result_name(jw_suspend_lease_result result);

void jw_suspend_policy_init(jw_suspend_policy *policy);
jw_suspend_decision jw_suspend_policy_auto_stage2(jw_suspend_policy *policy,
                                                   int active_leases);
jw_suspend_decision jw_suspend_policy_power_tap(jw_suspend_policy *policy,
                                                int active_leases);
jw_suspend_decision jw_suspend_policy_leases_changed(jw_suspend_policy *policy,
                                                     int active_leases);
jw_suspend_pending jw_suspend_policy_cancel_for_activity(jw_suspend_policy *policy);
jw_suspend_decision jw_suspend_policy_long_press(jw_suspend_policy *policy);

#endif
