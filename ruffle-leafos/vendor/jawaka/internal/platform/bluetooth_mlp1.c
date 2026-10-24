#include "internal/platform/bluetooth.h"
#include "internal/platform/paths.h"

#include "cJSON.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <spawn.h>
#include <unistd.h>

extern char **environ;

#define JW_BT_CMD_TIMEOUT_MS       4000
#define JW_BT_SCAN_TIMEOUT_MS     12000
#define JW_BT_CONNECT_TIMEOUT_MS  45000
#define JW_LOONG_DB_PATH "/oem/loong/loong.db"

bool jw_bt_available(void) {
    return true;
}

typedef int (*jw_writeconfig_fn)(const char *, const char *, const char *, int);

typedef struct {
    pid_t pid;
    int fd;
    jw_bt_operation_kind kind;
    long long deadline_ms;
    char message[160];
    size_t message_len;
} jw_bt_worker;

static jw_bt_worker s_worker = { .pid = -1, .fd = -1, .kind = JW_BT_OP_NONE };

static long long jw__bt_now_ms(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return 0;
    }
    return (long long)tv.tv_sec * 1000LL + (long long)(tv.tv_usec / 1000);
}

static void jw__bt_copy(char *dst, size_t dst_len, const char *src) {
    if (!dst || dst_len == 0) {
        return;
    }
    if (!src) {
        src = "";
    }
    size_t n = strlen(src);
    if (n >= dst_len) {
        n = dst_len - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static char *jw__bt_trim(char *s) {
    if (!s) {
        return s;
    }
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) {
        *--end = '\0';
    }
    return s;
}

bool jw_bt_mac_valid(const char *mac) {
    if (!mac || strlen(mac) != 17) {
        return false;
    }
    for (int i = 0; i < 17; i++) {
        if ((i + 1) % 3 == 0) {
            if (mac[i] != ':') {
                return false;
            }
        } else if (!isxdigit((unsigned char)mac[i])) {
            return false;
        }
    }
    return true;
}

void jw_bt_mac_canonical(const char *mac, char out[JW_BT_MAC_LEN]) {
    if (!out) {
        return;
    }
    out[0] = '\0';
    if (!jw_bt_mac_valid(mac)) {
        return;
    }
    for (int i = 0; i < 17; i++) {
        out[i] = (char)toupper((unsigned char)mac[i]);
    }
    out[17] = '\0';
}

static int jw__bt_run_argv(const char *const *argv, char *buf, size_t buf_size,
                           int timeout_ms) {
    if (!buf || buf_size == 0 || !argv || !argv[0]) {
        return -1;
    }
    buf[0] = '\0';

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }

    /* posix_spawn rather than fork()+execvp: plain fork() duplicates this
       process's page tables and contends on the malloc lock, and the launcher
       carries a large RSS (textures, glyph atlases, cover thumbnails) — so each
       status-poll fork stalled the render thread for a frame. posix_spawn is
       vfork-backed: it shares the address space and suspends only this thread
       until the child execs, so the render thread is never blocked. The child's
       stdout goes to the pipe, stderr to /dev/null; the pipe fds are closed in
       the child (the parent keeps the read end). */
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&fa, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    posix_spawn_file_actions_addclose(&fa, pipefd[1]);

    pid_t pid = 0;
    int spawn_rc = posix_spawnp(&pid, argv[0], &fa, NULL,
                                (char *const *)argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    if (spawn_rc != 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    size_t total = 0;
    int elapsed_ms = 0;
    int timed_out = 0;
    while (total + 1 < buf_size) {
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(pipefd[0], &rf);
        struct timeval tv = { 0, 200000 };
        int s = select(pipefd[0] + 1, &rf, NULL, NULL, &tv);
        if (s > 0) {
            ssize_t n = read(pipefd[0], buf + total, buf_size - 1 - total);
            if (n > 0) {
                total += (size_t)n;
            } else if (n == 0) {
                break;
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
        } else if (s == 0) {
            elapsed_ms += 200;
            if (elapsed_ms >= timeout_ms) {
                kill(pid, SIGKILL);
                timed_out = 1;
                break;
            }
        } else if (errno != EINTR) {
            break;
        }
    }
    buf[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (timed_out || !(WIFEXITED(status) && WEXITSTATUS(status) == 0)) {
        return -1;
    }
    return (int)total;
}

static int jw__btctl(char *buf, size_t buf_size, int timeout_ms,
                     const char *a, const char *b, const char *c) {
    const char *argv[6];
    int n = 0;
    argv[n++] = "bluetoothctl";
    if (a) argv[n++] = a;
    if (b) argv[n++] = b;
    if (c) argv[n++] = c;
    argv[n] = NULL;
    return jw__bt_run_argv(argv, buf, buf_size, timeout_ms);
}

static int jw__btctl_timeout_scan(char *buf, size_t buf_size) {
    const char *argv[] = { "bluetoothctl", "--timeout", "8", "scan", "on", NULL };
    return jw__bt_run_argv(argv, buf, buf_size, JW_BT_SCAN_TIMEOUT_MS);
}

static bool jw__bt_line_bool(const char *line, const char *key) {
    size_t n = strlen(key);
    if (strncmp(line, key, n) != 0) {
        return false;
    }
    const char *p = line + n;
    while (*p && (*p == ':' || isspace((unsigned char)*p))) {
        p++;
    }
    return strncmp(p, "yes", 3) == 0;
}

static bool jw__bt_line_value(const char *line, const char *key,
                              char *out, size_t out_len) {
    size_t n = strlen(key);
    if (strncmp(line, key, n) != 0) {
        return false;
    }
    const char *p = line + n;
    while (*p && (*p == ':' || isspace((unsigned char)*p))) {
        p++;
    }
    jw__bt_copy(out, out_len, p);
    return true;
}

static unsigned jw__bt_parse_hex(const char *s) {
    if (!s) {
        return 0;
    }
    while (*s && isspace((unsigned char)*s)) {
        s++;
    }
    return (unsigned)strtoul(s, NULL, 0);
}

static int jw__bt_parse_battery(const char *s) {
    if (!s) {
        return -1;
    }
    const char *paren = strchr(s, '(');
    if (paren) {
        return atoi(paren + 1);
    }
    const char *hex = strstr(s, "0x");
    if (hex) {
        return (int)strtoul(hex, NULL, 0);
    }
    return atoi(s);
}

static bool jw__bt_contains_ci(const char *haystack, const char *needle) {
    if (!haystack || !needle || !needle[0]) {
        return false;
    }
    size_t n = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        size_t i = 0;
        while (i < n && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == n) {
            return true;
        }
    }
    return false;
}

static bool jw__bt_name_has(const char *name, const char *const *keywords) {
    for (int i = 0; keywords[i]; i++) {
        if (jw__bt_contains_ci(name, keywords[i])) {
            return true;
        }
    }
    return false;
}

/* Classify a device into a coarse kind for the Nearby "type" label. Strongest
   signal first: the Class of Device (authoritative, present from an inquiry or
   `info`, but 0 when all we have is a scanned name), then the BlueZ icon / audio
   profile / BLE appearance, then a name-keyword fallback. The Nearby list is
   built from `bluetoothctl devices`, which carries only the name -- so the
   keywords are what classify a freshly scanned device before it is connected and
   its Class/Icon/UUIDs resolve. */
static void jw__bt_classify(jw_bt_device_t *d) {
    if (!d) {
        return;
    }

    /* 1. Class of Device (authoritative when we have it). Major class in bits
       8-12; for Peripheral, bits 2-5 give joystick/gamepad and bits 6/7 flag
       keyboard/pointing. */
    if (d->class_hex != 0) {
        unsigned major = (d->class_hex >> 8) & 0x1Fu;
        if (major == 0x04u) {                       /* Audio/Video */
            d->kind = JW_BT_DEVICE_HEADSET;
            return;
        }
        if (major == 0x05u) {                       /* Peripheral */
            unsigned low = (d->class_hex >> 2) & 0x0Fu;
            if (low == 0x01u || low == 0x02u) {     /* joystick / gamepad */
                d->kind = JW_BT_DEVICE_JOYPAD;
                return;
            }
            if ((d->class_hex >> 6) & 0x1u) {        /* keyboard */
                d->kind = JW_BT_DEVICE_KEYBOARD;
                return;
            }
            if ((d->class_hex >> 7) & 0x1u) {        /* pointing device */
                d->kind = JW_BT_DEVICE_MOUSE;
                return;
            }
        }
    }

    /* 2. Icon / audio profile / BLE appearance / name keywords. */
    static const char *const audio_kw[] = {
        "headphone", "headset", "earphone", "earbud", "buds", "airpod",
        "shokz", "openrun", "opencomm", "soundcore", "jbl", "bose", "beats",
        "jabra", "sennheiser", "skullcandy", "freebuds", "quietcomfort",
        "speaker", "soundbar", NULL
    };
    static const char *const pad_kw[] = {
        "controller", "gamepad", "joystick", "joypad", "xbox", "dualsense",
        "dualshock", "8bitdo", "stadia", "joy-con", "joycon", "gulikit",
        "sn30", "pro controller", NULL
    };
    static const char *const kbd_kw[] = { "keyboard", "keychron", NULL };
    static const char *const mouse_kw[] = { "mouse", "trackpad", NULL };

    if (jw__bt_contains_ci(d->icon, "audio") || d->has_audio_sink ||
        d->has_a2dp || jw__bt_name_has(d->name, audio_kw)) {
        d->kind = JW_BT_DEVICE_HEADSET;
        return;
    }
    if (jw__bt_contains_ci(d->icon, "gaming") ||
        d->appearance_hex == 0x03c3 || d->appearance_hex == 0x03c4 ||
        jw__bt_name_has(d->name, pad_kw)) {
        d->kind = JW_BT_DEVICE_JOYPAD;
        return;
    }
    if (jw__bt_contains_ci(d->icon, "keyboard") ||
        d->appearance_hex == 0x03c1 || jw__bt_name_has(d->name, kbd_kw)) {
        d->kind = JW_BT_DEVICE_KEYBOARD;
        return;
    }
    if (jw__bt_contains_ci(d->icon, "mouse") ||
        d->appearance_hex == 0x03c2 || jw__bt_name_has(d->name, mouse_kw)) {
        d->kind = JW_BT_DEVICE_MOUSE;
        return;
    }
    d->kind = JW_BT_DEVICE_OTHER;
}

const char *jw_bt_device_kind_name(jw_bt_device_kind kind) {
    switch (kind) {
        case JW_BT_DEVICE_HEADSET: return "Headset";
        case JW_BT_DEVICE_JOYPAD: return "Controller";
        case JW_BT_DEVICE_KEYBOARD: return "Keyboard";
        case JW_BT_DEVICE_MOUSE: return "Mouse";
        case JW_BT_DEVICE_OTHER: return "Other";
        default: return "Unknown";
    }
}

const char *jw_bt_device_kind_stock_name(jw_bt_device_kind kind) {
    switch (kind) {
        case JW_BT_DEVICE_HEADSET: return "HEADSET";
        case JW_BT_DEVICE_JOYPAD: return "JOYPAD";
        case JW_BT_DEVICE_KEYBOARD: return "KEYBOARD";
        case JW_BT_DEVICE_MOUSE: return "MOUSE";
        case JW_BT_DEVICE_OTHER: return "OTHER";
        default: return "OTHER";
    }
}

static int jw__bt_sql_value(const char *param, char *out, size_t out_len) {
    if (!param || !out || out_len == 0) {
        return -1;
    }
    char query[192];
    snprintf(query, sizeof(query),
             "select value from system_config where param='%s';", param);
    const char *argv[] = {
        "sqlite3", "-cmd", ".timeout 1000", JW_LOONG_DB_PATH, query, NULL
    };
    char buf[2048];
    if (jw__bt_run_argv(argv, buf, sizeof(buf), 2000) < 0) {
        return -1;
    }
    char *line = strtok(buf, "\r\n");
    if (!line) {
        return -1;
    }
    jw__bt_copy(out, out_len, line);
    return 0;
}

static int jw__bt_read_enable_flag(void) {
    char json[128];
    if (jw__bt_sql_value("BLUETOOTH_PARAM", json, sizeof(json)) != 0) {
        return -1;
    }
    const char *p = strstr(json, "enable");
    if (!p) {
        return -1;
    }
    while (*p && *p != '0' && *p != '1') {
        p++;
    }
    if (*p == '0') return 0;
    if (*p == '1') return 1;
    return -1;
}

static jw_writeconfig_fn jw__bt_writeconfig(void) {
    static jw_writeconfig_fn fn = NULL;
    static int tried = 0;
    if (!tried) {
        tried = 1;
        void *h = dlopen("/usr/lib/libloong_sdk.so", RTLD_NOW | RTLD_GLOBAL);
        if (h) {
            *(void **)(&fn) = dlsym(h, "WriteConfig");
        }
    }
    return fn;
}

static int jw__bt_write_stock(const char *param, const char *value) {
    jw_writeconfig_fn WriteConfig = jw__bt_writeconfig();
    if (!WriteConfig) {
        return -1;
    }
    return WriteConfig(param, value, "", 1) < 0 ? -1 : 0;
}

static void jw__bt_parse_show(char *dump, jw_bt_status_t *out) {
    char *save = NULL;
    for (char *line = strtok_r(dump, "\n", &save);
         line;
         line = strtok_r(NULL, "\n", &save)) {
        line = jw__bt_trim(line);
        if (strncmp(line, "Controller ", 11) == 0) {
            char mac[JW_BT_MAC_LEN] = { 0 };
            if (sscanf(line + 11, "%17s", mac) == 1 && jw_bt_mac_valid(mac)) {
                jw_bt_mac_canonical(mac, out->adapter_mac);
            }
        } else if (jw__bt_line_value(line, "Name", out->local_name, sizeof(out->local_name))) {
        } else if (!out->local_name[0] &&
                   jw__bt_line_value(line, "Alias", out->local_name, sizeof(out->local_name))) {
        } else if (strncmp(line, "Powered:", 8) == 0) {
            out->powered = jw__bt_line_bool(line, "Powered");
        } else if (strncmp(line, "Pairable:", 9) == 0) {
            out->pairable = jw__bt_line_bool(line, "Pairable");
        } else if (strncmp(line, "Discovering:", 12) == 0) {
            out->discovering = jw__bt_line_bool(line, "Discovering");
        }
    }
}

int jw_bt_status(jw_bt_status_t *out) {
    if (!out) {
        return -1;
    }
    memset(out, 0, sizeof(*out));

    char dump[4096];
    if (jw__btctl(dump, sizeof(dump), JW_BT_CMD_TIMEOUT_MS, "show", NULL, NULL) < 0) {
        return -1;
    }
    out->available = true;
    jw__bt_parse_show(dump, out);

    char name[JW_BT_NAME_LEN];
    if (jw__bt_sql_value("DEVICE_NAME", name, sizeof(name)) == 0 && name[0]) {
        jw__bt_copy(out->local_name, sizeof(out->local_name), name);
    }

    out->any_connected = jw_bt_any_connected() == 1;
    (void)jw_bt_sync_stock_saved_list();
    return 0;
}

/* HCIGETDEVINFO from the kernel's HCI socket ABI (include/net/bluetooth/
   hci_sock.h), spelled out because the toolchain has no BlueZ headers. Only
   dev_id (in) and flags (out; bit 0 is HCI_UP) are used; the rest sizes the
   buffer the kernel copies into. No capability is needed to ask. */
#define JW_AF_BLUETOOTH  31
#define JW_BTPROTO_HCI   1
#define JW_HCIGETDEVINFO _IOR('H', 211, int)
#define JW_HCI_UP        0
struct jw_hci_dev_info {
    uint16_t dev_id;
    char     name[8];
    uint8_t  bdaddr[6];
    uint32_t flags;
    uint8_t  type;
    uint8_t  features[8];
    uint32_t pkt_type;
    uint32_t link_policy;
    uint32_t link_mode;
    uint16_t acl_mtu;
    uint16_t acl_pkts;
    uint16_t sco_mtu;
    uint16_t sco_pkts;
    uint32_t stat[10];
};

int jw_bt_kernel_state(bool *powered, bool *connected) {
    int fd = socket(JW_AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, JW_BTPROTO_HCI);
    if (fd < 0) {
        return -1;
    }
    struct jw_hci_dev_info info;
    memset(&info, 0, sizeof(info));
    info.dev_id = 0;   /* hci0 */
    int rc = ioctl(fd, JW_HCIGETDEVINFO, &info);
    close(fd);
    if (rc != 0) {
        return -1;
    }
    bool up = (info.flags & (1u << JW_HCI_UP)) != 0;
    /* Each live ACL link is a device object named hci0:<handle>. */
    bool any = false;
    if (up) {
        DIR *dir = opendir("/sys/class/bluetooth");
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                if (strncmp(entry->d_name, "hci0:", 5) == 0) {
                    any = true;
                    break;
                }
            }
            closedir(dir);
        }
    }
    if (powered) *powered = up;
    if (connected) *connected = any;
    return 0;
}

bool jw_bt_radio_is_on(void) {
    /* Source of truth is the actual adapter state (BlueZ "Powered"), not the
       stock BLUETOOTH_PARAM enable flag: in Leaf mode no stock daemon keeps that
       flag in sync, so it goes stale and the UI can't see the radio turn off.
       Fall back to the stock flag only if the adapter can't be queried. */
    jw_bt_status_t status;
    if (jw_bt_status(&status) == 0) {
        return status.powered;
    }
    return jw__bt_read_enable_flag() > 0;
}

static void jw__bt_try_start_stack(void) {
    if (access("/usr/bin/wifibt-init.sh", X_OK) == 0) {
        pid_t pid = fork();
        if (pid == 0) {
            /* Silence the stock script's chatter ("handling start_bt for
               wifi/bt chip", etc.). When set_radio runs inside the connect
               worker, the worker's stdout is the pipe the UI reads as the
               operation message, so unredirected output leaks into the status
               line. We only want the script's side effect, not its words. */
            int dn = open("/dev/null", O_WRONLY);
            if (dn >= 0) {
                dup2(dn, STDOUT_FILENO);
                dup2(dn, STDERR_FILENO);
                if (dn > STDERR_FILENO) {
                    close(dn);
                }
            }
            execl("/usr/bin/wifibt-init.sh", "wifibt-init.sh", "start_bt", (char *)NULL);
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        }
    }
}

int jw_bt_set_radio(bool on) {
    if (on) {
        jw__bt_try_start_stack();
    }
    char buf[256];
    int rc = jw__btctl(buf, sizeof(buf), JW_BT_CMD_TIMEOUT_MS,
                       "power", on ? "on" : "off", NULL);
    if (on && rc == 0) {
        (void)jw__btctl(buf, sizeof(buf), JW_BT_CMD_TIMEOUT_MS,
                        "pairable", "on", NULL);
    } else if (!on) {
        (void)jw__btctl(buf, sizeof(buf), JW_BT_CMD_TIMEOUT_MS,
                        "scan", "off", NULL);
    }
    (void)jw__bt_write_stock("BLUETOOTH_PARAM", on ? "{\"enable\":1}" : "{\"enable\":0}");
    return rc < 0 ? -1 : 0;
}

static bool jw__bt_parse_device_line(const char *line,
                                     char mac[JW_BT_MAC_LEN],
                                     char name[JW_BT_NAME_LEN]) {
    if (!line) {
        return false;
    }
    const char *p = strstr(line, "Device ");
    if (!p) {
        return false;
    }
    p += 7;
    char raw[JW_BT_MAC_LEN] = { 0 };
    if (sscanf(p, "%17s", raw) != 1 || !jw_bt_mac_valid(raw)) {
        return false;
    }
    jw_bt_mac_canonical(raw, mac);
    p += 17;
    while (*p && isspace((unsigned char)*p)) {
        p++;
    }
    jw__bt_copy(name, JW_BT_NAME_LEN, *p ? p : mac);
    return true;
}

static bool jw__bt_mac_in_set(const char mac[JW_BT_MAC_LEN],
                              char set[][JW_BT_MAC_LEN], int count) {
    if (!mac) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (strcmp(mac, set[i]) == 0) {
            return true;
        }
    }
    return false;
}

