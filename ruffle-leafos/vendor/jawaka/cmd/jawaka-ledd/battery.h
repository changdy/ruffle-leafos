/* jawaka-ledd Battery Level: the ring's color follows the charge the kernel
 * reports. Pure logic, shared by main.c and battery_test.c so the test runs
 * the production code; the loop that drives it lives in main.c.
 *
 * A band spans 20% of charge, so the steady bands read the capacity once per
 * steady pass and write under the policy in steady.h. Only the flashing low
 * band writes every second. */
#ifndef JW_LEDD_BATTERY_H
#define JW_LEDD_BATTERY_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "steady.h"

/* Overridable so a test-only device build can read a fixture instead of the
   gauge. */
#ifndef JW_LEDD_CAPACITY_PATH
#define JW_LEDD_CAPACITY_PATH "/sys/class/power_supply/battery/capacity"
#endif

#define JW_LEDD_BATTERY_FLASH_MS    1000LL   /* one flash phase per pass */

typedef struct {
    uint8_t r, g, b;
    bool flash;
} jw_ledd_battery_band;

/* The charge as the kernel prints it: a whole number 0-100 and a newline.
   Anything else (no file, empty, junk, out of range) is -1, unknown, so a bad
   read can never pass for 0% or keep showing a stale high charge. */
static inline int jw_ledd_battery_read(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) return -1;
    char buf[8];
    size_t n = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);
    if (n == sizeof(buf)) return -1;
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) n--;
    if (n == 0 || n > 3) return -1;
    int value = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] < '0' || buf[i] > '9') return -1;
        value = value * 10 + (buf[i] - '0');
    }
    return value <= 100 ? value : -1;
}

/* Leaf #93's palette. Its endpoints overlapped (80 was in two bands); each
   boundary goes to the lower-charge band. */
static inline jw_ledd_battery_band jw_ledd_battery_band_for(int percent) {
    jw_ledd_battery_band band = { 0xFF, 0x00, 0x00, false };     /* 21-40: red */
    if (percent > 80)      { band.r = 0x00; band.g = 0x00; band.b = 0xFF; }  /* blue */
    else if (percent > 60) { band.r = 0x00; band.g = 0xFF; band.b = 0x00; }  /* green */
    else if (percent > 40) { band.r = 0xFF; band.g = 0x80; band.b = 0x00; }  /* orange */
    else if (percent <= 20) band.flash = true;                    /* 0-20: flashing red */
    return band;
}

/* True when the pass alternates lit and dark frames. At zero brightness both
   phases are the same dark frame, so the slow pass is enough. */
static inline bool jw_ledd_battery_flashing(int percent, int alpha_max) {
    return percent >= 0 && percent <= 100 && alpha_max > 0 &&
           jw_ledd_battery_band_for(percent).flash;
}

/* The 0xAARRGGBB every LED shows. 0 (dark) for an unknown charge, zero
   brightness, and the dark phase of the flash. */
static inline uint32_t jw_ledd_battery_color(int percent, int alpha_max, bool flash_lit) {
    if (percent < 0 || percent > 100 || alpha_max <= 0) return 0;
    jw_ledd_battery_band band = jw_ledd_battery_band_for(percent);
    if (band.flash && !flash_lit) return 0;
    if (alpha_max > 255) alpha_max = 255;
    return ((uint32_t)alpha_max << 24) | ((uint32_t)band.r << 16) |
           ((uint32_t)band.g << 8) | (uint32_t)band.b;
}

#endif
