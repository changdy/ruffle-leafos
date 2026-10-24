#include "internal/platform/platform_id.h"

#include <stdio.h>
#include <string.h>

const char *jw_platform_compiled_id(void) {
    return "mac";
}

void jw_platform_system_info(const char *fs_path, jw_system_info *out) {
    (void)fs_path;
    if (!out) return;
    memset(out, 0, sizeof(*out));
    snprintf(out->os_version, sizeof(out->os_version), "%s", "mock");
    snprintf(out->kernel, sizeof(out->kernel), "%s", "desktop");
    snprintf(out->device, sizeof(out->device), "%s", "Mock device");
    out->mem_total_kb = -1;
    out->mem_avail_kb = -1;
    out->sd_total_mb = -1;
    out->sd_free_mb = -1;
    out->battery_percent = -1;
    out->charging = 0;
    out->cpu_temp_c = -1;
    out->uptime_s = -1;
}