static void jw__bt_add_mac_to_set(const char mac[JW_BT_MAC_LEN],
                                  char set[][JW_BT_MAC_LEN], int *count, int max) {
    if (!mac || !count || *count >= max || jw__bt_mac_in_set(mac, set, *count)) {
        return;
    }
    jw__bt_copy(set[*count], JW_BT_MAC_LEN, mac);
    (*count)++;
}

/* BlueZ reports unnamed devices with their address (dash form) as the "name",
   e.g. "0C-3D-44-6A-62-EE". Treat those as nameless so the scan list can hide
   the address-only BLE clutter and only surface devices with a real name. */
static bool jw__bt_name_is_address(const char *name, const char mac[JW_BT_MAC_LEN]) {
    if (!name || !name[0]) {
        return true;
    }
    char norm[JW_BT_NAME_LEN];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(norm); i++) {
        char c = name[i];
        if (c == '-') c = ':';
        norm[i] = (char)toupper((unsigned char)c);
    }
    norm[i] = '\0';
    return strcmp(norm, mac) == 0;
}

static void jw__bt_summary_device(jw_bt_device_t *dev,
                                  const char mac[JW_BT_MAC_LEN],
                                  const char *name,
                                  bool paired, bool connected) {
    if (!dev) {
        return;
    }
    memset(dev, 0, sizeof(*dev));
    jw__bt_copy(dev->mac, sizeof(dev->mac), mac);
    jw__bt_copy(dev->name, sizeof(dev->name), name && name[0] ? name : mac);
    jw__bt_copy(dev->alias, sizeof(dev->alias), dev->name);
    dev->paired = paired;
    dev->bonded = paired;
    dev->connected = connected;
    dev->battery_percent = -1;
    jw__bt_classify(dev);
}

