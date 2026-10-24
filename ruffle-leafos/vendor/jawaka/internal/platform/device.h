#ifndef JW_PLATFORM_DEVICE_H
#define JW_PLATFORM_DEVICE_H

#include <stdbool.h>
#include <stddef.h>

#include "internal/platform/platform_id.h"

#define JW_PLATFORM_MAX_PATH    4096
#define JW_PLATFORM_MAX_MESSAGE 256
#define JW_PLATFORM_BRIGHTNESS_MIN_PERCENT 5
#define JW_PLATFORM_BRIGHTNESS_MAX_PERCENT 100
#define JW_PLATFORM_BRIGHTNESS_STEP_PERCENT 5
#define JW_PLATFORM_VOLUME_STEP_PERCENT 5

/* Display color-temperature correction, applied through the DRM CRTC gamma LUT.
   The value is the target fed to the black-body curve, not a measurement of the
   resulting white point: on a panel that is already warm, a target ABOVE
   NEUTRAL_K cools the image back toward neutral. NEUTRAL_K is the identity
   detent (an identity ramp, no correction); below it warms, above it cools. */
#define JW_PLATFORM_COLOR_TEMP_MIN_K     4000
#define JW_PLATFORM_COLOR_TEMP_MAX_K     10000
#define JW_PLATFORM_COLOR_TEMP_STEP_K    100
#define JW_PLATFORM_COLOR_TEMP_NEUTRAL_K 6500

#define JW_LED_BRIGHTNESS_MAX 10
#define JW_LED_SPEED_MAX 10

#define JW_PLATFORM_AUDIO_OUTPUT_COUNT 5
#define JW_PLATFORM_PERF_DOMAIN_COUNT 3
#define JW_PLATFORM_PERF_VALUE_MAX 64
#define JW_PLATFORM_PERF_LIST_MAX 512

#define JW_PLATFORM_AUDIO_EVENT_BLUETOOTH_CONNECTED    (1u << 0)
#define JW_PLATFORM_AUDIO_EVENT_BLUETOOTH_DISCONNECTED (1u << 1)
/* The active output changed on its own (cable in/out, headset connect). Each
   output restores its own stored level, so any cached volume is now stale and a
   volume keypress computed from it would jump instead of stepping. */
#define JW_PLATFORM_AUDIO_EVENT_OUTPUT_CHANGED         (1u << 2)

typedef enum {
    /* Stock modes are driven by the active platform LED backend. */
    JW_LED_MODE_STATIC = 0,   /* solid color */
    JW_LED_MODE_BREATH,       /* pulse the color */
    JW_LED_MODE_RAINBOW,      /* cycle hues (color ignored) */
    /* Custom effects may be driven by a platform-specific helper. */
    JW_LED_MODE_COMET,
    JW_LED_MODE_SWEEP,
    JW_LED_MODE_FOUNTAIN,
    JW_LED_MODE_HICCUP,
    /* Appended, never inserted: the wire names are persisted, and this one
       must stay inside the effect range so jawakad hands it to the helper. */
    JW_LED_MODE_BATTERY,      /* color follows the charge (MLP1 helper only) */
    JW_LED_MODE_COUNT
} jw_led_mode;

/* True for custom effects rather than stock platform LED modes. */
#define jw_led_mode_is_effect(m) ((m) >= JW_LED_MODE_COMET && (m) < JW_LED_MODE_COUNT)

typedef enum {
    JW_PLATFORM_AUDIO_OUTPUT_UNKNOWN = -1,
    JW_PLATFORM_AUDIO_OUTPUT_SPEAKER = 0,
    JW_PLATFORM_AUDIO_OUTPUT_HEADSET,
    JW_PLATFORM_AUDIO_OUTPUT_HDMI,
    JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH,
    /* Appended, never inserted: per-output volumes are stored indexed by this
       enum, so renumbering an existing member silently remaps saved levels. */
    JW_PLATFORM_AUDIO_OUTPUT_USB
} jw_platform_audio_output;

#define JW_PLATFORM_AUDIO_OUTPUT_BIT(o) (1u << (unsigned)(o))

typedef struct {
    bool enabled;
    jw_led_mode mode;
    unsigned char r, g, b;    /* color for static/breath */
    int brightness;           /* 0..JW_LED_BRIGHTNESS_MAX */
    int speed;                /* 0..JW_LED_SPEED_MAX (breath/rainbow) */
} jw_led_config;

