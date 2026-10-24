#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "internal/services/ownership.h"

#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void) {
    int ready[2];
    assert(pipe(ready) == 0);
    pid_t leader = fork();
    assert(leader >= 0);
    if (leader == 0) {
        close(ready[0]);
        if (setpgid(0, 0) != 0) {
            _exit(120);
        }
        pid_t writer = fork();
        if (writer < 0) {
            _exit(121);
        }
        if (writer == 0) {
            signal(SIGTERM, SIG_IGN);
            char byte = 'w';
            (void)write(ready[1], &byte, 1);
            for (;;) {
                pause();
            }
        }
        close(ready[1]);
        _exit(0); /* wrapper exits while its writer descendant remains */
    }
    close(ready[1]);
    if (setpgid(leader, leader) != 0) {
        assert(errno == EACCES || errno == EPERM || errno == ESRCH);
        assert(getpgid(leader) == leader);
    }
    char byte = 0;
    assert(read(ready[0], &byte, 1) == 1 && byte == 'w');
    close(ready[0]);

    siginfo_t info;
    memset(&info, 0, sizeof(info));
    for (int i = 0; i < 100 && info.si_pid != leader; i++) {
        assert(waitid(P_PID, (id_t)leader, &info,
                      WEXITED | WNOHANG | WNOWAIT) == 0);
        if (info.si_pid != leader) {
            usleep(10000);
        }
    }
    assert(info.si_pid == leader);
    /* The observed wrapper is deliberately still a zombie reservation, and
     * the live descendant means the launch is not writer-free. */
    assert(!jw_svc_group_absent(leader));

    assert(kill(-leader, SIGKILL) == 0 || errno == ESRCH);
    bool absent = false;
    for (int i = 0; i < 300 && !absent; i++) {
        absent = jw_svc_group_absent(leader);
        if (!absent) {
            usleep(10000);
        }
    }
    assert(absent);
    int status = 0;
    assert(waitpid(leader, &status, 0) == leader);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    puts("writer_group_test: ok");
    return 0;
}
