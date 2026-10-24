#include "cJSON.h"
#include "internal/platform/device_backend.h"
#include "internal/platform/bluetooth.h"
#include "internal/core/log.h"
#include "internal/storage/health.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <drm/drm.h>
#include <linux/fs.h>
#include <drm/drm_mode.h>
#include <linux/netlink.h>
#include <linux/input.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <dlfcn.h>
#include <dirent.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "internal/platform/power_request.h"
#include "internal/platform/weston_initd.h"
#include "internal/power/power_hard_cut.h"
#include <linux/i2c-dev.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* MLP1 backlight. The panel's low end is narrow and marginal: raw <= 55 flickers,
   and raw 56-58 is only faintly lit on a cool bench unit - it reads as FULLY BLACK
   once the device warms up, the room is bright, or the on-screen content is dark
   (LED forward voltage and the driver's minimum-on threshold drift with temp), so
   it is NOT a usable minimum. Above ~135 there is no visible change. (The original
   61 floor was conservative but reliably lit at its raw-65 minimum.)

   We map the 0-100% OSD range onto RAW_MIN..135. RAW_MIN is 56, and with the 5%
   minimum clamp the dimmest the user can select is raw 60 - reliably lit with
   headroom above the marginal 56-58 zone, yet still dimmer than the original
   raw-65 floor for night use. Percents below 5% (raw 56-59) are clamped away. */
#define JW_MLP1_BACKLIGHT_RAW_MIN 56
#define JW_MLP1_BACKLIGHT_RAW_MAX 135

/* Stock's userspace daemon (/loong/loong_power) watches the charger and refuses
   any backlight below raw 70: on a plug OR unplug transition it slams anything
   lower straight to raw 130 (~94% on our scale) and leaves it there. Bracketed
   on device: 70 held across three transitions in both directions, 69/68/64/59
   all jumped. Almost certainly deliberate -- the low end browns out when the
   charger moves the rail, which is why the floor sits well above the raw-59
   visible limit.

   We cannot outvote it and should not try; it is protecting the panel. Instead
   clamp UP to its floor while charging and restore the user's value on unplug.
   Writing exactly 70 is the point: it satisfies stock, so the two daemons never
   fight over the node. */
#define JW_MLP1_BACKLIGHT_CHARGE_MIN_RAW 70
#define JW_MLP1_CHARGE_SETTLE_MS 600
#define JW_MLP1_CHARGE_POLL_MS 400

#define JW_MLP1_BACKLIGHT_DIR "/sys/class/backlight/backlight"
#define JW_MLP1_BACKLIGHT_BRIGHTNESS JW_MLP1_BACKLIGHT_DIR "/brightness"
/* FB_BLANK control: 0 = unblank (on), 4 = powerdown (off). Independent of the
   brightness value, so blanking/unblanking preserves the user's brightness and
   loong_power doesn't fight it (unlike direct brightness writes to 0). */
#define JW_MLP1_BACKLIGHT_BL_POWER JW_MLP1_BACKLIGHT_DIR "/bl_power"
#define JW_MLP1_BACKLIGHT_ACTUAL JW_MLP1_BACKLIGHT_DIR "/actual_brightness"
#define JW_MLP1_BACKLIGHT_MAX JW_MLP1_BACKLIGHT_DIR "/max_brightness"

#define JW_MLP1_CPUFREQ_POLICY "/sys/devices/system/cpu/cpufreq/policy0"
#define JW_MLP1_SCHEDUTIL_RATE_LIMIT_US 50000
#define JW_MLP1_GPU_DEVFREQ "/sys/devices/platform/fde60000.gpu/devfreq/fde60000.gpu"
#define JW_MLP1_DMC_DEVFREQ "/sys/devices/platform/dmc/devfreq/dmc"
#define JW_MLP1_SOC_TEMP "/sys/class/thermal/thermal_zone0/temp"

#define JW_MLP1_LOONG_DB_PATH "/oem/loong/loong.db"
#define JW_MLP1_POWER_CFG "/oem/loong/record/config/loong_power.cfg"
#define JW_MLP1_POWER_DAEMON "loong_power"

#define JW_MLP1_PACTL_GET_VOLUME "pactl get-sink-volume @DEFAULT_SINK@ 2>/dev/null"
#define JW_MLP1_PACTL_SET_VOLUME "pactl set-sink-volume @DEFAULT_SINK@ %d%% 2>/dev/null"
#define JW_MLP1_PACTL_GET_DEFAULT_SINK "pactl get-default-sink 2>/dev/null"
#define JW_MLP1_PACTL_SET_DEFAULT_RK817 \
    "pactl set-default-sink alsa_output.platform-rk817-sound.stereo-fallback 2>/dev/null"
#define JW_MLP1_PACTL_SET_DEFAULT_HDMI \
    "pactl set-default-sink hdmi_out 2>/dev/null"
#define JW_MLP1_PACTL_RK817_SINK "alsa_output.platform-rk817-sound.stereo-fallback"
/* The HDMI sink jawakad loads on demand (device=hw:0,0 sink_name=hdmi_out); the
   vendor udev-detect name is never produced on this build, so we own the name. */
#define JW_MLP1_PACTL_HDMI_SINK "hdmi_out"
/* USB-C audio. The OTG port role-switches to host on its own when a device is
   attached and snd_usb_audio binds it as a new ALSA card, but Leaf's trimmed
   pulse-default.pa has no module-udev-detect, so pulse never learns about it.
   Load an explicit sink the same way HDMI does, on a name we own, rather than
   turning udev-detect on — that would duplicate both the rk817 sink (already
   loaded statically as alsa_output.hw_1_0) and the HDMI sink we manage here. */
#define JW_MLP1_PACTL_USB_SINK "usb_out"
#define JW_MLP1_PACTL_SET_DEFAULT_USB \
    "pactl set-default-sink usb_out 2>/dev/null"
#define JW_MLP1_PLAYBACK_PATH_CMD "amixer -c 1 cget numid=13 2>/dev/null"
#define JW_MLP1_PLAYBACK_PATH_SPK "amixer -c 1 cset numid=13 2 >/dev/null 2>&1"
#define JW_MLP1_PLAYBACK_PATH_HP  "amixer -c 1 cset numid=13 3 >/dev/null 2>&1"
#define JW_MLP1_PLAYBACK_PATH_BT  "amixer -c 1 cset numid=13 5 >/dev/null 2>&1"
/* Playback Path item #0 = OFF: disconnects the codec output stage. Used to mute
   before suspend so the powered-but-idle analog output doesn't hiss. */
#define JW_MLP1_PLAYBACK_PATH_OFF "amixer -c 1 cset numid=13 0 >/dev/null 2>&1"
#define JW_MLP1_HP_JACK_CMD "amixer -c 1 cget numid=1 2>/dev/null"
#define JW_MLP1_HDMI_STATUS "/sys/class/drm/card0-HDMI-A-1/status"
/* External speaker amp enable (0 = off, 1 = on). Stock loong_service drives it;
   in Leaf mode jawakad does (see jw__mlp1_speaker_gate_sync). */
#define JW_MLP1_SPK_CTL "/sys/kernel/powerCtrl/spk_ctl"
#define JW_MLP1_RK817_PCM_STATUS "/proc/asound/card1/pcm0p/sub0/status"
/* Every rk817 player opens this node, and inotify reports each open and close
   (devtmpfs delivers the events). The speaker gate follows them. */
#define JW_MLP1_RK817_PCM_DEV "/dev/snd/pcmC1D0p"
/* Without a watch (the node missing), poll the PCM state this often instead
   and try to arm the watch again. */
#define JW_MLP1_SPK_GATE_FALLBACK_POLL_MS 1000

/* The rk817 DAC (ALSA numid=16) is the dominant hardware loudness control and is
   pinned to a fixed level at boot (platform.d/00-audio-init.sh). The user-facing
   volume runs through PulseAudio's software sink volume above. PulseAudio can,
   however, drift this hardware element low (we have seen it stuck at 0 = muted
   and at ~167 = barely audible), leaving the speaker silent no matter the sink %.
   This self-heal re-asserts the pin whenever it has fallen below it. Keep the
   value (210) in sync with 00-audio-init.sh. */
#define JW_MLP1_DAC_PIN 210
#define JW_MLP1_ENSURE_DAC_FLOOR \
    "v=$(amixer -c 1 cget numid=16 2>/dev/null | " \
    "sed -n 's/.*: values=\\([0-9]*\\).*/\\1/p' | head -1); " \
    "if [ -n \"$v\" ] && [ \"$v\" -lt 210 ]; then " \
    "amixer -c 1 cset numid=16 210,210 >/dev/null 2>&1; fi"
/* Suspend/resume the PulseAudio sink around a system suspend. Suspending the sink
   closes the ALSA device, which powers the codec output stage down through the
   driver's DAPM sequence (pop-free, unlike an amixer Playback Path switch) - the
   same quiescing that happens when audio idles, which is why an idle sleep never
   hisses. Resuming re-opens the device so a game/music stream continues (a raw
   `echo mem` otherwise leaves the sink and its ALSA device stale = silent). */
#define JW_MLP1_PA_SUSPEND_SINK "pactl suspend-sink @DEFAULT_SINK@ 1 >/dev/null 2>&1"
#define JW_MLP1_PA_RESUME_SINK  "pactl suspend-sink @DEFAULT_SINK@ 0 >/dev/null 2>&1"
/* The rk817 headphone output amp has no external gate (unlike the speaker's
   spk_ctl), so it stays biased through `echo mem` and hisses once the SoC's I2S
   clocks stop. The codec's Playback Path control (numid=13) is a no-op at the
   register level, so it cannot power the amp down. Instead power down just the
   analog headphone stage over i2c (bus 0, addr 0x20): AHP_CFG0 0x3d=0xe0 and its
   charge pump AHP_CP 0x3f=0x09 — the same values the codec driver uses, so it is
   pop-free. It touches ONLY the headphone amp, not the shared DAC
   (DDAC_MUTE_MIXCTL/ADAC_CFG1), so the speaker path (same DAC) is unaffected.
   An earlier attempt muted the DAC in this path and lost PulseAudio, but that
   was the wake-time RLIMIT_RTTIME kill (the refill after resume ran at sleep
   clocks), not the mute: muting the DAC under a live stream leaves PulseAudio's
   realtime thread at ~110 ms of its budget (measured 2026-09-28). The two register values are read and restored on
   resume (see the SLEEP action). */
#define JW_MLP1_HP_POWERDOWN \
    "i2cset -f -y 0 0x20 0x3d 0xe0 >/dev/null 2>&1; " \
    "i2cset -f -y 0 0x20 0x3f 0x09 >/dev/null 2>&1"
#define JW_MLP1_WIFI_PROC "/proc/net/wireless"
#define JW_MLP1_SECONDARY_SOURCE_ID "secondary_sd"
#define JW_MLP1_SECONDARY_LABEL "Secondary SD"
#define JW_MLP1_LAUNCHER_SOURCE_ID "launcher_sd"
#define JW_MLP1_LAUNCHER_LABEL "Launcher SD"
#define JW_MLP1_FSCK_FAT "/usr/sbin/fsck.fat"
#define JW_MLP1_STORAGE_DEBOUNCE_MS 750

/* The stock loong_light daemon owns the AW20036 LED ring. It reads this JSON
   config and applies it on SIGUSR1 (write cfg, then signal). We cooperate with
   it rather than fighting its refresh loop. */
#define JW_MLP1_LED_CFG  "/oem/loong/record/config/loong_light.cfg"
#define JW_MLP1_LED_DAEMON "loong_light"

#define JW_MLP1_ADBD "/usr/bin/adbd"
#define JW_MLP1_USB_CONFIG "/etc/.usb_config"
#define JW_MLP1_USB_GADGET "/etc/init.d/S50usb-gadget.sh"
#define JW_MLP1_ADB_MARKER_FILE "adb-enabled"
#define JW_MLP1_BOOT_SPLASH_DISABLED_FILE "boot-splash-disabled"
#define JW_MLP1_ROOTFS_RW_CMD \
    "(mount -o remount,rw / >/dev/null 2>&1 || " \
    "mount -o remount,rw /dev/root / >/dev/null 2>&1)"
#define JW_MLP1_ADB_ENABLE_CMD \
    JW_MLP1_ROOTFS_RW_CMD " || exit 1; " \
    "chattr -i " JW_MLP1_USB_CONFIG " >/dev/null 2>&1 || true; " \
    "printf 'usb_adb_en\\n' >" JW_MLP1_USB_CONFIG " && " \
    "chattr +i " JW_MLP1_USB_CONFIG " && sync"
#define JW_MLP1_ADB_DISABLE_CMD \
    JW_MLP1_ROOTFS_RW_CMD " || exit 1; " \
    "chattr -i " JW_MLP1_USB_CONFIG " >/dev/null 2>&1 || true; " \
    "printf 'usb_mtp_en\\n' >" JW_MLP1_USB_CONFIG " && sync"
#define JW_MLP1_USB_RESTART_CMD \
    JW_MLP1_USB_GADGET " restart >/dev/null 2>&1 || " \
    JW_MLP1_USB_GADGET " start >/dev/null 2>&1"

/* Run a shell command via /bin/sh -c (for commands with arguments/pipes). */
static int jw__exec_shell(const char *cmd) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static bool jw__env_truthy(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0 &&
           strcmp(value, "false") != 0 && strcmp(value, "no") != 0;
}

typedef struct {
    int uevent_fd;
    int last_present;
    int last_mounted;
    bool pending_storage_event;
    long long debounce_until_ms;
    /* Brightness Leaf last applied, so a charger transition can restore it.
       The platform layer never sees the settings DB -- it is handed a percent
       over IPC -- so what it wrote itself is the only record of user intent. */
    int last_applied_raw;
    bool pending_charge_event;
    long long charge_settle_until_ms;
    long long charge_poll_next_ms;
    int last_ac_online;
    bool resume_mount_repair_pending;
    long long resume_mount_repair_at_ms;
} jw_mlp1_platform_data;

static int (*s_event_opend)(const char *id);
/* loong::PowerApi singleton accessor + standby method (C++ symbols; the method
   takes the singleton `this`). Used so suspend goes through loong's own power
   path — keeps loong_power in sync so the power button wakes on a single press
   (vs. our raw `echo mem`, which loong re-suspends on the first wake press). */
static int   (*s_write_config)(const char *param, const char *value,
                               const char *backup, int persist);
static bool s_loong_loaded;

static jw_platform_audio_output jw__mlp1_get_audio_output(void);
static unsigned jw__mlp1_get_audio_available_outputs(void);
static bool jw__mlp1_test_sound_active(void);
static void jw__mlp1_get_audio_volumes(int out[JW_PLATFORM_AUDIO_OUTPUT_COUNT]);
static int jw__mlp1_set_audio_output(jw_platform_audio_output output,
                                     jw_platform_result *out);
static void jw__mlp1_sync_sound_volume(jw_platform_audio_output output, int percent);
static int jw__mlp1_get_bluealsa_volume_percent(void);
static int jw__mlp1_set_bluealsa_volume_percent(int percent);
static int jw__mlp1_apply_power_cfg_live(void);
static int jw__mlp1_set_hdmi_output(int mode);
static bool jw__mlp1_hdmi_tv_active(void);
static bool jw__mlp1_hdmi_has_1080p120(void);
static int jw__mlp1_jack_input_state(void);
static bool jw__mlp1_bt_audio_present(void);
static jw_platform_audio_output jw__mlp1_desired_audio_output(bool allow_hdmi);
static unsigned jw__mlp1_audio_tick(jw_platform_context *ctx);
static void jw__mlp1_pcm_watch_arm(void);
static void jw__mlp1_speaker_gate_sync(bool force);
static void jw__mlp1_audio_reconcile(jw_platform_context *ctx, const char *reason);

/* The user's Refresh Rate setting (60/100/120), cached when set so the HDMI apply
   can choose 1080p120 vs the crisp 720p60. -1 until known; the HDMI apply then
   falls back to the live panel mode (which boots at the persisted rate). */
static int s_mlp1_target_refresh_hz = -1;

/* HDMI output mode (0 off / 1 4:3 / 2 stretch); last applied, -1 until set. */
enum { JW_MLP1_HDMI_OFF = 0, JW_MLP1_HDMI_4_3 = 1, JW_MLP1_HDMI_STRETCH = 2 };
static int s_mlp1_hdmi_mode = -1;

/* Color-temperature target in K, last value we programmed into the CRTC gamma
   LUT. -1 until set. The LUT survives Weston restarts (see the color
   temperature section below) but a cold power cycle clears it, so jawakad
   replays the persisted value at startup; this cache just answers
   platform-status without re-reading the ramp. */
static int s_mlp1_color_temp_k = -1;

static long long jw__monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (long long)ts.tv_sec * 1000LL + (long long)(ts.tv_nsec / 1000000L);
}

static bool jw__mlp1_source_is_secondary(const char *source_id) {
    return !source_id || !source_id[0] ||
           strcmp(source_id, JW_MLP1_SECONDARY_SOURCE_ID) == 0;
}

static void jw__mlp1_secondary_mount(const jw_platform_context *ctx,
                                     char *out, size_t out_size) {
    const char *primary = ctx && ctx->sdcard_root[0]
                              ? ctx->sdcard_root : "/mnt/sdcard";
    const char *paths = getenv("SDCARD_PATHS");
    if (paths && paths[0]) {
        const char *cursor = paths;
        while (*cursor) {
            const char *separator = strchr(cursor, ':');
            size_t length = separator ? (size_t)(separator - cursor)
                                      : strlen(cursor);
            bool known_mount =
                (length == strlen("/mnt/sdcard") &&
                 strncmp(cursor, "/mnt/sdcard", length) == 0) ||
                (length == strlen("/media/sdcard1") &&
                 strncmp(cursor, "/media/sdcard1", length) == 0);
            if (known_mount &&
                !(strlen(primary) == length &&
                  strncmp(cursor, primary, length) == 0) &&
                length + 1u <= out_size) {
                memcpy(out, cursor, length);
                out[length] = '\0';
                return;
            }
            if (!separator) {
                break;
            }
            cursor = separator + 1;
        }
    }
    snprintf(out, out_size, "%s",
             strcmp(primary, "/media/sdcard1") == 0
                 ? "/mnt/sdcard" : "/media/sdcard1");
}

static bool jw__mlp1_mount_lookup(const char *wanted_mount,
                                  char *device, size_t device_size) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        return false;
    }

    char dev[256];
    char mount[256];
    char type[64];
    bool active = false;
    while (fscanf(fp, "%255s %255s %63s %*s %*d %*d\n",
                  dev, mount, type) == 3) {
        if (strcmp(mount, wanted_mount) == 0) {
            if (device && device_size > 0) {
                snprintf(device, device_size, "%s", dev);
            }
            active = true;
            break;
        }
    }
    fclose(fp);
    return active;
}

static bool jw__mlp1_mount_has_option(const char *wanted_mount,
                                      const char *wanted_option) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        return false;
    }

    char dev[256];
    char mount[256];
    char type[64];
    char options[512];
    bool found = false;
    while (fscanf(fp, "%255s %255s %63s %511s %*d %*d\n",
                  dev, mount, type, options) == 4) {
        if (strcmp(mount, wanted_mount) != 0) {
            continue;
        }
        char *save = NULL;
        for (char *option = strtok_r(options, ",", &save);
             option;
             option = strtok_r(NULL, ",", &save)) {
            if (strcmp(option, wanted_option) == 0) {
                found = true;
                break;
            }
        }
        break;
    }
    fclose(fp);
    return found;
}

/* Resume can leave this process' cwd on a detached instance of the launcher
   card. Invoking `mount` through a shell from that cwd is unreliable (the MLP1
   shell emits getcwd failures and, on the observed firmware path, the remount
   never takes effect). Execute the rootfs mount binary directly from `/` and
   keep the wait bounded so a damaged card cannot wedge the daemon on wake. */
static int jw__mlp1_remount_exec_bounded(const char *mount, bool keep_read_only) {
    /* A card the kernel flipped read-only after a filesystem error, or one
       held for repair, must never come back writable through an exec repair. */
    char *const argv[] = {
        (char *)"mount",
        (char *)"-o",
        keep_read_only
            ? (char *)"remount,ro,exec,nosuid,nodev,noatime,nodiratime"
            : (char *)"remount,rw,exec,nosuid,nodev,noatime,nodiratime",
        (char *)mount,
        NULL,
    };
    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }
    if (pid == 0) {
        if (chdir("/") != 0) {
            _exit(126);
        }
        int null_fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (null_fd >= 0) {
            (void)dup2(null_fd, STDOUT_FILENO);
            (void)dup2(null_fd, STDERR_FILENO);
            close(null_fd);
        }
        execv("/bin/mount", argv);
        _exit(127);
    }

    int status = 0;
    for (int attempt = 0; attempt < 10; attempt++) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) {
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
        }
        if (waited < 0 && errno != EINTR) {
            return -1;
        }
        usleep(50000);
    }

    (void)kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        /* retry */
    }
    return -1;
}

/* Repair hold recorded on internal storage for the card behind device. */
static bool jw__mlp1_device_repair_held(const char *device) {
    jw_storage_probe_env env;
    jw_storage_probe_env_default(&env);
    char uuid[JW_STORAGE_UUID_MAX];
    return device && device[0] &&
           jw_storage_device_uuid(&env, device, uuid, sizeof(uuid)) &&
           jw_storage_repair_hold_for_uuid(&env, uuid, NULL, 0) !=
               JW_STORAGE_REPAIR_NONE;
}

