#include "internal/platform/device.h"
#include "internal/platform/device_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int jw__platform_uses_dot_system(const char *platform_id) {
    return platform_id &&
           (strcmp(platform_id, "tg5040") == 0 ||
            strcmp(platform_id, "tg5050") == 0 ||
            strcmp(platform_id, "my355") == 0);
}

void jw_platform_result_set(jw_platform_result *out,
                            jw_platform_result_code code,
                            const char *message) {
    if (!out) {
        return;
    }
    out->code = code;
    snprintf(out->message, sizeof(out->message), "%s", message ? message : "");
    out->has_value = false;
    out->value = 0;
}

void jw_platform_result_set_value(jw_platform_result *out,
                                  jw_platform_result_code code,
                                  const char *message,
                                  int value) {
    jw_platform_result_set(out, code, message);
    if (out) {
        out->has_value = true;
        out->value = value;
    }
}

void jw_platform_result_unsupported(jw_platform_action action,
                                    const char *platform_id,
                                    jw_platform_result *out) {
    char message[JW_PLATFORM_MAX_MESSAGE];
    snprintf(message, sizeof(message), "%s is unsupported on %s",
             jw_platform_action_name(action), platform_id ? platform_id : "unknown");
    jw_platform_result_set(out, JW_PLATFORM_RESULT_UNSUPPORTED, message);
}

int jw_platform_init(jw_platform_context *ctx, const char *runtime_dir, const char *sdcard_root) {
    if (!ctx || !runtime_dir || !sdcard_root) {
        return -1;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (!backend || !backend->platform_id || !backend->platform_name) {
        return -1;
    }

    memset(ctx, 0, sizeof(*ctx));
    snprintf(ctx->platform_id, sizeof(ctx->platform_id), "%s", backend->platform_id);
    snprintf(ctx->platform_name, sizeof(ctx->platform_name), "%s", backend->platform_name);
    snprintf(ctx->runtime_dir, sizeof(ctx->runtime_dir), "%s", runtime_dir);
    snprintf(ctx->sdcard_root, sizeof(ctx->sdcard_root), "%s", sdcard_root);
    ctx->capabilities = backend->capabilities;

    const char *script_env = getenv("JAWAKA_PLATFORM_SCRIPT_DIR");
    if (script_env && script_env[0]) {
        snprintf(ctx->script_dir, sizeof(ctx->script_dir), "%s", script_env);
    } else if ((script_env = getenv("UMRK_PLATFORM_PATH")) && script_env[0]) {
        snprintf(ctx->script_dir, sizeof(ctx->script_dir), "%s/platform.d", script_env);
    } else if ((script_env = getenv("SYSTEM_PATH")) && script_env[0]) {
        snprintf(ctx->script_dir, sizeof(ctx->script_dir), "%s/platform.d", script_env);
    } else {
        const char *prefix = jw__platform_uses_dot_system(ctx->platform_id)
            ? ".system"
            : "UMRK";
        snprintf(ctx->script_dir, sizeof(ctx->script_dir), "%s/%s/%s/platform.d",
                 sdcard_root, prefix, ctx->platform_id);
    }

    if (backend->init && backend->init(ctx) != 0) {
        return -1;
    }
    return 0;
}

void jw_platform_shutdown(jw_platform_context *ctx) {
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (ctx && backend && backend->shutdown) {
        backend->shutdown(ctx);
    }
}

static void jw__platform_status_init(jw_platform_status *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->battery_percent = -1;
    out->charging = -1;
    out->brightness_percent = -1;
    out->volume_percent = -1;
    out->audio_output = JW_PLATFORM_AUDIO_OUTPUT_UNKNOWN;
    out->audio_available_outputs = 0;
    for (int i = 0; i < JW_PLATFORM_AUDIO_OUTPUT_COUNT; i++) {
        out->audio_volume_percent[i] = -1;
    }
    out->wifi_connected = -1;
    out->wifi_strength = -1;
    out->bluetooth_connected = -1;
    out->adb_enabled = -1;
    out->adb_intent_enabled = -1;
    out->boot_splash_enabled = -1;
    out->refresh_rate_hz = -1;
    out->color_temp_kelvin = -1;
    out->hdmi_connected = -1;
    out->hdmi_output_mode = -1;
}

void jw_platform_get_status(jw_platform_context *ctx, jw_platform_status *out) {
    if (!ctx || !out) {
        return;
    }

    jw__platform_status_init(out);

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->get_status) {
        backend->get_status(ctx, out);
    }
}