static int jw__bt_devices_dump(const char *filter, char *buf, size_t buf_size) {
    if (filter) {
        return jw__btctl(buf, buf_size, JW_BT_CMD_TIMEOUT_MS,
                         "devices", filter, NULL);
    }
    return jw__btctl(buf, buf_size, JW_BT_CMD_TIMEOUT_MS,
                     "devices", NULL, NULL);
}

int jw_bt_refresh_device(const char *mac, jw_bt_device_t *out) {
    if (!out || !jw_bt_mac_valid(mac)) {
        return -1;
    }
    char canon[JW_BT_MAC_LEN];
    jw_bt_mac_canonical(mac, canon);
    memset(out, 0, sizeof(*out));
    jw__bt_copy(out->mac, sizeof(out->mac), canon);
    out->battery_percent = -1;

    char dump[8192];
    if (jw__btctl(dump, sizeof(dump), JW_BT_CMD_TIMEOUT_MS, "info", canon, NULL) < 0) {
        return -1;
    }

    char *save = NULL;
    for (char *line = strtok_r(dump, "\n", &save);
         line;
         line = strtok_r(NULL, "\n", &save)) {
        line = jw__bt_trim(line);
        char value[256];
        if (jw__bt_line_value(line, "Name", value, sizeof(value))) {
            jw__bt_copy(out->name, sizeof(out->name), value);
        } else if (jw__bt_line_value(line, "Alias", value, sizeof(value))) {
            jw__bt_copy(out->alias, sizeof(out->alias), value);
        } else if (jw__bt_line_value(line, "Icon", value, sizeof(value))) {
            jw__bt_copy(out->icon, sizeof(out->icon), value);
        } else if (jw__bt_line_value(line, "Class", value, sizeof(value))) {
            out->class_hex = jw__bt_parse_hex(value);
        } else if (jw__bt_line_value(line, "Appearance", value, sizeof(value))) {
            out->appearance_hex = jw__bt_parse_hex(value);
        } else if (jw__bt_line_value(line, "RSSI", value, sizeof(value))) {
            out->rssi = atoi(value);
        } else if (jw__bt_line_value(line, "Battery Percentage", value, sizeof(value))) {
            out->battery_percent = jw__bt_parse_battery(value);
        } else if (strncmp(line, "Paired:", 7) == 0) {
            out->paired = jw__bt_line_bool(line, "Paired");
        } else if (strncmp(line, "Bonded:", 7) == 0) {
            out->bonded = jw__bt_line_bool(line, "Bonded");
        } else if (strncmp(line, "Trusted:", 8) == 0) {
            out->trusted = jw__bt_line_bool(line, "Trusted");
        } else if (strncmp(line, "Connected:", 10) == 0) {
            out->connected = jw__bt_line_bool(line, "Connected");
        } else if (strncmp(line, "Blocked:", 8) == 0) {
            out->blocked = jw__bt_line_bool(line, "Blocked");
        } else if (jw__bt_line_value(line, "UUID", value, sizeof(value))) {
            if (jw__bt_contains_ci(value, "Audio Sink") ||
                jw__bt_contains_ci(value, "0000110b")) {
                out->has_audio_sink = true;
                out->has_a2dp = true;
            }
            if (jw__bt_contains_ci(value, "AV Remote") ||
                jw__bt_contains_ci(value, "0000110e") ||
                jw__bt_contains_ci(value, "0000110c")) {
                out->has_avrcp = true;
            }
            if (jw__bt_contains_ci(value, "Human Interface") ||
                jw__bt_contains_ci(value, "00001812") ||
                jw__bt_contains_ci(value, "00001124")) {
                out->has_hid = true;
            }
        }
    }

    if (!out->name[0] && out->alias[0]) {
        jw__bt_copy(out->name, sizeof(out->name), out->alias);
    }
    if (!out->alias[0] && out->name[0]) {
        jw__bt_copy(out->alias, sizeof(out->alias), out->name);
    }
    if (!out->name[0]) {
        jw__bt_copy(out->name, sizeof(out->name), out->mac);
    }
    jw__bt_classify(out);
    return 0;
}