static int jw__mlp1_remount_exec(const char *mount) {
    char device[256] = "";
    if (!mount ||
        (strcmp(mount, "/mnt/sdcard") != 0 &&
         strcmp(mount, "/media/sdcard1") != 0) ||
        !jw__mlp1_mount_lookup(mount, device, sizeof(device))) {
        return -1;
    }

    if (!jw__mlp1_mount_has_option(mount, "noexec")) {
        return 0;
    }
    bool keep_read_only = jw__mlp1_mount_has_option(mount, "ro") ||
                          jw__mlp1_device_repair_held(device);
    if (jw__mlp1_remount_exec_bounded(mount, keep_read_only) != 0) {
        return -1;
    }
    return jw__mlp1_mount_has_option(mount, "noexec") ? -1 : 0;
}

static bool jw__mlp1_secondary_device(const jw_platform_context *ctx,
                                      char *out, size_t out_size) {
    char secondary_mount[PATH_MAX];
    char primary_device[PATH_MAX] = {0};
    jw__mlp1_secondary_mount(ctx, secondary_mount, sizeof(secondary_mount));
    if (jw__mlp1_mount_lookup(secondary_mount, out, out_size)) {
        return true;
    }
    if (ctx && ctx->sdcard_root[0]) {
        (void)jw__mlp1_mount_lookup(ctx->sdcard_root, primary_device,
                                    sizeof(primary_device));
    }
    const char *candidates[] = {"/dev/mmcblk1p1", "/dev/mmcblk3p1"};
    for (size_t i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
        if (access(candidates[i], F_OK) == 0 &&
            strcmp(candidates[i], primary_device) != 0) {
            snprintf(out, out_size, "%s", candidates[i]);
            return true;
        }
    }
    if (out && out_size > 0) {
        out[0] = '\0';
    }
    return false;
}

static bool jw__mlp1_block_present(const jw_platform_context *ctx) {
    char device[PATH_MAX];
    return jw__mlp1_secondary_device(ctx, device, sizeof(device));
}

static bool jw__mlp1_mount_is_active(const jw_platform_context *ctx) {
    char mount[PATH_MAX];
    jw__mlp1_secondary_mount(ctx, mount, sizeof(mount));
    return jw__mlp1_mount_lookup(mount, NULL, 0);
}

static bool jw__mlp1_storage_busy(const jw_platform_context *ctx) {
    char mount[PATH_MAX];
    char command[PATH_MAX + 128];
    jw__mlp1_secondary_mount(ctx, mount, sizeof(mount));
    if (!jw__mlp1_mount_lookup(mount, NULL, 0)) {
        return false;
    }

    if (access("/usr/bin/fuser", X_OK) == 0 ||
        access("/bin/fuser", X_OK) == 0 ||
        access("/sbin/fuser", X_OK) == 0) {
        snprintf(command, sizeof(command), "fuser -m %s >/dev/null 2>&1", mount);
        return jw__exec_shell(command) == 0;
    }

    if (access("/usr/bin/lsof", X_OK) == 0 ||
        access("/bin/lsof", X_OK) == 0 ||
        access("/sbin/lsof", X_OK) == 0) {
        snprintf(command, sizeof(command),
                 "lsof +f -- %s 2>/dev/null | "
                 "awk 'NR > 1 { found = 1 } END { exit found ? 0 : 1 }'",
                 mount);
        return jw__exec_shell(command) == 0;
    }

    return false;
}

static int jw__mlp1_mount_secondary_if_needed(jw_platform_context *ctx) {
    char mount[PATH_MAX];
    char device[PATH_MAX];
    char command[PATH_MAX * 2 + 256];
    jw__mlp1_secondary_mount(ctx, mount, sizeof(mount));
    if (!jw__mlp1_secondary_device(ctx, device, sizeof(device))) {
        return -1;
    }

    mkdir(mount, 0755);

    if (jw__mlp1_mount_lookup(mount, NULL, 0)) {
        return jw__mlp1_remount_exec(mount);
    }
    if (jw__mlp1_device_repair_held(device)) {
        /* The boot repair runner owns this card until it is verified. */
        jw_log_warn("storage hotplug: %s is held for repair; not mounting", device);
        return -1;
    }

    snprintf(command, sizeof(command),
             "mount -t vfat -o rw,exec,nosuid,nodev,noatime,nodiratime,"
             "fmask=0022,dmask=0022,iocharset=utf8,shortname=mixed,"
             "errors=remount-ro %s %s >/dev/null 2>&1", device, mount);
    return jw__exec_shell(command);
}

/* Defined further down; needed here so init can seed the brightness/charger
   state. Seeding must happen at init, not lazily on the first charger event --
   by then stock may already have overwritten the value we want to remember. */
static int jw__read_int_file(const char *path);
static int jw__mlp1_charger_online(void);

static int jw__mlp1_open_uevent_socket(void) {
    /* SOCK_CLOEXEC: this daemon forks and execs -- RetroArch, the conversion
       pass, every helper script -- and without it each one inherits a live
       netlink socket it has no idea about, still bound to the uevent group and
       still queueing hotplug messages against the kernel's buffer. */
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        return -1;
    }

    struct sockaddr_nl addr;
    memset(&addr, 0, sizeof(addr));
    addr.nl_family = AF_NETLINK;
    addr.nl_groups = 1;

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }

    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fd;
}

static int jw__mlp1_init(jw_platform_context *ctx) {
    jw_mlp1_platform_data *data = (jw_mlp1_platform_data *)calloc(1, sizeof(*data));
    if (!data) {
        return -1;
    }

    data->last_applied_raw = jw__read_int_file(JW_MLP1_BACKLIGHT_ACTUAL);
    data->last_ac_online = jw__mlp1_charger_online();
    data->uevent_fd = jw__mlp1_open_uevent_socket();
    if (data->uevent_fd < 0) {
        jw_log_warn("storage hotplug: uevent socket unavailable: %s", strerror(errno));
    }

    if (jw__mlp1_block_present(ctx)) {
        if (jw__mlp1_mount_secondary_if_needed(ctx) == 0) {
            jw_log_info("storage hotplug: secondary SD mounted/remounted");
        } else {
            jw_log_warn("storage hotplug: secondary SD mount/remount failed");
        }
    }

    data->last_present = jw__mlp1_block_present(ctx) ? 1 : 0;
    data->last_mounted = jw__mlp1_mount_is_active(ctx) ? 1 : 0;
    ctx->backend_data = data;
    return 0;
}

static void jw__mlp1_bt_monitor_stop(void);

static void jw__mlp1_shutdown(jw_platform_context *ctx) {
    jw__mlp1_bt_monitor_stop();
    jw_mlp1_platform_data *data = ctx ? (jw_mlp1_platform_data *)ctx->backend_data : NULL;
    if (data) {
        if (data->uevent_fd >= 0) {
            close(data->uevent_fd);
        }
        free(data);
    }
    if (ctx) {
        ctx->backend_data = NULL;
    }
}

static int jw__read_int_file(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    int value = -1;
    if (fscanf(fp, "%d", &value) != 1) {
        value = -1;
    }
    fclose(fp);
    return value;
}

static int jw__write_int_file(const char *path, int value) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        return -1;
    }

    int rc = fprintf(fp, "%d\n", value) > 0 ? 0 : -1;
    if (fclose(fp) != 0) {
        rc = -1;
    }
    return rc;
}

static int jw__write_text_file(const char *path, const char *value) {
    FILE *fp = fopen(path, "w");
    if (!fp) {
        return -1;
    }

    int rc = fputs(value ? value : "", fp) >= 0 ? 0 : -1;
    if (fclose(fp) != 0) {
        rc = -1;
    }
    return rc;
}

static void jw__trim_line(char *s) {
    if (!s) {
        return;
    }
    size_t len = strlen(s);
    while (len > 0 &&
           (s[len - 1] == '\n' || s[len - 1] == '\r' ||
            s[len - 1] == ' ' || s[len - 1] == '\t')) {
        s[--len] = '\0';
    }
    char *start = s;
    while (*start == ' ' || *start == '\t') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
}

static int jw__read_line_file(const char *path, char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }
    if (!fgets(out, (int)out_size, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    jw__trim_line(out);
    return 0;
}

static int jw__join_sysfs_path(char *out, size_t out_size,
                               const char *base, const char *leaf) {
    if (!out || out_size == 0 || !base || !leaf) {
        return -1;
    }
    int n = snprintf(out, out_size, "%s/%s", base, leaf);
    return (n > 0 && n < (int)out_size) ? 0 : -1;
}

static bool jw__token_list_has(const char *list, const char *token) {
    if (!list || !token || !token[0]) {
        return false;
    }
    size_t token_len = strlen(token);
    const char *p = list;
    while (*p) {
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }
        const char *start = p;
        while (*p && !isspace((unsigned char)*p)) {
            p++;
        }
        size_t len = (size_t)(p - start);
        if (len == token_len && strncmp(start, token, token_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool jw__freq_list_has(const char *list, int freq) {
    char token[32];
    snprintf(token, sizeof(token), "%d", freq);
    return jw__token_list_has(list, token);
}

static void jw__mlp1_perf_domain_status(jw_platform_perf_domain domain,
                                        const char *base,
                                        jw_platform_perf_domain_status *out) {
    if (!out) {
        return;
    }

    const char *name = "CPU";
    const char *governor_leaf = "scaling_governor";
    const char *cur_leaf = "scaling_cur_freq";
    const char *set_leaf = "scaling_setspeed";
    const char *available_freq_leaf = "scaling_available_frequencies";
    const char *available_governor_leaf = "scaling_available_governors";
    if (domain == JW_PLATFORM_PERF_DOMAIN_GPU) {
        name = "GPU";
        governor_leaf = "governor";
        cur_leaf = "cur_freq";
        set_leaf = "userspace/set_freq";
        available_freq_leaf = "available_frequencies";
        available_governor_leaf = "available_governors";
    } else if (domain == JW_PLATFORM_PERF_DOMAIN_DMC) {
        name = "DMC";
        governor_leaf = "governor";
        cur_leaf = "cur_freq";
        set_leaf = "userspace/set_freq";
        available_freq_leaf = "available_frequencies";
        available_governor_leaf = "available_governors";
    }

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", name);
    out->current_freq = -1;
    out->set_freq = -1;

    char path[PATH_MAX];
    if (!base || jw__join_sysfs_path(path, sizeof(path), base, governor_leaf) != 0 ||
        access(path, R_OK) != 0) {
        return;
    }

    out->supported = true;
    (void)jw__read_line_file(path, out->governor, sizeof(out->governor));
    if (jw__join_sysfs_path(path, sizeof(path), base, cur_leaf) == 0) {
        out->current_freq = jw__read_int_file(path);
    }
    if (jw__join_sysfs_path(path, sizeof(path), base, set_leaf) == 0) {
        out->set_freq = jw__read_int_file(path);
    }
    if (jw__join_sysfs_path(path, sizeof(path), base, available_governor_leaf) == 0) {
        (void)jw__read_line_file(path, out->available_governors,
                                 sizeof(out->available_governors));
    }
    if (jw__join_sysfs_path(path, sizeof(path), base, available_freq_leaf) == 0) {
        (void)jw__read_line_file(path, out->available_frequencies,
                                 sizeof(out->available_frequencies));
    }
}

static const char *jw__mlp1_perf_base(jw_platform_perf_domain domain) {
    switch (domain) {
        case JW_PLATFORM_PERF_DOMAIN_GPU: return JW_MLP1_GPU_DEVFREQ;
        case JW_PLATFORM_PERF_DOMAIN_DMC: return JW_MLP1_DMC_DEVFREQ;
        case JW_PLATFORM_PERF_DOMAIN_CPU:
        default: return JW_MLP1_CPUFREQ_POLICY;
    }
}

static void jw__mlp1_get_performance_status(jw_platform_context *ctx,
                                            jw_platform_perf_status *out) {
    (void)ctx;
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->soc_temp_c = -1;
    for (int i = 0; i < JW_PLATFORM_PERF_DOMAIN_COUNT; i++) {
        jw__mlp1_perf_domain_status((jw_platform_perf_domain)i,
                                    jw__mlp1_perf_base((jw_platform_perf_domain)i),
                                    &out->domains[i]);
    }

    int temp = jw__read_int_file(JW_MLP1_SOC_TEMP);
    if (temp >= 1000) {
        out->soc_temp_c = temp / 1000;
    } else if (temp >= 0) {
        out->soc_temp_c = temp;
    }

    out->supported = out->domains[JW_PLATFORM_PERF_DOMAIN_CPU].supported &&
                     out->domains[JW_PLATFORM_PERF_DOMAIN_GPU].supported &&
                     out->domains[JW_PLATFORM_PERF_DOMAIN_DMC].supported;
    snprintf(out->message, sizeof(out->message), "%s",
             out->supported ? "performance ready" : "performance partially unavailable");
}

/* schedutil's default 10 ms rate limit lets it step the CPU clock up to 100
   times a second, and on this board every step is a VDD write to the tcs4525
   regulator over the i2c bus it shares with the rk817 PMIC and codec. At idle
   that measured 45-67 steps/s with the sugov:0 kthread near 4% CPU; 50 ms
   brings it under 20/s. The tunable lives in the governor's directory, which
   the kernel recreates at its default whenever schedutil is selected again,
   so it is written after every governor write, not once at boot. */
static void jw__mlp1_tune_schedutil(const char *base) {
    char path[PATH_MAX];
    if (jw__join_sysfs_path(path, sizeof(path), base,
                            "schedutil/rate_limit_us") != 0 ||
        access(path, W_OK) != 0) {
        return;
    }
    if (jw__write_int_file(path, JW_MLP1_SCHEDUTIL_RATE_LIMIT_US) != 0) {
        jw_log_warn("performance: schedutil rate limit write failed: %s",
                    strerror(errno));
    }
}

static int jw__mlp1_apply_perf_domain(jw_platform_perf_domain domain,
                                      const jw_platform_perf_domain_request *request,
                                      char *message,
                                      size_t message_size) {
    if (!request || !request->governor[0]) {
        return 0;
    }

    const char *base = jw__mlp1_perf_base(domain);
    jw_platform_perf_domain_status status;
    jw__mlp1_perf_domain_status(domain, base, &status);
    if (!status.supported) {
        snprintf(message, message_size, "%s performance unavailable", status.name);
        return -1;
    }
    if (!jw__token_list_has(status.available_governors, request->governor)) {
        snprintf(message, message_size, "%s governor unsupported: %s",
                 status.name, request->governor);
        return -1;
    }
    if (request->frequency >= 0) {
        if (strcmp(request->governor, "userspace") != 0) {
            snprintf(message, message_size, "%s fixed frequency requires userspace governor",
                     status.name);
            return -1;
        }
        if (!jw__freq_list_has(status.available_frequencies, request->frequency)) {
            snprintf(message, message_size, "%s frequency unsupported: %d",
                     status.name, request->frequency);
            return -1;
        }
    }

    char path[PATH_MAX];
    char value[JW_PLATFORM_PERF_VALUE_MAX + 4];
    const char *governor_leaf = (domain == JW_PLATFORM_PERF_DOMAIN_CPU)
                              ? "scaling_governor"
                              : "governor";
    if (jw__join_sysfs_path(path, sizeof(path), base, governor_leaf) != 0) {
        snprintf(message, message_size, "%s governor path invalid", status.name);
        return -1;
    }
    snprintf(value, sizeof(value), "%s\n", request->governor);
    if (jw__write_text_file(path, value) != 0) {
        snprintf(message, message_size, "%s governor write failed: %s",
                 status.name, strerror(errno));
        return -1;
    }
    if (domain == JW_PLATFORM_PERF_DOMAIN_CPU &&
        strcmp(request->governor, "schedutil") == 0) {
        jw__mlp1_tune_schedutil(base);
    }

    if (request->frequency >= 0) {
        const char *set_leaf = (domain == JW_PLATFORM_PERF_DOMAIN_CPU)
                             ? "scaling_setspeed"
                             : "userspace/set_freq";
        if (jw__join_sysfs_path(path, sizeof(path), base, set_leaf) != 0) {
            snprintf(message, message_size, "%s frequency path invalid", status.name);
            return -1;
        }
        if (jw__write_int_file(path, request->frequency) != 0) {
            snprintf(message, message_size, "%s frequency write failed: %s",
                     status.name, strerror(errno));
            return -1;
        }
    }

    return 0;
}

static void jw__mlp1_apply_performance(jw_platform_context *ctx,
                                       const jw_platform_perf_request *request,
                                       jw_platform_result *out) {
    (void)ctx;
    char message[JW_PLATFORM_MAX_MESSAGE];
    for (int i = 0; i < JW_PLATFORM_PERF_DOMAIN_COUNT; i++) {
        message[0] = '\0';
        if (jw__mlp1_apply_perf_domain((jw_platform_perf_domain)i,
                                       &request->domains[i],
                                       message, sizeof(message)) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   message[0] ? message : "performance apply failed");
            return;
        }
    }
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "performance applied");
}

/* Map raw backlight value (56-135) to 0-100% for the user-facing OSD. */
static int jw__brightness_raw_to_percent(int raw, int max_raw) {
    (void)max_raw;
    if (raw < 0) {
        return -1;
    }
    if (raw <= JW_MLP1_BACKLIGHT_RAW_MIN) return 0;
    if (raw >= JW_MLP1_BACKLIGHT_RAW_MAX) return 100;

    int range = JW_MLP1_BACKLIGHT_RAW_MAX - JW_MLP1_BACKLIGHT_RAW_MIN;
    return ((raw - JW_MLP1_BACKLIGHT_RAW_MIN) * 100 + range / 2) / range;
}

/* Map 0-100% OSD value to the usable raw range (56-135). */
static int jw__brightness_percent_to_raw(int percent, int max_raw) {
    (void)max_raw;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    int range = JW_MLP1_BACKLIGHT_RAW_MAX - JW_MLP1_BACKLIGHT_RAW_MIN;
    return JW_MLP1_BACKLIGHT_RAW_MIN + (percent * range + 50) / 100;
}

static int jw__mlp1_get_brightness_percent(void) {
    int max_raw = jw__read_int_file(JW_MLP1_BACKLIGHT_MAX);
    int raw = jw__read_int_file(JW_MLP1_BACKLIGHT_ACTUAL);
    if (raw < 0) {
        raw = jw__read_int_file(JW_MLP1_BACKLIGHT_BRIGHTNESS);
    }
    return jw__brightness_raw_to_percent(raw, max_raw);
}

static int jw__read_command_line(const char *cmd, char *out, size_t out_size) {
    if (!cmd || !out || out_size == 0) {
        return -1;
    }
    out[0] = '\0';
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    char *line = fgets(out, (int)out_size, fp);
    int close_rc = pclose(fp);
    if (!line || close_rc == -1) {
        out[0] = '\0';
        return -1;
    }
    out[strcspn(out, "\r\n")] = '\0';
    return out[0] ? 0 : -1;
}

static int jw__parse_percent_from_stream(FILE *fp) {
    if (!fp) {
        return -1;
    }

    char line[256];
    int percent = -1;
    while (fgets(line, sizeof(line), fp)) {
        char *cursor = line;
        while ((cursor = strchr(cursor, '[')) != NULL) {
            int val = 0;
            if (sscanf(cursor, "[%d%%]", &val) == 1 && val >= 0 && val <= 150) {
                percent = val > 100 ? 100 : val;
                break;
            }
            cursor++;
        }
        if (percent >= 0) {
            break;
        }
    }
    return percent;
}

/* The volume percent last read or set on the current output. The launcher's
   status poll asks every 5 s, and a fresh read is three popens (Playback Path,
   default sink, sink volume). jawakad makes every volume change itself, so its
   own sets keep this right; a route change drops it, and it is re-read at
   least once a minute in case something else moved the sink. */
#define JW_MLP1_VOLUME_CACHE_MS 60000
static int s_mlp1_volume_cache = -1;
static long long s_mlp1_volume_cache_ms = 0;

static void jw__mlp1_volume_cache_set(int percent) {
    s_mlp1_volume_cache = percent;
    s_mlp1_volume_cache_ms = jw__monotonic_ms();
}

static int jw__mlp1_read_volume_percent(void);

static int jw__mlp1_get_volume_percent(void) {
    if (s_mlp1_volume_cache >= 0 &&
        jw__monotonic_ms() - s_mlp1_volume_cache_ms < JW_MLP1_VOLUME_CACHE_MS) {
        return s_mlp1_volume_cache;
    }
    int percent = jw__mlp1_read_volume_percent();
    if (percent >= 0) {
        jw__mlp1_volume_cache_set(percent);
    }
    return percent;
}

static int jw__mlp1_read_volume_percent(void) {
    jw_platform_audio_output output = jw__mlp1_get_audio_output();
    if (output == JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH) {
        int bt_percent = jw__mlp1_get_bluealsa_volume_percent();
        if (bt_percent >= 0) {
            return bt_percent;
        }
        int volumes[JW_PLATFORM_AUDIO_OUTPUT_COUNT];
        jw__mlp1_get_audio_volumes(volumes);
        return volumes[JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH];
    }

    FILE *fp = popen(JW_MLP1_PACTL_GET_VOLUME, "r");
    if (!fp) {
        return -1;
    }

    char line[256];
    int percent = -1;
    while (fgets(line, sizeof(line), fp)) {
        /* pactl output: "Volume: front-left: 41943 /  64% / -11.63 dB, ..." */
        char *slash = strstr(line, "/");
        while (slash) {
            int val = 0;
            if (sscanf(slash + 1, " %d%%", &val) == 1 && val >= 0 && val <= 150) {
                percent = val > 100 ? 100 : val;
                break;
            }
            slash = strstr(slash + 1, "/");
        }
        if (percent >= 0) break;
    }
    pclose(fp);
    return percent;
}

static int jw__mlp1_set_volume_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;

    jw_platform_audio_output output = jw__mlp1_get_audio_output();
    int rc;
    if (output == JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH) {
        rc = jw__mlp1_set_bluealsa_volume_percent(percent);
        if (rc == 0) {
            jw__mlp1_sync_sound_volume(output, percent);
            jw__mlp1_volume_cache_set(percent);
        }
        return rc == 0 ? 0 : -1;
    }

    char cmd[128];
    snprintf(cmd, sizeof(cmd), JW_MLP1_PACTL_SET_VOLUME, percent);
    rc = jw__exec_shell(cmd);

    /* Safety net: ensure the hardware DAC has not been left stuck low/muted,
       which would silence the speaker regardless of the sink %. Re-assert the
       boot pin if it has drifted below it. Best-effort; its result does not
       affect whether the volume change itself succeeded. */
    (void)jw__exec_shell(JW_MLP1_ENSURE_DAC_FLOOR);

    if (rc == 0) {
        jw__mlp1_sync_sound_volume(output, percent);
        jw__mlp1_volume_cache_set(percent);
    }
    return rc == 0 ? 0 : -1;
}

static void jw__mlp1_get_wifi_status(jw_platform_status *out) {
    FILE *fp = fopen(JW_MLP1_WIFI_PROC, "r");
    if (!fp) {
        return;
    }

    char line[256];
    /* Skip header lines */
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return; }
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return; }

    /* Data line: " wlan0: 0000   95.   23.    0. ..." */
    if (fgets(line, sizeof(line), fp)) {
        int link = 0;
        if (sscanf(line, " %*[^:]: %*d %d.", &link) == 1) {
            out->wifi_connected = (link > 0) ? 1 : 0;
            if (link <= 0) {
                out->wifi_strength = 0;
            } else if (link < 40) {
                out->wifi_strength = 1;
            } else if (link < 70) {
                out->wifi_strength = 2;
            } else {
                out->wifi_strength = 3;
            }
        }
    }
    fclose(fp);
}

