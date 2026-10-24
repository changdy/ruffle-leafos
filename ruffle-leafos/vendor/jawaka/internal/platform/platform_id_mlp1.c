#include "internal/platform/platform_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/utsname.h>
#include <sys/statvfs.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>

const char *jw_platform_compiled_id(void) {
    return "mlp1";
}

/* Read a whole file into out, trimming trailing whitespace/newlines/nulls. */
static int jw__read_file_trim(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) return -1;
    out[0] = '\0';
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    size_t n = fread(out, 1, out_size - 1, f);
    fclose(f);
    out[n] = '\0';
    while (n > 0) {
        char c = out[n - 1];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t' || c == '\0') out[--n] = '\0';
        else break;
    }
    return 0;
}

static long jw__read_long_file(const char *path) {
    char buf[64];
    if (jw__read_file_trim(path, buf, sizeof(buf)) != 0) return -1;
    return strtol(buf, NULL, 10);
}

/* Stock LoongOS version lives in /loong/loong_version as JSON:
   {"verInner":...,"verShow":"1.3.0.32"}. Extract verShow without a JSON dep. */
static void jw__read_stock_version(char *out, size_t out_size) {
    if (out_size) out[0] = '\0';
    char buf[256];
    if (jw__read_file_trim("/loong/loong_version", buf, sizeof(buf)) != 0) return;
    char *p = strstr(buf, "\"verShow\"");
    if (!p) return;
    p = strchr(p + 9, '"');           /* opening quote of the value */
    if (!p) return;
    p++;
    char *end = strchr(p, '"');
    if (!end) return;
    size_t len = (size_t)(end - p);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, p, len);
    out[len] = '\0';
}

static void jw__read_meminfo(long *total_kb, long *avail_kb) {
    *total_kb = -1;
    *avail_kb = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        long v;
        if (sscanf(line, "MemTotal: %ld kB", &v) == 1) *total_kb = v;
        else if (sscanf(line, "MemAvailable: %ld kB", &v) == 1) *avail_kb = v;
    }
    fclose(f);
}

/* Find the first power-supply node exposing a capacity; read percent + charging. */
static void jw__read_battery(int *percent, int *charging) {
    *percent = -1;
    *charging = 0;
    DIR *d = opendir("/sys/class/power_supply");
    if (!d) return;
    struct dirent *e;
    char path[512];
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", e->d_name);
        long cap = jw__read_long_file(path);
        if (cap < 0) continue;
        *percent = (int)cap;
        char status[32];
        snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", e->d_name);
        if (jw__read_file_trim(path, status, sizeof(status)) == 0 &&
            strcmp(status, "Charging") == 0) {
            *charging = 1;
        }
        break;
    }
    closedir(d);
}

static void jw__maybe_take_addr(char *out, size_t out_size, int *best_score,
                                const char *addr, int score) {
    if (!out || out_size == 0 || !best_score || !addr || !addr[0]) return;
    if (score <= *best_score) return;
    snprintf(out, out_size, "%s", addr);
    *best_score = score;
}

static void jw__read_ips(char *ipv4, size_t ipv4_size, char *ipv6, size_t ipv6_size) {
    if (ipv4 && ipv4_size) ipv4[0] = '\0';
    if (ipv6 && ipv6_size) ipv6[0] = '\0';
    struct ifaddrs *ifaces = NULL;
    if (getifaddrs(&ifaces) != 0) return;
    int best_v4 = -1;
    int best_v6 = -1;
    for (struct ifaddrs *p = ifaces; p; p = p->ifa_next) {
        if (!p->ifa_addr || !p->ifa_name) continue;
        if ((p->ifa_flags & IFF_LOOPBACK) || strncmp(p->ifa_name, "lo", 2) == 0) continue;

        int is_wlan = (strncmp(p->ifa_name, "wlan", 4) == 0);
        if (p->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *addr = (struct sockaddr_in *)p->ifa_addr;
            if (addr->sin_addr.s_addr == htonl(INADDR_ANY) ||
                addr->sin_addr.s_addr == htonl(INADDR_LOOPBACK)) {
                continue;
            }
            char candidate[INET_ADDRSTRLEN];
            if (!inet_ntop(AF_INET, &addr->sin_addr, candidate, sizeof(candidate))) continue;
            jw__maybe_take_addr(ipv4, ipv4_size, &best_v4, candidate, 10 + is_wlan);
        } else if (p->ifa_addr->sa_family == AF_INET6) {
            struct sockaddr_in6 *addr = (struct sockaddr_in6 *)p->ifa_addr;
            if (IN6_IS_ADDR_UNSPECIFIED(&addr->sin6_addr) ||
                IN6_IS_ADDR_LOOPBACK(&addr->sin6_addr) ||
                IN6_IS_ADDR_MULTICAST(&addr->sin6_addr)) {
                continue;
            }
            int is_link_local = IN6_IS_ADDR_LINKLOCAL(&addr->sin6_addr);
            char text[INET6_ADDRSTRLEN];
            if (!inet_ntop(AF_INET6, &addr->sin6_addr, text, sizeof(text))) continue;
            char candidate[72];
            if (is_link_local)
                snprintf(candidate, sizeof(candidate), "%s%%%s", text, p->ifa_name);
            else
                snprintf(candidate, sizeof(candidate), "%s", text);
            jw__maybe_take_addr(ipv6, ipv6_size, &best_v6, candidate,
                                (is_link_local ? 0 : 10) + is_wlan);
        }
    }
    freeifaddrs(ifaces);
}

