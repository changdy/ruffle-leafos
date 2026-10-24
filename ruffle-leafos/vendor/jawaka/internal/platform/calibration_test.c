#include "internal/platform/calibration.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* loong1-joystick: anything within 1800 of center reads 0, and the input core's
   fuzz (1152) then averages that 0 with the last reported value. A released
   stick is left reading anywhere strictly inside +/-1152. */
#define DRIVER_DEADZONE 1800
#define KERNEL_FUZZ     1152

static char g_profile[256];

static void write_profile(const char *json) {
    FILE *fp = fopen(g_profile, "w");
    assert(fp);
    fputs(json, fp);
    fclose(fp);
}

/* A v1 profile as Joe's Calibrage writes it. */
static void write_v1(int x_min, int x_max, int y_min, int y_max,
                     int x_zero, int y_zero, int center_noise) {
    char json[512];
    snprintf(json, sizeof(json),
             "{ \"version\": 1, \"left\": {"
             " \"x_min\": %d, \"x_max\": %d, \"y_min\": %d, \"y_max\": %d,"
             " \"x_zero\": %d, \"y_zero\": %d, \"center_noise\": %d },"
             " \"derived\": { \"normalization_policy\": \"axis_deadzone_scale\" } }",
             x_min, x_max, y_min, y_max, x_zero, y_zero, center_noise);
    write_profile(json);
}

static void expect_rest_is_zero(const jw_stick_calibration *cal) {
    for (int v = -(KERNEL_FUZZ - 1); v <= KERNEL_FUZZ - 1; v++) {
        assert(jw_calibration_axis(cal, true, cal->x_zero + v) == 0);
        assert(jw_calibration_axis(cal, false, cal->y_zero + v) == 0);
    }
}

int main(void) {
    char dir[] = "/tmp/jawaka-calibration-XXXXXX";
    assert(mkdtemp(dir));
    snprintf(g_profile, sizeof(g_profile), "%s/loong-gamepad-calibration.json", dir);
    setenv("JAWAKA_STICK_CAL_PROFILE", g_profile, 1);

    /* No profile: the load fails, and the proxy falls back to the default. */
    jw_stick_calibration cal;
    assert(!jw_calibration_load(&cal));
    assert(!cal.loaded);

    jw_calibration_default(&cal);
    assert(cal.loaded);
    assert(cal.x_zero == 0 && cal.y_zero == 0);
    assert(cal.deadzone == DRIVER_DEADZONE);
    assert(!cal.radial_clamp);
    expect_rest_is_zero(&cal);

    /* The driver's first nonzero report is +/-1800; it now starts the ramp from
       zero instead of jumping to it. */
    assert(jw_calibration_axis(&cal, true, DRIVER_DEADZONE) == 0);
    assert(jw_calibration_axis(&cal, true, DRIVER_DEADZONE + 72) > 0);
    assert(jw_calibration_axis(&cal, true, DRIVER_DEADZONE + 72) < 200);
    assert(jw_calibration_axis(&cal, false, -(DRIVER_DEADZONE + 72)) < 0);

    /* Full output at the default reach, and past it. */
    assert(jw_calibration_axis(&cal, true, cal.x_max) == 32767);
    assert(jw_calibration_axis(&cal, true, 27306) == 32767);
    assert(jw_calibration_axis(&cal, true, cal.x_min) == -32768);
    assert(jw_calibration_axis(&cal, false, cal.y_max) == 32767);
    assert(jw_calibration_axis(&cal, false, cal.y_min) == -32768);

    /* Every unit measured so far reaches the default range, so none of them is
       left short of full output. */
    const int units[][4] = {
        { -21180, 23256, -21765, 25056 },
        { -22410, 24067, -21909, 23520 },
        { -22463, 27306, -22635, 24009 },
    };
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        assert(units[i][0] <= cal.x_min && units[i][1] >= cal.x_max);
        assert(units[i][2] <= cal.y_min && units[i][3] >= cal.y_max);
    }

    /* A profile captured with no center noise (what Joe's Calibrage saved
       before it stopped measuring a center) still gets the driver's
       deadzone, so its leftover no longer reads as a deflection. */
    write_v1(-22463, 27306, -22635, 24009, 0, 0, 0);
    assert(jw_calibration_load(&cal));
    assert(cal.loaded);
    assert(cal.x_max == 27306);
    assert(cal.deadzone == DRIVER_DEADZONE);
    expect_rest_is_zero(&cal);
    assert(jw_calibration_axis(&cal, false, 1080) == 0);   /* the 2026-10-10 report */

    /* A larger deadzone in the profile is kept. */
    write_v1(-22000, 24000, -22000, 24000, 0, 0, 2500);
    assert(jw_calibration_load(&cal));
    assert(cal.deadzone == 2500);

    /* Malformed profiles fail the load, so the proxy uses the default. */
    write_profile("{ not json");
    assert(!jw_calibration_load(&cal));
    write_v1(-1000, 1000, -22000, 24000, 0, 0, 0);   /* span inside the deadzone */
    assert(!jw_calibration_load(&cal));

    unlink(g_profile);
    rmdir(dir);
    printf("stick calibration tests passed\n");
    return 0;
}