static void jw__loong_load(void) {
    if (s_loong_loaded) {
        return;
    }
    s_loong_loaded = true;

    void *handle = dlopen("/usr/lib/libloong_sdk.so", RTLD_NOW | RTLD_GLOBAL);
    if (!handle) {
        jw_log_error("loong: dlopen libloong_sdk.so failed: %s", dlerror());
        return;
    }

    *(void **)(&s_event_opend) = dlsym(handle, "EventOpend");
    if (!s_event_opend) {
        jw_log_error("loong: EventOpend symbol not found: %s", dlerror());
    }

    *(void **)(&s_write_config) = dlsym(handle, "WriteConfig");
    if (!s_write_config) {
        jw_log_warn("loong: WriteConfig symbol not found (stock settings sync will use DB fallback)");
    }
}


static char *jw__read_text_file(const char *path, long max_bytes) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long len = ftell(fp);
    if (len < 0 || len > max_bytes || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)len, fp);
    fclose(fp);
    buf[got] = '\0';
    return buf;
}

static int jw__write_text_file_atomic(const char *path, const char *text) {
    char tmp[512];
    int needed = snprintf(tmp, sizeof(tmp), "%s.umrk.%ld", path, (long)getpid());
    if (needed < 0 || needed >= (int)sizeof(tmp)) {
        return -1;
    }

    FILE *fp = fopen(tmp, "w");
    if (!fp) {
        return -1;
    }
    int ok = fputs(text, fp) >= 0 && fputc('\n', fp) != EOF;
    ok = fclose(fp) == 0 && ok;
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    chmod(tmp, 0644);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static int jw__mkdir_p(const char *dir) {
    if (!dir || !dir[0]) {
        return -1;
    }

    char tmp[JW_PLATFORM_MAX_PATH];
    int needed = snprintf(tmp, sizeof(tmp), "%s", dir);
    if (needed < 0 || needed >= (int)sizeof(tmp)) {
        return -1;
    }

    size_t len = strlen(tmp);
    while (len > 1 && tmp[len - 1] == '/') {
        tmp[--len] = '\0';
    }

    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
            *p = '/';
            return -1;
        }
        *p = '/';
    }

    return (mkdir(tmp, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int jw__mkdir_parent(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }

    char dir[JW_PLATFORM_MAX_PATH];
    int needed = snprintf(dir, sizeof(dir), "%s", path);
    if (needed < 0 || needed >= (int)sizeof(dir)) {
        return -1;
    }

    char *slash = strrchr(dir, '/');
    if (!slash) {
        return 0;
    }
    if (slash == dir) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }
    return jw__mkdir_p(dir);
}

static int jw__mlp1_state_marker_path(jw_platform_context *ctx,
                                      const char *override_env,
                                      const char *file_name,
                                      char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return -1;
    }

    const char *override = override_env ? getenv(override_env) : NULL;
    if (override && override[0]) {
        int needed = snprintf(out, out_size, "%s", override);
        return needed >= 0 && needed < (int)out_size ? 0 : -1;
    }

    const char *state = getenv("UMRK_INTERNAL_DATA_PATH");
    if (state && state[0]) {
        int needed = snprintf(out, out_size, "%s/%s", state, file_name);
        return needed >= 0 && needed < (int)out_size ? 0 : -1;
    }

    /* Last resort when no env is set: the launcher control-state root lives at the
       SD root as .umrk/<platform> (NOT under the release-managed .system tree). */
    const char *sd = (ctx && ctx->sdcard_root[0]) ? ctx->sdcard_root : "/mnt/sdcard";
    int needed = snprintf(out, out_size, "%s/.umrk/mlp1/%s", sd, file_name);
    return needed >= 0 && needed < (int)out_size ? 0 : -1;
}

static int jw__mlp1_adb_marker_path(jw_platform_context *ctx,
                                    char *out, size_t out_size) {
    return jw__mlp1_state_marker_path(ctx, "UMRK_ADB_MARKER_PATH",
                                      JW_MLP1_ADB_MARKER_FILE,
                                      out, out_size);
}

static bool jw__mlp1_adb_intent_enabled(jw_platform_context *ctx) {
    char path[JW_PLATFORM_MAX_PATH];
    return jw__mlp1_adb_marker_path(ctx, path, sizeof(path)) == 0 &&
           access(path, F_OK) == 0;
}

static int jw__mlp1_set_adb_intent(jw_platform_context *ctx, bool enabled) {
    char path[JW_PLATFORM_MAX_PATH];
    if (jw__mlp1_adb_marker_path(ctx, path, sizeof(path)) != 0) {
        return -1;
    }

    if (!enabled) {
        return (unlink(path) == 0 || errno == ENOENT) ? 0 : -1;
    }

    if (jw__mkdir_parent(path) != 0) {
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        return -1;
    }
    int ok = fputs("1\n", fp) >= 0;
    return fclose(fp) == 0 && ok ? 0 : -1;
}

static int jw__mlp1_boot_splash_disabled_path(jw_platform_context *ctx,
                                              char *out, size_t out_size) {
    return jw__mlp1_state_marker_path(ctx, "UMRK_BOOT_SPLASH_DISABLED_PATH",
                                      JW_MLP1_BOOT_SPLASH_DISABLED_FILE,
                                      out, out_size);
}

static bool jw__mlp1_boot_splash_enabled(jw_platform_context *ctx) {
    char path[JW_PLATFORM_MAX_PATH];
    if (jw__mlp1_boot_splash_disabled_path(ctx, path, sizeof(path)) != 0) {
        return true;
    }
    return access(path, F_OK) != 0;
}

static int jw__mlp1_set_boot_splash(jw_platform_context *ctx, bool enabled) {
    char path[JW_PLATFORM_MAX_PATH];
    if (jw__mlp1_boot_splash_disabled_path(ctx, path, sizeof(path)) != 0) {
        return -1;
    }

    if (enabled) {
        return (unlink(path) == 0 || errno == ENOENT) ? 0 : -1;
    }

    if (jw__mkdir_parent(path) != 0) {
        return -1;
    }

    FILE *fp = fopen(path, "w");
    if (!fp) {
        return -1;
    }
    int ok = fputs("1\n", fp) >= 0;
    return fclose(fp) == 0 && ok ? 0 : -1;
}

/* ── Display refresh rate (60/100/120 Hz) ────────────────────────────────
   The MLP1 DSI panel advertises a single fixed 720x960@60 mode, but the
   rockchip DSI driver accepts a runtime modeline above it (verified). Weston
   takes its output mode from weston.ini; its HOME is /userdata/root, so a
   ~/.config/weston.ini override (a copy of the stock /etc/xdg config plus a
   `mode=` line in [output]) takes precedence with no stock-file edit. Above 60
   = write the override; 60 Hz = remove it (Weston falls back to the stock 60 Hz
   preferred mode). The override lives on rootfs and persists across reboots,
   so no boot hook is needed — Weston reads it at S49 every boot. */
#define JW_MLP1_WESTON_STOCK_INI    "/etc/xdg/weston/weston.ini"
#define JW_MLP1_WESTON_OVERRIDE_INI "/userdata/root/.config/weston.ini"
/* Stock 720x960 panel timing held constant; only the pixel clock scales with
   the target rate. htotal*vtotal = 769*1018 = 782842 px/frame, so the dot clock
   (MHz) = 782842 * hz / 1e6 (100->78.28, 120->93.94). -hsync -vsync
   matches the panel's stock mode flags. Driver+panel verified clean to 120 Hz on
   the MLP1; 110 is the highest rate that logged zero VOP errors, so 100 sits
   inside the clean range (120 logs one POST_BUF_EMPTY at the modeset only). */
#define JW_MLP1_PANEL_PX_PER_FRAME 782842.0
static void jw__mlp1_weston_mode_line(int hz, char *buf, size_t n) {
    double clk = (JW_MLP1_PANEL_PX_PER_FRAME * (double)hz) / 1.0e6;
    snprintf(buf, n,
             "mode=%.2f 720 735 749 769 960 990 998 1018 -hsync -vsync\n", clk);
}

/* Build the override from the stock config: copy it verbatim, drop any
   pre-existing `mode=` line (so re-toggling never duplicates), and append the
   modeline for the target rate. [output] is the stock file's last section, so
   EOF == inside it. Written via a temp + rename so a partial write never breaks
   the boot. */
static int jw__mlp1_write_weston_override(int hz) {
    FILE *in = fopen(JW_MLP1_WESTON_STOCK_INI, "r");
    if (!in) {
        return -1;
    }
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", JW_MLP1_WESTON_OVERRIDE_INI);
    if (jw__mkdir_parent(tmp) != 0) {
        fclose(in);
        return -1;
    }
    FILE *out = fopen(tmp, "w");
    if (!out) {
        fclose(in);
        return -1;
    }
    char line[512];
    int ok = 1;
    while (fgets(line, sizeof(line), in)) {
        const char *s = line;
        while (*s == ' ' || *s == '\t') {
            s++;
        }
        if (strncmp(s, "mode=", 5) == 0) {
            continue;
        }
        if (fputs(line, out) < 0) {
            ok = 0;
            break;
        }
    }
    char mode_line[96];
    jw__mlp1_weston_mode_line(hz, mode_line, sizeof(mode_line));
    if (ok && fputs(mode_line, out) < 0) {
        ok = 0;
    }
    fclose(in);
    if (fclose(out) != 0) {
        ok = 0;
    }
    if (!ok) {
        unlink(tmp);
        return -1;
    }
    if (rename(tmp, JW_MLP1_WESTON_OVERRIDE_INI) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

/* ── Color temperature (DRM CRTC gamma LUT) ─────────────────────────────
   The RK3566 VOP2 exposes a per-CRTC GAMMA_LUT (1024 entries here). We found no
   sysfs knob for it and no vendor color-temperature control; the legacy
   DRM_IOCTL_MODE_SETGAMMA path programs it directly. On this vendor kernel
   (5.10) SETGAMMA is not gated on DRM master, so a fresh, non-master fd
   succeeds while Weston holds master -- root does not bypass that check on
   mainline, so this is not portable. Verified on hardware: the ramp takes
   effect live with no compositor restart (unlike refresh rate).
   The LUT stays programmed across a Weston restart, an HDMI connector switch
   and the direct-DRM handoff (all observed on hardware); a cold power-off and
   power-on clears it (also observed: the panel came up neutral with a daemon
   that never touches it), so jawakad replays the persisted value at startup
   (jw__apply_color_temp). A warm reboot has not been tried.

   The kernel uapi headers (<drm/drm.h>) provide the ioctl structs, so there is
   no libdrm header/link dependency in the cross build. */
#define JW_MLP1_DRM_CARD "/dev/dri/card0"

/* Tanner Helland black-body approximation: kelvin -> per-channel linear scale in
   [0,1]. Above ~6600 K red (and a little green) is pulled down: cooler. Below it
   blue is pulled down instead: warmer. */
static void jw__mlp1_kelvin_to_rgb(int kelvin, double *r, double *g, double *b) {
    double t = kelvin / 100.0;
    double red, grn, blu;
    if (t <= 66.0) {
        red = 255.0;
        grn = 99.4708025861 * log(t) - 161.1195681661;
    } else {
        red = 329.698727446 * pow(t - 60.0, -0.1332047592);
        grn = 288.1221695283 * pow(t - 60.0, -0.0755148492);
    }
    if (t >= 66.0) {
        blu = 255.0;
    } else if (t <= 19.0) {
        blu = 0.0;
    } else {
        blu = 138.5177312231 * log(t - 10.0) - 305.0447927307;
    }
    *r = (red < 0 ? 0 : red > 255 ? 255 : red) / 255.0;
    *g = (grn < 0 ? 0 : grn > 255 ? 255 : grn) / 255.0;
    *b = (blu < 0 ? 0 : blu > 255 ? 255 : blu) / 255.0;
}

/* Find the CRTC that has a valid mode and copy its state (the core GETCRTC ioctl)
   to *out. MLP1 exposes a single CRTC that DSI and HDMI share. Returns 0, or -1
   when none has a valid mode. */
static int jw__mlp1_drm_active_crtc(int fd, struct drm_mode_crtc *out) {
    struct drm_mode_card_res res;
    memset(&res, 0, sizeof(res));
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0 || res.count_crtcs == 0) {
        return -1;
    }
    uint32_t n = res.count_crtcs;
    uint32_t *ids = calloc(n, sizeof(*ids));
    if (!ids) {
        return -1;
    }
    memset(&res, 0, sizeof(res));
    res.crtc_id_ptr = (uint64_t)(uintptr_t)ids;
    res.count_crtcs = n;
    int rc = -1;
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) == 0) {
        uint32_t got = res.count_crtcs < n ? res.count_crtcs : n;
        for (uint32_t i = 0; i < got; i++) {
            struct drm_mode_crtc c;
            memset(&c, 0, sizeof(c));
            c.crtc_id = ids[i];
            if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &c) != 0) {
                continue;
            }
            if (c.mode_valid) {
                *out = c;
                rc = 0;
                break;
            }
        }
    }
    free(ids);
    return rc;
}

/* Pick the CRTC to program: the active one, with a gamma table of at least two
   entries (the ramp writer divides by size - 1). Only an active CRTC is used on
   purpose: the gamma LUT is not written while the CRTC has no mode (cable pulled,
   restart in flight). Returns 0 and fills *crtc_id / *gamma_size, -1 otherwise. */
static int jw__mlp1_drm_pick_crtc(int fd, uint32_t *crtc_id, uint32_t *gamma_size) {
    struct drm_mode_crtc c;
    if (jw__mlp1_drm_active_crtc(fd, &c) != 0 || c.gamma_size < 2) {
        return -1;
    }
    *crtc_id = c.crtc_id;
    *gamma_size = c.gamma_size;
    return 0;
}

/* Current mode of the active output, from the DRM core. This deliberately does not
   read /sys/kernel/debug/dri/0/summary: on this vendor kernel the summary dump
   (vop2_crtc_debugfs_dump) dereferences NULL when it runs during a CRTC state
   change, which oopsed the kernel and left the display dead until a hard reset
   (the daemon read it every second). */
static int jw__mlp1_get_display_mode(jw_platform_context *ctx, int *width, int *height,
                                     int *hz) {
    (void)ctx;
    int fd = open(JW_MLP1_DRM_CARD, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    struct drm_mode_crtc c;
    int rc = jw__mlp1_drm_active_crtc(fd, &c);
    close(fd);
    if (rc != 0) {
        return -1;
    }
    if (width) *width = (int)c.mode.hdisplay;
    if (height) *height = (int)c.mode.vdisplay;
    if (hz) *hz = (int)c.mode.vrefresh;
    return 0;
}

/* Current refresh rate in Hz, or -1 when there is no active mode. */
static int jw__mlp1_get_refresh_hz(void) {
    int hz = -1;
    return jw__mlp1_get_display_mode(NULL, NULL, NULL, &hz) == 0 ? hz : -1;
}

/* Write a linear ramp scaled by the given per-channel factors (1.0 each =
   identity) to the CRTC's gamma LUT. Only touches the hardware; callers own any
   bookkeeping. Returns 0 on success. */
static int jw__mlp1_write_gamma_ramp(double rf, double gf, double bf) {
    int fd = open(JW_MLP1_DRM_CARD, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        jw_log_warn("color-temp: open %s: %s", JW_MLP1_DRM_CARD, strerror(errno));
        return -1;
    }

    uint32_t crtc_id = 0, gamma_size = 0;
    int rc = -1;
    if (jw__mlp1_drm_pick_crtc(fd, &crtc_id, &gamma_size) != 0) {
        jw_log_warn("color-temp: no gamma-capable CRTC");
        close(fd);
        return -1;
    }

    uint16_t *r = malloc(gamma_size * sizeof(*r));
    uint16_t *g = malloc(gamma_size * sizeof(*g));
    uint16_t *b = malloc(gamma_size * sizeof(*b));
    if (r && g && b) {
        for (uint32_t i = 0; i < gamma_size; i++) {
            double base = (double)i / (double)(gamma_size - 1); /* 0..1 */
            double rv = base * rf, gv = base * gf, bv = base * bf;
            r[i] = (uint16_t)lround((rv < 0 ? 0 : rv > 1 ? 1 : rv) * 65535.0);
            g[i] = (uint16_t)lround((gv < 0 ? 0 : gv > 1 ? 1 : gv) * 65535.0);
            b[i] = (uint16_t)lround((bv < 0 ? 0 : bv > 1 ? 1 : bv) * 65535.0);
        }
        struct drm_mode_crtc_lut lut;
        memset(&lut, 0, sizeof(lut));
        lut.crtc_id = crtc_id;
        lut.gamma_size = gamma_size;
        lut.red = (uint64_t)(uintptr_t)r;
        lut.green = (uint64_t)(uintptr_t)g;
        lut.blue = (uint64_t)(uintptr_t)b;
        if (ioctl(fd, DRM_IOCTL_MODE_SETGAMMA, &lut) == 0) {
            rc = 0;
        } else {
            jw_log_warn("color-temp: SETGAMMA crtc %u: %s", crtc_id, strerror(errno));
        }
    }
    free(r);
    free(g);
    free(b);
    close(fd);
    return rc;
}

/* Program the CRTC gamma LUT for the given color-temperature target and, on
   success, record it as the applied panel value. NEUTRAL_K writes an identity
   ramp (no correction). Returns 0 on success. */
static int jw__mlp1_set_color_temp(int kelvin) {
    kelvin = jw_platform_clamp_color_temp_k(kelvin);
    double rf = 1.0, gf = 1.0, bf = 1.0;
    if (kelvin != JW_PLATFORM_COLOR_TEMP_NEUTRAL_K) {
        jw__mlp1_kelvin_to_rgb(kelvin, &rf, &gf, &bf);
    }
    int rc = jw__mlp1_write_gamma_ramp(rf, gf, bf);
    if (rc == 0) {
        s_mlp1_color_temp_k = kelvin;
    }
    return rc;
}

static int jw__mlp1_get_color_temp(void) {
    return s_mlp1_color_temp_k;
}

static int jw__mlp1_set_refresh_rate(int hz) {
    s_mlp1_target_refresh_hz = hz;
    /* Persist the panel modeline regardless, so the internal screen uses this
       rate now (panel active) and later when HDMI is turned off. */
    if (hz > 60) {
        if (jw__mlp1_write_weston_override(hz) != 0) {
            return -1;
        }
    } else {
        if (unlink(JW_MLP1_WESTON_OVERRIDE_INI) != 0 && errno != ENOENT) {
            return -1;
        }
    }

    /* If the TV is the active output, re-apply the HDMI mode at the new rate
       (1080p120 vs the crisp 720p60) rather than restarting onto the panel. */
    if (jw__mlp1_hdmi_tv_active()) {
        return jw__mlp1_set_hdmi_output(s_mlp1_hdmi_mode);
    }

    /* Restart Weston so it re-reads the config at the new mode, then drop the
       launcher AND the OSD so jawakad respawns fresh ones that reconnect to the
       new compositor and repaint (both Wayland clients black-screen across a
       restart — the old OSD survives the kill of Weston but never redraws).
       Detached child so the action returns to the caller immediately. Double-fork
       so the grandchild reparents to init and is auto-reaped (no zombie). */
    (void)jw_weston_initd_spawn_detached(
        JW_WESTON_INITD("restart") "; sleep 1; "
        "kill -9 $(pgrep -x jawaka-launcher) $(pgrep -x jawaka-osd) 2>/dev/null");
    return 0;
}

/* ── HDMI output ──────────────────────────────────────────────────────────
   The vendor-patched Weston DRM backend exposes env knobs + a live command file
   (/tmp/.weston_drm.conf). 4:3 = single-head HDMI at a universal CEA mode
   (1280x720; PC modes like 1024x768 show black on TVs) with the 960x720 (4:3)
   Leaf surface placed 1:1 centered via a corner-coord rect <160,0,1120,720>
   (160px pillarbox bars each side, no stretch). Stretch = same minus the rect.
   Off = revert to the DSI panel. Reverse-engineered + proven on hardware; see
   memory mlp1-weston-display.md. */
/* Does the connected TV advertise a 1080p120 mode? modetest lists the live EDID
   modes per connector; only HDMI exposes 1920x1080 at ~120 here, so a global
   match is safe. Gates the 120Hz path so a 60Hz-only TV stays on crisp 720p. */
static bool jw__mlp1_hdmi_has_1080p120(void) {
    FILE *fp = popen("modetest -c 2>/dev/null", "r");
    if (!fp) {
        return false;
    }
    char line[256];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "1920x1080") &&
            (strstr(line, " 120.") || strstr(line, " 119."))) {
            found = true;
            break;
        }
    }
    pclose(fp);
    return found;
}