void jw_platform_get_audio_status(jw_platform_context *ctx, jw_platform_status *out) {
    if (!ctx || !out) {
        return;
    }

    jw__platform_status_init(out);

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->get_audio_status) {
        backend->get_audio_status(ctx, out);
    } else if (backend && backend->get_status) {
        backend->get_status(ctx, out);
    }
}

unsigned jw_platform_audio_tick(jw_platform_context *ctx) {
    if (!ctx) {
        return 0;
    }
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->audio_tick) {
        return backend->audio_tick(ctx);
    }
    return 0;
}

int jw_platform_poll_fds(jw_platform_context *ctx, int *fds, int max) {
    if (!ctx || !fds || max <= 0) {
        return 0;
    }
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->poll_fds) {
        return backend->poll_fds(ctx, fds, max);
    }
    return 0;
}

long long jw_platform_next_deadline_ms(jw_platform_context *ctx, long long now_ms) {
    if (!ctx) {
        return -1;
    }
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->next_deadline_ms) {
        return backend->next_deadline_ms(ctx, now_ms);
    }
    return -1;
}

void jw_platform_sleep_audio(jw_platform_context *ctx) {
    if (!ctx) {
        return;
    }
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->sleep_audio) {
        backend->sleep_audio(ctx);
    }
}

void jw_platform_wake_audio(jw_platform_context *ctx) {
    if (!ctx) {
        return;
    }
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->wake_audio) {
        backend->wake_audio(ctx);
    }
}

void jw_platform_audio_reconcile(jw_platform_context *ctx, const char *reason) {
    if (!ctx) {
        return;
    }
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->audio_reconcile) {
        backend->audio_reconcile(ctx, reason);
    }
}

void jw_platform_frontend_ready(jw_platform_context *ctx, const char *role, jw_platform_result *out) {
    if (!ctx || !role || !role[0]) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "missing frontend role");
        return;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->frontend_ready) {
        backend->frontend_ready(ctx, role, out);
        return;
    }

    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "frontend ready noted");
}

bool jw_platform_parse_action(const char *name, jw_platform_action *out) {
    if (!name || !out) {
        return false;
    }

    if (strcmp(name, "sleep") == 0) {
        *out = JW_PLATFORM_ACTION_SLEEP;
    } else if (strcmp(name, "poweroff") == 0) {
        *out = JW_PLATFORM_ACTION_POWEROFF;
    } else if (strcmp(name, "reboot") == 0) {
        *out = JW_PLATFORM_ACTION_REBOOT;
    } else if (strcmp(name, "set-brightness") == 0) {
        *out = JW_PLATFORM_ACTION_SET_BRIGHTNESS;
    } else if (strcmp(name, "set-volume") == 0) {
        *out = JW_PLATFORM_ACTION_SET_VOLUME;
    } else if (strcmp(name, "wifi-on") == 0) {
        *out = JW_PLATFORM_ACTION_WIFI_ON;
    } else if (strcmp(name, "wifi-off") == 0) {
        *out = JW_PLATFORM_ACTION_WIFI_OFF;
    } else if (strcmp(name, "bluetooth-on") == 0) {
        *out = JW_PLATFORM_ACTION_BLUETOOTH_ON;
    } else if (strcmp(name, "bluetooth-off") == 0) {
        *out = JW_PLATFORM_ACTION_BLUETOOTH_OFF;
    } else if (strcmp(name, "set-audio-output") == 0) {
        *out = JW_PLATFORM_ACTION_SET_AUDIO_OUTPUT;
    } else if (strcmp(name, "set-auto-sleep") == 0) {
        *out = JW_PLATFORM_ACTION_SET_AUTO_SLEEP;
    } else if (strcmp(name, "screen-off") == 0) {
        *out = JW_PLATFORM_ACTION_SCREEN_OFF;
    } else if (strcmp(name, "screen-on") == 0) {
        *out = JW_PLATFORM_ACTION_SCREEN_ON;
    } else if (strcmp(name, "enable-adb") == 0) {
        *out = JW_PLATFORM_ACTION_ENABLE_ADB;
    } else if (strcmp(name, "disable-adb") == 0) {
        *out = JW_PLATFORM_ACTION_DISABLE_ADB;
    } else if (strcmp(name, "set-boot-splash") == 0) {
        *out = JW_PLATFORM_ACTION_SET_BOOT_SPLASH;
    } else if (strcmp(name, "play-test-sound") == 0) {
        *out = JW_PLATFORM_ACTION_PLAY_TEST_SOUND;
    } else if (strcmp(name, "set-refresh-rate") == 0) {
        *out = JW_PLATFORM_ACTION_SET_REFRESH_RATE;
    } else if (strcmp(name, "set-hdmi-output") == 0) {
        *out = JW_PLATFORM_ACTION_SET_HDMI_OUTPUT;
    } else if (strcmp(name, "set-color-temp") == 0) {
        *out = JW_PLATFORM_ACTION_SET_COLOR_TEMP;
    } else {
        return false;
    }

    return true;
}

