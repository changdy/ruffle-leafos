/* MLP1 qualification: run beside jawakad with its physical and virtual event
   paths as arguments. Only forked children enter a private mount namespace. */
#include "internal/platform/input_roster_mlp1.c"
#include <stdlib.h>
#include <sys/wait.h>

static void expect(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "input roster: %s\n", what); exit(1); }
}

int main(int argc, char **argv) {
    expect(argc == 3, "usage: input-roster-test PHYSICAL VIRTUAL");
    jw_input_proxy proxy = {0};
    jw_input_roster roster;
    char error[256];
    expect(jw_input_roster_build(&proxy, &roster, error, sizeof(error)) < 0 &&
           strstr(error, JW_INPUT_ROSTER_ERR_PROXY), "inactive fails closed");
    proxy.enabled = true;
    expect(jw_input_roster_build(&proxy, &roster, error, sizeof(error)) < 0 &&
           strstr(error, JW_INPUT_ROSTER_ERR_WATCH), "watch-only fails closed");
    snprintf(proxy.physical_event_path, sizeof(proxy.physical_event_path), "%s", argv[1]);
    snprintf(proxy.virtual_event_path, sizeof(proxy.virtual_event_path), "%s", argv[1]);
    expect(jw_input_roster_build(&proxy, &roster, error, sizeof(error)) < 0,
           "aliased physical and virtual paths fail closed");
    snprintf(proxy.virtual_event_path, sizeof(proxy.virtual_event_path), "/dev/input/no-such-device");
    expect(jw_input_roster_build(&proxy, &roster, error, sizeof(error)) < 0,
           "missing virtual fails closed");
    snprintf(proxy.virtual_event_path, sizeof(proxy.virtual_event_path), "/dev/null");
    expect(jw_input_roster_build(&proxy, &roster, error, sizeof(error)) < 0,
           "unreadable capabilities fail closed");
    snprintf(proxy.virtual_event_path, sizeof(proxy.virtual_event_path), "%s", argv[2]);
    expect(jw_input_roster_build(&proxy, &roster, error, sizeof(error)) == 0, error);
    expect(roster.count == roster.external_count + 1 &&
           roster.controllers[roster.count - 1].is_virtual, "virtual mandatory last");
    for (int i = 0; i < roster.count; i++)
        expect(roster.controllers[i].rdev != roster.physical_rdev, "physical excluded");
    jw_input_roster_log(&roster, "qualification");

    int physical = open(argv[1], O_RDONLY), virtual = open(argv[2], O_RDONLY);
    expect(physical >= 0 && virtual >= 0, "open Loong pair");
    char names[2][128] = {{0}};
    struct input_id ids[2];
    expect(ioctl(physical, EVIOCGNAME(sizeof(names[0])), names[0]) >= 0 &&
           ioctl(virtual, EVIOCGNAME(sizeof(names[1])), names[1]) >= 0 &&
           !strcmp(names[0], names[1]), "cloned name");
    expect(ioctl(physical, EVIOCGID, &ids[0]) == 0 &&
           ioctl(virtual, EVIOCGID, &ids[1]) == 0 &&
           !memcmp(&ids[0], &ids[1], sizeof(ids[0])), "cloned input ID");
    unsigned char bits[2][(KEY_MAX + 8) / 8];
    const int types[] = {EV_KEY, EV_ABS};
    for (int i = 0; i < 2; i++) {
        memset(bits, 0, sizeof(bits));
        expect(ioctl(physical, EVIOCGBIT(types[i], sizeof(bits[0])), bits[0]) >= 0 &&
               ioctl(virtual, EVIOCGBIT(types[i], sizeof(bits[1])), bits[1]) >= 0 &&
               !memcmp(bits[0], bits[1], sizeof(bits[0])), "cloned key/axis capabilities");
    }
    close(physical); close(virtual);
    char dir[PATH_MAX];
    expect(jw_input_namespace_prepare(&roster, getpid(), dir, sizeof(dir)) == 0,
           "prepare namespace");
    pid_t child = fork();
    expect(child >= 0, "fork namespace check");
    if (child == 0) {
        expect(jw_input_namespace_enter(dir, &roster, error, sizeof(error)) == 0, error);
        expect(access(argv[1], F_OK) < 0, "physical absent in child");
        for (int i = 0; i < roster.count; i++) {
            struct stat st;
            expect(stat(roster.controllers[i].path, &st) == 0 &&
                   S_ISCHR(st.st_mode) && st.st_rdev == roster.controllers[i].rdev,
                   "roster member remains its original device");
        }
        _exit(0);
    }
    int status;
    expect(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0, "private namespace check");
    jw_input_namespace_cleanup_dir(dir);
    puts("Input roster, clone and namespace checks passed");
    return 0;
}
