#include "internal/services/ownership.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool jw_svc_group_tracking_may_end(bool group_absent,
                                   bool supervisor_exiting) {
    return group_absent || supervisor_exiting;
}

#if defined(__linux__)

#include <dirent.h>

/* Parses one line of /proc/<pid>/stat. Field 2 (comm) is parenthesized and
 * may itself contain ')' or spaces, so -- like internal/power/
 * suspend_inhibit.c's jw__process_start() -- this locates the LAST ')' on
 * the line and treats everything after it as the remaining space-
 * delimited fields, per proc(5). Field 3 is state (a single character,
 * 'Z' for zombie), field 5 is pgrp, and field 20 is num_threads. The
 * thread count matters because /proc/<pid>/task may be unavailable after
 * a multithreaded process's leader has exited. */
static bool jw__proc_stat_group_state(const char *line, pid_t *out_pgid,
                                      bool *out_zombie, int *out_threads) {
    const char *close_paren = strrchr(line, ')');
    if (!close_paren || close_paren[1] != ' ') {
        return false;
    }
    const char *cursor = close_paren + 2; /* field 3: state */
    if (!cursor[0]) {
        return false;
    }
    *out_zombie = (cursor[0] == 'Z');
    if (cursor[1] != ' ') {
        return false;
    }
    cursor += 2; /* field 4: ppid */
    for (int field = 4; field <= 20; field++) {
        errno = 0;
        char *end = NULL;
        long long value = strtoll(cursor, &end, 10);
        if (end == cursor || errno == ERANGE || *end != ' ') {
            return false;
        }
        if (field == 5) {
            pid_t pgid = (pid_t)value;
            /* Kernel threads legitimately report pgrp 0. They cannot match
             * the positive supervised pgid, but rejecting the stat row as
             * malformed would make any such unrelated thread prevent every
             * absence proof on MLP1. */
            if (value < 0 || (long long)pgid != value) {
                return false;
            }
            *out_pgid = pgid;
        } else if (field == 20) {
            if (value < 0 || value > INT_MAX) {
                return false;
            }
            *out_threads = (int)value;
            return true;
        }
        while (*end == ' ') {
            end++;
        }
        cursor = end;
    }
    return false;
}

#if defined(JW_SVC_OWNERSHIP_TESTING)
bool jw_svc_proc_stat_group_state_for_test(const char *line, pid_t *out_pgid,
                                           bool *out_zombie,
                                           int *out_threads) {
    return jw__proc_stat_group_state(line, out_pgid, out_zombie, out_threads);
}
#endif

static bool jw__pid_dir_name(const char *s, pid_t *out_pid) {
    if (!s || !s[0]) {
        return false;
    }
    for (const char *p = s; *p; p++) {
        if (!isdigit((unsigned char)*p)) {
            return false;
        }
    }
    errno = 0;
    char *end = NULL;
    long value = strtol(s, &end, 10);
    pid_t pid = (pid_t)value;
    if (end == s || *end != '\0' || errno == ERANGE ||
        value <= 0 || (long)pid != value) {
        return false;
    }
    *out_pid = pid;
    return true;
}

typedef enum {
    JW__PROC_STAT_OK,
    JW__PROC_STAT_GONE,
    JW__PROC_STAT_UNKNOWN,
} jw__proc_stat_result;

static jw__proc_stat_result jw__read_proc_stat(const char *path,
                                               pid_t *out_pgid,
                                               bool *out_zombie,
                                               int *out_threads) {
    errno = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return errno == ENOENT || errno == ESRCH
            ? JW__PROC_STAT_GONE : JW__PROC_STAT_UNKNOWN;
    }

    /* comm is only TASK_COMM_LEN bytes, but it may contain a newline. Read
     * the complete pseudo-file rather than using fgets(), which would stop
     * inside the parenthesized field and make a live process unparseable. */
    char stat_text[4096];
    size_t used = fread(stat_text, 1, sizeof(stat_text) - 1, fp);
    bool read_failed = ferror(fp);
    bool truncated = false;
    if (!read_failed && used == sizeof(stat_text) - 1) {
        int extra = fgetc(fp);
        truncated = extra != EOF;
        read_failed = ferror(fp);
    }
    fclose(fp);

    if (read_failed || truncated || used == 0) {
        return JW__PROC_STAT_UNKNOWN;
    }
    stat_text[used] = '\0';
    return jw__proc_stat_group_state(stat_text, out_pgid, out_zombie,
                                     out_threads)
        ? JW__PROC_STAT_OK : JW__PROC_STAT_UNKNOWN;
}

bool jw_svc_process_is_live_group_member(pid_t pid, pid_t pgid) {
    if (pid <= 0 || pgid <= 0) {
        return false;
    }
    char stat_path[64];
    int n = snprintf(stat_path, sizeof(stat_path),
                     "/proc/%ld/stat", (long)pid);
    if (n < 0 || (size_t)n >= sizeof(stat_path)) {
        return false;
    }
    pid_t actual_pgid = 0;
    bool zombie = false;
    int threads = 0;
    return jw__read_proc_stat(stat_path, &actual_pgid, &zombie, &threads) ==
               JW__PROC_STAT_OK &&
           actual_pgid == pgid && !zombie && threads > 0;
}