typedef struct {
    bool battery;
    bool charging;
    bool sleep;
    bool poweroff;
    bool reboot;
    bool brightness;
    bool volume;
    bool wifi;
    bool bluetooth;
    bool adb;
    bool boot_splash;
    bool refresh_rate;   /* runtime display refresh-rate switching (e.g. 60/100/120 Hz) */
    bool color_temperature; /* runtime display color-temperature correction (gamma LUT) */
    bool hdmi_output;    /* HDMI external output: 4:3 pillarbox / stretch / off */
    bool led;
    bool performance;
} jw_platform_capabilities;

typedef struct {
    char platform_id[32];
    char platform_name[64];
    char runtime_dir[JW_PLATFORM_MAX_PATH];
    char sdcard_root[JW_PLATFORM_MAX_PATH];
    char script_dir[JW_PLATFORM_MAX_PATH];
    jw_platform_capabilities capabilities;
    bool home_ready_sent;
    void *backend_data;
} jw_platform_context;

typedef struct {
    int battery_percent;       /* -1 when unknown */
    int charging;              /* -1 unknown, 0 no, 1 yes */
    int brightness_percent;    /* -1 when unknown */
    int volume_percent;        /* -1 when unknown */
    jw_platform_audio_output audio_output;
    unsigned audio_available_outputs;  /* bitmask of JW_PLATFORM_AUDIO_OUTPUT_BIT(output) */
    int audio_volume_percent[JW_PLATFORM_AUDIO_OUTPUT_COUNT]; /* -1 when unknown */
    int audio_test_playing;    /* 1 while the Test Sound clip is playing, else 0 */
    int wifi_connected;        /* -1 unknown, 0 no, 1 yes */
    int wifi_strength;         /* -1 unknown, 0 off/disconnected, 1..3 strength */
    int bluetooth_connected;   /* -1 unknown, 0 no, 1 yes */
    int adb_enabled;           /* -1 unknown/unavailable, 0 no, 1 yes */
    int adb_intent_enabled;    /* -1 unknown/unavailable, 0 no, 1 yes */
    int boot_splash_enabled;   /* -1 unknown/unavailable, 0 no, 1 yes */
    int refresh_rate_hz;       /* current panel refresh in Hz; -1 unknown/unsupported */
    int color_temp_kelvin;     /* last panel color-temperature target applied, in K; -1 not applied yet/unsupported.
                                  Kept while HDMI is active, though the LUT is identity then. */
    int hdmi_connected;        /* HDMI cable: -1 unknown, 0 disconnected, 1 connected */
    int hdmi_output_mode;      /* applied output: -1 unknown, 0 off, 1 4:3, 2 stretch */
} jw_platform_status;

/* Black Frame Insertion needs a refresh that is exactly twice a content rate, so
   every emulated frame gets one lit refresh and one black one. Returns that
   content rate in fps, or 0 at rates where BFI cannot work.

       100 Hz -> 50 fps (PAL)      120 Hz -> 60 fps (NTSC)

   60 Hz has no spare refresh to insert into. Defined once, here, because the
   daemon publishes JAWAKA_BFI to RetroArch while the settings UI offers the
   toggle: if the two disagree the UI presents a switch the daemon ignores, which
   looks exactly like a broken setting. */
static inline int jw_bfi_content_fps(int refresh_hz) {
    switch (refresh_hz) {
    case 100: return 50;
    case 120: return 60;
    default:  return 0;
    }
}

typedef struct {
    char source_id[32];
    char label[64];
    char mount_path[JW_PLATFORM_MAX_PATH];
    char device_path[JW_PLATFORM_MAX_PATH];
    char message[JW_PLATFORM_MAX_MESSAGE];
    bool present;
    bool mounted;
    bool busy;
    bool can_unmount;
} jw_platform_storage_status;

#define JW_PLATFORM_STORAGE_LAUNCHER_ID "launcher_sd"
#define JW_PLATFORM_STORAGE_SECONDARY_ID "secondary_sd"

/* A card root the platform can report health for. */
typedef struct {
    char source_id[32];
    char label[64];
    char root[JW_PLATFORM_MAX_PATH];
} jw_platform_storage_root;

/* Repair capability for one observed card. Unsupported platforms, filesystems
   and missing tools report false with a stable reason key. */
typedef struct {
    bool supported;
    char unavailable_reason[64];
    char mode[16];   /* "reboot" when supported */
} jw_platform_storage_repair_capability;

typedef enum {
    JW_PLATFORM_PERF_DOMAIN_CPU = 0,
    JW_PLATFORM_PERF_DOMAIN_GPU,
    JW_PLATFORM_PERF_DOMAIN_DMC
} jw_platform_perf_domain;