/* Apply the requested HDMI output mode by restarting Weston with the right env +
   conf, then respawning the launcher AND OSD (same detached pattern + reason as
   jw__mlp1_set_refresh_rate). Removes any leftover ~/.config/weston.ini panel
   override so it can't pin the mode and block the switch.

   Mode pick: when the user wants 120Hz (Refresh Rate = 120) AND the TV advertises
   1080p120, drive 1920x1080@120 so BFI + smoothness work over HDMI; otherwise the
   crisp, universal 720p60 (the 960x720 4:3 UI maps 1:1). 4:3 pillarboxes via a
   centered rect; Stretch fills the frame. */
static int jw__mlp1_set_hdmi_output(int mode) {
    char script[1024];
    if (mode == JW_MLP1_HDMI_OFF) {
        snprintf(script, sizeof(script),
            "rm -f /tmp/.weston_drm.conf %s; "
            JW_WESTON_ROOTFS_ENV " WESTON_DRM_SINGLE_HEAD=1 WESTON_DRM_PRIMARY=DSI-1 "
            JW_WESTON_INITD_SCRIPT " restart; sleep 1; "
            "kill -9 $(pgrep -x jawaka-launcher) $(pgrep -x jawaka-osd) 2>/dev/null",
            JW_MLP1_WESTON_OVERRIDE_INI);
    } else {
        int want = (s_mlp1_target_refresh_hz > 0) ? s_mlp1_target_refresh_hz
                                                  : jw__mlp1_get_refresh_hz();
        bool use120 = (want >= 120) && jw__mlp1_hdmi_has_1080p120();
        const char *modestr = use120 ? "1920x1080@120" : "1280x720";
        char rectline[64] = "";
        if (mode == JW_MLP1_HDMI_4_3) {
            snprintf(rectline, sizeof(rectline),
                     "output:HDMI-A-1:rect=<%s>\\n",
                     use120 ? "240,0,1680,1080" : "160,0,1120,720");
        }
        snprintf(script, sizeof(script),
            "rm -f %s; "
            "printf 'output:HDMI-A-1:mode=%s\\n%soutput:HDMI-A-1:primary\\n' > /tmp/.weston_drm.conf; "
            JW_WESTON_ROOTFS_ENV " WESTON_DRM_SINGLE_HEAD=1 WESTON_DRM_PRIMARY=HDMI-A-1 "
            "WESTON_DRM_VIRTUAL_SIZE=960x720 WESTON_DRM_CONFIG=/tmp/.weston_drm.conf "
            JW_WESTON_INITD_SCRIPT " restart; sleep 1; "
            "kill -9 $(pgrep -x jawaka-launcher) $(pgrep -x jawaka-osd) 2>/dev/null",
            JW_MLP1_WESTON_OVERRIDE_INI, modestr, rectline);
    }

    /* The color-temperature LUT stays programmed on the (shared) CRTC across the
       Weston restart and the connector switch, so it would tint the TV. It is
       deliberately NOT swapped here: on hardware, LUT writes made during an HDMI
       switch were followed by kernel oopses in the vendor's debugfs display dump
       (which Jawaka no longer reads, see jw__mlp1_get_display_mode). jawakad swaps
       the LUT from its HDMI poll instead, once the link has been quiet for a
       couple of seconds and the display has a mode again, so it does not race the
       mode change (see jw__tick_hdmi): identity for a TV, the panel value again
       when the panel is back. */

    /* Detached (double-fork) so the action returns at once; see
       jw_weston_initd_spawn_detached. */
    if (jw_weston_initd_spawn_detached(script) != 0) {
        return -1;
    }
    s_mlp1_hdmi_mode = mode;
    return 0;
}

static bool jw__mlp1_adb_supported(void) {
    return access(JW_MLP1_ADBD, X_OK) == 0 &&
           access(JW_MLP1_USB_GADGET, X_OK) == 0;
}

static bool jw__mlp1_usb_config_is_adb(void) {
    char *text = jw__read_text_file(JW_MLP1_USB_CONFIG, 64);
    if (!text) {
        return false;
    }
    text[strcspn(text, "\r\n")] = '\0';
    bool result = strcmp(text, "usb_adb_en") == 0;
    free(text);
    return result;
}

/* The same flag lsattr prints as 'i', read with the ioctl lsattr itself uses;
   this runs on every 5 s platform-status. */
static bool jw__mlp1_usb_config_is_immutable(void) {
    int fd = open(JW_MLP1_USB_CONFIG, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return false;
    }
    int attrs = 0;
    bool immutable = ioctl(fd, FS_IOC_GETFLAGS, &attrs) == 0 &&
                     (attrs & FS_IMMUTABLE_FL) != 0;
    close(fd);
    return immutable;
}

static bool jw__mlp1_adb_is_pinned(void) {
    return jw__mlp1_adb_supported() &&
           jw__mlp1_usb_config_is_adb() &&
           jw__mlp1_usb_config_is_immutable();
}

static void jw__mlp1_enable_adb(jw_platform_context *ctx,
                                jw_platform_result *out) {
    if (!jw__mlp1_adb_supported()) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "ADB support unavailable");
        return;
    }

    if (jw__exec_shell(JW_MLP1_ADB_ENABLE_CMD) != 0 ||
        !jw__mlp1_adb_is_pinned()) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "ADB pin failed");
        return;
    }

    if (jw__mlp1_set_adb_intent(ctx, true) != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "ADB enabled, restore marker failed");
        return;
    }

    if (jw__exec_shell(JW_MLP1_USB_RESTART_CMD) != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               "ADB enabled; USB restart failed");
        return;
    }

    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "ADB enabled");
}

static void jw__mlp1_disable_adb(jw_platform_context *ctx,
                                 jw_platform_result *out) {
    if (!jw__mlp1_adb_supported()) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "ADB support unavailable");
        return;
    }

    if (jw__exec_shell(JW_MLP1_ADB_DISABLE_CMD) != 0 ||
        jw__mlp1_adb_is_pinned()) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "ADB disable failed");
        return;
    }

    if (jw__mlp1_set_adb_intent(ctx, false) != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "ADB disabled, marker removal failed");
        return;
    }

    if (jw__exec_shell(JW_MLP1_USB_RESTART_CMD) != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               "ADB disabled; USB restart failed");
        return;
    }

    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "ADB disabled");
}

static int jw__json_put_string(cJSON *object, const char *key, const char *value) {
    if (!object || !key || !value) {
        return -1;
    }
    cJSON *item = cJSON_CreateString(value);
    if (!item) {
        return -1;
    }
    if (cJSON_GetObjectItemCaseSensitive(object, key)) {
        if (!cJSON_ReplaceItemInObjectCaseSensitive(object, key, item)) {
            cJSON_Delete(item);
            return -1;
        }
        return 0;
    }
    if (!cJSON_AddItemToObject(object, key, item)) {
        cJSON_Delete(item);
        return -1;
    }
    return 0;
}

static int jw__json_put_number(cJSON *object, const char *key, int value) {
    if (!object || !key) {
        return -1;
    }
    cJSON *item = cJSON_CreateNumber(value);
    if (!item) {
        return -1;
    }
    if (cJSON_GetObjectItemCaseSensitive(object, key)) {
        if (!cJSON_ReplaceItemInObjectCaseSensitive(object, key, item)) {
            cJSON_Delete(item);
            return -1;
        }
        return 0;
    }
    if (!cJSON_AddItemToObject(object, key, item)) {
        cJSON_Delete(item);
        return -1;
    }
    return 0;
}

static int jw__json_ensure_number(cJSON *object, const char *key, int value) {
    return cJSON_GetObjectItemCaseSensitive(object, key)
               ? 0
               : jw__json_put_number(object, key, value);
}

static int jw__json_ensure_string(cJSON *object, const char *key, const char *value) {
    return cJSON_GetObjectItemCaseSensitive(object, key)
               ? 0
               : jw__json_put_string(object, key, value);
}

static cJSON *jw__load_json_object_file(const char *path, long max_bytes) {
    char *text = jw__read_text_file(path, max_bytes);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    return root;
}

static int jw__mlp1_update_power_cfg(int seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds > 86400) seconds = 86400;

    cJSON *root = jw__load_json_object_file(JW_MLP1_POWER_CFG, 4096);
    if (!root) {
        return -1;
    }

    bool disabled = seconds <= 0;
    long timeout_ms = disabled ? 0L : (long)seconds * 1000L;
    char timeout[32];
    snprintf(timeout, sizeof(timeout), "%ld", timeout_ms);

    int rc = 0;
    rc |= jw__json_ensure_string(root, "autoOffLight", "20");
    rc |= jw__json_put_string(root, "hibernateDisable", disabled ? "1" : "0");
    rc |= jw__json_put_string(root, "screenLockDisable", disabled ? "1" : "0");
    rc |= jw__json_put_string(root, "screenLockTimeout", timeout);
    rc |= jw__json_ensure_string(root, "standbyMuteDisable", "0");

    char *printed = rc == 0 ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (!printed) {
        return -1;
    }

    rc = jw__write_text_file_atomic(JW_MLP1_POWER_CFG, printed);
    cJSON_free(printed);
    if (rc == 0) {
        sync();
    }
    return rc;
}

static bool jw__mlp1_json_string_equals(const cJSON *object, const char *key,
                                        const char *expected) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring &&
           strcmp(item->valuestring, expected) == 0;
}

/* True when the on-disk power cfg already encodes `seconds`, so the rewrite
   (and the loong_power restart it forces) can be skipped. Fields written by
   jw__mlp1_update_power_cfg must match exactly; ensure-only fields just need
   to exist. Missing/corrupt cfg yields false and falls back to the write path. */
static bool jw__mlp1_power_cfg_matches(int seconds) {
    cJSON *root = jw__load_json_object_file(JW_MLP1_POWER_CFG, 4096);
    if (!root) {
        return false;
    }

    bool disabled = seconds <= 0;
    char timeout[32];
    snprintf(timeout, sizeof(timeout), "%ld",
             disabled ? 0L : (long)seconds * 1000L);

    bool matches =
        cJSON_GetObjectItemCaseSensitive(root, "autoOffLight") != NULL &&
        cJSON_GetObjectItemCaseSensitive(root, "standbyMuteDisable") != NULL &&
        jw__mlp1_json_string_equals(root, "hibernateDisable", disabled ? "1" : "0") &&
        jw__mlp1_json_string_equals(root, "screenLockDisable", disabled ? "1" : "0") &&
        jw__mlp1_json_string_equals(root, "screenLockTimeout", timeout);
    cJSON_Delete(root);
    return matches;
}

static char *jw__mlp1_read_system_config(const char *param) {
    sqlite3 *db = NULL;
    if (sqlite3_open(JW_MLP1_LOONG_DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return NULL;
    }
    sqlite3_busy_timeout(db, 1000);

    sqlite3_stmt *stmt = NULL;
    const char *sql = "SELECT value FROM system_config WHERE param = ?;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return NULL;
    }
    sqlite3_bind_text(stmt, 1, param, -1, SQLITE_TRANSIENT);

    char *out = NULL;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *text = sqlite3_column_text(stmt, 0);
        if (text) {
            out = strdup((const char *)text);
        }
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return out;
}

static int jw__mlp1_write_system_config(const char *param, const char *json) {
    sqlite3 *db = NULL;
    if (sqlite3_open(JW_MLP1_LOONG_DB_PATH, &db) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return -1;
    }
    sqlite3_busy_timeout(db, 1000);

    sqlite3_stmt *stmt = NULL;
    const char *sql =
        "INSERT INTO system_config (param, value, backup) VALUES (?, ?, ?) "
        "ON CONFLICT(param) DO UPDATE SET value = excluded.value, backup = excluded.backup;";
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, param, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, json, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, json, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return rc;
}

static char *jw__mlp1_make_display_param(int seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds > 86400) seconds = 86400;

    char *current = jw__mlp1_read_system_config("DISPLAY_PARAM");
    cJSON *root = current ? cJSON_Parse(current) : NULL;
    free(current);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (!root) {
        return NULL;
    }

    int rc = 0;
    rc |= jw__json_ensure_number(root, "dark", 1);
    rc |= jw__json_ensure_number(root, "color", 0);
    rc |= jw__json_ensure_number(root, "temperature", 10);
    rc |= jw__json_ensure_number(root, "brightness", 7);
    rc |= jw__json_put_number(root, "lock", seconds > 0 ? seconds : 0);

    char *printed = rc == 0 ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    return printed;
}

/* Counterpart of jw__mlp1_power_cfg_matches for the loong DISPLAY_PARAM row:
   `lock` (the only field jw__mlp1_make_display_param overwrites) must equal
   `seconds`; the ensure-only fields just need to exist. */
static bool jw__mlp1_display_param_matches(int seconds) {
    if (seconds < 0) seconds = 0;
    if (seconds > 86400) seconds = 86400;

    char *current = jw__mlp1_read_system_config("DISPLAY_PARAM");
    cJSON *root = current ? cJSON_Parse(current) : NULL;
    free(current);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return false;
    }

    const cJSON *lock = cJSON_GetObjectItemCaseSensitive(root, "lock");
    bool matches =
        cJSON_IsNumber(lock) && lock->valueint == (seconds > 0 ? seconds : 0) &&
        cJSON_GetObjectItemCaseSensitive(root, "dark") != NULL &&
        cJSON_GetObjectItemCaseSensitive(root, "color") != NULL &&
        cJSON_GetObjectItemCaseSensitive(root, "temperature") != NULL &&
        cJSON_GetObjectItemCaseSensitive(root, "brightness") != NULL;
    cJSON_Delete(root);
    return matches;
}

static int jw__mlp1_update_display_param(int seconds) {
    char *json = jw__mlp1_make_display_param(seconds);
    if (!json) {
        return -1;
    }

    jw__loong_load();
    if (s_write_config) {
        (void)s_write_config("DISPLAY_PARAM", json, json, 1);
    }
    int rc = jw__mlp1_write_system_config("DISPLAY_PARAM", json);
    cJSON_free(json);
    return rc;
}

static const char *jw__mlp1_sound_output_key(jw_platform_audio_output output) {
    switch (output) {
        case JW_PLATFORM_AUDIO_OUTPUT_SPEAKER: return "SPEAKER";
        case JW_PLATFORM_AUDIO_OUTPUT_HEADSET: return "HEADSET";
        case JW_PLATFORM_AUDIO_OUTPUT_HDMI: return "HDMI";
        case JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH: return "BLUETOOTH";
        /* Without a key here the level is neither restored nor saved: the switch
           silently keeps whatever the previous output was at, and the first volume
           press jumps to an unrelated value. */
        case JW_PLATFORM_AUDIO_OUTPUT_USB: return "USB";
        default: return NULL;
    }
}

static int jw__mlp1_percent_to_stock_level(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    return (percent + 5) / 10;
}

static int jw__mlp1_stock_level_to_percent(int level) {
    if (level < 0) level = 0;
    if (level > 10) level = 10;
    return level * 10;
}

static cJSON *jw__mlp1_load_sound_param(void) {
    char *current = jw__mlp1_read_system_config("SOUND_PARAM");
    cJSON *root = current ? cJSON_Parse(current) : NULL;
    free(current);
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        root = cJSON_CreateObject();
    }
    if (!root) {
        return NULL;
    }

    (void)jw__json_ensure_number(root, "bgm", 0);
    (void)jw__json_ensure_number(root, "sysVolume", 10);
    (void)jw__json_ensure_number(root, "toneVolume", 10);
    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!cJSON_IsObject(devices)) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "devices");
        devices = cJSON_CreateObject();
        if (!devices || !cJSON_AddItemToObject(root, "devices", devices)) {
            cJSON_Delete(devices);
            cJSON_Delete(root);
            return NULL;
        }
    }
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        const char *key = jw__mlp1_sound_output_key((jw_platform_audio_output)i);
        if (key) {
            (void)jw__json_ensure_number(devices, key, 10);
        }
    }
    return root;
}

static int jw__mlp1_write_sound_param(cJSON *root) {
    if (!root) {
        return -1;
    }
    char *printed = cJSON_PrintUnformatted(root);
    if (!printed) {
        return -1;
    }

    jw__loong_load();
    if (s_write_config) {
        (void)s_write_config("SOUND_PARAM", printed, printed, 1);
    }
    int rc = jw__mlp1_write_system_config("SOUND_PARAM", printed);
    cJSON_free(printed);
    return rc;
}

static void jw__mlp1_get_audio_volumes(int out[JW_PLATFORM_AUDIO_OUTPUT_COUNT]) {
    if (!out) {
        return;
    }
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        out[i] = -1;
    }

    cJSON *root = jw__mlp1_load_sound_param();
    if (!root) {
        return;
    }
    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        const char *key = jw__mlp1_sound_output_key((jw_platform_audio_output)i);
        const cJSON *level = key ? cJSON_GetObjectItemCaseSensitive(devices, key) : NULL;
        if (cJSON_IsNumber(level)) {
            out[i] = jw__mlp1_stock_level_to_percent(level->valueint);
        }
    }
    cJSON_Delete(root);
}

static void jw__mlp1_sync_sound_volume(jw_platform_audio_output output, int percent) {
    const char *key = jw__mlp1_sound_output_key(output);
    if (!key) {
        return;
    }
    cJSON *root = jw__mlp1_load_sound_param();
    if (!root) {
        return;
    }
    cJSON *devices = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (cJSON_IsObject(devices)) {
        (void)jw__json_put_number(devices, key,
                                  jw__mlp1_percent_to_stock_level(percent));
    }
    (void)jw__mlp1_write_sound_param(root);
    cJSON_Delete(root);
}

static int jw__mlp1_playback_path(void) {
    FILE *fp = popen(JW_MLP1_PLAYBACK_PATH_CMD, "r");
    if (!fp) {
        return -1;
    }
    char line[256];
    int value = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, " : values=%d", &value) == 1 ||
            sscanf(line, "  : values=%d", &value) == 1 ||
            sscanf(line, ": values=%d", &value) == 1) {
            break;
        }
    }
    pclose(fp);
    return value;
}

static bool jw__mlp1_pulse_sink_exists(const char *needle) {
    if (!needle || !needle[0]) {
        return false;
    }
    FILE *fp = popen("pactl list short sinks 2>/dev/null", "r");
    if (!fp) {
        return false;
    }
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, needle)) {
            found = true;
            break;
        }
    }
    pclose(fp);
    return found;
}

static bool jw__mlp1_hdmi_connected(void) {
    /* sysfs files report st_size 0, so jw__read_text_file (ftell-based) reads
       empty — read the connector status directly with fgets instead. */
    FILE *fp = fopen(JW_MLP1_HDMI_STATUS, "r");
    if (!fp) {
        return false;
    }
    char s[32] = { 0 };
    char *got = fgets(s, sizeof(s), fp);
    fclose(fp);
    if (!got) {
        return false;
    }
    s[strcspn(s, "\r\n")] = '\0';
    return strcmp(s, "connected") == 0;
}

/* Headphone jack detection (rk817 'Headphones Jack' kcontrol, numid=1). The
   value line reads ": values=on"/"=off" (or 1/0). Used to offer the Headset
   output only when something is actually plugged in, mirroring how HDMI and
   Bluetooth are gated on being connected. */
static bool jw__mlp1_headphone_jack_present(void) {
    FILE *fp = popen(JW_MLP1_HP_JACK_CMD, "r");
    if (!fp) {
        return false;
    }
    char line[256];
    bool present = false;
    while (fgets(line, sizeof(line), fp)) {
        const char *v = strstr(line, ": values=");
        if (v) {
            v += strlen(": values=");
            present = (strncmp(v, "on", 2) == 0 || v[0] == '1');
            break;
        }
    }
    pclose(fp);
    return present;
}

/* The rk817 (analog) sink's PulseAudio name is not stable: udev-detect names it
   "alsa_output.platform-rk817-sound.stereo-fallback", a raw module-alsa-sink
   load names it "alsa_output.hw_1_0", and which one wins is a boot-time race.
   Resolve it at runtime as the first sink that is neither the HDMI nor the USB-C
   sink, instead of hardcoding a name that may not exist. Skipping usb_out matters
   as much as hdmi_out: this is what speaker/headset routing falls back to, and
   with a USB headset attached the first sink is no longer guaranteed to be the
   analog one. Returns false if no analog sink found. */
static bool jw__mlp1_default_audio_sink(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return false;
    }
    FILE *fp = popen("pactl list short sinks 2>/dev/null", "r");
    if (!fp) {
        return false;
    }
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), fp)) {
        char name[256];
        /* fields: index <tab> name <tab> module ... */
        if (sscanf(line, "%*s %255s", name) == 1) {
            if (strstr(name, "hdmi") || strstr(name, JW_MLP1_PACTL_USB_SINK) ||
                strstr(name, ".usb-")) {
                continue;
            }
            snprintf(out, out_size, "%s", name);
            found = true;
            break;
        }
    }
    pclose(fp);
    return found;
}

/* ── HDMI audio (follows the HDMI display output) ─────────────────────────────
   udev-detect doesn't expose the HDMI card as a PulseAudio sink here, so load an
   explicit one on hw:0,0 when the TV becomes the active display and drop it on
   revert — an unplugged TV must never strand audio on a dead sink. The module
   index is tracked so the unload is precise and can never hit the analog sink. */
