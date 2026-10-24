#include "internal/platform/calibration.h"
#include "internal/core/log.h"

#include "cJSON.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Deadzone floor in raw stick units: the driver's own deadzone (DT
   button-adc-deadzone, 1800). loong1-joystick reports exactly 0 for anything
   within 1800 of its center, but the input core's fuzz (1152) averages that 0
   with the last reported value, and the driver then stops sending zeros. A
   released stick therefore reads a leftover of up to 1151 that never decays,
   a different one after each release. A smaller deadzone forwards it as a
   constant deflection; this one swallows it and also smooths the driver's jump
   from 0 straight to 1800. */
#define JW_CAL_MIN_DEADZONE 1800
/* Reject obviously-garbage bounds (the panel is a signed 16-bit axis). */
#define JW_CAL_SANE_LIMIT   40000
#define JW_CAL_MAX_BYTES    (64 * 1024)

static char *jw__cal_read_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return NULL;
    }
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return NULL;
    }
    long size = ftell(fp);
    if (size < 0 || size > JW_CAL_MAX_BYTES || fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return NULL;
    }
    char *buf = (char *)malloc((size_t)size + 1u);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)size, fp);
    int read_error = ferror(fp);
    fclose(fp);
    if (got != (size_t)size || read_error) {
        free(buf);
        return NULL;
    }
    buf[size] = '\0';
    return buf;
}

static bool jw__cal_int(const cJSON *obj, const char *name, int32_t *out) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *out = (int32_t)item->valuedouble;
    return true;
}

static void jw__cal_int_default(const cJSON *obj, const char *name,
                                int32_t fallback, int32_t *out) {
    if (!jw__cal_int(obj, name, out)) {
        *out = fallback;
    }
}

/* Resolve the profile path: explicit override first, then the public per-platform
   userdata location. Returns a pointer into the static buffer or NULL. */
static const char *jw__cal_profile_path(void) {
    const char *override_path = getenv("JAWAKA_STICK_CAL_PROFILE");
    if (override_path && override_path[0]) {
        return override_path;
    }
    const char *userdata = getenv("USERDATA_PATH");
    if (!userdata || !userdata[0]) {
        return NULL;
    }
    static char path[1024];
    int n = snprintf(path, sizeof(path),
                     "%s/input/loong-gamepad-calibration.json", userdata);
    if (n <= 0 || (size_t)n >= sizeof(path)) {
        return NULL;
    }
    return path;
}

static bool jw__cal_axis_sane(int32_t mn, int32_t zero, int32_t mx,
                              int32_t deadzone) {
    if (mn >= zero || zero >= mx) {
        return false;
    }
    if (mn < -JW_CAL_SANE_LIMIT || mx > JW_CAL_SANE_LIMIT) {
        return false;
    }
    /* Both directions must have positive travel beyond the deadzone, or the
       per-axis scale would divide by a non-positive span. */
    if ((mx - zero - deadzone) <= 0 || (zero - mn - deadzone) <= 0) {
        return false;
    }
    return true;
}

/* Reach measured on three units (2026): X -21180..-22463 / 23256..27306,
   Y -21765..-22635 / 23520..25056. Every one reaches these, so a stick with more
   throw just hits full output a little early rather than never getting there.
   No center is needed: the driver re-centers at every boot (median of its last
   nine boot-time readings) and zeroes everything inside its deadzone. */
void jw_calibration_default(jw_stick_calibration *out) {
    if (!out) {
        return;
    }
    *out = (jw_stick_calibration){
        .loaded = true,
        .x_min = -21000, .x_max = 23000,
        .y_min = -21000, .y_max = 23000,
        .x_zero = 0, .y_zero = 0,
        .deadzone = JW_CAL_MIN_DEADZONE,
        .out_min = -32768, .out_max = 32767,
        .radial_clamp = false,   /* per-axis scaling, as Phase 0 settled */
    };
}