typedef enum {
    JW_PLATFORM_PERF_PROFILE_AUTO = 0,
    JW_PLATFORM_PERF_PROFILE_FRONTEND,
    JW_PLATFORM_PERF_PROFILE_BALANCED,
    JW_PLATFORM_PERF_PROFILE_PERFORMANCE,
    JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER,
    JW_PLATFORM_PERF_PROFILE_SLEEP,
    JW_PLATFORM_PERF_PROFILE_CUSTOM,
    JW_PLATFORM_PERF_PROFILE_COUNT
} jw_platform_perf_profile;

typedef struct {
    bool supported;
    char name[JW_PLATFORM_PERF_VALUE_MAX];
    char governor[JW_PLATFORM_PERF_VALUE_MAX];
    int current_freq;
    int set_freq;
    char available_governors[JW_PLATFORM_PERF_LIST_MAX];
    char available_frequencies[JW_PLATFORM_PERF_LIST_MAX];
} jw_platform_perf_domain_status;

typedef struct {
    bool supported;
    int soc_temp_c;
    char message[JW_PLATFORM_MAX_MESSAGE];
    jw_platform_perf_domain_status domains[JW_PLATFORM_PERF_DOMAIN_COUNT];
} jw_platform_perf_status;

typedef struct {
    char governor[JW_PLATFORM_PERF_VALUE_MAX];
    int frequency; /* -1 = no explicit userspace frequency */
} jw_platform_perf_domain_request;

typedef struct {
    jw_platform_perf_domain_request domains[JW_PLATFORM_PERF_DOMAIN_COUNT];
} jw_platform_perf_request;

typedef enum {
    JW_PLATFORM_ACTION_SLEEP = 0,
    JW_PLATFORM_ACTION_POWEROFF,
    JW_PLATFORM_ACTION_REBOOT,
    JW_PLATFORM_ACTION_SET_BRIGHTNESS,
    JW_PLATFORM_ACTION_SET_VOLUME,
    JW_PLATFORM_ACTION_WIFI_ON,
    JW_PLATFORM_ACTION_WIFI_OFF,
    JW_PLATFORM_ACTION_BLUETOOTH_ON,
    JW_PLATFORM_ACTION_BLUETOOTH_OFF,
    JW_PLATFORM_ACTION_SET_AUDIO_OUTPUT,
    JW_PLATFORM_ACTION_SET_AUTO_SLEEP,
    JW_PLATFORM_ACTION_SCREEN_OFF,   /* blank the backlight (display stays composed) */
    JW_PLATFORM_ACTION_SCREEN_ON,    /* unblank the backlight */
    JW_PLATFORM_ACTION_ENABLE_ADB,
    JW_PLATFORM_ACTION_DISABLE_ADB,
    JW_PLATFORM_ACTION_SET_BOOT_SPLASH,
    JW_PLATFORM_ACTION_PLAY_TEST_SOUND,  /* play a short clip on the current audio output */
    JW_PLATFORM_ACTION_SET_REFRESH_RATE, /* value = target panel refresh in Hz (60/90) */
    JW_PLATFORM_ACTION_SET_HDMI_OUTPUT,  /* value = 0 off / 1 4:3 pillarbox / 2 stretch */
    JW_PLATFORM_ACTION_SET_COLOR_TEMP    /* value = color-temperature target in K (see JW_PLATFORM_COLOR_TEMP_*) */
} jw_platform_action;

typedef enum {
    JW_PLATFORM_RESULT_OK = 0,
    JW_PLATFORM_RESULT_UNSUPPORTED,
    JW_PLATFORM_RESULT_UNAVAILABLE,
    JW_PLATFORM_RESULT_FAILED,
    JW_PLATFORM_RESULT_INVALID
} jw_platform_result_code;

typedef struct {
    jw_platform_result_code code;
    char message[JW_PLATFORM_MAX_MESSAGE];
    bool has_value;
    int value;
} jw_platform_result;

int  jw_platform_init(jw_platform_context *ctx, const char *runtime_dir, const char *sdcard_root);
void jw_platform_shutdown(jw_platform_context *ctx);
void jw_platform_get_status(jw_platform_context *ctx, jw_platform_status *out);
void jw_platform_get_audio_status(jw_platform_context *ctx, jw_platform_status *out);
/* Poll for audio edge events and re-route audio. Call periodically. */
unsigned jw_platform_audio_tick(jw_platform_context *ctx);
/* Descriptors the daemon should add to its poll set (POLLIN) so the audio and
   storage ticks run when they have something to read. Returns the count
   written, at most max; 0 when the backend has none. */
