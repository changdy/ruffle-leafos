/* Battery Level's reader, palette, and flash (battery.h), and the write
   decision static, off, and Battery Level share (steady.h), run against the
   production code with temporary capacity files. No LED writes.

   Usage: led-battery-test */
#include "battery.h"
#include "internal/platform/device.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_failures;
static char g_dir[] = "/tmp/led-battery-test.XXXXXX";

#define CHECK(cond, ...)                                              \
    do {                                                              \
        if (!(cond)) {                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
            g_failures++;                                             \
        }                                                             \
    } while (0)

/* Writes `text` (len bytes, so an empty file is possible) and reads it back
   through the production reader. */
static int read_text(const char *text, size_t len) {
    char path[sizeof(g_dir) + 16];
    snprintf(path, sizeof(path), "%s/capacity", g_dir);
    FILE *fp = fopen(path, "w");
    if (!fp) {
        CHECK(0, "could not write %s", path);
        return -2;
    }
    fwrite(text, 1, len, fp);
    fclose(fp);
    return jw_ledd_battery_read(path);
}

#define READ(s) read_text((s), sizeof(s) - 1)

static void test_reader(void) {
    CHECK(READ("57\n") == 57, "plain reading");
    CHECK(READ("0\n") == 0, "empty battery is 0, not unknown");
    CHECK(READ("100\n") == 100, "full battery");
    CHECK(READ("42") == 42, "no trailing newline");

    CHECK(READ("") == -1, "empty file is unknown");
    CHECK(READ("\n") == -1, "blank line is unknown");
    CHECK(READ("abc\n") == -1, "text is unknown");
    CHECK(READ("12a\n") == -1, "trailing junk is unknown");
    CHECK(READ(" 50\n") == -1, "leading space is unknown");
    CHECK(READ("5 0\n") == -1, "split number is unknown");
    CHECK(READ("-1\n") == -1, "negative is unknown");
    CHECK(READ("101\n") == -1, "over 100 is unknown");
    CHECK(READ("0000000000\n") == -1, "overlong reading is unknown");

    char missing[sizeof(g_dir) + 16];
    snprintf(missing, sizeof(missing), "%s/missing", g_dir);
    CHECK(jw_ledd_battery_read(missing) == -1, "missing file is unknown");

    /* A bad read must not stick: the next valid one is used as is. */
    CHECK(READ("junk") == -1 && READ("63\n") == 63, "valid read after a bad one");
}

static void test_bands(void) {
    static const struct {
        int percent;
        uint32_t rgb;
        bool flash;
    } cases[] = {
        { 100, 0x0000FF, false }, { 81, 0x0000FF, false },
        { 80,  0x00FF00, false }, { 61, 0x00FF00, false },
        { 60,  0xFF8000, false }, { 41, 0xFF8000, false },
        { 40,  0xFF0000, false }, { 21, 0xFF0000, false },
        { 20,  0xFF0000, true  }, { 0,  0xFF0000, true  },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        int pct = cases[i].percent;
        jw_ledd_battery_band band = jw_ledd_battery_band_for(pct);
        uint32_t rgb = ((uint32_t)band.r << 16) | ((uint32_t)band.g << 8) | band.b;
        CHECK(rgb == cases[i].rgb, "%d%%: color %06x, want %06x", pct,
              (unsigned)rgb, (unsigned)cases[i].rgb);
        CHECK(band.flash == cases[i].flash, "%d%%: flash %d", pct, band.flash);
        CHECK(jw_ledd_battery_flashing(pct, 255) == cases[i].flash,
              "%d%%: flashing pass", pct);
        CHECK(jw_ledd_battery_color(pct, 255, true) == (0xFF000000u | cases[i].rgb),
              "%d%%: lit frame", pct);
    }
}

static void test_unknown_and_flash(void) {
    CHECK(jw_ledd_battery_color(-1, 255, true) == 0, "unknown charge is dark");
    CHECK(jw_ledd_battery_color(101, 255, true) == 0, "out-of-range charge is dark");
    CHECK(!jw_ledd_battery_flashing(-1, 255), "unknown charge does not flash");

    CHECK(jw_ledd_battery_color(15, 255, true) == 0xFFFF0000u, "flash lit phase is red");
    CHECK(jw_ledd_battery_color(15, 255, false) == 0, "flash dark phase is dark");
    CHECK(jw_ledd_battery_color(50, 255, false) == jw_ledd_battery_color(50, 255, true),
          "steady bands ignore the flash phase");
}