bool jw_calibration_load(jw_stick_calibration *out) {
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const char *path = jw__cal_profile_path();
    if (!path) {
        return false;
    }
    char *text = jw__cal_read_file(path);
    if (!text) {
        return false;   /* absent is the normal uncalibrated state — no warning */
    }

    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root) {
        jw_log_warn("calibration: %s is not valid JSON; using the default", path);
        return false;
    }

    int32_t version = 0;
    if (!jw__cal_int(root, "version", &version) || version < 1) {
        jw_log_warn("calibration: %s missing/unsupported version; using the default",
                    path);
        cJSON_Delete(root);
        return false;
    }

    const cJSON *left = cJSON_GetObjectItemCaseSensitive(root, "left");
    if (!cJSON_IsObject(left)) {
        jw_log_warn("calibration: %s has no 'left' block; using the default", path);
        cJSON_Delete(root);
        return false;
    }

    int32_t center_noise = 0;
    if (!jw__cal_int(left, "x_min", &out->x_min) ||
        !jw__cal_int(left, "x_max", &out->x_max) ||
        !jw__cal_int(left, "y_min", &out->y_min) ||
        !jw__cal_int(left, "y_max", &out->y_max)) {
        jw_log_warn("calibration: %s missing axis bounds; using the default", path);
        cJSON_Delete(root);
        return false;
    }
    jw__cal_int_default(left, "x_zero", 0, &out->x_zero);
    jw__cal_int_default(left, "y_zero", 0, &out->y_zero);
    jw__cal_int_default(left, "center_noise", 0, &center_noise);

    const cJSON *derived = cJSON_GetObjectItemCaseSensitive(root, "derived");
    jw__cal_int_default(derived, "normalized_abs_min", -32768, &out->out_min);
    jw__cal_int_default(derived, "normalized_abs_max", 32767, &out->out_max);

    out->radial_clamp = true;   /* default on; a policy string can opt out */
    const cJSON *policy = derived
        ? cJSON_GetObjectItemCaseSensitive(derived, "normalization_policy")
        : NULL;
    if (cJSON_IsString(policy) && policy->valuestring &&
        strstr(policy->valuestring, "radial") == NULL) {
        out->radial_clamp = false;
    }

    cJSON_Delete(root);

    out->deadzone = (center_noise > JW_CAL_MIN_DEADZONE) ? center_noise
                                                         : JW_CAL_MIN_DEADZONE;

    if (out->out_min >= 0 || out->out_max <= 0 ||
        !jw__cal_axis_sane(out->x_min, out->x_zero, out->x_max, out->deadzone) ||
        !jw__cal_axis_sane(out->y_min, out->y_zero, out->y_max, out->deadzone)) {
        jw_log_warn("calibration: %s failed sanity checks; using the default", path);
        memset(out, 0, sizeof(*out));
        return false;
    }

    out->loaded = true;
    jw_log_info("calibration: loaded %s — X[%d..%d z%d] Y[%d..%d z%d] "
                "dz=%d out[%d..%d] radial=%d",
                path, out->x_min, out->x_max, out->x_zero,
                out->y_min, out->y_max, out->y_zero,
                out->deadzone, out->out_min, out->out_max, out->radial_clamp);
    return true;
}

int32_t jw_calibration_axis(const jw_stick_calibration *cal, bool is_x,
                            int32_t raw) {
    if (!cal || !cal->loaded) {
        return raw;
    }
    int32_t mn = is_x ? cal->x_min : cal->y_min;
    int32_t mx = is_x ? cal->x_max : cal->y_max;
    int32_t zero = is_x ? cal->x_zero : cal->y_zero;
    long dz = cal->deadzone;
    long delta = (long)raw - zero;

    if (delta <= dz && delta >= -dz) {
        return 0;
    }
    if (delta > 0) {
        long span = (long)mx - zero - dz;   /* > 0 by load-time validation */
        long val = (delta - dz) * (long)cal->out_max / span;
        if (val > cal->out_max) {
            val = cal->out_max;
        }
        return (int32_t)val;
    }
    long span = (long)zero - mn - dz;        /* > 0 by load-time validation */
    long val = (delta + dz) * (long)(-(long)cal->out_min) / span;
    if (val < cal->out_min) {
        val = cal->out_min;
    }
    return (int32_t)val;
}

void jw_calibration_radial_clamp(const jw_stick_calibration *cal,
                                 int32_t *x, int32_t *y) {
    if (!cal || !cal->loaded || !cal->radial_clamp || !x || !y) {
        return;
    }
    double xx = (double)*x;
    double yy = (double)*y;
    double mag_sq = xx * xx + yy * yy;
    double max = (double)cal->out_max;
    if (mag_sq <= max * max) {
        return;
    }
    double scale = max / sqrt(mag_sq);
    *x = (int32_t)lround(xx * scale);
    *y = (int32_t)lround(yy * scale);
}
