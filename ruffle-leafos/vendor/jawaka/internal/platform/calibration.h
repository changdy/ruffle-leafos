#ifndef JW_PLATFORM_CALIBRATION_H
#define JW_PLATFORM_CALIBRATION_H

#include <stdbool.h>
#include <stdint.h>

/* Analog-stick calibration profile for the single MLP1 stick (the upstream
   "left" stick). Parsed from the JSON profile written by the calibration app;
   consumed by the input proxy to normalize ABS_X/ABS_Y before forwarding them
   to the virtual gamepad. See umrk-workspace/plans/joes-calibrage-mlp1.md. */
typedef struct {
    bool    loaded;          /* a valid, sane profile was parsed */
    int32_t x_min, x_max;    /* measured physical extent on X */
    int32_t y_min, y_max;    /* measured physical extent on Y */
    int32_t x_zero, y_zero;  /* measured center */
    int32_t deadzone;        /* center deadzone in raw units (>= center_noise) */
    int32_t out_min, out_max;/* normalized output range (default -32768..32767) */
    bool    radial_clamp;    /* clamp combined magnitude to out_max after scale */
} jw_stick_calibration;

/* Load the profile from $JAWAKA_STICK_CAL_PROFILE, else
   $USERDATA_PATH/input/loong-gamepad-calibration.json. On success fills *out
   with loaded=true and returns true. On any failure (missing, unreadable,
   malformed, or failing sanity checks) sets out->loaded=false and returns
   false; callers then use jw_calibration_default(). A profile's deadzone never
   drops below the driver's own (1800). */
bool jw_calibration_load(jw_stick_calibration *out);

/* The built-in Loong Gamepad calibration: a range every measured unit reaches,
   zero at the driver's center, and the driver's deadzone. loaded=true. */
void jw_calibration_default(jw_stick_calibration *out);

/* Map one raw axis value to the normalized output range using deadzone-aware
   per-axis scaling. Pure; safe to call only when cal->loaded. is_x selects the
   X vs Y bounds. */
int32_t jw_calibration_axis(const jw_stick_calibration *cal, bool is_x, int32_t raw);

/* Apply the radial clamp to a normalized (x, y) pair so the combined vector
   magnitude never exceeds out_max (prevents impossible fast diagonals). No-op
   when cal->radial_clamp is false. */
void jw_calibration_radial_clamp(const jw_stick_calibration *cal,
                                 int32_t *x, int32_t *y);

#endif /* JW_PLATFORM_CALIBRATION_H */
