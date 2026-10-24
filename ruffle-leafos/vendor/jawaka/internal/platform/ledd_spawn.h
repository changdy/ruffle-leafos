#ifndef JW_LEDD_SPAWN_H
#define JW_LEDD_SPAWN_H

#include <signal.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif

/* jawaka-ledd has no job once its daemon is gone, but it runs from the
   launcher bundle on the card with the session log as stdout. A daemon that
   died without stopping it (SIGKILL, a crash) used to leave it reparented to
   init: the next daemon never knew about it, and at shutdown its open log kept
   the card from remounting read-only. */

/* Child side, between fork() and exec: make SIGTERM arrive when the parent
   dies, then confirm the parent is still `expected_parent`. A parent that died
   before the prctl() leaves the signal armed against the new parent, where it
   never fires, so that case fails instead of running unsupervised. Only
   async-signal-safe calls: the daemon is multithreaded. */
static inline int jw_ledd_arm_parent_death(pid_t expected_parent) {
#if defined(__linux__)
    /* An ignored or blocked SIGTERM survives exec and would swallow the death
       signal until ledd installs its own handler, or for good if blocked. */
    struct sigaction dfl;
    memset(&dfl, 0, sizeof(dfl));
    dfl.sa_handler = SIG_DFL;
    sigset_t term;
    if (sigemptyset(&dfl.sa_mask) != 0 || sigemptyset(&term) != 0 ||
        sigaddset(&term, SIGTERM) != 0 ||
        sigaction(SIGTERM, &dfl, NULL) != 0 ||
        sigprocmask(SIG_UNBLOCK, &term, NULL) != 0) {
        return -1;
    }
    /* SIGTERM, not SIGKILL: ledd's handler hands the LED ring back. */
    if (prctl(PR_SET_PDEATHSIG, (unsigned long)SIGTERM, 0L, 0L, 0L) != 0) {
        return -1;
    }
    if (getppid() != expected_parent) {
        return -1;
    }
#else
    (void)expected_parent;
#endif
    return 0;
}

/* Start ledd as a child that is sent SIGTERM when its parent goes away. Call
   it from the daemon's main thread: Linux sends the signal when the thread
   that forked exits, not the process. Returns the pid, or -1 with errno set. */
static inline pid_t jw_ledd_spawn(const char *path, char *const argv[]) {
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid != 0) {
        return pid;
    }
    if (jw_ledd_arm_parent_death(parent) != 0) {
        _exit(127);
    }
    execv(path, argv);
    _exit(127);
}

/* When ledd exits on its own, jawakad restarts it so a crash mid-session does
   not leave the ring dark. A helper that dies at once would turn that into a
   SIGCHLD-driven loop of forks, so the restart is decided here:
     - exit status 2 is ledd's answer to bad arguments or an effect it does
       not know (an older ledd beside a newer jawakad); a retry gets the same
       answer, so it is not restarted;
     - an exit within JW_LEDD_QUICK_EXIT_MS of the start is quick, and the
       third quick exit inside JW_LEDD_QUICK_WINDOW_MS stops the restarts;
     - a helper that ran longer comes back at once.
   jawakad clears the count whenever the lighting is set again. */
#define JW_LEDD_EXIT_USAGE        2
#define JW_LEDD_QUICK_EXIT_MS     10000LL
#define JW_LEDD_QUICK_WINDOW_MS   60000LL
#define JW_LEDD_QUICK_EXIT_LIMIT  3

typedef struct {
    int quick_exits;
    long long first_quick_ms;
} jw_ledd_restarts;

static inline void jw_ledd_restarts_reset(jw_ledd_restarts *r) {
    r->quick_exits = 0;
    r->first_quick_ms = 0;
}

static inline int jw_ledd_should_restart(jw_ledd_restarts *r, int usage_exit,
                                         long long ran_ms, long long now_ms) {
    if (usage_exit) return 0;
    if (ran_ms >= JW_LEDD_QUICK_EXIT_MS) return 1;
    if (r->quick_exits == 0 || now_ms - r->first_quick_ms > JW_LEDD_QUICK_WINDOW_MS) {
        r->quick_exits = 0;
        r->first_quick_ms = now_ms;
    }
    r->quick_exits++;
    return r->quick_exits < JW_LEDD_QUICK_EXIT_LIMIT;
}

#endif