static int s_mlp1_hdmi_audio_module = -1;

static bool jw__mlp1_hdmi_tv_active(void) {
    return s_mlp1_hdmi_mode == JW_MLP1_HDMI_4_3 ||
           s_mlp1_hdmi_mode == JW_MLP1_HDMI_STRETCH;
}

static void jw__mlp1_hdmi_audio_on(void) {
    if (s_mlp1_hdmi_audio_module < 0) {
        FILE *fp = popen("pactl load-module module-alsa-sink device=hw:0,0 "
                         "sink_name=hdmi_out 2>/dev/null", "r");
        if (fp) {
            char buf[32];
            if (fgets(buf, sizeof(buf), fp)) {
                int idx = atoi(buf);
                if (idx > 0) {
                    s_mlp1_hdmi_audio_module = idx;
                }
            }
            pclose(fp);
        }
    }
    /* Make HDMI the default sink and pull any already-playing streams over. */
    (void)jw__exec_shell(
        "pactl set-default-sink hdmi_out 2>/dev/null; "
        "pactl list short sink-inputs | while read id _rest; do "
        "pactl move-sink-input \"$id\" hdmi_out 2>/dev/null; done");
}

static void jw__mlp1_hdmi_audio_off(void) {
    /* Point default back at the analog codec sink (default_audio_sink skips any
       'hdmi' sink) and move any streams over. */
    char sink[256];
    if (jw__mlp1_default_audio_sink(sink, sizeof(sink))) {
        char cmd[768];
        snprintf(cmd, sizeof(cmd),
                 "pactl set-default-sink %s 2>/dev/null; "
                 "pactl list short sink-inputs | while read id _rest; do "
                 "pactl move-sink-input \"$id\" %s 2>/dev/null; done",
                 sink, sink);
        (void)jw__exec_shell(cmd);
    }
    /* Keep the HDMI sink loaded on a deliberate Off (cable still in) — unloading
       then reloading hw:0,0 on the next TV-on races the ALSA device-busy window
       and the reload silently fails, stranding audio on the speaker. Only drop it
       when the cable is actually gone, so a fresh replug rebinds hw:0,0 cleanly. */
    if (s_mlp1_hdmi_audio_module >= 0 && !jw__mlp1_hdmi_connected()) {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "pactl unload-module %d 2>/dev/null",
                 s_mlp1_hdmi_audio_module);
        (void)jw__exec_shell(cmd);
        s_mlp1_hdmi_audio_module = -1;
    }
}

/* ── USB-C audio (follows a USB Audio Class device on the OTG port) ───────────
   The kernel does all the hard work already: attaching a device role-switches
   the dwc3 to host, brings up xhci, and snd_usb_audio registers a new ALSA card.
   All that's missing is telling pulse, which has no udev-detect here. */
static int s_mlp1_usb_audio_module = -1;

/* ALSA card index of an attached USB audio device, or -1 when none: the first
   /proc/asound/cards line naming the USB-Audio driver. Read directly, since
   the audio tick asks once a second. */
static int jw__mlp1_usb_audio_card(void) {
    FILE *fp = fopen("/proc/asound/cards", "r");
    if (!fp) {
        return -1;
    }
    char line[256];
    int idx = -1;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "USB-Audio")) {
            int n = -1;
            if (sscanf(line, " %d", &n) == 1 && n >= 0 && n <= 31) {
                idx = n;
            }
            break;
        }
    }
    fclose(fp);
    return idx;
}

static bool jw__mlp1_usb_audio_present(void) {
    return jw__mlp1_usb_audio_card() >= 0;
}

/* Load a sink for the USB card if one isn't loaded yet. The card index is not
   fixed (it depends on probe order), so it is resolved at load time rather than
   hardcoded. Returns true when a usable sink exists afterwards. */
static bool jw__mlp1_usb_audio_ensure_sink(void) {
    if (s_mlp1_usb_audio_module >= 0) {
        return true;
    }
    int card = jw__mlp1_usb_audio_card();
    if (card < 0) {
        return false;
    }
    char cmd[192];
    snprintf(cmd, sizeof(cmd),
             "pactl load-module module-alsa-sink device=hw:%d,0 "
             "sink_name=%s 2>/dev/null",
             card, JW_MLP1_PACTL_USB_SINK);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return false;
    }
    char buf[32];
    bool ok = false;
    if (fgets(buf, sizeof(buf), fp)) {
        int idx = atoi(buf);
        if (idx > 0) {
            s_mlp1_usb_audio_module = idx;
            ok = true;
        }
    }
    pclose(fp);
    return ok;
}

/* Tearing the USB sink down leaves the analog sink wedged: pulse still reports
   it RUNNING, streams reattach to it, the codec path is SPK and the DAC is open
   — and nothing is audible, because the underlying ALSA device is never reopened.
   Suspending and resuming forces that reopen in place. Deliberately not a module
   reload: that changes the sink index, and every stream pointed at the old one
   has to be chased down and moved again. */
static void jw__mlp1_analog_sink_reset(void) {
    char sink[256];
    if (!jw__mlp1_default_audio_sink(sink, sizeof(sink))) {
        return;
    }
    char cmd[640];
    /* The gap matters. Resuming immediately after the suspend — or straight off
       the back of the USB sink's unload — reopens into the same wedged state;
       pulse needs a beat to finish tearing the old device down first. */
    snprintf(cmd, sizeof(cmd),
             "pactl suspend-sink %s 1 2>/dev/null; sleep 1; "
             "pactl suspend-sink %s 0 2>/dev/null",
             sink, sink);
    (void)jw__exec_shell(cmd);
}

/* Drop the USB sink and move any streams back to the analog codec. Unlike HDMI
   we always unload once the device is gone: the ALSA card disappears with it, so
   keeping the sink loaded would strand every stream on a dead device. */
static void jw__mlp1_usb_audio_drop(void) {
    if (s_mlp1_usb_audio_module < 0) {
        return;
    }
    char sink[256];
    if (jw__mlp1_default_audio_sink(sink, sizeof(sink))) {
        char cmd[768];
        snprintf(cmd, sizeof(cmd),
                 "pactl set-default-sink %s 2>/dev/null; "
                 "pactl list short sink-inputs | while read id _rest; do "
                 "pactl move-sink-input \"$id\" %s 2>/dev/null; done",
                 sink, sink);
        (void)jw__exec_shell(cmd);
    }
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "pactl unload-module %d 2>/dev/null",
             s_mlp1_usb_audio_module);
    (void)jw__exec_shell(cmd);
    s_mlp1_usb_audio_module = -1;
}

/* Escape a string for safe inclusion inside single quotes in a shell command:
   each ' becomes '\''. Everything else (spaces, UTF-8, apostrophes from device
   names like "Eric's AirPods Pro") is passed through literally. */
static void jw__mlp1_shell_squote(const char *in, char *out, size_t out_size) {
    size_t o = 0;
    if (!out || out_size == 0) {
        return;
    }
    for (const char *p = in ? in : ""; *p && o + 4 < out_size; p++) {
        if (*p == '\'') {
            out[o++] = '\''; out[o++] = '\\'; out[o++] = '\''; out[o++] = '\'';
        } else {
            out[o++] = *p;
        }
    }
    out[o] = '\0';
}

/* BlueALSA presence without polling. A long-lived dbus-monitor child prints
   whenever org.bluealsa signals (PCMAdded/PCMRemoved) or the service starts or
   stops. Its stdout sits in jawakad's poll set, and the control name cached
   below is probed again with amixer only after it has said something. Before,
   the audio tick forked amixer every 1.5 s, and each probe also cost
   dbus-daemon a new connection and a GetManagedObjects. Without dbus-monitor
   (or while it restarts) every call probes, as before. */
#define JW_MLP1_BT_MONITOR "/usr/bin/dbus-monitor"
static pid_t s_mlp1_bt_monitor_pid = -1;
static int s_mlp1_bt_monitor_fd = -1;
static long long s_mlp1_bt_monitor_retry_ms = 0;
/* Probes until then: dbus-monitor needs a moment to subscribe, and a PCM that
   appears in that window would otherwise never be seen. */
static long long s_mlp1_bt_cache_trust_ms = 0;
static bool s_mlp1_bt_cache_valid = false;
static char s_mlp1_bt_cache_ctl[128];

static void jw__mlp1_bt_monitor_stop(void) {
    if (s_mlp1_bt_monitor_fd >= 0) {
        close(s_mlp1_bt_monitor_fd);
        s_mlp1_bt_monitor_fd = -1;
    }
    if (s_mlp1_bt_monitor_pid > 0) {
        kill(s_mlp1_bt_monitor_pid, SIGKILL);
        (void)waitpid(s_mlp1_bt_monitor_pid, NULL, 0);
        s_mlp1_bt_monitor_pid = -1;
    }
    s_mlp1_bt_cache_valid = false;
}

static void jw__mlp1_bt_monitor_start(void) {
    long long now = jw__monotonic_ms();
    if (s_mlp1_bt_monitor_pid > 0 || now < s_mlp1_bt_monitor_retry_ms) {
        return;
    }
    s_mlp1_bt_monitor_retry_ms = now + 5000;
    if (access(JW_MLP1_BT_MONITOR, X_OK) != 0) {
        return;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        return;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return;
    }
    if (pid == 0) {
        (void)prctl(PR_SET_PDEATHSIG, SIGTERM);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, STDIN_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execl(JW_MLP1_BT_MONITOR, "dbus-monitor", "--system",
              "type='signal',sender='org.bluealsa'",
              "type='signal',interface='org.freedesktop.DBus',"
              "member='NameOwnerChanged',arg0='org.bluealsa'",
              (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    (void)fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    int flags = fcntl(fds[0], F_GETFL);
    if (flags >= 0) {
        (void)fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    }
    s_mlp1_bt_monitor_pid = pid;
    s_mlp1_bt_monitor_fd = fds[0];
    s_mlp1_bt_cache_valid = false;
    s_mlp1_bt_cache_trust_ms = now + 2000;
}

/* Any output means "look again"; end of file means the monitor died, and the
   next start attempt replaces it. */
static void jw__mlp1_bt_monitor_drain(void) {
    if (s_mlp1_bt_monitor_fd < 0) {
        return;
    }
    char buf[1024];
    for (;;) {
        ssize_t n = read(s_mlp1_bt_monitor_fd, buf, sizeof(buf));
        if (n > 0) {
            s_mlp1_bt_cache_valid = false;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return;
        }
        jw_log_warn("audio: bluealsa monitor exited; probing until it restarts");
        jw__mlp1_bt_monitor_stop();
        return;
    }
}

/* Pick the BlueALSA mixer control to drive: prefer the A2DP (music) profile over
   SCO (call audio). Returns the raw control name; callers must shell-escape it
   with jw__mlp1_shell_squote since device names contain spaces/apostrophes. */
static bool jw__mlp1_bluealsa_control(char *out, size_t out_size) {
    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    jw__mlp1_bt_monitor_start();
    jw__mlp1_bt_monitor_drain();
    bool trusted = s_mlp1_bt_monitor_fd >= 0 &&
                   jw__monotonic_ms() >= s_mlp1_bt_cache_trust_ms;
    if (trusted && s_mlp1_bt_cache_valid) {
        snprintf(out, out_size, "%s", s_mlp1_bt_cache_ctl);
        return out[0] != '\0';
    }
    FILE *fp = popen("amixer -D bluealsa scontrols 2>/dev/null", "r");
    if (!fp) {
        return false;
    }
    char line[256];
    char first[128] = { 0 };
    bool have_a2dp = false;
    while (fgets(line, sizeof(line), fp)) {
        char ctl[128];
        if (sscanf(line, "Simple mixer control '%127[^']'", ctl) != 1) {
            continue;
        }
        if (!first[0]) {
            snprintf(first, sizeof(first), "%s", ctl);
        }
        if (strstr(ctl, "A2DP") || strstr(ctl, "a2dp")) {
            snprintf(out, out_size, "%s", ctl);
            have_a2dp = true;
            break;
        }
    }
    pclose(fp);
    if (!have_a2dp && first[0]) {
        snprintf(out, out_size, "%s", first);
    }
    snprintf(s_mlp1_bt_cache_ctl, sizeof(s_mlp1_bt_cache_ctl), "%s", out);
    s_mlp1_bt_cache_valid = trusted;
    return out[0] != '\0';
}

static int jw__mlp1_get_bluealsa_volume_percent(void) {
    char ctl[128];
    if (!jw__mlp1_bluealsa_control(ctl, sizeof(ctl))) {
        return -1;
    }
    char esc[256], cmd[320];
    jw__mlp1_shell_squote(ctl, esc, sizeof(esc));
    snprintf(cmd, sizeof(cmd), "amixer -D bluealsa sget '%s' 2>/dev/null", esc);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    int percent = jw__parse_percent_from_stream(fp);
    pclose(fp);
    return percent;
}

static int jw__mlp1_set_bluealsa_volume_percent(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    char ctl[128];
    if (!jw__mlp1_bluealsa_control(ctl, sizeof(ctl))) {
        return -1;
    }
    char esc[256], cmd[320];
    jw__mlp1_shell_squote(ctl, esc, sizeof(esc));
    snprintf(cmd, sizeof(cmd),
             "amixer -D bluealsa sset '%s' %d%% >/dev/null 2>&1",
             esc, percent);
    return jw__exec_shell(cmd);
}

static unsigned jw__mlp1_get_audio_available_outputs(void) {
    unsigned mask = JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_SPEAKER);
    if (jw__mlp1_headphone_jack_present()) {
        mask |= JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_HEADSET);
    }
    if (jw__mlp1_hdmi_connected() &&
        jw__mlp1_pulse_sink_exists(JW_MLP1_PACTL_HDMI_SINK)) {
        mask |= JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_HDMI);
    }
    if (jw__mlp1_bt_audio_present() || jw_bt_audio_connected() == 1) {
        mask |= JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH);
    }
    /* Offer USB-C whenever the card is present, loading the sink on demand — the
       card can appear well before anything asks to route to it. */
    if (jw__mlp1_usb_audio_present() && jw__mlp1_usb_audio_ensure_sink()) {
        mask |= JW_PLATFORM_AUDIO_OUTPUT_BIT(JW_PLATFORM_AUDIO_OUTPUT_USB);
    }
    return mask;
}

static jw_platform_audio_output jw__mlp1_get_audio_output(void) {
    int path = jw__mlp1_playback_path();
    if (path == 5) {
        return JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH;
    }

    char sink[256];
    if (jw__read_command_line(JW_MLP1_PACTL_GET_DEFAULT_SINK, sink, sizeof(sink)) == 0) {
        if (strstr(sink, "hdmi")) {
            return JW_PLATFORM_AUDIO_OUTPUT_HDMI;
        }
        /* USB-C is a sink swap, not a codec Playback Path change, so it has to be
           read off the default sink — the rk817 path still says speaker/headset. */
        if (strstr(sink, JW_MLP1_PACTL_USB_SINK)) {
            return JW_PLATFORM_AUDIO_OUTPUT_USB;
        }
    }

    if (path == 3 || path == 4) {
        return JW_PLATFORM_AUDIO_OUTPUT_HEADSET;
    }
    return JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
}

static int jw__mlp1_apply_stored_audio_volume(jw_platform_audio_output output) {
    int volumes[JW_PLATFORM_AUDIO_OUTPUT_COUNT];
    jw__mlp1_get_audio_volumes(volumes);
    int percent = (output >= 0 && output < JW_PLATFORM_AUDIO_OUTPUT_COUNT)
                    ? volumes[output]
                    : -1;
    if (percent < 0) {
        return 0;
    }
    if (output == JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH) {
        (void)jw__mlp1_set_bluealsa_volume_percent(percent);
        return 0;
    }
    return jw__mlp1_set_volume_percent(percent);
}

static int jw__mlp1_set_audio_output(jw_platform_audio_output output,
                                     jw_platform_result *out) {
    if (output < 0 || output >= JW_PLATFORM_AUDIO_OUTPUT_COUNT) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID,
                               "audio output invalid");
        return -1;
    }

    /* Each output keeps its own level; the one cached belongs to the old route. */
    s_mlp1_volume_cache = -1;

    unsigned available = jw__mlp1_get_audio_available_outputs();
    if ((available & JW_PLATFORM_AUDIO_OUTPUT_BIT(output)) == 0) {
        char message[JW_PLATFORM_MAX_MESSAGE];
        snprintf(message, sizeof(message), "%s output unavailable",
                 jw_platform_audio_output_label(output));
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE, message);
        return -1;
    }

    int rc = -1;
    switch (output) {
        case JW_PLATFORM_AUDIO_OUTPUT_SPEAKER:
        case JW_PLATFORM_AUDIO_OUTPUT_HEADSET: {
            /* The in-codec Playback Path is the actual speaker/headphone switch,
               and both are the same PulseAudio sink, so it must run on its own —
               NOT chained behind the sink-default change, which fails whenever
               pulse named the sink something other than the descriptive udev
               name (then '&&' would skip the path write and nothing switches). */
            rc = jw__exec_shell(output == JW_PLATFORM_AUDIO_OUTPUT_HEADSET
                                    ? JW_MLP1_PLAYBACK_PATH_HP
                                    : JW_MLP1_PLAYBACK_PATH_SPK);
            /* Best effort: point pulse back at the analog (rk817) sink in case we
               were on HDMI. Resolve the live sink name rather than hardcoding. */
            char sink[256];
            if (jw__mlp1_default_audio_sink(sink, sizeof(sink))) {
                char cmd[320];
                snprintf(cmd, sizeof(cmd),
                         "pactl set-default-sink %s 2>/dev/null", sink);
                (void)jw__exec_shell(cmd);
            }
            (void)jw__exec_shell(JW_MLP1_ENSURE_DAC_FLOOR);
            break;
        }
        case JW_PLATFORM_AUDIO_OUTPUT_HDMI:
            rc = jw__exec_shell(JW_MLP1_PACTL_SET_DEFAULT_HDMI);
            break;
        case JW_PLATFORM_AUDIO_OUTPUT_USB:
            /* Mute the analog stage so a speaker that is still on the rk817 path
               doesn't keep playing alongside the headset, then take the sink and
               drag any live streams across (same move HDMI makes). */
            (void)jw__exec_shell(JW_MLP1_PLAYBACK_PATH_OFF);
            rc = jw__exec_shell(
                JW_MLP1_PACTL_SET_DEFAULT_USB "; "
                "pactl list short sink-inputs | while read id _rest; do "
                "pactl move-sink-input \"$id\" " JW_MLP1_PACTL_USB_SINK
                " 2>/dev/null; done");
            break;
        case JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH:
            rc = jw__exec_shell(JW_MLP1_PLAYBACK_PATH_BT);
            break;
        default:
            break;
    }

    if (rc != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "audio output route failed");
        return -1;
    }

    (void)jw__mlp1_apply_stored_audio_volume(output);
    char message[JW_PLATFORM_MAX_MESSAGE];
    snprintf(message, sizeof(message), "audio output: %s",
             jw_platform_audio_output_label(output));
    jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, message, (int)output);
    return 0;
}

/* The headphone jack's input device (event3, "rk817-codec Headphones"). The fd
   is opened once by name and sits in jawakad's poll set, so a plug or unplug
   wakes the daemon; the state is kept from its EV_SW events rather than asked
   for with an ioctl on every pass. -2 = not yet probed, -1 = no device. */
static int s_mlp1_jack_fd = -2;
static int s_mlp1_jack_state = -1;   /* 1 plugged, 0 unplugged, -1 unknown */

static void jw__mlp1_jack_close(void) {
    if (s_mlp1_jack_fd >= 0) {
        close(s_mlp1_jack_fd);
    }
    /* Back to the "not probed" sentinel so the next call re-scans (driver
       rebind, suspend/resume, or a device that had not appeared at boot). */
    s_mlp1_jack_fd = -2;
    s_mlp1_jack_state = -1;
}

/* Re-read the switch with EVIOCGSW: on open, after SYN_DROPPED, and on wake,
   when events may have been lost. */
static void jw__mlp1_jack_resync(void) {
    if (s_mlp1_jack_fd < 0) {
        return;
    }
    unsigned long bits[2] = {0};   /* SW_HEADPHONE_INSERT (2) lives in word 0 */
    if (ioctl(s_mlp1_jack_fd, EVIOCGSW(sizeof(bits)), bits) < 0) {
        jw__mlp1_jack_close();
        return;
    }
    s_mlp1_jack_state = (bits[0] >> SW_HEADPHONE_INSERT) & 1UL ? 1 : 0;
}

static int jw__mlp1_jack_fd(void) {
    if (s_mlp1_jack_fd != -2) {
        return s_mlp1_jack_fd;
    }
    for (int i = 0; i < 32; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/input/event%d", i);
        int f = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (f < 0) {
            continue;
        }
        char name[128] = {0};
        if (ioctl(f, EVIOCGNAME(sizeof(name)), name) >= 0 &&
            strstr(name, "rk817-codec Headphones")) {
            s_mlp1_jack_fd = f;
            jw__mlp1_jack_resync();
            return s_mlp1_jack_fd;
        }
        close(f);
    }
    /* Nothing yet (the codec input device can appear after jawakad starts):
       stay "not probed" so the next call scans again. */
    return -1;
}

/* Returns 1 = plugged, 0 = unplugged, -1 = no device. Drains pending switch
   events first; with nothing queued that is one read returning EAGAIN. */