static int jw__bt_list_devices(const char *filter, jw_bt_device_t *out, int max,
                               bool skip_paired) {
    if (!out || max <= 0) {
        return -1;
    }
    char dump[8192];
    int rc;
    if (filter) {
        rc = jw__btctl(dump, sizeof(dump), JW_BT_CMD_TIMEOUT_MS,
                       "devices", filter, NULL);
    } else {
        rc = jw__btctl(dump, sizeof(dump), JW_BT_CMD_TIMEOUT_MS,
                       "devices", NULL, NULL);
    }
    if (rc < 0) {
        return -1;
    }

    int count = 0;
    char *save = NULL;
    for (char *line = strtok_r(dump, "\n", &save);
         line && count < max;
         line = strtok_r(NULL, "\n", &save)) {
        char mac[JW_BT_MAC_LEN];
        char name[JW_BT_NAME_LEN];
        if (!jw__bt_parse_device_line(line, mac, name)) {
            continue;
        }

        jw_bt_device_t dev;
        if (jw_bt_refresh_device(mac, &dev) != 0) {
            memset(&dev, 0, sizeof(dev));
            jw__bt_copy(dev.mac, sizeof(dev.mac), mac);
            jw__bt_copy(dev.name, sizeof(dev.name), name);
            dev.kind = JW_BT_DEVICE_UNKNOWN;
            dev.battery_percent = -1;
        } else if (name[0] && strcmp(name, mac) != 0 &&
                   (!dev.name[0] || strcmp(dev.name, dev.mac) == 0)) {
            jw__bt_copy(dev.name, sizeof(dev.name), name);
            if (!dev.alias[0] || strcmp(dev.alias, dev.mac) == 0) {
                jw__bt_copy(dev.alias, sizeof(dev.alias), name);
            }
        }
        if (skip_paired && dev.paired) {
            continue;
        }
        out[count++] = dev;
    }
    return count;
}