const char *jw_platform_action_name(jw_platform_action action) {
    switch (action) {
        case JW_PLATFORM_ACTION_SLEEP: return "sleep";
        case JW_PLATFORM_ACTION_POWEROFF: return "poweroff";
        case JW_PLATFORM_ACTION_REBOOT: return "reboot";
        case JW_PLATFORM_ACTION_SET_BRIGHTNESS: return "set-brightness";
        case JW_PLATFORM_ACTION_SET_VOLUME: return "set-volume";
        case JW_PLATFORM_ACTION_WIFI_ON: return "wifi-on";
        case JW_PLATFORM_ACTION_WIFI_OFF: return "wifi-off";
        case JW_PLATFORM_ACTION_BLUETOOTH_ON: return "bluetooth-on";
        case JW_PLATFORM_ACTION_BLUETOOTH_OFF: return "bluetooth-off";
        case JW_PLATFORM_ACTION_SET_AUDIO_OUTPUT: return "set-audio-output";
        case JW_PLATFORM_ACTION_SET_AUTO_SLEEP: return "set-auto-sleep";
        case JW_PLATFORM_ACTION_SCREEN_OFF: return "screen-off";
        case JW_PLATFORM_ACTION_SCREEN_ON: return "screen-on";
        case JW_PLATFORM_ACTION_ENABLE_ADB: return "enable-adb";
        case JW_PLATFORM_ACTION_DISABLE_ADB: return "disable-adb";
        case JW_PLATFORM_ACTION_SET_BOOT_SPLASH: return "set-boot-splash";
        case JW_PLATFORM_ACTION_PLAY_TEST_SOUND: return "play-test-sound";
        case JW_PLATFORM_ACTION_SET_REFRESH_RATE: return "set-refresh-rate";
        case JW_PLATFORM_ACTION_SET_HDMI_OUTPUT: return "set-hdmi-output";
        case JW_PLATFORM_ACTION_SET_COLOR_TEMP: return "set-color-temp";
        default: return "unknown";
    }
}

const char *jw_platform_result_code_name(jw_platform_result_code code) {
    switch (code) {
        case JW_PLATFORM_RESULT_OK: return "ok";
        case JW_PLATFORM_RESULT_UNSUPPORTED: return "unsupported";
        case JW_PLATFORM_RESULT_UNAVAILABLE: return "unavailable";
        case JW_PLATFORM_RESULT_FAILED: return "failed";
        case JW_PLATFORM_RESULT_INVALID: return "invalid";
        default: return "unknown";
    }
}

void jw_platform_perform_action(jw_platform_context *ctx, jw_platform_action action,
                                int value, jw_platform_result *out) {
    if (!ctx) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "platform not initialized");
        return;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->perform_action) {
        backend->perform_action(ctx, action, value, out);
        return;
    }

    jw_platform_result_unsupported(action, ctx->platform_id, out);
}

static void jw__platform_perf_status_init(jw_platform_perf_status *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->soc_temp_c = -1;
    const char *names[JW_PLATFORM_PERF_DOMAIN_COUNT] = { "CPU", "GPU", "DMC" };
    for (int i = 0; i < JW_PLATFORM_PERF_DOMAIN_COUNT; i++) {
        snprintf(out->domains[i].name, sizeof(out->domains[i].name), "%s", names[i]);
        out->domains[i].current_freq = -1;
        out->domains[i].set_freq = -1;
    }
    snprintf(out->message, sizeof(out->message), "%s", "performance unsupported");
}

void jw_platform_get_performance_status(jw_platform_context *ctx,
                                        jw_platform_perf_status *out) {
    if (!out) {
        return;
    }
    jw__platform_perf_status_init(out);
    if (!ctx) {
        snprintf(out->message, sizeof(out->message), "%s", "platform not initialized");
        return;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->get_performance_status) {
        backend->get_performance_status(ctx, out);
    }
}

void jw_platform_apply_performance(jw_platform_context *ctx,
                                   const jw_platform_perf_request *request,
                                   jw_platform_result *out) {
    if (!ctx) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "platform not initialized");
        return;
    }
    if (!request) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "missing performance request");
        return;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->apply_performance) {
        backend->apply_performance(ctx, request, out);
        return;
    }

    jw_platform_result_set(out, JW_PLATFORM_RESULT_UNSUPPORTED,
                           "performance unsupported");
}