static int jw__mlp1_jack_input_state(void) {
    int fd = jw__mlp1_jack_fd();
    if (fd < 0) {
        return -1;
    }
    struct input_event ev;
    for (;;) {
        ssize_t got = read(fd, &ev, sizeof(ev));
        if (got == (ssize_t)sizeof(ev)) {
            if (ev.type == EV_SW && ev.code == SW_HEADPHONE_INSERT) {
                s_mlp1_jack_state = ev.value ? 1 : 0;
            } else if (ev.type == EV_SYN && ev.code == SYN_DROPPED) {
                jw__mlp1_jack_resync();
                if (s_mlp1_jack_fd < 0) {
                    return -1;
                }
            }
            continue;
        }
        if (got < 0 && errno == EINTR) {
            continue;
        }
        if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        }
        /* ENODEV or a short read: the node went stale. */
        jw__mlp1_jack_close();
        return -1;
    }
    return s_mlp1_jack_state;
}

/* Re-route audio when the headphone jack changes. On this hardware the rk817
   mutes its DAC (numid=16 -> 0) on jack insert and nothing restores it in Leaf
   (no stock daemon owns the jack), so audio stays silent until a manual volume
   nudge. Re-applying the matching output runs ENSURE_DAC_FLOOR (un-mute) and sets
   the path — doing automatically what the volume press did by hand. Acts only on
   an edge; the steady-state cost is one cheap ioctl per tick. */
/* Cheap "is a Bluetooth audio sink connected" probe: bluealsa publishes an ALSA
   mixer control per connected A2DP/SCO device, so this is a LOCAL query to the
   bluealsa daemon (~30ms, one amixer fork) — no bluetoothctl, no radio paging,
   so it is safe to poll, unlike the Wi-Fi/BT-coexistence-killing bluetoothctl
   polling. Controllers create no bluealsa control, so this only trips for audio. */
static bool jw__mlp1_bt_audio_present(void) {
    char ctl[160];
    return jw__mlp1_bluealsa_control(ctl, sizeof(ctl));
}

static jw_platform_audio_output jw__mlp1_desired_audio_output(bool allow_hdmi) {
    if (allow_hdmi && jw__mlp1_hdmi_tv_active()) {
        return JW_PLATFORM_AUDIO_OUTPUT_HDMI;
    }

    int jack = jw__mlp1_jack_input_state();
    if (jack < 0) {
        jack = jw__mlp1_headphone_jack_present() ? 1 : 0;
    }
    if (jack > 0) {
        return JW_PLATFORM_AUDIO_OUTPUT_HEADSET;
    }
    /* Below the 3.5mm jack, above Bluetooth: plugging a cable in is a more
       deliberate act than a headset that merely happens to be paired and awake. */
    if (jw__mlp1_usb_audio_present() && jw__mlp1_usb_audio_ensure_sink()) {
        return JW_PLATFORM_AUDIO_OUTPUT_USB;
    }
    if (jw__mlp1_bt_audio_present()) {
        return JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH;
    }
    return JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
}

static void jw__mlp1_audio_reconcile(jw_platform_context *ctx, const char *reason) {
    (void)ctx;
    const char *why = (reason && reason[0]) ? reason : "unknown";
    /* Switch events can be lost across a suspend; take the kernel's word. Arm
       the playback-node watch again too, and settle the gate from scratch. */
    jw__mlp1_jack_resync();
    jw__mlp1_pcm_watch_arm();
    jw__mlp1_speaker_gate_sync(true);
    jw_platform_audio_output target = jw__mlp1_desired_audio_output(true);
    jw_platform_audio_output current = jw__mlp1_get_audio_output();

    if (target == JW_PLATFORM_AUDIO_OUTPUT_HDMI) {
        jw__mlp1_hdmi_audio_on();
    }

    jw_platform_result res;
    jw_platform_result_set(&res, JW_PLATFORM_RESULT_OK, "");
    if (current != target) {
        if (jw__mlp1_set_audio_output(target, &res) != 0) {
            jw_log_warn("audio: reconcile %s -> %s failed: %s",
                        why, jw_platform_audio_output_label(target),
                        res.message[0] ? res.message : "route failed");
            return;
        }
    } else {
        (void)jw__mlp1_apply_stored_audio_volume(target);
        if (target == JW_PLATFORM_AUDIO_OUTPUT_SPEAKER ||
            target == JW_PLATFORM_AUDIO_OUTPUT_HEADSET ||
            target == JW_PLATFORM_AUDIO_OUTPUT_HDMI) {
            (void)jw__exec_shell(JW_MLP1_ENSURE_DAC_FLOOR);
        }
    }

    jw_log_info("audio: reconcile %s -> %s",
                why, jw_platform_audio_output_label(target));
}

/* rk817 playback substream state: 1 = a stream is being opened, set up or
   running (OPEN, SETUP, PREPARED, RUNNING), 0 = closed, -1 = anything else
   (XRUN, DRAINING, PAUSED, SUSPENDED) or unreadable. OPEN counts because the
   inotify IN_OPEN that triggers a sync arrives while the stream is in it, ~5 ms
   before SETUP. */
static int jw__mlp1_rk817_playback_state(void) {
    int fd = open(JW_MLP1_RK817_PCM_STATUS, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    char buf[32];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }
    buf[n] = '\0';
    if (strncmp(buf, "state: RUNNING", 14) == 0 ||
        strncmp(buf, "state: PREPARED", 15) == 0 ||
        strncmp(buf, "state: SETUP", 12) == 0 ||
        strncmp(buf, "state: OPEN", 11) == 0) {
        return 1;
    }
    if (strncmp(buf, "closed", 6) == 0) {
        return 0;
    }
    return -1;
}

/* Gate value for an active rk817 stream: off with wired headphones in. */
static int jw__mlp1_speaker_gate_for_playback(void) {
    int jack = jw__mlp1_jack_input_state();
    if (jack < 0) {
        jack = jw__mlp1_headphone_jack_present() ? 1 : 0;
    }
    return jack > 0 ? 0 : 1;
}

/* Own the external speaker amp gate. The rk817 feeds its DAC to the speaker amp
   and the headphone jack at once (Playback Path is a register-level no-op), so
   spk_ctl is both "speaker on" during playback and the only way to mute the
   speaker for wired headphones.

   Raise it (jack empty) as soon as a stream opens: OPEN and SETUP come ~150 ms
   before the first samples reach the DAC, so the amp is on and settled before
   the sound starts. Waiting for RUNNING clipped the start of every sound after
   an idle gap (PulseAudio's suspend-on-idle closes the device after 1 s) and
   switched the amp on under a live DAC. Drop it for headphones, and once the
   device is closed, when DAPM has already powered the output stage down and
   the drop cannot pop. XRUN, DRAINING and PAUSED leave the gate alone so a
   hiccup never toggles the amp. HDMI, USB-C and Bluetooth never open this
   substream, so they need no case of their own.

   The sync runs when something happens: an open or close of the playback
   node (inotify), a jack edge, a wake. Any player counts, PulseAudio or
   straight ALSA, launched by jawakad or not, and an idle launcher costs
   nothing. It replaced a platform.d shell loop that forked grep/sleep five
   times a second, about 10 points of one core at idle. */
static long long s_mlp1_gate_polled_ms = -1;
static int s_mlp1_pcm_watch_fd = -1;
static int s_mlp1_pcm_watch_wd = -1;

/* (Re-)arm the watch. Adding it again for the same inode keeps the same watch;
   a node recreated by a driver rebind gets a new one. */
static void jw__mlp1_pcm_watch_arm(void) {
    if (s_mlp1_pcm_watch_fd < 0) {
        s_mlp1_pcm_watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        if (s_mlp1_pcm_watch_fd < 0) {
            return;
        }
    }
    s_mlp1_pcm_watch_wd = inotify_add_watch(s_mlp1_pcm_watch_fd,
                                            JW_MLP1_RK817_PCM_DEV,
                                            IN_OPEN | IN_CLOSE);
}

/* True when the playback node was opened or closed since the last call, or
   its watch went away (then it is re-armed on the fallback poll). */
static bool jw__mlp1_pcm_watch_drain(void) {
    if (s_mlp1_pcm_watch_fd < 0) {
        return false;
    }
    char buf[1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    bool any = false;
    for (;;) {
        ssize_t n = read(s_mlp1_pcm_watch_fd, buf, sizeof(buf));
        if (n <= 0) {
            break;   /* EAGAIN: drained */
        }
        for (char *p = buf; p < buf + n;) {
            const struct inotify_event *ev = (const struct inotify_event *)p;
            if ((ev->mask & IN_IGNORED) && ev->wd == s_mlp1_pcm_watch_wd) {
                s_mlp1_pcm_watch_wd = -1;
            }
            any = true;
            p += sizeof(*ev) + ev->len;
        }
    }
    return any;
}

static void jw__mlp1_speaker_gate_sync(bool force) {
    long long now = jw__monotonic_ms();
    bool changed = jw__mlp1_pcm_watch_drain();
    if (s_mlp1_pcm_watch_wd < 0 &&
        (s_mlp1_gate_polled_ms < 0 ||
         now - s_mlp1_gate_polled_ms >= JW_MLP1_SPK_GATE_FALLBACK_POLL_MS)) {
        jw__mlp1_pcm_watch_arm();
        force = true;
    }
    if (!force && !changed) {
        return;
    }
    s_mlp1_gate_polled_ms = now;

    int playback = jw__mlp1_rk817_playback_state();
    if (playback < 0) {
        return;
    }
    int want = playback == 1 ? jw__mlp1_speaker_gate_for_playback() : 0;
    int current = jw__read_int_file(JW_MLP1_SPK_CTL);
    if (current == want) {
        return;
    }
    static bool warned = false;
    if (jw__write_int_file(JW_MLP1_SPK_CTL, want) != 0) {
        if (!warned) {
            warned = true;
            jw_log_warn("audio: speaker gate write failed: %s", strerror(errno));
        }
        return;
    }
    warned = false;
}

/* Set by the SLEEP action when it suspended a playing sink; the sink is
   resumed by jw__mlp1_wake_audio once the daemon has put the CPU, GPU and
   DMC back on their wake profile. Resuming it inside the action ran the
   refill of a 2 s buffer at 408 MHz CPU / 324 MHz DMC (~375 ms of one
   realtime thread, past PulseAudio's old 200 ms RLIMIT_RTTIME). */
static bool s_mlp1_sink_resume_pending = false;

static void jw__mlp1_wake_audio(jw_platform_context *ctx) {
    (void)ctx;
    /* Before the sink reopens the device, so its IN_OPEN cannot be missed. */
    jw__mlp1_pcm_watch_arm();
    if (!s_mlp1_sink_resume_pending) {
        return;
    }
    s_mlp1_sink_resume_pending = false;
    /* Raise the gate before the stream comes back rather than leaving it to
       the audio tick, which only runs after the service rescan and the audio
       reconcile: ~0.8 s of a game playing into a muted speaker. The DAC is
       still idle here, so the amp comes up silent. */
    (void)jw__write_int_file(JW_MLP1_SPK_CTL, jw__mlp1_speaker_gate_for_playback());
    (void)jw__exec_shell(JW_MLP1_PA_RESUME_SINK);
}

static unsigned jw__mlp1_audio_tick(jw_platform_context *ctx) {
    unsigned events = 0;

    /* A caller that performed SLEEP without jw_platform_wake_audio(). */
    jw__mlp1_wake_audio(ctx);

    /* Reap the test-clip player if it finished on its own (keeps it from
       lingering as a zombie and keeps the play/stop state accurate). */
    (void)jw__mlp1_test_sound_active();

    jw__mlp1_speaker_gate_sync(false);

    /* ── Headphone-jack edge (cheap kernel switch, checked every loop). ──
       On unplug, fall back to Bluetooth if a headset is still connected, not
       always the speaker — otherwise removing wired headphones strands audio on
       the speaker even though the BT headset is live (and leaves the codec path
       off bluealsa, so BT stays silent in games/apps until the headset
       reconnects). */
    static int last_present = -1;
    int present = jw__mlp1_jack_input_state();
    if (present >= 0 && present != last_present && !jw__mlp1_hdmi_tv_active()) {
        last_present = present;
        jw_platform_result res;
        jw_platform_result_set(&res, JW_PLATFORM_RESULT_OK, "");
        jw_platform_audio_output target = jw__mlp1_desired_audio_output(true);
        jw__mlp1_set_audio_output(target, &res);
        /* Mute or unmute the speaker now rather than on the next poll. */
        jw__mlp1_speaker_gate_sync(true);
        events |= JW_PLATFORM_AUDIO_EVENT_OUTPUT_CHANGED;
        jw_log_info("audio: headphone jack %s, routed to %s",
                    present ? "inserted" : "removed",
                    target == JW_PLATFORM_AUDIO_OUTPUT_HEADSET   ? "headset" :
                    target == JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH ? "bluetooth" : "speaker");
    }

    /* ── USB-C audio attach/detach edge (throttled). ──
       Reading /proc/asound/cards is a popen per check, so keep it off the fast
       path. On detach the sink must be dropped before re-routing: its ALSA card
       is already gone and anything still pointed at it plays into nothing. */
    static long long usb_next_ms = 0;
    static int last_usb = -1;
    long long usb_now = jw__monotonic_ms();
    if (usb_now >= usb_next_ms) {
        usb_next_ms = usb_now + 1000;
        int usb = jw__mlp1_usb_audio_present() ? 1 : 0;
        if (usb != last_usb) {
            int prev = last_usb;
            last_usb = usb;
            if (prev >= 0 && !jw__mlp1_hdmi_tv_active()) {
                if (!usb) {
                    jw__mlp1_usb_audio_drop();
                }
                jw_platform_result res;
                jw_platform_result_set(&res, JW_PLATFORM_RESULT_OK, "");
                jw_platform_audio_output target = jw__mlp1_desired_audio_output(true);
                jw__mlp1_set_audio_output(target, &res);
                /* Last, once the codec path and default sink are already correct:
                   the reset only sticks if there is nothing left to reconfigure
                   the device afterwards. */
                if (!usb) {
                    jw__mlp1_analog_sink_reset();
                }
                events |= JW_PLATFORM_AUDIO_EVENT_OUTPUT_CHANGED;
                jw_log_info("audio: usb-c audio %s, routed to %s",
                            usb ? "attached" : "detached",
                            jw_platform_audio_output_label(target));
            } else if (prev < 0 && !usb) {
                /* First sample with nothing attached: make sure a stale sink from
                   a previous session can't linger. */
                jw__mlp1_usb_audio_drop();
            }
        }
    }

    /* ── Bluetooth audio connect/disconnect edge (throttled). ──
       Auto-routes a headset that (dis)connects OUTSIDE the Bluetooth settings
       page — auto-reconnect, powering the headset on/off, dropping out of range.
       On connect we only take over from the speaker (a plugged-in wired headset
       or HDMI wins); on disconnect we leave Bluetooth for the jack or speaker. */
    static long long bt_next_ms = 0;
    static int last_bt = -1;
    long long now = jw__monotonic_ms();
    if (now >= bt_next_ms) {
        bt_next_ms = now + 1500;
        int bt = jw__mlp1_bt_audio_present() ? 1 : 0;
        if (bt != last_bt) {
            int prev = last_bt;
            last_bt = bt;
            if (prev >= 0 && !jw__mlp1_hdmi_tv_active()) {   /* skip first sample; HDMI TV wins */
                jw_platform_audio_output cur = jw__mlp1_get_audio_output();
                jw_platform_result res;
                jw_platform_result_set(&res, JW_PLATFORM_RESULT_OK, "");
                if (bt) {
                    jw_platform_audio_output target = jw__mlp1_desired_audio_output(true);
                    if (cur == JW_PLATFORM_AUDIO_OUTPUT_SPEAKER &&
                        target == JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH) {
                        jw__mlp1_set_audio_output(target, &res);
                        jw_log_info("audio: bluetooth connected, routed to bluetooth");
                    }
                    events |= JW_PLATFORM_AUDIO_EVENT_BLUETOOTH_CONNECTED |
                              JW_PLATFORM_AUDIO_EVENT_OUTPUT_CHANGED;
                } else if (cur == JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH) {
                    jw_platform_audio_output fb = jw__mlp1_desired_audio_output(true);
                    jw__mlp1_set_audio_output(fb, &res);
                    jw_log_info("audio: bluetooth disconnected, routed to %s",
                                fb == JW_PLATFORM_AUDIO_OUTPUT_HEADSET ? "headset" : "speaker");
                    events |= JW_PLATFORM_AUDIO_EVENT_BLUETOOTH_DISCONNECTED |
                              JW_PLATFORM_AUDIO_EVENT_OUTPUT_CHANGED;
                }
            }
        }
    }
    return events;
}

static void jw__mlp1_set_auto_sleep(int seconds, jw_platform_result *out) {
    if (seconds < 0) seconds = 0;
    if (seconds > 86400) seconds = 86400;

    /* Boot-time sync calls this with an unchanged setting on every boot; when
       the stock stores already match, skip the rewrites and especially the
       loong_power restart, which otherwise lands while the launcher is waiting
       on its hello and adds ~300ms to time-to-first-frame. */
    if (jw__mlp1_display_param_matches(seconds) && jw__mlp1_power_cfg_matches(seconds)) {
        jw_log_info("stock auto-sleep already in sync (%ds)", seconds);
        char synced[JW_PLATFORM_MAX_MESSAGE];
        if (seconds <= 0) {
            snprintf(synced, sizeof(synced), "stock auto-sleep disabled");
        } else {
            snprintf(synced, sizeof(synced), "stock auto-sleep set to %ds", seconds);
        }
        jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, synced, seconds);
        return;
    }

    int display_rc = jw__mlp1_update_display_param(seconds);
    int power_rc = jw__mlp1_update_power_cfg(seconds);
    int live_rc = (display_rc == 0 && power_rc == 0) ? jw__mlp1_apply_power_cfg_live() : 0;
    if (display_rc != 0 || power_rc != 0 || live_rc != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "stock auto-sleep sync failed");
        return;
    }

    char message[JW_PLATFORM_MAX_MESSAGE];
    if (seconds <= 0) {
        snprintf(message, sizeof(message), "stock auto-sleep disabled");
    } else {
        snprintf(message, sizeof(message), "stock auto-sleep set to %ds", seconds);
    }
    jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, message, seconds);
}

static void jw__mlp1_get_status(jw_platform_context *ctx, jw_platform_status *out) {
    if (!out) {
        return;
    }

    int battery = jw__read_int_file("/sys/class/power_supply/battery/capacity");
    if (battery >= 0 && battery <= 100) {
        out->battery_percent = battery;
    }

    int charger = jw__read_int_file("/sys/class/power_supply/ac/online");
    if (charger != 1) {
        charger = jw__read_int_file("/sys/class/power_supply/usb/online");
    }
    if (charger >= 0) {
        out->charging = (charger == 1) ? 1 : 0;
    }

    out->brightness_percent = jw__mlp1_get_brightness_percent();
    out->volume_percent = jw__mlp1_get_volume_percent();
    jw__mlp1_get_wifi_status(out);

    if (jw__mlp1_adb_supported()) {
        out->adb_enabled = jw__mlp1_adb_is_pinned() ? 1 : 0;
        out->adb_intent_enabled = jw__mlp1_adb_intent_enabled(ctx) ? 1 : 0;
    }
    out->boot_splash_enabled = jw__mlp1_boot_splash_enabled(ctx) ? 1 : 0;
    out->refresh_rate_hz = jw__mlp1_get_refresh_hz();
    out->color_temp_kelvin = jw__mlp1_get_color_temp();
    out->hdmi_connected = jw__mlp1_hdmi_connected() ? 1 : 0;
    out->hdmi_output_mode = s_mlp1_hdmi_mode;
}

static void jw__mlp1_get_audio_status(jw_platform_context *ctx, jw_platform_status *out) {
    (void)ctx;
    if (!out) {
        return;
    }
    out->volume_percent = jw__mlp1_get_volume_percent();
    out->audio_output = jw__mlp1_get_audio_output();
    out->audio_available_outputs = jw__mlp1_get_audio_available_outputs();
    jw__mlp1_get_audio_volumes(out->audio_volume_percent);
    if (out->audio_output >= 0 && out->audio_output < JW_PLATFORM_AUDIO_OUTPUT_COUNT &&
        out->volume_percent >= 0) {
        out->audio_volume_percent[out->audio_output] = out->volume_percent;
    }
    out->audio_test_playing = jw__mlp1_test_sound_active() ? 1 : 0;
}

static void jw__mlp1_frontend_ready(jw_platform_context *ctx, const char *role,
                                    jw_platform_result *out) {
    if (strcmp(role, "launcher") != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "frontend ready noted");
        return;
    }

    if (ctx->home_ready_sent) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "home ready already sent");
        return;
    }

    if (jw__env_truthy("UMRK_LEAF_MODE")) {
        (void)jw__exec_shell("killall loong_transition >/dev/null 2>&1 || true");
        jw_log_info("loong: dismissed Leaf boot transition");
        ctx->home_ready_sent = true;
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               "leaf boot transition dismissed");
        return;
    }

    jw__loong_load();
    if (!s_event_opend) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "loong SDK EventOpend unavailable");
        return;
    }

    int rc = s_event_opend("HOME");
    jw_log_info("loong: EventOpend(\"HOME\") -> %d (dismissing boot transition)", rc);
    (void)jw__exec_shell("killall loong_transition >/dev/null 2>&1 || true");
    ctx->home_ready_sent = true;
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "home ready sent");
}

/* The test-clip player, tracked so it can be stopped (and reaped). -1 = idle. */
static pid_t g_test_sound_pid = -1;

/* True while the test clip is still playing; reaps the player if it has finished
   so it never lingers as a zombie. Cheap (one WNOHANG waitpid). */
static bool jw__mlp1_test_sound_active(void) {
    if (g_test_sound_pid <= 0) {
        return false;
    }
    if (waitpid(g_test_sound_pid, NULL, WNOHANG) == 0) {
        return true;            /* still running */
    }
    g_test_sound_pid = -1;      /* exited (or already gone) */
    return false;
}

