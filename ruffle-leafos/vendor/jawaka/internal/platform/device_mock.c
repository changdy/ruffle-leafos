#include "internal/platform/device_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int brightness_percent;
    char perf_governor[JW_PLATFORM_PERF_DOMAIN_COUNT][JW_PLATFORM_PERF_VALUE_MAX];
    int perf_freq[JW_PLATFORM_PERF_DOMAIN_COUNT];
} jw_mock_platform_data;

static int jw__env_int(const char *name, int min_value, int max_value) {
    const char *value = getenv(name);
    if (!value || !value[0]) {
        return -1;
    }

    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || (end && *end != '\0')) {
        return -1;
    }
    if (parsed < min_value) parsed = min_value;
    if (parsed > max_value) parsed = max_value;
    return (int)parsed;
}

static jw_mock_platform_data *jw__mock_data(jw_platform_context *ctx) {
    return ctx ? (jw_mock_platform_data *)ctx->backend_data : NULL;
}

static int jw__mock_init(jw_platform_context *ctx) {
    jw_mock_platform_data *data = (jw_mock_platform_data *)calloc(1, sizeof(*data));
    if (!data) {
        return -1;
    }

    data->brightness_percent = jw__env_int("CAT_PREVIEW_BRIGHTNESS_PERCENT",
                                           JW_PLATFORM_BRIGHTNESS_MIN_PERCENT,
                                           JW_PLATFORM_BRIGHTNESS_MAX_PERCENT);
    if (data->brightness_percent < 0) {
        data->brightness_percent = 50;
    }
    snprintf(data->perf_governor[JW_PLATFORM_PERF_DOMAIN_CPU],
             sizeof(data->perf_governor[JW_PLATFORM_PERF_DOMAIN_CPU]), "%s", "schedutil");
    snprintf(data->perf_governor[JW_PLATFORM_PERF_DOMAIN_GPU],
             sizeof(data->perf_governor[JW_PLATFORM_PERF_DOMAIN_GPU]), "%s", "simple_ondemand");
    snprintf(data->perf_governor[JW_PLATFORM_PERF_DOMAIN_DMC],
             sizeof(data->perf_governor[JW_PLATFORM_PERF_DOMAIN_DMC]), "%s", "dmc_ondemand");
    data->perf_freq[JW_PLATFORM_PERF_DOMAIN_CPU] = 1608000;
    data->perf_freq[JW_PLATFORM_PERF_DOMAIN_GPU] = 400000000;
    data->perf_freq[JW_PLATFORM_PERF_DOMAIN_DMC] = 528000000;
    ctx->backend_data = data;
    return 0;
}

static void jw__mock_shutdown(jw_platform_context *ctx) {
    free(ctx ? ctx->backend_data : NULL);
    if (ctx) {
        ctx->backend_data = NULL;
    }
}

static void jw__mock_get_status(jw_platform_context *ctx, jw_platform_status *out) {
    jw_mock_platform_data *data = jw__mock_data(ctx);
    if (!data || !out) {
        return;
    }

    out->battery_percent = jw__env_int("CAT_PREVIEW_BATTERY_PERCENT", 0, 100);
    int charging = jw__env_int("CAT_PREVIEW_CHARGING", 0, 1);
    if (charging >= 0) {
        out->charging = charging ? 1 : 0;
    }
    out->brightness_percent = data->brightness_percent;
}

static void jw__mock_frontend_ready(jw_platform_context *ctx, const char *role,
                                    jw_platform_result *out) {
    (void)role;
    if (ctx) {
        ctx->home_ready_sent = true;
    }
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "frontend ready noted");
}

static void jw__mock_perform_action(jw_platform_context *ctx, jw_platform_action action,
                                    int value, jw_platform_result *out) {
    jw_mock_platform_data *data = jw__mock_data(ctx);
    if (action != JW_PLATFORM_ACTION_SET_BRIGHTNESS) {
        jw_platform_result_unsupported(action, ctx ? ctx->platform_id : "mac", out);
        return;
    }
    if (!data) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "platform not initialized");
        return;
    }

    int percent = jw_platform_clamp_brightness_percent(value);
    data->brightness_percent = percent;

    char message[JW_PLATFORM_MAX_MESSAGE];
    snprintf(message, sizeof(message), "brightness preview set to %d%%", percent);
    jw_platform_result_set_value(out, JW_PLATFORM_RESULT_OK, message, percent);
}