bool jw_platform_storage_tick(jw_platform_context *ctx) {
    if (!ctx) {
        return false;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->storage_tick) {
        return backend->storage_tick(ctx);
    }
    return false;
}

void jw_platform_get_storage_status(jw_platform_context *ctx, const char *source_id,
                                    jw_platform_storage_status *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->source_id, sizeof(out->source_id), "%s",
             source_id && source_id[0] ? source_id : "secondary_sd");
    snprintf(out->label, sizeof(out->label), "%s", "Secondary SD");
    snprintf(out->message, sizeof(out->message), "%s", "storage source unavailable");

    if (!ctx) {
        return;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->get_storage_status) {
        backend->get_storage_status(ctx, source_id, out);
    }
}

void jw_platform_safe_unmount_storage(jw_platform_context *ctx, const char *source_id,
                                      jw_platform_result *out) {
    if (!ctx) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "platform not initialized");
        return;
    }

    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->safe_unmount_storage) {
        backend->safe_unmount_storage(ctx, source_id, out);
        return;
    }

    jw_platform_result_set(out, JW_PLATFORM_RESULT_UNSUPPORTED,
                           "storage source unavailable");
}

int jw_platform_get_display_mode(jw_platform_context *ctx, int *width, int *height,
                                 int *hz) {
    if (!ctx) {
        return -1;
    }
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->get_display_mode) {
        return backend->get_display_mode(ctx, width, height, hz);
    }
    return -1;
}

void jw_platform_set_led(jw_platform_context *ctx, const jw_led_config *cfg,
                         jw_platform_result *out) {
    if (!ctx) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "platform not initialized");
        return;
    }
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->set_led) {
        backend->set_led(ctx, cfg, out);
        return;
    }
    jw_platform_result_set(out, JW_PLATFORM_RESULT_UNSUPPORTED, "led not supported");
}

void jw_platform_set_power_hard_cut_s(jw_platform_context *ctx, int seconds,
                                      jw_platform_result *out) {
    if (!ctx) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "platform not initialized");
        return;
    }
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->set_power_hard_cut) {
        backend->set_power_hard_cut(ctx, seconds, out);
        return;
    }
    jw_platform_result_set(out, JW_PLATFORM_RESULT_UNSUPPORTED,
                           "power hard cut not supported");
}

int jw_platform_storage_roots(jw_platform_context *ctx, jw_platform_storage_root *out,
                              int max) {
    if (!ctx || !out || max <= 0) {
        return 0;
    }
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (backend && backend->storage_roots) {
        return backend->storage_roots(ctx, out, max);
    }

    /* Generic: the launcher root, then the second configured card root. */
    int count = 0;
    memset(&out[0], 0, sizeof(out[0]));
    snprintf(out[0].source_id, sizeof(out[0].source_id), "%s",
             JW_PLATFORM_STORAGE_LAUNCHER_ID);
    snprintf(out[0].label, sizeof(out[0].label), "%s", "Launcher SD");
    snprintf(out[0].root, sizeof(out[0].root), "%s", ctx->sdcard_root);
    count++;

    const char *paths = getenv("SDCARD_PATHS");
    if (count < max && paths && paths[0]) {
        char copy[JW_PLATFORM_MAX_PATH * 2];
        snprintf(copy, sizeof(copy), "%s", paths);
        char *save = NULL;
        for (char *token = strtok_r(copy, ":", &save); token;
             token = strtok_r(NULL, ":", &save)) {
            if (!token[0] || strcmp(token, ctx->sdcard_root) == 0) {
                continue;
            }
            memset(&out[count], 0, sizeof(out[count]));
            snprintf(out[count].source_id, sizeof(out[count].source_id), "%s",
                     JW_PLATFORM_STORAGE_SECONDARY_ID);
            snprintf(out[count].label, sizeof(out[count].label), "%s", "Secondary SD");
            snprintf(out[count].root, sizeof(out[count].root), "%s", token);
            count++;
            break;
        }
    }
    return count;
}

void jw_platform_get_storage_repair_capability(jw_platform_context *ctx,
                                           const char *fs_type,
                                           const char *uuid,
                                           bool block_write_protected,
                                           jw_platform_storage_repair_capability *out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    snprintf(out->unavailable_reason, sizeof(out->unavailable_reason), "%s",
             "unsupported-platform");
    const jw_platform_backend *backend = jw_platform_get_backend();
    if (ctx && backend && backend->storage_repair_capability) {
        backend->storage_repair_capability(ctx, fs_type, uuid,
                                           block_write_protected, out);
    }
}