int jw_bt_list_paired(jw_bt_device_t *out, int max) {
    return jw__bt_list_devices("Paired", out, max, false);
}

int jw_bt_list_nearby(jw_bt_device_t *out, int max) {
    return jw__bt_list_devices(NULL, out, max, true);
}

int jw_bt_list_summaries(jw_bt_device_t *paired, int paired_max, int *paired_count,
                         jw_bt_device_t *nearby, int nearby_max, int *nearby_count) {
    if (paired_count) {
        *paired_count = 0;
    }
    if (nearby_count) {
        *nearby_count = 0;
    }

    bool want_paired = paired && paired_max > 0;
    bool want_nearby = nearby && nearby_max > 0;
    if (!want_paired && !want_nearby) {
        return 0;
    }

    char paired_dump[8192];
    char connected_dump[8192];
    if (jw__bt_devices_dump("Paired", paired_dump, sizeof(paired_dump)) < 0 ||
        jw__bt_devices_dump("Connected", connected_dump, sizeof(connected_dump)) < 0) {
        return -1;
    }

    char connected_macs[JW_BT_MAX_DEVICES][JW_BT_MAC_LEN];
    int connected_count = 0;
    char *save = NULL;
    for (char *line = strtok_r(connected_dump, "\n", &save);
         line;
         line = strtok_r(NULL, "\n", &save)) {
        char mac[JW_BT_MAC_LEN];
        char name[JW_BT_NAME_LEN];
        if (jw__bt_parse_device_line(line, mac, name)) {
            jw__bt_add_mac_to_set(mac, connected_macs, &connected_count,
                                  JW_BT_MAX_DEVICES);
        }
    }

    char paired_macs[JW_BT_MAX_DEVICES][JW_BT_MAC_LEN];
    int paired_set_count = 0;
    int out_paired_count = 0;
    save = NULL;
    for (char *line = strtok_r(paired_dump, "\n", &save);
         line;
         line = strtok_r(NULL, "\n", &save)) {
        char mac[JW_BT_MAC_LEN];
        char name[JW_BT_NAME_LEN];
        if (!jw__bt_parse_device_line(line, mac, name)) {
            continue;
        }
        jw__bt_add_mac_to_set(mac, paired_macs, &paired_set_count, JW_BT_MAX_DEVICES);
        if (want_paired && out_paired_count < paired_max) {
            jw__bt_summary_device(&paired[out_paired_count], mac, name, true,
                                  jw__bt_mac_in_set(mac, connected_macs,
                                                    connected_count));
            out_paired_count++;
        }
    }
    if (paired_count) {
        *paired_count = out_paired_count;
    }

    if (want_nearby) {
        char all_dump[8192];
        if (jw__bt_devices_dump(NULL, all_dump, sizeof(all_dump)) < 0) {
            return -1;
        }
        int out_nearby_count = 0;
        save = NULL;
        for (char *line = strtok_r(all_dump, "\n", &save);
             line && out_nearby_count < nearby_max;
             line = strtok_r(NULL, "\n", &save)) {
            char mac[JW_BT_MAC_LEN];
            char name[JW_BT_NAME_LEN];
            if (!jw__bt_parse_device_line(line, mac, name) ||
                jw__bt_mac_in_set(mac, paired_macs, paired_set_count)) {
                continue;
            }
            if (jw__bt_name_is_address(name, mac)) {
                continue;   /* hide address-only (unnamed) devices */
            }
            jw__bt_summary_device(&nearby[out_nearby_count], mac, name, false,
                                  jw__bt_mac_in_set(mac, connected_macs,
                                                    connected_count));
            out_nearby_count++;
        }
        if (nearby_count) {
            *nearby_count = out_nearby_count;
        }
    }

    return 0;
}