static const char *jw__mock_perf_name(jw_platform_perf_domain domain) {
    switch (domain) {
        case JW_PLATFORM_PERF_DOMAIN_GPU: return "GPU";
        case JW_PLATFORM_PERF_DOMAIN_DMC: return "DMC";
        case JW_PLATFORM_PERF_DOMAIN_CPU:
        default: return "CPU";
    }
}

static const char *jw__mock_perf_governors(jw_platform_perf_domain domain) {
    switch (domain) {
        case JW_PLATFORM_PERF_DOMAIN_GPU:
        case JW_PLATFORM_PERF_DOMAIN_DMC:
            return "dmc_ondemand vdec2_ondemand userspace powersave performance simple_ondemand";
        case JW_PLATFORM_PERF_DOMAIN_CPU:
        default:
            return "interactive conservative ondemand userspace powersave performance schedutil";
    }
}

static const char *jw__mock_perf_freqs(jw_platform_perf_domain domain) {
    switch (domain) {
        case JW_PLATFORM_PERF_DOMAIN_GPU:
            return "800000000 700000000 600000000 400000000 300000000 200000000";
        case JW_PLATFORM_PERF_DOMAIN_DMC:
            return "324000000 528000000 780000000 1056000000";
        case JW_PLATFORM_PERF_DOMAIN_CPU:
        default:
            return "408000 600000 816000 1104000 1416000 1608000 1800000 1992000";
    }
}

static void jw__mock_get_performance_status(jw_platform_context *ctx,
                                            jw_platform_perf_status *out) {
    jw_mock_platform_data *data = jw__mock_data(ctx);
    if (!data || !out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->supported = true;
    out->soc_temp_c = 42;
    snprintf(out->message, sizeof(out->message), "%s", "performance preview ready");
    for (int i = 0; i < JW_PLATFORM_PERF_DOMAIN_COUNT; i++) {
        jw_platform_perf_domain domain = (jw_platform_perf_domain)i;
        jw_platform_perf_domain_status *d = &out->domains[i];
        d->supported = true;
        snprintf(d->name, sizeof(d->name), "%s", jw__mock_perf_name(domain));
        snprintf(d->governor, sizeof(d->governor), "%s", data->perf_governor[i]);
        d->current_freq = data->perf_freq[i];
        d->set_freq = strcmp(data->perf_governor[i], "userspace") == 0
                    ? data->perf_freq[i]
                    : -1;
        snprintf(d->available_governors, sizeof(d->available_governors),
                 "%s", jw__mock_perf_governors(domain));
        snprintf(d->available_frequencies, sizeof(d->available_frequencies),
                 "%s", jw__mock_perf_freqs(domain));
    }
}

static void jw__mock_apply_performance(jw_platform_context *ctx,
                                       const jw_platform_perf_request *request,
                                       jw_platform_result *out) {
    jw_mock_platform_data *data = jw__mock_data(ctx);
    if (!data || !request) {
        jw_platform_result_set(out, JW_PLATFORM_RESULT_INVALID, "platform not initialized");
        return;
    }
    for (int i = 0; i < JW_PLATFORM_PERF_DOMAIN_COUNT; i++) {
        if (request->domains[i].governor[0]) {
            snprintf(data->perf_governor[i], sizeof(data->perf_governor[i]),
                     "%s", request->domains[i].governor);
        }
        if (request->domains[i].frequency >= 0) {
            data->perf_freq[i] = request->domains[i].frequency;
        }
    }
    jw_platform_result_set(out, JW_PLATFORM_RESULT_OK, "performance preview applied");
}

const jw_platform_backend *jw_platform_get_backend(void) {
    static const jw_platform_backend backend = {
        .platform_id = "mac",
        .platform_name = "Mac Preview",
        .capabilities = {
            .battery = true,
            .charging = true,
            .brightness = true,
            .performance = true,
        },
        .init = jw__mock_init,
        .shutdown = jw__mock_shutdown,
        .get_status = jw__mock_get_status,
        .frontend_ready = jw__mock_frontend_ready,
        .perform_action = jw__mock_perform_action,
        .get_performance_status = jw__mock_get_performance_status,
        .apply_performance = jw__mock_apply_performance,
    };
    return &backend;
}