static void test_brightness(void) {
    /* main.c's alpha_max for brightness 5 of 10. */
    CHECK(jw_ledd_battery_color(90, 127, true) == 0x7F0000FFu, "brightness scales alpha");
    CHECK(jw_ledd_battery_color(90, 400, true) == 0xFF0000FFu, "alpha clamps at 255");
    for (int pct = 0; pct <= 100; pct++) {
        CHECK(jw_ledd_battery_color(pct, 0, true) == 0, "%d%%: brightness 0 is dark", pct);
    }
    CHECK(!jw_ledd_battery_flashing(10, 0), "brightness 0 takes the slow pass");
}

static void test_write_decision(void) {
    uint32_t blue = jw_ledd_battery_color(90, 255, true);
    uint32_t green = jw_ledd_battery_color(70, 255, true);
    long long refresh = JW_LEDD_REFRESH_MS;

    CHECK(jw_ledd_should_write(false, 0, blue, false, 0), "first frame is written");
    CHECK(!jw_ledd_should_write(true, blue, blue, false, 5000),
          "unchanged frame is skipped");
    CHECK(!jw_ledd_should_write(true, blue, blue, false, refresh - 1),
          "unchanged frame is skipped until the refresh");
    CHECK(jw_ledd_should_write(true, blue, blue, false, refresh),
          "refresh rewrites an unchanged frame");
    CHECK(jw_ledd_should_write(true, blue, green, false, 0), "band change is written");
    CHECK(jw_ledd_should_write(true, blue, blue, true, 0), "resume forces a write");

    /* Brightness 0 through a whole minute of 5 s passes: one write. */
    int writes = 0;
    bool have_written = false;
    uint32_t last = 0;
    long long last_write = 0;
    for (long long now = 0; now < refresh; now += JW_LEDD_STEADY_PASS_MS) {
        uint32_t color = jw_ledd_battery_color(10, 0, (now / 5000) % 2 == 0);
        if (jw_ledd_should_write(have_written, last, color, false, now - last_write)) {
            writes++;
            have_written = true;
            last = color;
            last_write = now;
        }
    }
    CHECK(writes == 1, "brightness 0 wrote %d times in a minute, want 1", writes);
}

/* static and off never change their frame: five minutes of 5 s passes write
   at the start and on each refresh, where they used to write every second. */
static int steady_writes(uint32_t color, long long resume_at) {
    int writes = 0;
    bool have_written = false;
    uint32_t last = 0;
    long long last_write = 0;
    for (long long now = 0; now < 5 * JW_LEDD_REFRESH_MS; now += JW_LEDD_STEADY_PASS_MS) {
        bool resumed = now == resume_at;
        if (jw_ledd_should_write(have_written, last, color, resumed, now - last_write)) {
            writes++;
            have_written = true;
            last = color;
            last_write = now;
        }
    }
    return writes;
}

static void test_static_and_off(void) {
    uint32_t green = 0x7F23DC00u;   /* static 35 220 0 at brightness 5 */
    int w = steady_writes(green, -1);
    CHECK(w == 5, "static wrote %d times in five minutes, want 5", w);
    w = steady_writes(0, -1);
    CHECK(w == 5, "off wrote %d times in five minutes, want 5", w);
    /* A resume at 90 s adds a write, and the refresh counts on from it. */
    w = steady_writes(green, 90000);
    CHECK(w == 6, "static with a resume wrote %d times, want 6", w);
}

static void test_mode(void) {
    jw_led_mode mode = JW_LED_MODE_STATIC;
    CHECK(strcmp(jw_led_mode_name(JW_LED_MODE_BATTERY), "battery") == 0, "wire name");
    CHECK(jw_led_mode_parse("battery", &mode) && mode == JW_LED_MODE_BATTERY,
          "wire name parses back");
    CHECK(jw_led_mode_is_effect(JW_LED_MODE_BATTERY),
          "battery is a helper effect, so the stock backend gets Static");
    for (int m = 0; m < JW_LED_MODE_COUNT; m++) {
        jw_led_mode back = JW_LED_MODE_COUNT;
        CHECK(jw_led_mode_parse(jw_led_mode_name((jw_led_mode)m), &back) &&
              (int)back == m, "mode %d round-trips", m);
    }
}

int main(void) {
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 2;
    }
    test_reader();
    test_bands();
    test_unknown_and_flash();
    test_brightness();
    test_write_decision();
    test_static_and_off();
    test_mode();

    char path[sizeof(g_dir) + 16];
    snprintf(path, sizeof(path), "%s/capacity", g_dir);
    unlink(path);
    rmdir(g_dir);

    if (g_failures) {
        fprintf(stderr, "led-battery-test: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("led-battery-test: ok");
    return 0;
}
