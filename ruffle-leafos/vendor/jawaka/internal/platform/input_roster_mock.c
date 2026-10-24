/* Desktop/mock backend: no roster, no namespaces. Desktop launches keep their
   direct-input behavior; only device builds fail closed on roster errors. */

#include "internal/platform/input_roster.h"

#include <stdio.h>

bool jw_input_roster_supported(void) {
    return false;
}

bool jw_input_device_is_gamepad(const char *path) {
    (void)path;
    return false;
}

int jw_input_roster_build(const jw_input_proxy *proxy, jw_input_roster *roster,
                          char *error, size_t error_size) {
    (void)proxy;
    (void)roster;
    if (error && error_size > 0) {
        snprintf(error, error_size, "%s: roster unsupported on this platform",
                 JW_INPUT_ROSTER_ERR_UNSUPPORTED);
    }
    return -1;
}

size_t jw_input_roster_sdl_devices(const jw_input_roster *roster,
                                   char *out, size_t out_size) {
    (void)roster;
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    return 0;
}

void jw_input_roster_log(const jw_input_roster *roster, const char *tag) {
    (void)roster;
    (void)tag;
}

int jw_input_namespace_prepare(const jw_input_roster *roster, pid_t child_pid,
                               char *dir_out, size_t dir_out_size) {
    (void)roster;
    (void)child_pid;
    if (dir_out && dir_out_size > 0) {
        dir_out[0] = '\0';
    }
    return -1;
}

int jw_input_namespace_enter(const char *dir, const jw_input_roster *roster,
                             char *error, size_t error_size) {
    (void)dir;
    (void)roster;
    (void)error;
    (void)error_size;
    return -1;
}

void jw_input_namespace_cleanup_dir(const char *dir) {
    (void)dir;
}

void jw_input_namespace_startup_sweep(void) {
}