/* Toggle the bundled "Leaf" test clip on whatever the current audio output is,
   so the user (and we) can verify sound on this device. A second press stops it.
   Speaker/headset/HDMI play through PulseAudio (paplay -> the default sink the
   codec is routed to); Bluetooth goes straight to the BlueALSA A2DP pcm (it is
   not a Pulse sink), via the `plug` wrapper so the clip's rate/format/channels
   are converted to the A2DP link. The player is a tracked child so a second tap
   can SIGTERM it; the daemon never blocks on playback. */
static void jw__mlp1_play_test_sound(jw_platform_result *out) {
    if (jw__mlp1_test_sound_active()) {
        kill(g_test_sound_pid, SIGTERM);
        waitpid(g_test_sound_pid, NULL, 0);
        g_test_sound_pid = -1;
        jw_log_info("audio: test sound stopped");
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "test sound stopped");
        return;
    }

    const char *base = getenv("UMRK_LAUNCHER_PATH");
    char wav[PATH_MAX];
    if (base && base[0]) {
        snprintf(wav, sizeof(wav), "%s/res/sounds/leaf-test.wav", base);
    } else {
        snprintf(wav, sizeof(wav),
                 "/mnt/sdcard/.system/leaf/platforms/mlp1/launcher/res/sounds/leaf-test.wav");
    }
    if (access(wav, R_OK) != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "test sound file missing");
        return;
    }

    bool bt = (jw__mlp1_get_audio_output() == JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH);
    pid_t pid = fork();
    if (pid < 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "test sound spawn failed");
        return;
    }
    if (pid == 0) {
        /* Silence the player's chatter; exec it directly (no shell) so this pid
           IS the player and SIGTERM stops it. */
        int devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
        if (devnull >= 0) {
            dup2(devnull, STDOUT_FILENO);
            dup2(devnull, STDERR_FILENO);
        }
        if (bt) {
            execlp("aplay", "aplay", "-q", "-D", "plug:bluealsa", wav, (char *)NULL);
        } else {
            execlp("paplay", "paplay", wav, (char *)NULL);
        }
        _exit(127);
    }
    g_test_sound_pid = pid;
    jw_log_info("audio: playing test sound (%s) pid=%d", bt ? "bluealsa" : "pulse", (int)pid);
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "playing test sound");
}

/* True when the PulseAudio sink is actively playing (a game/music stream is
   pulling it). Used to decide whether to quiesce/restore audio around suspend:
   if nothing is playing, PA has already powered the codec down, and forcing the
   sink back open on resume would power an idle output stage back up — which
   hisses — for no benefit (there is nothing to recover). */
static bool jw__mlp1_pa_sink_running(void) {
    FILE *fp = popen("pactl list short sinks 2>/dev/null", "r");
    if (!fp) {
        return false;
    }
    char line[256];
    bool running = false;
    while (fgets(line, sizeof(line), fp)) {
        if (strstr(line, "RUNNING")) {
            running = true;
            break;
        }
    }
    pclose(fp);
    return running;
}

/* Read one rk817 codec register over i2c (bus 0, addr 0x20). Returns the byte
   value (0-255) or -1 on failure. Used to snapshot the headphone-amp registers
   before the pre-suspend power-down so they can be restored exactly on resume. */
static int jw__mlp1_read_codec_reg(int reg) {
    char cmd[80];
    snprintf(cmd, sizeof(cmd), "i2cget -f -y 0 0x20 0x%02x 2>/dev/null", reg & 0xff);
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        return -1;
    }
    char buf[32];
    int val = -1;
    if (fgets(buf, sizeof(buf), fp)) {
        val = (int)strtol(buf, NULL, 0);
    }
    pclose(fp);
    return (val >= 0 && val <= 0xff) ? val : -1;
}

/* True once jw__mlp1_sleep_audio has quiesced audio for the coming SLEEP. */
static bool s_mlp1_sleep_audio_done = false;

/* Suspend a playing sink while the daemon's clocks are still up: a refill
   that the sleep profile's drop to 408 MHz catches mid-way ran 344 ms on
   PulseAudio's realtime thread (measured), past the old 200 ms limit. */
static void jw__mlp1_sleep_audio(jw_platform_context *ctx) {
    (void)ctx;
    bool playing = jw__mlp1_pa_sink_running();
    if (playing) {
        (void)jw__exec_shell(JW_MLP1_PA_SUSPEND_SINK);
        usleep(200000);
    }
    s_mlp1_sink_resume_pending = playing;
    s_mlp1_sleep_audio_done = true;
}

static void jw__mlp1_perform_action(jw_platform_context *ctx, jw_platform_action action,
                                    int value, jw_platform_result *out) {
    if (action == JW_PLATFORM_ACTION_PLAY_TEST_SOUND) {
        jw__mlp1_play_test_sound(out);
        return;
    }

    if (action == JW_PLATFORM_ACTION_POWEROFF) {
        jw_log_info("platform: poweroff requested");
        if (jw_power_request_publish("poweroff") != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "safe power handoff unavailable");
            return;
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "powering off");
        return;
    }

    if (action == JW_PLATFORM_ACTION_REBOOT) {
        jw_log_info("platform: reboot requested");
        if (jw_power_request_publish("reboot") != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "safe power handoff unavailable");
            return;
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "rebooting");
        return;
    }

    if (action == JW_PLATFORM_ACTION_SLEEP) {
        jw_log_info("platform: sleep -> real suspend-to-RAM");
        /* Real deep suspend via `echo mem`. (The earlier loong PowerApi::powerStandby()
           path only blanked the screen — the system never actually suspended.) The
           input proxy holds the power key exclusively, so loong_power can't see the
           wake press and re-suspend us (the two-press-wake gotcha); the PMIC wake
           source (rk805-pwrkey) resumes the kernel on a single press. The write
           blocks here until we resume. */
        /* Two independent audio problems across suspend:

           1. Audio return. A raw `echo mem` leaves an open PulseAudio sink / ALSA
              device stale, so a live game/music stream comes back silent until it
              is relaunched. When a stream is playing, suspend the sink first to
              close the device cleanly, and un-suspend on resume to continue it.

           2. Headphone hiss. The rk817 headphone amp has no external gate (the
              speaker's spk_ctl is dropped below, so speaker sleeps are already
              silent), so it stays biased through `echo mem` and hisses once the
              I2S clocks stop — whether or not anything was playing, and suspending
              the sink does NOT fix it (verified: it re-hisses through a real
              suspend). The Playback Path control is a register-level no-op. What
              works is powering the analog HP stage down over i2c (AHP_CFG0 + its
              charge pump) before suspend and restoring it on resume. This touches
              ONLY the HP amp, never the shared DAC, so the speaker is
              unaffected. Done unconditionally — it is cheap and DAC-safe
              — so a jack change during sleep can never leave a biased amp hissing.
              Snapshot the two registers first so the restore matches the live
              state (idle vs active playback). */
        /* Normally done already, before the sleep profile. */
        if (!s_mlp1_sleep_audio_done) {
            jw__mlp1_sleep_audio(ctx);
        }
        s_mlp1_sleep_audio_done = false;
        int hp_r3d = jw__mlp1_read_codec_reg(0x3d);
        int hp_r3f = jw__mlp1_read_codec_reg(0x3f);
        (void)jw__exec_shell(JW_MLP1_HP_POWERDOWN);
        /* Speaker amp off for the sleep. The sink is suspended or already idle,
           so nothing is driving it. A stream that was playing gets the gate
           back just before its sink resumes, below. */
        (void)jw__write_int_file(JW_MLP1_SPK_CTL, 0);
        int sfd = open("/sys/power/state", O_WRONLY | O_CLOEXEC);
        int rc = -1;
        if (sfd >= 0) {
            rc = (write(sfd, "mem\n", 4) == 4) ? 0 : -1;   /* blocks until resume */
            close(sfd);
        }
        /* Firmware may restore either card with noexec after resume. This is
           observable when the marked launcher card moved to /media/sdcard1:
           the already-running daemon survives, but every service restart and
           control helper then fails with EACCES. Repair both dynamic roles
           before returning to the daemon loop, where suspend-sensitive
           services are eligible to start again. */
        jw_mlp1_platform_data *resume_data = NULL;
        if (rc == 0) {
            resume_data =
                ctx ? (jw_mlp1_platform_data *)ctx->backend_data : NULL;
            if (resume_data) {
                /* Arm before the immediate attempt so a transient failure still
                   gets the firmware-settle pass. Its deadline starts after all
                   synchronous resume work below has finished. */
                resume_data->resume_mount_repair_pending = true;
            }
            /* Leave any detached SD cwd immediately. The firmware's late
               post-resume mount worker can recreate the card more than once,
               so merely chdir'ing to the first visible instance is not enough. */
            if (chdir("/") != 0) {
                jw_log_warn("platform: resume could not leave stale SD cwd: %s",
                            strerror(errno));
                rc = -1;
            }
            bool active_needed_repair = false;
            int active_rc = -1;
            int secondary_rc = -1;
            /* Repair once before services resume, then return immediately. The
               firmware's later mount pass is checked from the platform tick so
               a lit screen is never paired with a fixed multi-second input stall. */
            if (jw__mlp1_mount_has_option(ctx->sdcard_root, "noexec")) {
                active_needed_repair = true;
            }
            active_rc = jw__mlp1_remount_exec(ctx->sdcard_root);
            secondary_rc = jw__mlp1_block_present(ctx)
                               ? jw__mlp1_mount_secondary_if_needed(ctx)
                               : 0;
            if (active_rc != 0 || secondary_rc != 0) {
                jw_log_warn("platform: resume SD exec repair failed "
                            "active=%s active_rc=%d secondary_rc=%d",
                            ctx->sdcard_root, active_rc, secondary_rc);
                rc = -1;
            } else {
                const char *launcher = getenv("UMRK_LAUNCHER_PATH");
                if (!launcher || !launcher[0]) {
                    launcher = strcmp(ctx->sdcard_root, "/media/sdcard1") == 0
                                   ? "/media/sdcard1/.system/leaf/platforms/mlp1/launcher"
                                   : "/mnt/sdcard/.system/leaf/platforms/mlp1/launcher";
                }
                /* The firmware may unmount and recreate the active mount while
                   suspended. A cwd held on the detached old mount then makes
                   every later shell child fail getcwd even though the path is
                   visible again. Re-enter the live launcher directory before
                   audio restoration or any service is restarted. */
                if (chdir(launcher) != 0) {
                    jw_log_warn("platform: resume could not refresh launcher "
                                "cwd %s: %s", launcher, strerror(errno));
                    rc = -1;
                } else {
                    jw_log_info("platform: resume SD mounts settled and "
                                "launcher cwd refreshed active=%s repaired=%d",
                                ctx->sdcard_root,
                                active_needed_repair ? 1 : 0);
                }
            }
        }
        /* Resumed: restore the two HP-amp registers (charge pump then output
           stage), falling back to the codec's power-up defaults if a snapshot read
           failed. Re-assert the DAC floor (PulseAudio can drift it low). A
           sink suspended above is resumed later, by jw__mlp1_wake_audio. */
        char hp_restore[160];
        snprintf(hp_restore, sizeof(hp_restore),
                 "i2cset -f -y 0 0x20 0x3f 0x%02x >/dev/null 2>&1; "
                 "i2cset -f -y 0 0x20 0x3d 0x%02x >/dev/null 2>&1",
                 hp_r3f >= 0 ? hp_r3f : 0x11, hp_r3d >= 0 ? hp_r3d : 0x80);
        (void)jw__exec_shell(hp_restore);
        (void)jw__exec_shell(JW_MLP1_ENSURE_DAC_FLOOR);
        /* A sink suspended by jw__mlp1_sleep_audio comes back in
           jw__mlp1_wake_audio, after the daemon has restored its wake
           performance profile. */
        if (resume_data) {
            resume_data->resume_mount_repair_at_ms = jw__monotonic_ms() + 2500;
        }
        jw_platform_result_set(out,
                               rc == 0 ? JW_PLATFORM_RESULT_OK : JW_PLATFORM_RESULT_FAILED,
                               rc == 0 ? "suspended" : "sleep failed");
        return;
    }

    if (action == JW_PLATFORM_ACTION_SCREEN_OFF ||
        action == JW_PLATFORM_ACTION_SCREEN_ON) {
        int blank = (action == JW_PLATFORM_ACTION_SCREEN_OFF) ? 4 : 0;
        if (jw__write_int_file(JW_MLP1_BACKLIGHT_BL_POWER, blank) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   "backlight blank write failed");
            return;
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               blank ? "screen off" : "screen on");
        return;
    }

    if (action == JW_PLATFORM_ACTION_SET_AUTO_SLEEP) {
        jw__mlp1_set_auto_sleep(value, out);
        return;
    }

    if (action == JW_PLATFORM_ACTION_SET_VOLUME) {
        int percent = value;
        if (percent < 0) percent = 0;
        if (percent > 100) percent = 100;
        if (jw__mlp1_set_volume_percent(percent) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   "volume set failed");
            return;
        }
        char message[JW_PLATFORM_MAX_MESSAGE];
        snprintf(message, sizeof(message), "volume set to %d%%", percent);
        jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, message, percent);
        return;
    }

    if (action == JW_PLATFORM_ACTION_SET_AUDIO_OUTPUT) {
        (void)jw__mlp1_set_audio_output((jw_platform_audio_output)value, out);
        return;
    }

    if (action == JW_PLATFORM_ACTION_BLUETOOTH_ON ||
        action == JW_PLATFORM_ACTION_BLUETOOTH_OFF) {
        bool on = (action == JW_PLATFORM_ACTION_BLUETOOTH_ON);
        if (jw_bt_set_radio(on) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   on ? "bluetooth enable failed"
                                      : "bluetooth disable failed");
            return;
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               on ? "bluetooth enabled" : "bluetooth disabled");
        return;
    }

    if (action == JW_PLATFORM_ACTION_ENABLE_ADB) {
        jw__mlp1_enable_adb(ctx, out);
        return;
    }

    if (action == JW_PLATFORM_ACTION_DISABLE_ADB) {
        jw__mlp1_disable_adb(ctx, out);
        return;
    }

    if (action == JW_PLATFORM_ACTION_SET_BOOT_SPLASH) {
        bool enabled = value != 0;
        if (jw__mlp1_set_boot_splash(ctx, enabled) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   enabled ? "boot splash enable failed"
                                           : "boot splash disable failed");
            return;
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               enabled ? "boot splash enabled"
                                       : "boot splash disabled");
        return;
    }

    if (action == JW_PLATFORM_ACTION_SET_REFRESH_RATE) {
        /* Clamp to the panel's verified range; <=60 removes the override. */
        int hz = value;
        if (hz < 60) hz = 60;
        if (hz > 120) hz = 120;
        if (jw__mlp1_set_refresh_rate(hz) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   "refresh rate change failed");
            return;
        }
        jw_log_info("display: refresh rate -> %d Hz", hz);
        char msg[32];
        snprintf(msg, sizeof(msg), "switching to %d Hz", hz);
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, msg);
        return;
    }

    if (action == JW_PLATFORM_ACTION_SET_COLOR_TEMP) {
        /* This LUT compensates for the internal panel's known warm cast; the
           value has no meaning on an external TV, and DSI and HDMI share the
           one CRTC on this device, so applying it while HDMI is active would
           just tint whatever the TV is showing. While HDMI is the active
           output only a neutral request is honoured: it writes an identity ramp
           so the TV isn't tinted, and leaves the tracked panel value alone.
           Anything else is refused; jawakad sends the neutral request, and the
           panel value again once the panel is back (see jw__tick_hdmi). */
        if (jw__mlp1_hdmi_tv_active()) {
            if (jw_platform_clamp_color_temp_k(value) != JW_PLATFORM_COLOR_TEMP_NEUTRAL_K) {
                jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                                       "Color temperature unavailable while HDMI is active");
                return;
            }
            if (jw__mlp1_write_gamma_ramp(1.0, 1.0, 1.0) != 0) {
                jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                       "Color temperature reset for the external display failed");
                return;
            }
            jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                                   "Color temperature neutral for the external display");
            return;
        }
        int kelvin = jw_platform_clamp_color_temp_k(value);
        if (jw__mlp1_set_color_temp(kelvin) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   "Color temperature change failed");
            return;
        }
        jw_log_info("display: color temperature -> %d K", kelvin);
        char msg[40];
        if (kelvin == JW_PLATFORM_COLOR_TEMP_NEUTRAL_K) {
            snprintf(msg, sizeof(msg), "Color temperature neutral");
        } else {
            snprintf(msg, sizeof(msg), "Color temperature %d K", kelvin);
        }
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, msg);
        return;
    }

    if (action == JW_PLATFORM_ACTION_SET_HDMI_OUTPUT) {
        int mode = value;
        if (mode < JW_MLP1_HDMI_OFF || mode > JW_MLP1_HDMI_STRETCH) {
            mode = JW_MLP1_HDMI_OFF;
        }
        if (jw__mlp1_set_hdmi_output(mode) != 0) {
            jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                                   "HDMI output change failed");
            return;
        }
        /* Audio follows the display: TV on -> HDMI sink; back to the panel ->
           the analog codec, honoring a plugged headset / live Bluetooth. */
        if (mode == JW_MLP1_HDMI_OFF) {
            jw__mlp1_hdmi_audio_off();
            jw_platform_result ar;
            jw_platform_result_set(&ar, JW_PLATFORM_RESULT_OK, "");
            jw_platform_audio_output tgt = jw__mlp1_desired_audio_output(false);
            (void)jw__mlp1_set_audio_output(tgt, &ar);
        } else {
            jw__mlp1_hdmi_audio_on();
        }
        static const char *const names[] = { "off", "4:3", "stretch" };
        jw_log_info("display: HDMI output -> %s", names[mode]);
        char msg[48];
        snprintf(msg, sizeof(msg), "HDMI output %s", names[mode]);
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, msg);
        return;
    }

    if (action != JW_PLATFORM_ACTION_SET_BRIGHTNESS) {
        jw_platform_result_unsupported(action, ctx ? ctx->platform_id : "mlp1", out);
        return;
    }

    int percent = jw_platform_clamp_brightness_percent(value);
    int max_raw = jw__read_int_file(JW_MLP1_BACKLIGHT_MAX);
    int raw = jw__brightness_percent_to_raw(percent, max_raw);
    if (raw < 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "backlight max brightness unavailable");
        return;
    }
    if (jw__write_int_file(JW_MLP1_BACKLIGHT_BRIGHTNESS, raw) != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "backlight brightness write failed");
        return;
    }
    {
        jw_mlp1_platform_data *data =
            ctx ? (jw_mlp1_platform_data *)ctx->backend_data : NULL;
        if (data) {
            data->last_applied_raw = raw;
        }
    }

    char message[JW_PLATFORM_MAX_MESSAGE];
    snprintf(message, sizeof(message), "brightness set to %d%%", percent);
    jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, message, percent);
}

/* Is a charger attached? Either rail counts -- the device reports AC and USB
   separately and a plain USB-C charger only lights the second. */
static int jw__mlp1_charger_online(void) {
    int ac = jw__read_int_file("/sys/class/power_supply/ac/online");
    int usb = jw__read_int_file("/sys/class/power_supply/usb/online");
    if (ac < 0 && usb < 0) return -1;
    return ((ac > 0) || (usb > 0)) ? 1 : 0;
}

/* Re-assert the right brightness after stock has had its say. Deliberately runs
   AFTER a settle delay rather than racing loong_power: it writes raw 130 on the
   transition, and whoever writes last wins. Losing that race would leave the
   panel at 130 with Leaf believing otherwise, so we let stock go first and
   correct it. Costs a brief flash to full brightness; the alternative is a
   flicker loop between two daemons. */
static void jw__mlp1_apply_charge_brightness(jw_mlp1_platform_data *data, int online) {
    if (!data || data->last_applied_raw <= 0) {
        return;
    }
    int want = data->last_applied_raw;
    if (online > 0 && want < JW_MLP1_BACKLIGHT_CHARGE_MIN_RAW) {
        want = JW_MLP1_BACKLIGHT_CHARGE_MIN_RAW;
    }
    int now_raw = jw__read_int_file(JW_MLP1_BACKLIGHT_ACTUAL);
    if (now_raw == want) {
        return;
    }
    if (jw__write_int_file(JW_MLP1_BACKLIGHT_BRIGHTNESS, want) == 0) {
        jw_log_info("backlight: charger %s, raw %d -> %d (user %d)",
                    online > 0 ? "attached" : "removed", now_raw, want,
                    data->last_applied_raw);
    }
}