int jw_bt_list_paired_summary(jw_bt_device_t *out, int max) {
    int count = 0;
    if (jw_bt_list_summaries(out, max, &count, NULL, 0, NULL) != 0) {
        return -1;
    }
    return count;
}

int jw_bt_list_nearby_summary(jw_bt_device_t *out, int max) {
    int count = 0;
    if (jw_bt_list_summaries(NULL, 0, NULL, out, max, &count) != 0) {
        return -1;
    }
    return count;
}

int jw_bt_any_connected(void) {
    char dump[1024];
    if (jw__btctl(dump, sizeof(dump), JW_BT_CMD_TIMEOUT_MS,
                  "devices", "Connected", NULL) < 0) {
        return -1;
    }
    return strstr(dump, "Device ") ? 1 : 0;
}

int jw_bt_audio_connected(void) {
    char dump[4096];
    if (jw__btctl(dump, sizeof(dump), JW_BT_CMD_TIMEOUT_MS,
                  "devices", "Connected", NULL) < 0) {
        return -1;
    }

    bool saw_connected = false;
    char *save = NULL;
    for (char *line = strtok_r(dump, "\n", &save);
         line;
         line = strtok_r(NULL, "\n", &save)) {
        char mac[JW_BT_MAC_LEN];
        char name[JW_BT_NAME_LEN];
        if (!jw__bt_parse_device_line(line, mac, name)) {
            continue;
        }
        saw_connected = true;
        jw_bt_device_t dev;
        if (jw_bt_refresh_device(mac, &dev) == 0 &&
            (dev.has_audio_sink || dev.has_a2dp ||
             dev.kind == JW_BT_DEVICE_HEADSET)) {
            return 1;
        }
    }
    return saw_connected ? 0 : 0;
}

static int jw__bt_worker_running(void) {
    if (s_worker.pid <= 0) {
        return 0;
    }
    int status = 0;
    pid_t r = waitpid(s_worker.pid, &status, WNOHANG);
    if (r == 0) {
        return 1;
    }
    return 0;
}

static void jw__bt_worker_reset(void) {
    if (s_worker.fd >= 0) {
        close(s_worker.fd);
    }
    memset(&s_worker, 0, sizeof(s_worker));
    s_worker.pid = -1;
    s_worker.fd = -1;
    s_worker.kind = JW_BT_OP_NONE;
}

void jw_bt_cancel_operation(void) {
    if (s_worker.pid > 0) {
        kill(s_worker.pid, SIGKILL);
        waitpid(s_worker.pid, NULL, 0);
    }
    jw__bt_worker_reset();
}

static int jw__bt_worker_start(jw_bt_operation_kind kind, int timeout_ms,
                               void (*child_fn)(const char *mac, bool pair, int out_fd),
                               const char *mac, bool pair) {
    if (s_worker.pid > 0 && jw__bt_worker_running()) {
        return -1;
    }
    jw__bt_worker_reset();

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }

    if (pid == 0) {
        /* The parent reads this child's output as the operation message. Give the
           child a dedicated result channel (out_fd) and route its stdout/stderr to
           /dev/null, so chatter that stock helpers and libraries scribble into the
           forked child -- the Mali "arm_release_ver" banner, wifibt-init.sh, etc.
           -- never reaches the status line. Children write only their status on
           out_fd. */
        int out_fd = dup(pipefd[1]);
        int dn = open("/dev/null", O_WRONLY);
        if (dn >= 0) {
            dup2(dn, STDOUT_FILENO);
            dup2(dn, STDERR_FILENO);
            if (dn > STDERR_FILENO) {
                close(dn);
            }
        }
        close(pipefd[0]);
        close(pipefd[1]);
        child_fn(mac, pair, out_fd >= 0 ? out_fd : STDOUT_FILENO);
        _exit(1);
    }

    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    s_worker.pid = pid;
    s_worker.fd = pipefd[0];
    s_worker.kind = kind;
    s_worker.deadline_ms = jw__bt_now_ms() + timeout_ms;
    s_worker.message_len = 0;
    s_worker.message[0] = '\0';
    return 0;
}

static void jw__bt_worker_read(void) {
    if (s_worker.fd < 0 || s_worker.message_len + 1 >= sizeof(s_worker.message)) {
        return;
    }
    while (s_worker.message_len + 1 < sizeof(s_worker.message)) {
        ssize_t n = read(s_worker.fd,
                         s_worker.message + s_worker.message_len,
                         sizeof(s_worker.message) - 1 - s_worker.message_len);
        if (n > 0) {
            s_worker.message_len += (size_t)n;
            s_worker.message[s_worker.message_len] = '\0';
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            break;
        } else {
            break;
        }
    }
}