int  jw_platform_poll_fds(jw_platform_context *ctx, int *fds, int max);
/* The next monotonic ms at which the audio or storage tick has timed work, or
   -1 when it only waits for its poll fds. Periodic checks that tolerate a
   second of delay are left out; the daemon's own heartbeat covers them. */
long long jw_platform_next_deadline_ms(jw_platform_context *ctx, long long now_ms);
/* Quiesce audio for JW_PLATFORM_ACTION_SLEEP before the caller drops to its
   sleep performance profile: a playing stream is suspended while the clocks
   are still up, since a buffer refill caught by the drop runs long enough to
   trip PulseAudio's realtime limit. The SLEEP action does it itself when this
   was not called. */
void jw_platform_sleep_audio(jw_platform_context *ctx);
/* Finish waking audio after JW_PLATFORM_ACTION_SLEEP returns, once the
   caller has restored its wake performance profile: a stream suspended for
   the sleep refills its buffer here, and at sleep clocks that refill takes
   long enough to trip PulseAudio's realtime limit. The backend also does it
   on its next audio tick if nobody calls this. */
void jw_platform_wake_audio(jw_platform_context *ctx);
/* Best-effort repair of live audio route/volume after wake or before launch. */
void jw_platform_audio_reconcile(jw_platform_context *ctx, const char *reason);
void jw_platform_frontend_ready(jw_platform_context *ctx, const char *role, jw_platform_result *out);

bool jw_platform_parse_action(const char *name, jw_platform_action *out);
const char *jw_platform_action_name(jw_platform_action action);
bool jw_platform_parse_audio_output(const char *name, jw_platform_audio_output *out);
const char *jw_platform_audio_output_name(jw_platform_audio_output output);
const char *jw_platform_audio_output_label(jw_platform_audio_output output);
bool jw_platform_parse_perf_profile(const char *name, jw_platform_perf_profile *out);
const char *jw_platform_perf_profile_name(jw_platform_perf_profile profile);
const char *jw_platform_perf_profile_label(jw_platform_perf_profile profile);
const char *jw_platform_result_code_name(jw_platform_result_code code);
int  jw_platform_clamp_brightness_percent(int percent);
/* Clamp to [MIN_K, MAX_K] and snap to the nearest STEP_K. */
int  jw_platform_clamp_color_temp_k(int kelvin);
void jw_platform_perform_action(jw_platform_context *ctx, jw_platform_action action,
                                int value, jw_platform_result *out);
void jw_platform_get_performance_status(jw_platform_context *ctx,
                                        jw_platform_perf_status *out);
void jw_platform_apply_performance(jw_platform_context *ctx,
                                   const jw_platform_perf_request *request,
                                   jw_platform_result *out);
bool jw_platform_storage_tick(jw_platform_context *ctx);
void jw_platform_get_storage_status(jw_platform_context *ctx, const char *source_id,
                                    jw_platform_storage_status *out);
void jw_platform_safe_unmount_storage(jw_platform_context *ctx, const char *source_id,
                                      jw_platform_result *out);
/* launcher_sd first, then secondary_sd where the platform has one. */
int  jw_platform_storage_roots(jw_platform_context *ctx, jw_platform_storage_root *out,
                               int max);
void jw_platform_get_storage_repair_capability(jw_platform_context *ctx,
                                           const char *fs_type,
                                           const char *uuid,
                                           bool block_write_protected,
                                           jw_platform_storage_repair_capability *out);

const char *jw_led_mode_name(jw_led_mode mode);     /* "FOREVER"/"BREATH"/"RAINBOW" */
bool        jw_led_mode_parse(const char *name, jw_led_mode *out);
/* Current mode of the active display output (width, height, refresh in Hz), read
   from the DRM core. Returns 0 and fills the outputs, or -1 when it is unknown or
   the platform cannot report it. */
int  jw_platform_get_display_mode(jw_platform_context *ctx, int *width, int *height,
                                  int *hz);
void jw_platform_set_led(jw_platform_context *ctx, const jw_led_config *cfg,
                         jw_platform_result *out);
/* Force-off hold: how long the power button must stay down before the PMIC
   cuts power on its own (Settings > System > Force Off Hold). `seconds` is one
   of jw_power_hard_cut_options_s (6/8/10/12). OK carries the hold now in
   force in `value`; UNSUPPORTED where the platform has no such register
   (everything but the MLP1); FAILED when the register could not be read, or
   did not read back as written, in which case it was left alone. */
void jw_platform_set_power_hard_cut_s(jw_platform_context *ctx, int seconds,
                                      jw_platform_result *out);

#endif /* JW_PLATFORM_DEVICE_H */
