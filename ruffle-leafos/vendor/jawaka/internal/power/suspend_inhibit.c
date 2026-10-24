#include "internal/power/suspend_inhibit.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static bool jw__safe_text(const char *value, size_t max_len) {
    if (!value || !value[0] || strlen(value) >= max_len) return false;
    for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
        if (*p < 0x20 || *p == 0x7f) return false;
    }
    return true;
}

static bool jw__valid_token(const char *token) {
    if (!token || strlen(token) != JW_SUSPEND_INHIBIT_TOKEN_LEN) return false;
    for (size_t i = 0; i < JW_SUSPEND_INHIBIT_TOKEN_LEN; i++) {
        if (!isxdigit((unsigned char)token[i])) return false;
    }
    return true;
}

static unsigned long long jw__process_start(pid_t pid) {
#ifdef __linux__
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    char line[4096];
    if (!fgets(line, sizeof(line), fp)) {
        fclose(fp);
        return 0;
    }
    fclose(fp);
    char *close_paren = strrchr(line, ')');
    if (!close_paren || close_paren[1] != ' ') return 0;
    char *cursor = close_paren + 2; /* field 3: state */
    if (!cursor[0] || cursor[1] != ' ') return 0;
    cursor += 2;
    for (int field = 4; field <= 22; field++) {
        char *end = NULL;
        unsigned long long value = strtoull(cursor, &end, 10);
        if (end == cursor) return 0;
        if (field == 22) return value;
        while (*end == ' ') end++;
        cursor = end;
    }
#else
    (void)pid;
#endif
    return 0;
}

static void jw__token(jw_suspend_inhibitor *inhibitor, char out[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1]) {
    unsigned char bytes[JW_SUSPEND_INHIBIT_TOKEN_LEN / 2];
    bool random_ok = false;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        size_t done = 0;
        while (done < sizeof(bytes)) {
            ssize_t got = read(fd, bytes + done, sizeof(bytes) - done);
            if (got <= 0) break;
            done += (size_t)got;
        }
        close(fd);
        random_ok = done == sizeof(bytes);
    }
    if (!random_ok) {
        unsigned long long x = ++inhibitor->nonce ^
            (unsigned long long)time(NULL) ^ (unsigned long long)getpid();
        for (size_t i = 0; i < sizeof(bytes); i++) {
            x ^= x << 13;
            x ^= x >> 7;
            x ^= x << 17;
            bytes[i] = (unsigned char)x;
        }
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(bytes); i++) {
        out[i * 2] = hex[bytes[i] >> 4];
        out[i * 2 + 1] = hex[bytes[i] & 15];
    }
    out[JW_SUSPEND_INHIBIT_TOKEN_LEN] = '\0';
}

static void jw__remember_release(jw_suspend_inhibitor *inhibitor,
                                 const char *token, pid_t pid) {
    unsigned int slot = inhibitor->released_cursor++ % JW_SUSPEND_INHIBIT_MAX_LEASES;
    snprintf(inhibitor->released[slot].token,
             sizeof(inhibitor->released[slot].token), "%s", token);
    inhibitor->released[slot].pid = pid;
}

/* Both scopes block deep suspend; JW_SUSPEND_SCOPE_SCREEN additionally blocks
   the stage-1 backlight blank, for foreground work the user is watching rather
   than waiting on (video playback). Unknown scopes are rejected so a typo fails
   loudly at acquire instead of silently inhibiting nothing. */
static bool jw__known_scope(const char *scope) {
    if (!scope) return false;
    return strcmp(scope, JW_SUSPEND_SCOPE_SUSPEND) == 0 ||
           strcmp(scope, JW_SUSPEND_SCOPE_SCREEN) == 0;
}

void jw_suspend_inhibitor_init(jw_suspend_inhibitor *inhibitor) {
    if (inhibitor) memset(inhibitor, 0, sizeof(*inhibitor));
}

jw_suspend_lease_result jw_suspend_inhibitor_acquire(
    jw_suspend_inhibitor *inhibitor, pid_t pid, const char *scope,
    const char *reason, long long acquired_ms,
    char token[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1]) {
    if (!inhibitor || pid <= 0 || !jw__known_scope(scope) ||
        !jw__safe_text(reason, sizeof(inhibitor->leases[0].reason)) || !token) {
        return JW_SUSPEND_LEASE_INVALID;
    }
    for (int i = 0; i < JW_SUSPEND_INHIBIT_MAX_LEASES; i++) {
        jw_suspend_lease *lease = &inhibitor->leases[i];
        if (lease->active) continue;
        memset(lease, 0, sizeof(*lease));
        lease->active = true;
        lease->pid = pid;
        lease->process_start = jw__process_start(pid);
        lease->acquired_ms = acquired_ms;
        snprintf(lease->scope, sizeof(lease->scope), "%s", scope);
        snprintf(lease->reason, sizeof(lease->reason), "%s", reason);
        jw__token(inhibitor, lease->token);
        snprintf(token, JW_SUSPEND_INHIBIT_TOKEN_LEN + 1, "%s", lease->token);
        return JW_SUSPEND_LEASE_OK;
    }
    return JW_SUSPEND_LEASE_FULL;
}