/* DT memory shorthand → marketing name (e.g. "LP4X" → "LPDDR4X"). */
static void jw__expand_ram_type(const char *token, char *out, size_t out_size) {
    if      (strcasecmp(token, "LP4X") == 0) snprintf(out, out_size, "LPDDR4X");
    else if (strcasecmp(token, "LP4")  == 0) snprintf(out, out_size, "LPDDR4");
    else if (strcasecmp(token, "LP5X") == 0) snprintf(out, out_size, "LPDDR5X");
    else if (strcasecmp(token, "LP5")  == 0) snprintf(out, out_size, "LPDDR5");
    else                                     snprintf(out, out_size, "%s", token);
}

/* Parse the device-tree model string (e.g. "Rockchip RK3566 RK817 MANGMI LP4X
 * Board") into friendly hardware fields. Tokens are classified by shape:
 * "Rockchip" is the SoC vendor, RK3xxx is the SoC, RK8xx is the PMIC, LP4X/etc.
 * is the RAM type, "Board"/"Tablet" are filler, and the first leftover token is
 * the board / ODM name. Any field left empty means "not recognized" — the About
 * page then falls back to showing the raw model line. */
static void jw__parse_dt_model(const char *model, jw_system_info *out) {
    if (!model || !model[0]) return;
    char vendor[24] = "";
    char chip[24]   = "";
    char copy[80];
    snprintf(copy, sizeof(copy), "%s", model);
    for (char *token = strtok(copy, " \t"); token; token = strtok(NULL, " \t")) {
        int is_rk = (token[0] == 'R' || token[0] == 'r') &&
                    (token[1] == 'K' || token[1] == 'k');
        if (strcasecmp(token, "Rockchip") == 0) {
            snprintf(vendor, sizeof(vendor), "%s", token);
        } else if (is_rk && token[2] == '3') {
            snprintf(chip, sizeof(chip), "%s", token);                  /* SoC, e.g. RK3566 */
        } else if (is_rk && token[2] == '8') {
            snprintf(out->pmic, sizeof(out->pmic), "%s", token);        /* PMIC, e.g. RK817 */
        } else if (strncasecmp(token, "LP", 2) == 0 ||
                   strncasecmp(token, "DDR", 3) == 0) {
            jw__expand_ram_type(token, out->ram_type, sizeof(out->ram_type));
        } else if (strcasecmp(token, "Board") == 0 ||
                   strcasecmp(token, "Tablet") == 0) {
            /* filler — ignore */
        } else if (!out->board[0]) {
            snprintf(out->board, sizeof(out->board), "%s", token);      /* board / ODM name */
        }
    }
    if (chip[0]) {
        if (vendor[0]) snprintf(out->soc, sizeof(out->soc), "%s %s", vendor, chip);
        else           snprintf(out->soc, sizeof(out->soc), "%s", chip);
    }
}

void jw_platform_system_info(const char *fs_path, jw_system_info *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->mem_total_kb = -1;
    out->mem_avail_kb = -1;
    out->sd_total_mb = -1;
    out->sd_free_mb = -1;
    out->battery_percent = -1;
    out->cpu_temp_c = -1;
    out->uptime_s = -1;

    jw__read_stock_version(out->os_version, sizeof(out->os_version));

    struct utsname uts;
    if (uname(&uts) == 0)
        snprintf(out->kernel, sizeof(out->kernel), "%.63s", uts.release);

    jw__read_file_trim("/proc/device-tree/model", out->device, sizeof(out->device));
    jw__parse_dt_model(out->device, out);

    jw__read_meminfo(&out->mem_total_kb, &out->mem_avail_kb);

    if (fs_path && fs_path[0]) {
        struct statvfs vfs;
        if (statvfs(fs_path, &vfs) == 0) {
            unsigned long long block = vfs.f_frsize ? vfs.f_frsize : vfs.f_bsize;
            out->sd_total_mb = (long)((block * (unsigned long long)vfs.f_blocks) / (1024ULL * 1024ULL));
            out->sd_free_mb  = (long)((block * (unsigned long long)vfs.f_bavail) / (1024ULL * 1024ULL));
        }
    }

    jw__read_ips(out->ipv4, sizeof(out->ipv4), out->ipv6, sizeof(out->ipv6));
    snprintf(out->ip, sizeof(out->ip), "%s", out->ipv4);
    jw__read_battery(&out->battery_percent, &out->charging);

    long milli = jw__read_long_file("/sys/class/thermal/thermal_zone0/temp");
    if (milli > 0) out->cpu_temp_c = (int)(milli / 1000);

    char up[64];
    if (jw__read_file_trim("/proc/uptime", up, sizeof(up)) == 0)
        out->uptime_s = strtol(up, NULL, 10);
}
