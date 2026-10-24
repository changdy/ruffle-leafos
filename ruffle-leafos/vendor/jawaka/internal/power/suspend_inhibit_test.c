#include "internal/power/suspend_inhibit.h"

#include <assert.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    jw_suspend_inhibitor inhibitor;
    jw_suspend_inhibitor_init(&inhibitor);
    char one[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    char two[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    assert(jw_suspend_inhibitor_acquire(&inhibitor, getpid(), "block-suspend",
                                        "download", 10, one) == JW_SUSPEND_LEASE_OK);
    assert(jw_suspend_inhibitor_acquire(&inhibitor, getpid(), "block-suspend",
                                        "archive", 20, two) == JW_SUSPEND_LEASE_OK);
    assert(strcmp(one, two) != 0 && jw_suspend_inhibitor_count(&inhibitor) == 2);

    bool released = false;
    assert(jw_suspend_inhibitor_release(&inhibitor, getpid() + 1, one, &released) ==
           JW_SUSPEND_LEASE_WRONG_OWNER);
    assert(jw_suspend_inhibitor_release(&inhibitor, getpid(), "bad", &released) ==
           JW_SUSPEND_LEASE_INVALID);
    assert(jw_suspend_inhibitor_release(&inhibitor, getpid(), one, &released) ==
           JW_SUSPEND_LEASE_OK && released);
    assert(jw_suspend_inhibitor_release(&inhibitor, getpid(), one, &released) ==
           JW_SUSPEND_LEASE_OK && !released); /* duplicate release is idempotent */

    pid_t child = fork();
    assert(child >= 0);
    if (child == 0) pause();
    char dead[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    assert(jw_suspend_inhibitor_acquire(&inhibitor, child, "block-suspend",
                                        "dead holder", 30, dead) == JW_SUSPEND_LEASE_OK);
    kill(child, SIGKILL);
    waitpid(child, NULL, 0);
    assert(jw_suspend_inhibitor_reap(&inhibitor) == 1);

    /* Scope handling. A screen lease must be countable on its own, because the
       stage-1 backlight blank keys off it while stage 2 keys off the total. */
    jw_suspend_inhibitor_clear(&inhibitor);
    char suspend_lease[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    char screen_lease[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    assert(jw_suspend_inhibitor_acquire(&inhibitor, getpid(), JW_SUSPEND_SCOPE_SUSPEND,
                                        "download", 40, suspend_lease) == JW_SUSPEND_LEASE_OK);
    /* A plain block-suspend lease must NOT keep the backlight on. */
    assert(jw_suspend_inhibitor_count(&inhibitor) == 1);
    assert(jw_suspend_inhibitor_count_scope(&inhibitor, JW_SUSPEND_SCOPE_SCREEN) == 0);

    assert(jw_suspend_inhibitor_acquire(&inhibitor, getpid(), JW_SUSPEND_SCOPE_SCREEN,
                                        "playback", 50, screen_lease) == JW_SUSPEND_LEASE_OK);
    assert(jw_suspend_inhibitor_count_scope(&inhibitor, JW_SUSPEND_SCOPE_SCREEN) == 1);
    /* A screen lease also defers deep suspend: it is in the total count. */
    assert(jw_suspend_inhibitor_count(&inhibitor) == 2);

    /* Releasing the screen lease re-arms blanking but keeps the suspend hold. */
    assert(jw_suspend_inhibitor_release(&inhibitor, getpid(), screen_lease, &released) ==
           JW_SUSPEND_LEASE_OK && released);
    assert(jw_suspend_inhibitor_count_scope(&inhibitor, JW_SUSPEND_SCOPE_SCREEN) == 0);
    assert(jw_suspend_inhibitor_count(&inhibitor) == 1);

    /* A crashed player must not pin the backlight on forever. */
    pid_t viewer = fork();
    assert(viewer >= 0);
    if (viewer == 0) pause();
    char viewer_lease[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    assert(jw_suspend_inhibitor_acquire(&inhibitor, viewer, JW_SUSPEND_SCOPE_SCREEN,
                                        "playback", 60, viewer_lease) == JW_SUSPEND_LEASE_OK);
    assert(jw_suspend_inhibitor_count_scope(&inhibitor, JW_SUSPEND_SCOPE_SCREEN) == 1);
    kill(viewer, SIGKILL);
    waitpid(viewer, NULL, 0);
    assert(jw_suspend_inhibitor_reap(&inhibitor) == 1);
    assert(jw_suspend_inhibitor_count_scope(&inhibitor, JW_SUSPEND_SCOPE_SCREEN) == 0);

    /* Unknown scopes still fail loudly rather than inhibiting nothing. */
    char bogus[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    assert(jw_suspend_inhibitor_acquire(&inhibitor, getpid(), "block-everything",
                                        "typo", 70, bogus) == JW_SUSPEND_LEASE_INVALID);
    assert(jw_suspend_inhibitor_count_scope(&inhibitor, NULL) == 0);
    jw_suspend_inhibitor_clear(&inhibitor);

    jw_suspend_policy policy;
    jw_suspend_policy_init(&policy);
    assert(jw_suspend_policy_auto_stage2(&policy, 1) == JW_SUSPEND_DECISION_SCREEN_OFF);
    assert(policy.pending == JW_SUSPEND_PENDING_AUTO);
    assert(jw_suspend_policy_cancel_for_activity(&policy) == JW_SUSPEND_PENDING_AUTO);
    assert(jw_suspend_policy_power_tap(&policy, 1) == JW_SUSPEND_DECISION_SCREEN_OFF);
    assert(policy.pending == JW_SUSPEND_PENDING_EXPLICIT);
    assert(jw_suspend_policy_cancel_for_activity(&policy) == JW_SUSPEND_PENDING_EXPLICIT);
    assert(jw_suspend_policy_power_tap(&policy, 1) == JW_SUSPEND_DECISION_SCREEN_OFF);
    assert(jw_suspend_policy_leases_changed(&policy, 0) == JW_SUSPEND_DECISION_DEEP_SLEEP);
    assert(jw_suspend_policy_long_press(&policy) == JW_SUSPEND_DECISION_POWEROFF);

    jw_suspend_inhibitor_clear(&inhibitor);
    assert(jw_suspend_inhibitor_count(&inhibitor) == 0);
    for (int i = 0; i < JW_SUSPEND_INHIBIT_MAX_LEASES; i++) {
        char token[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
        assert(jw_suspend_inhibitor_acquire(&inhibitor, getpid(), "block-suspend",
                                            "capacity", i, token) == JW_SUSPEND_LEASE_OK);
    }
    char overflow[JW_SUSPEND_INHIBIT_TOKEN_LEN + 1];
    assert(jw_suspend_inhibitor_acquire(&inhibitor, getpid(), "block-suspend",
                                        "overflow", 99, overflow) == JW_SUSPEND_LEASE_FULL);
    jw_suspend_inhibitor_clear(&inhibitor);
    return 0;
}
