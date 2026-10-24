#include "internal/platform/device.h"
#include "internal/i18n/i18n.h"

#include <string.h>

/* Stock modes map to platform cfg names; effects map to helper command names.
   Both travel over the same IPC "mode" string. */
const char *jw_led_mode_name(jw_led_mode mode) {
    switch (mode) {
        case JW_LED_MODE_BREATH:   return "BREATH";
        case JW_LED_MODE_RAINBOW:  return "RAINBOW";
        case JW_LED_MODE_COMET:    return "comet";
        case JW_LED_MODE_SWEEP:    return "sweep";
        case JW_LED_MODE_FOUNTAIN: return "fountain";
        case JW_LED_MODE_HICCUP:   return "hiccup";
        case JW_LED_MODE_BATTERY:  return "battery";
        case JW_LED_MODE_STATIC:
        default:                   return "FOREVER";
    }
}

bool jw_led_mode_parse(const char *name, jw_led_mode *out) {
    if (!name || !out) return false;
    if (strcmp(name, "FOREVER") == 0)  { *out = JW_LED_MODE_STATIC;   return true; }
    if (strcmp(name, "BREATH") == 0)   { *out = JW_LED_MODE_BREATH;   return true; }
    if (strcmp(name, "RAINBOW") == 0)  { *out = JW_LED_MODE_RAINBOW;  return true; }
    if (strcmp(name, "comet") == 0)    { *out = JW_LED_MODE_COMET;    return true; }
    if (strcmp(name, "sweep") == 0)    { *out = JW_LED_MODE_SWEEP;    return true; }
    if (strcmp(name, "fountain") == 0) { *out = JW_LED_MODE_FOUNTAIN; return true; }
    if (strcmp(name, "hiccup") == 0)   { *out = JW_LED_MODE_HICCUP;   return true; }
    if (strcmp(name, "battery") == 0)  { *out = JW_LED_MODE_BATTERY;  return true; }
    return false;
}

bool jw_platform_parse_audio_output(const char *name, jw_platform_audio_output *out) {
    if (!name || !out) return false;
    if (strcmp(name, "SPEAKER") == 0 || strcmp(name, "speaker") == 0) {
        *out = JW_PLATFORM_AUDIO_OUTPUT_SPEAKER;
    } else if (strcmp(name, "HEADSET") == 0 || strcmp(name, "headset") == 0 ||
               strcmp(name, "HEADPHONE") == 0 || strcmp(name, "headphone") == 0) {
        *out = JW_PLATFORM_AUDIO_OUTPUT_HEADSET;
    } else if (strcmp(name, "HDMI") == 0 || strcmp(name, "hdmi") == 0) {
        *out = JW_PLATFORM_AUDIO_OUTPUT_HDMI;
    } else if (strcmp(name, "BLUETOOTH") == 0 || strcmp(name, "bluetooth") == 0 ||
               strcmp(name, "BT") == 0 || strcmp(name, "bt") == 0) {
        *out = JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH;
    } else if (strcmp(name, "USB") == 0 || strcmp(name, "usb") == 0 ||
               strcmp(name, "USB-C") == 0 || strcmp(name, "usb-c") == 0) {
        *out = JW_PLATFORM_AUDIO_OUTPUT_USB;
    } else {
        return false;
    }
    return true;
}

bool jw_platform_parse_perf_profile(const char *name, jw_platform_perf_profile *out) {
    if (!name || !out) return false;
    if (strcmp(name, "auto") == 0 || strcmp(name, "AUTO") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_AUTO;
    } else if (strcmp(name, "frontend") == 0 || strcmp(name, "FRONTEND") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_FRONTEND;
    } else if (strcmp(name, "balanced") == 0 || strcmp(name, "BALANCED") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_BALANCED;
    } else if (strcmp(name, "performance") == 0 || strcmp(name, "PERFORMANCE") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_PERFORMANCE;
    } else if (strcmp(name, "battery-saver") == 0 ||
               strcmp(name, "battery_saver") == 0 ||
               strcmp(name, "BATTERY-SAVER") == 0 ||
               strcmp(name, "BATTERY_SAVER") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER;
    } else if (strcmp(name, "sleep") == 0 || strcmp(name, "SLEEP") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_SLEEP;
    } else if (strcmp(name, "custom") == 0 || strcmp(name, "CUSTOM") == 0) {
        *out = JW_PLATFORM_PERF_PROFILE_CUSTOM;
    } else {
        return false;
    }
    return true;
}

