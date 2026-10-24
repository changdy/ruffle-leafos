/* Time-zone data probe: does this libc, with this rootfs zoneinfo, produce the
 * local time the Time Zone picker promises?
 *
 * The picker is a table of IANA ids handed to TZ. Everything below that is the
 * C library and /usr/share/zoneinfo, and neither is ours. A missing id is the
 * dangerous case: glibc does not fail, it silently falls back to UTC, so a zone
 * we never shipped data for looks like a working picker row that quietly tells
 * a New Zealander the wrong time. Stale data is the other case: the id resolves
 * and the offset looks right in January, but a daylight-saving transition fires
 * on the wrong date.
 *
 * So this runs against the shipped table (internal/settings/timezones.c), not a
 * copy of it, and checks two things:
 *
 *   1. Every row's standard-time offset matches what the picker prints beside
 *      it, sampled in January and July so a southern-hemisphere row cannot pass
 *      on its daylight offset. Fixed rows must not move between the two.
 *   2. Named transition fixtures, including both sides of New Zealand's 2026
 *      and 2027 transitions, land on exact wall-clock times.
 *
 * It takes fixed UTC instants and never reads or sets the system clock, so it
 * is safe to run on a device with the launcher up. Build it for the host with
 * `make timezone-test`, and for the MLP1 with `make mlp1-device-timezone-test`.
 */

#include "internal/settings/timezones.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* 2026-01-15 and 2026-07-15, 00:00:00 UTC: one sample per hemisphere's summer,
   which is what makes "the standard offset" identifiable from outside. */
#define JW_TZ_JANUARY  1768435200L
#define JW_TZ_JULY     1784073600L

static int failures = 0;