jw_suspend_lease_result jw_suspend_inhibitor_release(
    jw_suspend_inhibitor *inhibitor, pid_t pid, const char *token,
    bool *released_active) {
    if (released_active) *released_active = false;
    if (!inhibitor || pid <= 0 || !jw__valid_token(token)) return JW_SUSPEND_LEASE_INVALID;
    for (int i = 0; i < JW_SUSPEND_INHIBIT_MAX_LEASES; i++) {
        jw_suspend_lease *lease = &inhibitor->leases[i];
        if (!lease->active || strcmp(lease->token, token) != 0) continue;
        if (lease->pid != pid) return JW_SUSPEND_LEASE_WRONG_OWNER;
        jw__remember_release(inhibitor, lease->token, lease->pid);
        memset(lease, 0, sizeof(*lease));
        if (released_active) *released_active = true;
        return JW_SUSPEND_LEASE_OK;
    }
    for (int i = 0; i < JW_SUSPEND_INHIBIT_MAX_LEASES; i++) {
        if (strcmp(inhibitor->released[i].token, token) != 0) continue;
        return inhibitor->released[i].pid == pid
            ? JW_SUSPEND_LEASE_OK : JW_SUSPEND_LEASE_WRONG_OWNER;
    }
    return JW_SUSPEND_LEASE_NOT_FOUND;
}

int jw_suspend_inhibitor_reap(jw_suspend_inhibitor *inhibitor) {
    if (!inhibitor) return 0;
    int reaped = 0;
    for (int i = 0; i < JW_SUSPEND_INHIBIT_MAX_LEASES; i++) {
        jw_suspend_lease *lease = &inhibitor->leases[i];
        if (!lease->active) continue;
        bool dead = kill(lease->pid, 0) != 0 && errno == ESRCH;
        if (!dead && lease->process_start != 0) {
            unsigned long long current = jw__process_start(lease->pid);
            dead = current != 0 && current != lease->process_start;
        }
        if (!dead) continue;
        jw__remember_release(inhibitor, lease->token, lease->pid);
        memset(lease, 0, sizeof(*lease));
        reaped++;
    }
    return reaped;
}

int jw_suspend_inhibitor_count(const jw_suspend_inhibitor *inhibitor) {
    if (!inhibitor) return 0;
    int count = 0;
    for (int i = 0; i < JW_SUSPEND_INHIBIT_MAX_LEASES; i++) {
        if (inhibitor->leases[i].active) count++;
    }
    return count;
}

int jw_suspend_inhibitor_count_scope(const jw_suspend_inhibitor *inhibitor,
                                     const char *scope) {
    if (!inhibitor || !scope) return 0;
    int count = 0;
    for (int i = 0; i < JW_SUSPEND_INHIBIT_MAX_LEASES; i++) {
        if (inhibitor->leases[i].active &&
            strcmp(inhibitor->leases[i].scope, scope) == 0) count++;
    }
    return count;
}

void jw_suspend_inhibitor_clear(jw_suspend_inhibitor *inhibitor) {
    if (!inhibitor) return;
    memset(inhibitor->leases, 0, sizeof(inhibitor->leases));
    memset(inhibitor->released, 0, sizeof(inhibitor->released));
    inhibitor->released_cursor = 0;
}

const char *jw_suspend_lease_result_name(jw_suspend_lease_result result) {
    switch (result) {
        case JW_SUSPEND_LEASE_OK: return "ok";
        case JW_SUSPEND_LEASE_INVALID: return "invalid lease request";
        case JW_SUSPEND_LEASE_FULL: return "suspend inhibitor limit reached";
        case JW_SUSPEND_LEASE_NOT_FOUND: return "unknown suspend inhibitor token";
        case JW_SUSPEND_LEASE_WRONG_OWNER: return "suspend inhibitor token belongs to another process";
        default: return "suspend inhibitor error";
    }
}

void jw_suspend_policy_init(jw_suspend_policy *policy) {
    if (policy) policy->pending = JW_SUSPEND_PENDING_NONE;
}

jw_suspend_decision jw_suspend_policy_auto_stage2(jw_suspend_policy *policy,
                                                   int active_leases) {
    if (!policy) return JW_SUSPEND_DECISION_NONE;
    if (active_leases > 0) {
        if (policy->pending == JW_SUSPEND_PENDING_NONE)
            policy->pending = JW_SUSPEND_PENDING_AUTO;
        return JW_SUSPEND_DECISION_SCREEN_OFF;
    }
    policy->pending = JW_SUSPEND_PENDING_NONE;
    return JW_SUSPEND_DECISION_DEEP_SLEEP;
}

jw_suspend_decision jw_suspend_policy_power_tap(jw_suspend_policy *policy,
                                                int active_leases) {
    if (!policy) return JW_SUSPEND_DECISION_NONE;
    if (active_leases > 0) {
        policy->pending = JW_SUSPEND_PENDING_EXPLICIT;
        return JW_SUSPEND_DECISION_SCREEN_OFF;
    }
    policy->pending = JW_SUSPEND_PENDING_NONE;
    return JW_SUSPEND_DECISION_DEEP_SLEEP;
}

jw_suspend_decision jw_suspend_policy_leases_changed(jw_suspend_policy *policy,
                                                     int active_leases) {
    if (!policy || active_leases > 0 || policy->pending == JW_SUSPEND_PENDING_NONE)
        return JW_SUSPEND_DECISION_NONE;
    policy->pending = JW_SUSPEND_PENDING_NONE;
    return JW_SUSPEND_DECISION_DEEP_SLEEP;
}

jw_suspend_pending jw_suspend_policy_cancel_for_activity(jw_suspend_policy *policy) {
    if (!policy) return JW_SUSPEND_PENDING_NONE;
    jw_suspend_pending cancelled = policy->pending;
    policy->pending = JW_SUSPEND_PENDING_NONE;
    return cancelled;
}

jw_suspend_decision jw_suspend_policy_long_press(jw_suspend_policy *policy) {
    if (policy) policy->pending = JW_SUSPEND_PENDING_NONE;
    return JW_SUSPEND_DECISION_POWEROFF;
}