static jw_bt_operation_status jw__bt_worker_poll(jw_bt_operation_kind kind,
                                                 char *message,
                                                 size_t message_len) {
    if (s_worker.pid <= 0 || s_worker.kind != kind) {
        return JW_BT_OP_IDLE;
    }

    jw__bt_worker_read();
    if (jw__bt_now_ms() > s_worker.deadline_ms) {
        kill(s_worker.pid, SIGKILL);
        waitpid(s_worker.pid, NULL, 0);
        jw__bt_copy(message, message_len, "Bluetooth operation timed out");
        jw__bt_worker_reset();
        return JW_BT_OP_TIMEOUT;
    }

    int status = 0;
    pid_t r = waitpid(s_worker.pid, &status, WNOHANG);
    if (r == 0) {
        return JW_BT_OP_RUNNING;
    }
    if (r < 0 && errno == EINTR) {
        return JW_BT_OP_RUNNING;
    }

    jw__bt_worker_read();
    jw_bt_operation_status result =
        (r > 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0)
        ? JW_BT_OP_OK
        : JW_BT_OP_FAILED;
    if (s_worker.message[0]) {
        char *m = jw__bt_trim(s_worker.message);
        jw__bt_copy(message, message_len, m);
    } else {
        jw__bt_copy(message, message_len,
                    result == JW_BT_OP_OK ? "Bluetooth operation complete"
                                          : "Bluetooth operation failed");
    }
    jw__bt_worker_reset();
    return result;
}

static void jw__bt_scan_child(const char *mac, bool pair, int out_fd) {
    (void)mac;
    (void)pair;
    FILE *out = fdopen(out_fd, "w");
    if (!out) {
        out = stdout;
    }
    char buf[8192];
    int rc = jw__btctl_timeout_scan(buf, sizeof(buf));
    (void)jw__btctl(buf, sizeof(buf), JW_BT_CMD_TIMEOUT_MS, "scan", "off", NULL);
    if (rc < 0) {
        fprintf(out, "Bluetooth scan failed\n");
        fflush(out);
        _exit(1);
    }
    fprintf(out, "Bluetooth scan complete\n");
    fflush(out);
    _exit(0);
}

int jw_bt_scan_start(void) {
    return jw__bt_worker_start(JW_BT_OP_SCAN, JW_BT_SCAN_TIMEOUT_MS + 3000,
                               jw__bt_scan_child, NULL, false);
}

jw_bt_operation_status jw_bt_scan_poll(char *message, size_t message_len) {
    return jw__bt_worker_poll(JW_BT_OP_SCAN, message, message_len);
}

static bool jw__bt_device_seen(const char *mac) {
    char dump[8192];
    if (jw__btctl(dump, sizeof(dump), JW_BT_CMD_TIMEOUT_MS,
                  "devices", NULL, NULL) < 0) {
        return false;
    }
    return strstr(dump, mac) != NULL;
}

static bool jw__bt_device_connected(const char *mac) {
    jw_bt_device_t dev;
    return jw_bt_refresh_device(mac, &dev) == 0 && dev.connected;
}

/* Single audio sink policy: A2DP is point-to-point and this radio only feeds one
   headset cleanly, so when an audio device connects we disconnect any OTHER
   connected audio device. Non-audio devices (controllers, etc.) are left alone --
   a controller and a headset can be connected at the same time. `keep` is the
   canonical MAC of the just-connected device to preserve. */
static void jw__bt_enforce_single_audio_sink(const char *keep) {
    char dump[4096];
    if (jw__btctl(dump, sizeof(dump), JW_BT_CMD_TIMEOUT_MS,
                  "devices", "Connected", NULL) < 0) {
        return;
    }
    char *save = NULL;
    for (char *line = strtok_r(dump, "\n", &save);
         line;
         line = strtok_r(NULL, "\n", &save)) {
        char mac[JW_BT_MAC_LEN], name[JW_BT_NAME_LEN];
        if (!jw__bt_parse_device_line(line, mac, name)) {
            continue;
        }
        char canon[JW_BT_MAC_LEN];
        jw_bt_mac_canonical(mac, canon);
        if (!jw_bt_mac_valid(canon) || (keep && strcmp(canon, keep) == 0)) {
            continue;
        }
        jw_bt_device_t dev;
        if (jw_bt_refresh_device(canon, &dev) == 0 &&
            (dev.has_audio_sink || dev.has_a2dp ||
             dev.kind == JW_BT_DEVICE_HEADSET)) {
            char buf[256];
            (void)jw__btctl(buf, sizeof(buf), JW_BT_CMD_TIMEOUT_MS,
                            "disconnect", canon, NULL);
        }
    }
}