static void fail(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("FAIL ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    failures++;
}

/* Parse the picker's offset column ("UTC", "UTC+0", "UTC-9:30") into seconds,
   so the check does not depend on which of the two spellings of zero a row
   happens to use. */
static bool parse_offset(const char *s, long *out) {
    if (!s || strncmp(s, "UTC", 3) != 0) return false;
    s += 3;
    if (!*s) { *out = 0; return true; }
    int sign = (*s == '-') ? -1 : (*s == '+') ? 1 : 0;
    if (!sign) return false;
    s++;
    char *end = NULL;
    long h = strtol(s, &end, 10);
    if (end == s || h < 0 || h > 14) return false;
    long m = 0;
    if (*end == ':') {
        const char *ms = end + 1;
        m = strtol(ms, &end, 10);
        if (end == ms || m < 0 || m > 59) return false;
    }
    if (*end) return false;
    *out = sign * (h * 3600 + m * 60);
    return true;
}

static const char *offset_text(long seconds, char *buf, size_t n) {
    long a = seconds < 0 ? -seconds : seconds;
    snprintf(buf, n, "%c%02ld:%02ld", seconds < 0 ? '-' : '+', a / 3600, (a % 3600) / 60);
    return buf;
}

/* One TZ conversion. Returns false only when localtime_r itself fails. */
static bool local_at(const char *tz, time_t when, struct tm *out) {
    setenv("TZ", tz, 1);
    tzset();
    return localtime_r(&when, out) != NULL;
}

/* Does this region observe daylight saving in 2026 under current IANA rules?
 *
 * Checking offsets alone is not enough, and this is where old data actually
 * shows up: a zone whose rules were abolished still resolves, still reports its
 * correct standard offset in winter, and then quietly shifts an hour in summer
 * because the rootfs remembers a rule the country has dropped. Iran was the live
 * example -- DST abolished in tzdb 2022a, still applied by the MLP1's roughly
 * 2021 rootfs data, and caught here while passing every offset check above. That
 * is why Asia/Tehran is not in the picker; see the note in timezones.c before
 * putting it back.
 *
 * Only the DST answer lives here, not labels or offsets: those stay in the one
 * shipped table. Every row must be listed, and check_table() fails on a row
 * that is not, so a zone cannot join the picker without someone deciding what
 * its rules are. */
static const struct { const char *tz; bool dst; } kDstRules2026[] = {
    { "Etc/GMT+12",          false }, { "Pacific/Pago_Pago",   false },
    { "Pacific/Honolulu",    false }, { "Pacific/Marquesas",   false },
    { "America/Anchorage",   true  }, { "America/Los_Angeles", true  },
    { "America/Denver",      true  }, { "America/Phoenix",     false },
    { "America/Chicago",     true  }, { "America/New_York",    true  },
    { "America/Halifax",     true  }, { "America/St_Johns",    true  },
    { "America/Sao_Paulo",   false }, { "America/Noronha",     false },
    { "Atlantic/Cape_Verde", false }, { "UTC",                 false },
    { "Europe/London",       true  }, { "Europe/Paris",        true  },
    { "Europe/Athens",       true  }, { "Europe/Moscow",       false },
    { "Asia/Dubai",          false },
    { "Asia/Kabul",          false }, { "Asia/Karachi",        false },
    { "Asia/Kolkata",        false }, { "Asia/Kathmandu",      false },
    { "Asia/Dhaka",          false }, { "Asia/Yangon",         false },
    { "Asia/Bangkok",        false }, { "Asia/Shanghai",       false },
    { "Australia/Eucla",     false }, { "Asia/Tokyo",          false },
    { "Australia/Adelaide",  true  }, { "Australia/Darwin",    false },
    { "Australia/Sydney",    true  }, { "Australia/Lord_Howe", true  },
    { "Pacific/Guadalcanal", false }, { "Pacific/Auckland",    true  },
    { "Pacific/Chatham",     true  }, { "Pacific/Tongatapu",   false },
    { "Pacific/Kiritimati",  false },
};

static bool dst_rule_for(const char *tz, bool *out) {
    for (unsigned i = 0; i < sizeof(kDstRules2026) / sizeof(kDstRules2026[0]); ++i)
        if (strcmp(kDstRules2026[i].tz, tz) == 0) { *out = kDstRules2026[i].dst; return true; }
    return false;
}

/* glibc answers an unknown TZ with UTC rather than an error, which is exactly
   why the sweep below compares offsets instead of trusting that a row resolved.
   Assert that fallback here, so a libc that grew a different failure mode does
   not leave the sweep quietly checking nothing. */
static void check_unknown_zone_falls_back_to_utc(void) {
    struct tm tm;
    if (!local_at("Jawaka/Not_A_Zone", JW_TZ_JANUARY, &tm)) {
        fail("localtime_r failed outright for an unknown zone");
        return;
    }
    if (tm.tm_gmtoff != 0)
        fail("an unknown zone resolved to %+ld, not UTC: this probe's missing-data "
             "check assumes the UTC fallback", (long)tm.tm_gmtoff);
}

/* Every row: the offset the picker prints must be the region's standard-time
   offset on this device, and a region that does not observe daylight saving
   must read the same in January and July. */
static void check_table(void) {
    for (int i = 0; i < kJawakaTimeZoneCount; ++i) {
        const jw_timezone_entry *e = &kJawakaTimeZones[i];
        long want = 0;
        if (!parse_offset(e->off, &want)) {
            fail("%s: offset column \"%s\" is not parseable", e->tz, e->off);
            continue;
        }
        bool wants_dst = false;
        if (!dst_rule_for(e->tz, &wants_dst)) {
            fail("%s is in the picker but has no daylight-saving expectation "
                 "in kDstRules2026", e->tz);
            continue;
        }
        struct tm jan, jul;
        if (!local_at(e->tz, JW_TZ_JANUARY, &jan) ||
            !local_at(e->tz, JW_TZ_JULY, &jul)) {
            fail("%s: localtime_r failed", e->tz);
            continue;
        }
        char a[16], b[16], c[16];
        if (jan.tm_isdst > 0 && jul.tm_isdst > 0) {
            fail("%s: daylight saving in both January and July", e->tz);
            continue;
        }
        long standard = (jan.tm_isdst > 0) ? jul.tm_gmtoff : jan.tm_gmtoff;
        if (standard != want)
            fail("%s (%s): standard offset is %s, the picker prints %s",
                 e->tz, e->label, offset_text(standard, a, sizeof(a)), e->off);

        /* The rules check. A mismatch means this device's zoneinfo is too old
           to ship this row, which is the data gate the plan puts ahead of the
           menu change. */
        bool got_dst = (jan.tm_isdst > 0 || jul.tm_isdst > 0);
        if (got_dst != wants_dst)
            fail("%s (%s): this device's zoneinfo %s daylight saving in 2026, "
                 "current IANA rules %s. The rootfs data is too old for this row",
                 e->tz, e->label, got_dst ? "applies" : "does not apply",
                 wants_dst ? "do" : "do not");

        /* A region with no DST must not drift between the samples, and a row
           that changes offset without ever setting tm_isdst would print a lie
           for half the year. */
        if (!got_dst && jan.tm_gmtoff != jul.tm_gmtoff)
            fail("%s: offset changed between January (%s) and July (%s) without "
                 "daylight saving", e->tz,
                 offset_text(jan.tm_gmtoff, b, sizeof(b)),
                 offset_text(jul.tm_gmtoff, c, sizeof(c)));
    }
}

/* Exact wall-clock expectations, including both sides of each transition. The
   September 2026 and April 2027 instants are New Zealand's published change
   dates; one second before and one second after is what catches data that has
   the rule but the wrong date. */
static const struct {
    const char *tz;
    long        utc;
    const char *local;
    const char *offset;
    int         dst;
} kFixtures[] = {
    { "Pacific/Auckland", 1768435200L, "2026-01-15 13:00:00", "+13:00", 1 },
    { "Pacific/Auckland", 1784073600L, "2026-07-15 12:00:00", "+12:00", 0 },
    { "Pacific/Auckland", 1790431199L, "2026-09-27 01:59:59", "+12:00", 0 },
    { "Pacific/Auckland", 1790431200L, "2026-09-27 03:00:00", "+13:00", 1 },
    { "Pacific/Auckland", 1806760799L, "2027-04-04 02:59:59", "+13:00", 1 },
    { "Pacific/Auckland", 1806760800L, "2027-04-04 02:00:00", "+12:00", 0 },
    { "Pacific/Chatham",  1768435200L, "2026-01-15 13:45:00", "+13:45", 1 },
    { "Pacific/Chatham",  1784073600L, "2026-07-15 12:45:00", "+12:45", 0 },
    { "Pacific/Chatham",  1790431199L, "2026-09-27 02:44:59", "+12:45", 0 },
    { "Pacific/Chatham",  1790431200L, "2026-09-27 03:45:00", "+13:45", 1 },
    { "Pacific/Chatham",  1806760799L, "2027-04-04 03:44:59", "+13:45", 1 },
    { "Pacific/Chatham",  1806760800L, "2027-04-04 02:45:00", "+12:45", 0 },
    /* Newfoundland's daylight offset is a half hour too, not a whole one. */
    { "America/St_Johns", 1768435200L, "2026-01-14 20:30:00", "-03:30", 0 },
    { "America/St_Johns", 1784073600L, "2026-07-14 21:30:00", "-02:30", 1 },
    /* Lord Howe shifts by thirty minutes, the only zone that does. */
    { "Australia/Lord_Howe", 1768435200L, "2026-01-15 11:00:00", "+11:00", 1 },
    { "Australia/Lord_Howe", 1784073600L, "2026-07-15 10:30:00", "+10:30", 0 },
    { "Asia/Kathmandu",  1768435200L, "2026-01-15 05:45:00", "+05:45", 0 },
    { "Asia/Kathmandu",  1784073600L, "2026-07-15 05:45:00", "+05:45", 0 },
    { "Australia/Eucla", 1768435200L, "2026-01-15 08:45:00", "+08:45", 0 },
    { "Australia/Eucla", 1784073600L, "2026-07-15 08:45:00", "+08:45", 0 },
};

static void check_fixtures(void) {
    for (unsigned i = 0; i < sizeof(kFixtures) / sizeof(kFixtures[0]); ++i) {
        struct tm tm;
        if (!local_at(kFixtures[i].tz, (time_t)kFixtures[i].utc, &tm)) {
            fail("%s: localtime_r failed at %ld", kFixtures[i].tz, kFixtures[i].utc);
            continue;
        }
        char got[32], off[16];
        strftime(got, sizeof(got), "%Y-%m-%d %H:%M:%S", &tm);
        offset_text(tm.tm_gmtoff, off, sizeof(off));
        bool dst = tm.tm_isdst > 0;
        if (strcmp(got, kFixtures[i].local) != 0 ||
            strcmp(off, kFixtures[i].offset) != 0 ||
            dst != (kFixtures[i].dst != 0))
            fail("%s at %ld: got %s %s dst=%d, expected %s %s dst=%d",
                 kFixtures[i].tz, kFixtures[i].utc, got, off, (int)dst,
                 kFixtures[i].local, kFixtures[i].offset, kFixtures[i].dst);
    }
}

int main(int argc, char **argv) {
    bool verbose = (argc > 1 && strcmp(argv[1], "-v") == 0);

    check_unknown_zone_falls_back_to_utc();
    check_table();
    check_fixtures();

    if (verbose) {
        printf("%-22s %-22s %-9s %-9s %s\n", "LABEL", "TZ", "OFF", "JANUARY", "JULY");
        for (int i = 0; i < kJawakaTimeZoneCount; ++i) {
            const jw_timezone_entry *e = &kJawakaTimeZones[i];
            struct tm jan, jul;
            char a[16] = "?", b[16] = "?";
            if (local_at(e->tz, JW_TZ_JANUARY, &jan)) offset_text(jan.tm_gmtoff, a, sizeof(a));
            if (local_at(e->tz, JW_TZ_JULY, &jul))    offset_text(jul.tm_gmtoff, b, sizeof(b));
            printf("%-22s %-22s %-9s %-9s %s\n", e->label, e->tz, e->off, a, b);
        }
    }

    printf("timezone-probe: %d rows, %u fixtures, %d failures\n",
           kJawakaTimeZoneCount,
           (unsigned)(sizeof(kFixtures) / sizeof(kFixtures[0])), failures);
    if (failures) return 1;
    puts("PASS timezone-probe");
    return 0;
}