const char *jw_platform_perf_profile_name(jw_platform_perf_profile profile) {
    switch (profile) {
        case JW_PLATFORM_PERF_PROFILE_AUTO: return "auto";
        case JW_PLATFORM_PERF_PROFILE_FRONTEND: return "frontend";
        case JW_PLATFORM_PERF_PROFILE_BALANCED: return "balanced";
        case JW_PLATFORM_PERF_PROFILE_PERFORMANCE: return "performance";
        case JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER: return "battery-saver";
        case JW_PLATFORM_PERF_PROFILE_SLEEP: return "sleep";
        case JW_PLATFORM_PERF_PROFILE_CUSTOM: return "custom";
        default: return "unknown";
    }
}

/* Display labels go through T(); a process with no table loaded (the daemon)
   gets the English literal back, which is also what the logs should carry.
   jw_platform_perf_profile_name() is the wire/config identity and must never
   be translated. */
const char *jw_platform_perf_profile_label(jw_platform_perf_profile profile) {
    switch (profile) {
        case JW_PLATFORM_PERF_PROFILE_AUTO: return T("Auto");
        case JW_PLATFORM_PERF_PROFILE_FRONTEND: return T("Frontend");
        case JW_PLATFORM_PERF_PROFILE_BALANCED: return T("Balanced");
        case JW_PLATFORM_PERF_PROFILE_PERFORMANCE: return T("Performance");
        case JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER: return T("Battery Saver");
        case JW_PLATFORM_PERF_PROFILE_SLEEP: return T("Sleep");
        case JW_PLATFORM_PERF_PROFILE_CUSTOM: return T("Custom");
        default: return "Unknown";
    }
}

const char *jw_platform_audio_output_name(jw_platform_audio_output output) {
    switch (output) {
        case JW_PLATFORM_AUDIO_OUTPUT_SPEAKER: return "SPEAKER";
        case JW_PLATFORM_AUDIO_OUTPUT_HEADSET: return "HEADSET";
        case JW_PLATFORM_AUDIO_OUTPUT_HDMI: return "HDMI";
        case JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH: return "BLUETOOTH";
        case JW_PLATFORM_AUDIO_OUTPUT_USB: return "USB";
        default: return "UNKNOWN";
    }
}

/* JW_UI rather than T(): these strings have two kinds of caller. The Settings
   row draws them, and the row renderer translates whatever it is handed; the
   device layer also builds diagnostic result messages from them, which must
   stay English. Marking gets them extracted without translating at the source,
   so each caller gets what it needs. */
const char *jw_platform_audio_output_label(jw_platform_audio_output output) {
    switch (output) {
        case JW_PLATFORM_AUDIO_OUTPUT_SPEAKER: return JW_UI("Speaker");
        case JW_PLATFORM_AUDIO_OUTPUT_HEADSET: return JW_UI("Headset");
        case JW_PLATFORM_AUDIO_OUTPUT_HDMI: return "HDMI";
        case JW_PLATFORM_AUDIO_OUTPUT_BLUETOOTH: return "Bluetooth";
        case JW_PLATFORM_AUDIO_OUTPUT_USB: return "USB-C";
        default: return JW_UI("Unknown");
    }
}

int jw_platform_clamp_brightness_percent(int percent) {
    if (percent < JW_PLATFORM_BRIGHTNESS_MIN_PERCENT) {
        return JW_PLATFORM_BRIGHTNESS_MIN_PERCENT;
    }
    if (percent > JW_PLATFORM_BRIGHTNESS_MAX_PERCENT) {
        return JW_PLATFORM_BRIGHTNESS_MAX_PERCENT;
    }
    return percent;
}

int jw_platform_clamp_color_temp_k(int kelvin) {
    if (kelvin < JW_PLATFORM_COLOR_TEMP_MIN_K) {
        kelvin = JW_PLATFORM_COLOR_TEMP_MIN_K;
    }
    if (kelvin > JW_PLATFORM_COLOR_TEMP_MAX_K) {
        kelvin = JW_PLATFORM_COLOR_TEMP_MAX_K;
    }
    /* Snap to the step grid so the slider lands on round values regardless of
       where a stored / IPC value came from. */
    int step = JW_PLATFORM_COLOR_TEMP_STEP_K;
    return ((kelvin + step / 2) / step) * step;
}
