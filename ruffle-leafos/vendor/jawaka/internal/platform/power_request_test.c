#include "internal/platform/power_request.h"
#include <assert.h>
#include <limits.h>

int main(void) {
    unsetenv("UMRK_POWER_REQUEST_DIR");
    assert(!jw_power_request_available());
    assert(jw_power_request_publish("reboot") == -1);
    char root[] = "/tmp/jawaka-power-request-XXXXXX";
    assert(mkdtemp(root));
    assert(setenv("UMRK_POWER_REQUEST_DIR", root, 1) == 0);
    assert(!jw_power_request_available());
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/ready", root);
    FILE *f = fopen(path, "w"); assert(f); fputs("1\n", f); fclose(f);
    assert(jw_power_request_available());
    assert(jw_power_request_publish("$(reboot)") == -1);
    assert(jw_power_request_publish("reboot") == 0);
    snprintf(path, sizeof(path), "%s/request", root);
    f = fopen(path, "r"); assert(f);
    char action[32] = {0}; assert(fread(action, 1, sizeof(action), f) == 6); fclose(f);
    assert(strcmp(action, "reboot") == 0);
    snprintf(path, sizeof(path), "%s/complete", root);
    assert(access(path, F_OK) != 0); /* accepting is not completing */
    assert(jw_power_request_complete() == 0);
    assert(access(path, F_OK) == 0);
    /* Reasons are allowlisted; the incomplete record must start with reason=. */
    assert(jw_power_request_reason("low-battery") == 0);
    assert(jw_power_request_reason("$(reboot)") == -1);
    assert(jw_power_request_incomplete("service=x pgid=1\n") == -1);
    assert(jw_power_request_incomplete("reason=game-child\ngame pid=42 start=7\n") == 0);
    char other[PATH_MAX];
    snprintf(other, sizeof(other), "%s/reason", root);
    f = fopen(other, "r"); assert(f);
    char reason[32] = {0}; assert(fread(reason, 1, sizeof(reason), f) == 12); fclose(f);
    assert(strcmp(reason, "low-battery\n") == 0);
    unlink(other);
    snprintf(other, sizeof(other), "%s/incomplete", root);
    assert(access(other, F_OK) == 0);
    unlink(other);
    /* Another generation has no capability and cannot inherit an old request. */
    char next[] = "/tmp/jawaka-power-request-XXXXXX";
    assert(mkdtemp(next)); setenv("UMRK_POWER_REQUEST_DIR", next, 1);
    assert(jw_power_request_publish("poweroff") == -1);
    rmdir(next);
    setenv("UMRK_POWER_REQUEST_DIR", root, 1);
    chmod(root, 0777);
    assert(!jw_power_request_available());
    chmod(root, 0700);
    unlink(path);
    snprintf(path, sizeof(path), "%s/request", root); unlink(path);
    snprintf(path, sizeof(path), "%s/ready", root); unlink(path);
    rmdir(root);
    puts("power-request-test: ok");
    return 0;
}
