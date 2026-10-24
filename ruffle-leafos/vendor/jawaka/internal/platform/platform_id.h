#ifndef JW_PLATFORM_PLATFORM_ID_H
#define JW_PLATFORM_PLATFORM_ID_H

const char *jw_platform_compiled_id(void);

/* Device/system facts for the About screen. */
typedef struct {
    char os_version[48];   /* stock OS version string, e.g. "1.3.0.32" */
    char kernel[64];       /* uname release, e.g. "5.10.209" */
    char device[80];       /* raw device-tree model, e.g. "Rockchip RK3566 RK817 MANGMI LP4X Board" */
    char soc[48];          /* parsed SoC marketing name, e.g. "Rockchip RK3566"; "" if unknown */
    char pmic[24];         /* parsed power-management IC, e.g. "RK817"; "" if unknown */
    char board[24];        /* parsed board / ODM name, e.g. "MANGMI"; "" if unknown */
    char ram_type[16];     /* parsed memory technology, e.g. "LPDDR4X"; "" if unknown */
    char ip[40];           /* legacy alias for ipv4; "" if none */
    char ipv4[40];         /* primary non-loopback IPv4, prefers wlan; "" if none */
    char ipv6[72];         /* primary non-loopback IPv6; link-local includes %iface */
    long mem_total_kb;     /* -1 if unknown */
    long mem_avail_kb;     /* -1 if unknown */
    long sd_total_mb;      /* -1 if unknown */
    long sd_free_mb;       /* -1 if unknown */
    int  battery_percent;  /* -1 if unknown */
    int  charging;         /* 1 if charging, else 0 */
    int  cpu_temp_c;       /* -1 if unknown */
    long uptime_s;         /* -1 if unknown */
} jw_system_info;

/* Fill out with best-effort device facts. Unknown strings are left empty and
 * unknown numbers set to -1. fs_path is any path on the filesystem whose
 * free/total space should be reported (e.g. the SD card root or DB path). */
void jw_platform_system_info(const char *fs_path, jw_system_info *out);

#endif /* JW_PLATFORM_PLATFORM_ID_H */