bool jw_svc_group_absent(pid_t pgid) {
    if (pgid <= 0) {
        return false;
    }

    DIR *proc = opendir("/proc");
    if (!proc) {
        return false; /* cannot enumerate: never claim absence */
    }

    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(proc);
        if (!entry) {
            bool complete = errno == 0;
            closedir(proc);
            return complete;
        }

        pid_t process_pid = 0;
        if (!jw__pid_dir_name(entry->d_name, &process_pid)) {
            continue; /* not a pid directory (".", "..", "self", "net", ...) */
        }

        char stat_path[64];
        int n = snprintf(stat_path, sizeof(stat_path),
                         "/proc/%ld/stat", (long)process_pid);
        if (n < 0 || (size_t)n >= sizeof(stat_path)) {
            closedir(proc);
            return false;
        }
        pid_t member_pgid = 0;
        bool zombie = false;
        int member_threads = 0;
        jw__proc_stat_result stat_result =
            jw__read_proc_stat(stat_path, &member_pgid, &zombie,
                               &member_threads);
        if (stat_result == JW__PROC_STAT_GONE) {
            continue; /* exited between readdir() and open/read */
        }
        if (stat_result == JW__PROC_STAT_UNKNOWN) {
            closedir(proc);
            return false; /* unreadable/unparseable: absence is not proven */
        }
        if (member_pgid != pgid) {
            continue;
        }
        if (!zombie) {
            closedir(proc);
            return false;
        }

        /* /proc enumeration hides non-leader thread IDs, and /proc/PID/task
         * is documented as potentially unavailable once the leader exits.
         * A zombie leader with more than one thread therefore still has a
         * live worker and is not absent. Zero/one means no worker remains. */
        if (member_threads > 1) {
            closedir(proc);
            return false;
        }
    }
}

#elif defined(__APPLE__)

#include <libproc.h>
#include <sys/proc.h>
#include <sys/proc_info.h>

bool jw_svc_process_is_live_group_member(pid_t pid, pid_t pgid) {
    if (pid <= 0 || pgid <= 0) {
        return false;
    }
    struct proc_bsdinfo info;
    errno = 0;
    int r = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &info, sizeof(info));
    return r == (int)sizeof(info) && (pid_t)info.pbi_pgid == pgid &&
           info.pbi_status != SZOMB;
}

/* macOS has no /proc; libproc's process table enumeration is the closest
 * real equivalent (used for real dev-time testing here, not just as a
 * stub -- this function is exercised by fork()/setpgid() fixtures in
 * ownership_test.c on this platform too, not only on Linux/MLP1). */
bool jw_svc_group_absent(pid_t pgid) {
    if (pgid <= 0) {
        return false;
    }

    /* Ask the kernel for this group specifically. Besides avoiding unrelated
     * proc_pidinfo permission failures, this keeps the proof scoped to the
     * only entries that could affect the answer. proc_listpgrppids() returns
     * a PID COUNT (unlike proc_listpids(), whose return is bytes). */
    for (int attempt = 0; attempt < 4; attempt++) {
        errno = 0;
        int needed = proc_listpgrppids(pgid, NULL, 0);
        if (needed < 0 || (needed == 0 && errno != 0)) {
            return false;
        }
        if (needed == 0) {
            return true;
        }

        size_t capacity = (size_t)needed + 8 + (size_t)needed / 2;
        if (capacity > (size_t)INT_MAX / sizeof(pid_t)) {
            return false;
        }
        pid_t *pids = (pid_t *)malloc(capacity * sizeof(pid_t));
        if (!pids) {
            return false;
        }

        errno = 0;
        int got_count =
            proc_listpgrppids(pgid, pids, (int)(capacity * sizeof(pid_t)));
        if (got_count < 0 || (got_count == 0 && errno != 0)) {
            free(pids);
            return false;
        }
        if ((size_t)got_count >= capacity) {
            free(pids);
            continue;
        }

        for (int i = 0; i < got_count; i++) {
            struct proc_bsdinfo info;
            errno = 0;
            int r = proc_pidinfo(pids[i], PROC_PIDTBSDINFO, 0, &info, sizeof(info));
            if (r != (int)sizeof(info)) {
                if (r == 0 && errno == ESRCH) {
                    continue; /* exited after the group listing */
                }
                free(pids);
                return false; /* inaccessible/short result: absence not proven */
            }
            if ((pid_t)info.pbi_pgid == pgid && info.pbi_status != SZOMB) {
                free(pids);
                return false;
            }
        }
        free(pids);
        return true;
    }
    return false; /* process table kept growing past every retry: cannot prove absence */
}

#else

bool jw_svc_process_is_live_group_member(pid_t pid, pid_t pgid) {
    (void)pid;
    (void)pgid;
    return false;
}

bool jw_svc_group_absent(pid_t pgid) {
    (void)pgid;
    return false; /* unsupported platform: never claim absence */
}

#endif