static void jw__bt_connect_child(const char *mac, bool pair_if_needed, int out_fd) {
    /* out_fd is the dedicated result channel from jw__bt_worker_start; this child's
       stdout/stderr are already routed to /dev/null there, so stock chatter
       (start_bt, arm_release_ver, ...) can't reach the status line. We emit only
       our own status messages, on out_fd. */
    FILE *out = fdopen(out_fd, "w");
    if (!out) {
        out = stdout;
    }

    if (!jw_bt_mac_valid(mac)) {
        fprintf(out, "Invalid Bluetooth address\n");
        fflush(out);
        _exit(1);
    }
    char canon[JW_BT_MAC_LEN];
    jw_bt_mac_canonical(mac, canon);
    char buf[4096];

    (void)jw_bt_set_radio(true);

    jw_bt_device_t before;
    bool bonded = jw_bt_refresh_device(canon, &before) == 0 && before.bonded;
    if (!bonded && !pair_if_needed) {
        fprintf(out, "Device is not paired\n");
        fflush(out);
        _exit(1);
    }

    /* Discover the device first when we still have to pair it. A one-shot scan is
       fine here — scanning needs no agent. */
    if (!bonded) {
        bool seen = jw__bt_device_seen(canon);
        for (int i = 0; !seen && i < 5; i++) {
            (void)jw__btctl_timeout_scan(buf, sizeof(buf));
            (void)jw__btctl(buf, sizeof(buf), JW_BT_CMD_TIMEOUT_MS,
                            "scan", "off", NULL);
            seen = jw__bt_device_seen(canon);
        }
        if (!seen) {
            fprintf(out, "Device not found\n");
            fflush(out);
            _exit(1);
        }
    }

    /* Pair + connect inside ONE bluetoothctl session. The NoInputNoOutput agent
       MUST stay registered across the `pair`: running `agent`/`pair` as separate
       one-shot `bluetoothctl` processes drops the agent the instant each exits,
       so a "just works" pair completes Paired-but-NOT-Bonded (no link key) and
       BlueZ evicts the device on its first disconnect. Holding a single session
       keeps the agent alive and writes a real bond that survives — same shape as
       stock connect-bt.sh. canon is a validated BT address, safe to inline. */
    char cmd[1024];
    int n = snprintf(cmd, sizeof(cmd),
        "( printf 'power on\\n'; sleep 1;"
        " printf 'agent NoInputNoOutput\\n'; sleep 1;"
        " printf 'default-agent\\n'; sleep 1;");
    if (!bonded && n > 0 && (size_t)n < sizeof(cmd)) {
        n += snprintf(cmd + n, sizeof(cmd) - (size_t)n,
            " printf 'trust %s\\n'; sleep 1;"
            " printf 'pair %s\\n'; sleep 8;",
            canon, canon);
    }
    if (n > 0 && (size_t)n < sizeof(cmd)) {
        (void)snprintf(cmd + n, sizeof(cmd) - (size_t)n,
            " printf 'trust %s\\n'; sleep 1;"
            " printf 'connect %s\\n'; sleep 4;"
            " printf 'quit\\n' ) | bluetoothctl >/dev/null 2>&1",
            canon, canon);
    }

    const char *argv[] = { "sh", "-c", cmd, NULL };
    (void)jw__bt_run_argv(argv, buf, sizeof(buf), bonded ? 20000 : 35000);

    if (!jw__bt_device_connected(canon)) {
        (void)jw_bt_sync_stock_saved_list();
        fprintf(out, "Connect failed\n");
        fflush(out);
        _exit(1);
    }

    /* Single audio sink: if we just connected an audio device, drop any other
       connected audio device so only one headset receives audio at a time. */
    jw_bt_device_t dev;
    bool have = (jw_bt_refresh_device(canon, &dev) == 0);
    if (have && (dev.has_audio_sink || dev.has_a2dp ||
                 dev.kind == JW_BT_DEVICE_HEADSET)) {
        jw__bt_enforce_single_audio_sink(canon);
    }

    (void)jw_bt_sync_stock_saved_list();
    fprintf(out, "Connected to %s\n", (have && dev.name[0]) ? dev.name : canon);
    fflush(out);
    _exit(0);
}

int jw_bt_connect_start(const char *mac, bool pair_if_needed) {
    if (!jw_bt_mac_valid(mac)) {
        return -1;
    }
    return jw__bt_worker_start(pair_if_needed ? JW_BT_OP_PAIR_CONNECT : JW_BT_OP_CONNECT,
                               JW_BT_CONNECT_TIMEOUT_MS,
                               jw__bt_connect_child, mac, pair_if_needed);
}

jw_bt_operation_status jw_bt_connect_poll(char *message, size_t message_len) {
    jw_bt_operation_status st =
        jw__bt_worker_poll(JW_BT_OP_PAIR_CONNECT, message, message_len);
    if (st != JW_BT_OP_IDLE) {
        return st;
    }
    return jw__bt_worker_poll(JW_BT_OP_CONNECT, message, message_len);
}

int jw_bt_sync_stock_saved_list(void) {
    jw_bt_device_t devices[JW_BT_MAX_DEVICES];
    int count = jw_bt_list_paired(devices, JW_BT_MAX_DEVICES);
    if (count < 0) {
        return -1;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return -1;
    }
    for (int i = 0; i < count; i++) {
        const jw_bt_device_t *d = &devices[i];
        cJSON *item = cJSON_CreateObject();
        if (!item) {
            continue;
        }
        cJSON_AddNumberToObject(item, "connected", d->connected ? 1 : 0);
        cJSON_AddStringToObject(item, "device", jw_bt_device_kind_stock_name(d->kind));
        cJSON_AddStringToObject(item, "mac", d->mac);
        cJSON_AddStringToObject(item, "name", d->name[0] ? d->name : d->mac);
        cJSON_AddBoolToObject(item, "saved", true);
        cJSON_AddItemToObject(root, d->mac, item);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return -1;
    }
    char *state_dir = jw_state_dir();
    if (state_dir) {
        char path[512];
        int needed = snprintf(path, sizeof(path), "%s/bluetooth.json", state_dir);
        if (needed > 0 && (size_t)needed < sizeof(path)) {
            FILE *fp = fopen(path, "wb");
            if (fp) {
                fputs(json, fp);
                fputc('\n', fp);
                fclose(fp);
            }
        }
        free(state_dir);
    }
    int rc = jw__bt_write_stock("BLUETOOTH_SAVED_LIST", json);
    cJSON_free(json);
    return rc;
}

int jw_bt_disconnect(const char *mac) {
    if (!jw_bt_mac_valid(mac)) {
        return -1;
    }
    char canon[JW_BT_MAC_LEN];
    jw_bt_mac_canonical(mac, canon);
    char buf[512];
    int rc = jw__btctl(buf, sizeof(buf), JW_BT_CMD_TIMEOUT_MS,
                       "disconnect", canon, NULL);
    (void)jw_bt_sync_stock_saved_list();
    return rc < 0 ? -1 : 0;
}

int jw_bt_forget(const char *mac) {
    if (!jw_bt_mac_valid(mac)) {
        return -1;
    }
    char canon[JW_BT_MAC_LEN];
    jw_bt_mac_canonical(mac, canon);
    char buf[512];
    (void)jw__btctl(buf, sizeof(buf), JW_BT_CMD_TIMEOUT_MS,
                    "disconnect", canon, NULL);
    int rc = jw__btctl(buf, sizeof(buf), JW_BT_CMD_TIMEOUT_MS,
                       "remove", canon, NULL);
    (void)jw_bt_sync_stock_saved_list();
    return rc < 0 ? -1 : 0;
}