static bool jw__mlp1_storage_tick(jw_platform_context *ctx) {
    jw_mlp1_platform_data *data = ctx ? (jw_mlp1_platform_data *)ctx->backend_data : NULL;
    if (!data) {
        return false;
    }

    bool changed = false;
    if (data->resume_mount_repair_pending &&
        jw__monotonic_ms() >= data->resume_mount_repair_at_ms) {
        data->resume_mount_repair_pending = false;
        int active_rc = jw__mlp1_remount_exec(ctx->sdcard_root);
        int secondary_rc = jw__mlp1_block_present(ctx)
                               ? jw__mlp1_mount_secondary_if_needed(ctx)
                               : 0;
        if (active_rc != 0 || secondary_rc != 0) {
            jw_log_warn("platform: delayed resume SD exec repair failed "
                        "active_rc=%d secondary_rc=%d",
                        active_rc, secondary_rc);
        } else {
            jw_log_info("platform: delayed resume SD exec repair complete");
        }
    }

    if (data->uevent_fd >= 0) {
        char buf[4096];
        while (1) {
            ssize_t n = recv(data->uevent_fd, buf, sizeof(buf) - 1, 0);
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                    jw_log_warn("storage hotplug: uevent recv failed: %s", strerror(errno));
                }
                break;
            }
            if (n == 0) {
                break;
            }
            buf[n] = '\0';
            if (strstr(buf, "mmcblk1") || strstr(buf, "mmcblk3")) {
                data->pending_storage_event = true;
                data->debounce_until_ms = jw__monotonic_ms() + JW_MLP1_STORAGE_DEBOUNCE_MS;
            }
        }
    }

    /* Poll rather than watch uevents. The netlink match on "power_supply" was
       tried first and never fired on this device -- either the charger driver
       does not broadcast or it does not carry that string -- and the failure is
       silent, which is the worst kind. Two small sysfs reads on a throttle
       cannot miss a transition. */
    long long now_ms = jw__monotonic_ms();
    if (now_ms >= data->charge_poll_next_ms) {
        data->charge_poll_next_ms = now_ms + JW_MLP1_CHARGE_POLL_MS;
        int online_now = jw__mlp1_charger_online();
        if (online_now >= 0 && online_now != data->last_ac_online) {
            data->last_ac_online = online_now;
            data->pending_charge_event = true;
            /* Let stock write its 130 first; whoever writes last wins. */
            data->charge_settle_until_ms = now_ms + JW_MLP1_CHARGE_SETTLE_MS;
        }
    }

    if (data->pending_charge_event &&
        jw__monotonic_ms() >= data->charge_settle_until_ms) {
        data->pending_charge_event = false;
        jw__mlp1_apply_charge_brightness(data, data->last_ac_online);
    }

    int present = jw__mlp1_block_present(ctx) ? 1 : 0;
    int mounted = jw__mlp1_mount_is_active(ctx) ? 1 : 0;

    if (data->pending_storage_event &&
        jw__monotonic_ms() >= data->debounce_until_ms) {
        data->pending_storage_event = false;
        if (present) {
            if (jw__mlp1_mount_secondary_if_needed(ctx) == 0) {
                jw_log_info("storage hotplug: secondary SD mounted/remounted");
            } else {
                jw_log_warn("storage hotplug: secondary SD mount/remount failed");
            }
        }
        present = jw__mlp1_block_present(ctx) ? 1 : 0;
        mounted = jw__mlp1_mount_is_active(ctx) ? 1 : 0;
        changed = true;
    }

    if (present != data->last_present || mounted != data->last_mounted) {
        changed = true;
    }

    if (changed) {
        jw_log_info("storage hotplug: secondary SD present=%d mounted=%d",
                    present, mounted);
        data->last_present = present;
        data->last_mounted = mounted;
    }
    return changed;
}

/* The hotplug netlink socket, the headphone jack, the BlueALSA monitor and
   the playback-node watch. Everything else the audio and storage ticks watch is either a timer
   (next_deadline_ms) or a periodic check that runs on whatever pass comes
   next. */
static int jw__mlp1_poll_fds(jw_platform_context *ctx, int *fds, int max) {
    jw_mlp1_platform_data *data = ctx ? (jw_mlp1_platform_data *)ctx->backend_data : NULL;
    int count = 0;
    if (data && data->uevent_fd >= 0 && count < max) {
        fds[count++] = data->uevent_fd;
    }
    int jack = jw__mlp1_jack_fd();
    if (jack >= 0 && count < max) {
        fds[count++] = jack;
    }
    if (s_mlp1_bt_monitor_fd >= 0 && count < max) {
        fds[count++] = s_mlp1_bt_monitor_fd;
    }
    if (s_mlp1_pcm_watch_fd >= 0 && count < max) {
        fds[count++] = s_mlp1_pcm_watch_fd;
    }
    return count;
}

static void jw__mlp1_deadline_min(long long *best, long long at) {
    if (*best < 0 || at < *best) {
        *best = at;
    }
}

/* Timed work in the audio and storage ticks. Left out on purpose, because a
   second of lateness is harmless and the daemon wakes at least once a second:
   the 400 ms charger poll (charger uevents never arrive on this device, see
   the storage tick), the 1 s USB-audio and 1.5 s Bluetooth-audio probes, and
   the speaker-gate fallback poll used only while the playback node cannot be
   watched. */
static long long jw__mlp1_next_deadline_ms(jw_platform_context *ctx, long long now_ms) {
    jw_mlp1_platform_data *data = ctx ? (jw_mlp1_platform_data *)ctx->backend_data : NULL;
    long long best = -1;
    if (data && data->resume_mount_repair_pending) {
        jw__mlp1_deadline_min(&best, data->resume_mount_repair_at_ms);
    }
    if (data && data->pending_storage_event) {
        jw__mlp1_deadline_min(&best, data->debounce_until_ms);
    }
    if (data && data->pending_charge_event) {
        jw__mlp1_deadline_min(&best, data->charge_settle_until_ms);
    }
    return best;
}

static void jw__mlp1_get_storage_status(jw_platform_context *ctx,
                                        const char *source_id,
                                        jw_platform_storage_status *out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    if (source_id && strcmp(source_id, JW_MLP1_LAUNCHER_SOURCE_ID) == 0) {
        snprintf(out->source_id, sizeof(out->source_id), "%s", source_id);
        snprintf(out->label, sizeof(out->label), "%s", JW_MLP1_LAUNCHER_LABEL);
        snprintf(out->mount_path, sizeof(out->mount_path), "%s",
                 ctx && ctx->sdcard_root[0] ? ctx->sdcard_root : "/mnt/sdcard");
        out->mounted = jw__mlp1_mount_lookup(out->mount_path, out->device_path,
                                             sizeof(out->device_path));
        out->present = out->mounted;
        out->busy = out->mounted;   /* Leaf runs from this card */
        out->can_unmount = false;
        snprintf(out->message, sizeof(out->message), "%s",
                 out->mounted ? "Mounted" : "Not mounted");
        return;
    }
    snprintf(out->source_id, sizeof(out->source_id), "%s",
             source_id && source_id[0] ? source_id : JW_MLP1_SECONDARY_SOURCE_ID);
    snprintf(out->label, sizeof(out->label), "%s", JW_MLP1_SECONDARY_LABEL);
    jw__mlp1_secondary_mount(ctx, out->mount_path, sizeof(out->mount_path));
    (void)jw__mlp1_secondary_device(ctx, out->device_path,
                                    sizeof(out->device_path));

    if (!jw__mlp1_source_is_secondary(source_id)) {
        snprintf(out->message, sizeof(out->message), "%s", "storage source unavailable");
        return;
    }

    out->present = jw__mlp1_block_present(ctx);
    out->mounted = jw__mlp1_mount_is_active(ctx);
    out->busy = out->mounted ? jw__mlp1_storage_busy(ctx) : false;
    out->can_unmount = out->mounted && !out->busy;
    snprintf(out->message, sizeof(out->message), "%s",
             out->busy ? "Busy" : (out->mounted ? "Mounted" : "Not mounted"));
}

static int jw__mlp1_storage_roots(jw_platform_context *ctx,
                                  jw_platform_storage_root *out, int max) {
    if (!out || max <= 0) {
        return 0;
    }
    memset(out, 0, sizeof(*out) * (size_t)max);
    snprintf(out[0].source_id, sizeof(out[0].source_id), "%s", JW_MLP1_LAUNCHER_SOURCE_ID);
    snprintf(out[0].label, sizeof(out[0].label), "%s", JW_MLP1_LAUNCHER_LABEL);
    snprintf(out[0].root, sizeof(out[0].root), "%s",
             ctx && ctx->sdcard_root[0] ? ctx->sdcard_root : "/mnt/sdcard");
    if (max < 2) {
        return 1;
    }
    snprintf(out[1].source_id, sizeof(out[1].source_id), "%s", JW_MLP1_SECONDARY_SOURCE_ID);
    snprintf(out[1].label, sizeof(out[1].label), "%s", JW_MLP1_SECONDARY_LABEL);
    jw__mlp1_secondary_mount(ctx, out[1].root, sizeof(out[1].root));
    return 2;
}

/* Reboot repair uses the stock rootfs fsck.fat through the switcher's runner.
   exFAT stays unavailable until its own fixture acceptance passes. */
static void jw__mlp1_storage_repair_capability(jw_platform_context *ctx,
                                               const char *fs_type,
                                               const char *uuid,
                                               bool block_write_protected,
                                               jw_platform_storage_repair_capability *out) {
    (void)ctx;
    const char *reason = NULL;
    if (!fs_type || !fs_type[0]) {
        reason = "not-mounted";
    } else if (strcmp(fs_type, "vfat") != 0 && strcmp(fs_type, "msdos") != 0) {
        reason = strcmp(fs_type, "exfat") == 0 ? "exfat-not-validated"
                                               : "unsupported-filesystem";
    } else if (!jw_storage_uuid_valid(uuid)) {
        reason = "unknown-identity";
    } else if (block_write_protected) {
        reason = "write-protected";
    } else if (access(JW_MLP1_FSCK_FAT, X_OK) != 0) {
        reason = "tool-missing";
    } else if (!jw_power_request_available()) {
        reason = "power-handoff-missing";
    } else if (access(JW_STORAGE_MLP1_REPAIR_TOOL, X_OK) != 0) {
        reason = "repair-runner-missing";
    }
    out->supported = reason == NULL;
    snprintf(out->unavailable_reason, sizeof(out->unavailable_reason), "%s",
             reason ? reason : "");
    snprintf(out->mode, sizeof(out->mode), "%s", reason ? "" : "reboot");
}

static void jw__mlp1_safe_unmount_storage(jw_platform_context *ctx,
                                          const char *source_id,
                                          jw_platform_result *out) {
    jw_mlp1_platform_data *data = ctx ? (jw_mlp1_platform_data *)ctx->backend_data : NULL;
    if (!jw__mlp1_source_is_secondary(source_id)) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID,
                               "only secondary SD can be unmounted");
        return;
    }

    char mount[PATH_MAX];
    char command[PATH_MAX + 64];
    jw__mlp1_secondary_mount(ctx, mount, sizeof(mount));

    if (!jw__mlp1_mount_is_active(ctx)) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "Secondary SD is not mounted");
        return;
    }

    if (jw__mlp1_storage_busy(ctx)) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_UNAVAILABLE,
                               "Secondary SD is busy");
        return;
    }

    sync();
    snprintf(command, sizeof(command), "umount %s >/dev/null 2>&1", mount);
    if (jw__exec_shell(command) != 0) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED,
                               "Secondary SD unmount failed");
        return;
    }

    if (data) {
        data->last_present = jw__mlp1_block_present(ctx) ? 1 : 0;
        data->last_mounted = 0;
    }
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "Secondary SD unmounted");
}

/* Send a signal to every process whose /proc/<pid>/comm matches name.
   Avoids a pgrep dependency. Returns the number of processes signalled. */
static int jw__mlp1_signal_by_name(const char *name, int sig) {
    DIR *proc = opendir("/proc");
    if (!proc) return 0;
    struct dirent *entry;
    char path[300];
    char comm[128];
    int signalled = 0;
    while ((entry = readdir(proc)) != NULL) {
        if (entry->d_name[0] < '0' || entry->d_name[0] > '9') continue;
        snprintf(path, sizeof(path), "/proc/%s/comm", entry->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        if (fgets(comm, sizeof(comm), fp)) {
            comm[strcspn(comm, "\n")] = '\0';
            if (strcmp(comm, name) == 0) {
                if (kill((pid_t)atoi(entry->d_name), sig) == 0) signalled++;
            }
        }
        fclose(fp);
    }
    closedir(proc);
    return signalled;
}

static int jw__mlp1_spawn_loong_power(void) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Double-fork: the grandchild reparents to init and is auto-reaped, so
           we leave no zombie. The intermediate exits immediately; the original
           parent reaps only it (below). */
        pid_t grandchild = fork();
        if (grandchild == 0) {
            chdir("/loong");
            const char *old_ld = getenv("LD_LIBRARY_PATH");
            char ld[512];
            if (old_ld && old_ld[0]) {
                snprintf(ld, sizeof(ld), "./:%s", old_ld);
            } else {
                snprintf(ld, sizeof(ld), "%s", "./");
            }
            setenv("LD_LIBRARY_PATH", ld, 1);
            /* The supervisor resolves loong_power's low-battery "poweroff" to our
               power handoff; a restarted daemon must keep that PATH entry. */
            const char *handoff = getenv("UMRK_LOONG_POWER_HANDOFF_DIR");
            if (handoff && handoff[0] == '/') {
                const char *old_path = getenv("PATH");
                char path[768];
                snprintf(path, sizeof(path), "%s:%s", handoff,
                         old_path && old_path[0] ? old_path : "/usr/bin:/usr/sbin");
                setenv("PATH", path, 1);
            }
            execl("/loong/loong_power", "loong_power", JW_MLP1_POWER_CFG, (char *)NULL);
            _exit(127);
        }
        _exit(0);   /* intermediate child exits immediately; grandchild reparents to init */
    }
    int status = 0;
    waitpid(pid, &status, 0);   /* reap the intermediate child only */
    return 0;
}

static int jw__mlp1_restart_leaf_loong_power(void) {
    int signalled = jw__mlp1_signal_by_name(JW_MLP1_POWER_DAEMON, SIGTERM);
    if (signalled > 0) {
        usleep(300000);
        jw__mlp1_signal_by_name(JW_MLP1_POWER_DAEMON, SIGKILL);
    }
    return jw__mlp1_spawn_loong_power();
}

static int jw__mlp1_apply_power_cfg_live(void) {
    if (!jw__env_truthy("UMRK_LEAF_MODE")) {
        return 0;
    }
    if (jw__mlp1_restart_leaf_loong_power() != 0) {
        jw_log_warn("loong_power: restart failed after power cfg update");
        return -1;
    }
    jw_log_info("loong_power: restarted after power cfg update");
    return 0;
}

/* ── Force-off hold (RK817 PWRON_KEY, register 0xf7) ───────────────────────
   Bits 5:4 pick how long the power button must stay down before the PMIC cuts
   power on its own: 00 = 6 s, 01 = 8 s, 10 = 10 s, 11 = 12 s. Bit 6 is the
   action (0 off, 1 restart) and bits 3:0 are unrelated; both are kept as read.

   U-Boot programs this register from the device tree on EVERY boot (measured
   2026-10-03: 0x26 written, reboot, 0x06 again), and the kernel rk808 driver
   never touches it, so the daemon re-applies the setting after each start.
   Writing it from userspace leaves the kernel's regmap cache for 0xf7 stale.
   That is harmless: no driver reads the register, so nothing acts on the old
   value; `i2cget -f -y 0 0x20 0xf7` shows the real byte, the regmap debugfs
   dump the cached one. Stock firmware is unaffected: any boot where Leaf does
   not start keeps U-Boot's 6 s.

   Plain /dev/i2c-0 read/write with I2C_SLAVE_FORCE, no i2cset subprocess. The
   force is needed because the rk808 driver already claims address 0x20, which
   is also why the codec helpers above pass -f to i2cget/i2cset. */
#define JW_MLP1_PMIC_I2C_DEV  "/dev/i2c-0"
#define JW_MLP1_PMIC_I2C_ADDR 0x20

/* One PMIC register byte (0..255) or -1. The slave address is already set on fd. */
static int jw__mlp1_pmic_reg_read(int fd, int reg) {
    unsigned char r = (unsigned char)reg;
    unsigned char v = 0;
    if (write(fd, &r, 1) != 1) return -1;
    if (read(fd, &v, 1) != 1) return -1;
    return v;
}

static int jw__mlp1_pmic_reg_write(int fd, int reg, int value) {
    unsigned char buf[2] = { (unsigned char)reg, (unsigned char)value };
    return write(fd, buf, sizeof(buf)) == (ssize_t)sizeof(buf) ? 0 : -1;
}

static void jw__mlp1_set_power_hard_cut(jw_platform_context *ctx, int seconds,
                                        jw_platform_result *out) {
    (void)ctx;
    if (!jw_power_hard_cut_valid_s(seconds)) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID,
                               "force-off hold must be 6, 8, 10 or 12 s");
        return;
    }

    int fd = open(JW_MLP1_PMIC_I2C_DEV, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        jw_log_warn("power hard cut: open %s failed: %s; register left alone",
                    JW_MLP1_PMIC_I2C_DEV, strerror(errno));
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "pmic i2c bus unavailable");
        return;
    }
    if (ioctl(fd, I2C_SLAVE_FORCE, JW_MLP1_PMIC_I2C_ADDR) < 0) {
        jw_log_warn("power hard cut: I2C_SLAVE_FORCE 0x%02x failed: %s; register left alone",
                    JW_MLP1_PMIC_I2C_ADDR, strerror(errno));
        close(fd);
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "pmic i2c address unavailable");
        return;
    }

    int cur = jw__mlp1_pmic_reg_read(fd, JW_POWER_HARD_CUT_REG);
    if (cur < 0) {
        jw_log_warn("power hard cut: read 0x%02x failed: %s; register left alone",
                    JW_POWER_HARD_CUT_REG, strerror(errno));
        close(fd);
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "pmic register read failed");
        return;
    }

    int want = jw_power_hard_cut_apply_reg((unsigned char)cur, seconds);
    char message[JW_PLATFORM_MAX_MESSAGE];
    if (want == cur) {
        /* Already in force (a re-apply after wake, or the setting matches
           U-Boot's value): nothing to write and nothing worth a log line. */
        close(fd);
        snprintf(message, sizeof(message), "power hard cut already %d s", seconds);
        jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, message, seconds);
        return;
    }

    if (jw__mlp1_pmic_reg_write(fd, JW_POWER_HARD_CUT_REG, want) != 0) {
        jw_log_warn("power hard cut: write 0x%02x -> 0x%02x failed: %s",
                    cur, want, strerror(errno));
        close(fd);
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "pmic register write failed");
        return;
    }
    int back = jw__mlp1_pmic_reg_read(fd, JW_POWER_HARD_CUT_REG);
    close(fd);
    if (back != want) {
        jw_log_warn("power hard cut: wrote 0x%02x, register reads back 0x%02x (was 0x%02x)",
                    want, back, cur);
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "pmic register read-back mismatch");
        return;
    }

    jw_log_info("power hard cut: 0x%02x -> 0x%02x (%d s)", cur, want, seconds);
    snprintf(message, sizeof(message), "power hard cut set to %d s", seconds);
    jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, message, seconds);
}

static void jw__mlp1_set_led(jw_platform_context *ctx, const jw_led_config *cfg,
                             jw_platform_result *out) {
    (void)ctx;
    if (!cfg) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "no led config");
        return;
    }
    int brightness = cfg->brightness;
    if (brightness < 0) brightness = 0;
    if (brightness > JW_LED_BRIGHTNESS_MAX) brightness = JW_LED_BRIGHTNESS_MAX;
    int speed = cfg->speed;
    if (speed < 0) speed = 0;
    if (speed > JW_LED_SPEED_MAX) speed = JW_LED_SPEED_MAX;

    FILE *fp = fopen(JW_MLP1_LED_CFG, "w");
    if (!fp) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_FAILED, "led cfg write failed");
        return;
    }
    /* colors is a JSON STRING holding an array of {a,r,g,b} (loong's schema). */
    fprintf(fp,
            "{\"brightness\":\"%d\","
            "\"colors\":\"[{\\\"a\\\":0,\\\"b\\\":%u,\\\"g\\\":%u,\\\"r\\\":%u}]\","
            "\"enable\":\"%d\",\"mode\":\"%s\",\"speed\":\"%d\"}",
            brightness,
            (unsigned)cfg->b, (unsigned)cfg->g, (unsigned)cfg->r,
            cfg->enabled ? 1 : 0, jw_led_mode_name(cfg->mode), speed);
    fclose(fp);

    if (jw__env_truthy("UMRK_LEAF_MODE")) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK,
                               "led cfg written (leaf driver owns live state)");
        return;
    }

    if (jw__mlp1_signal_by_name(JW_MLP1_LED_DAEMON, SIGUSR1) <= 0) {
        /* cfg is written; daemon will pick it up on its next reload anyway. */
        jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "led cfg written (daemon not signalled)");
        return;
    }
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "led applied");
}

const jw_platform_backend *jw_platform_get_backend(void) {
    static const jw_platform_backend backend = {
        .platform_id = "mlp1",
        .platform_name = "Miniloong Pocket 1",
        .capabilities = {
            .battery = true,
            .charging = true,
            .sleep = true,
            .poweroff = true,
            .reboot = true,
            .brightness = true,
            .volume = true,
            .wifi = true,
            .bluetooth = true,
            .adb = true,
            .boot_splash = true,
            .refresh_rate = true,
            .color_temperature = true,
            .hdmi_output = true,
            .led = true,
            .performance = true,
        },
        .init = jw__mlp1_init,
        .shutdown = jw__mlp1_shutdown,
        .get_status = jw__mlp1_get_status,
        .get_audio_status = jw__mlp1_get_audio_status,
        .audio_tick = jw__mlp1_audio_tick,
        .poll_fds = jw__mlp1_poll_fds,
        .next_deadline_ms = jw__mlp1_next_deadline_ms,
        .sleep_audio = jw__mlp1_sleep_audio,
        .wake_audio = jw__mlp1_wake_audio,
        .audio_reconcile = jw__mlp1_audio_reconcile,
        .frontend_ready = jw__mlp1_frontend_ready,
        .perform_action = jw__mlp1_perform_action,
        .get_performance_status = jw__mlp1_get_performance_status,
        .apply_performance = jw__mlp1_apply_performance,
        .storage_tick = jw__mlp1_storage_tick,
        .get_storage_status = jw__mlp1_get_storage_status,
        .storage_roots = jw__mlp1_storage_roots,
        .storage_repair_capability = jw__mlp1_storage_repair_capability,
        .safe_unmount_storage = jw__mlp1_safe_unmount_storage,
        .set_led = jw__mlp1_set_led,
        .get_display_mode = jw__mlp1_get_display_mode,
        .set_power_hard_cut = jw__mlp1_set_power_hard_cut,
    };
    return &backend;
}
