#include "internal/settings/timezones.h"

#include <string.h>

/* Ordered by UTC standard-time offset, the convention OS time-zone pickers use.
   `off` is the standard offset, so a region that observes daylight saving reads
   one hour (or, for Lord Howe, thirty minutes) higher for part of the year; the
   picker subheader says so. Do not reorder seasonally.

   Every row names a place, not a raw offset. A place carries its daylight-saving
   rules with it: pick New Zealand once and you get UTC+12 in winter and UTC+13
   in summer, with no second trip to Settings when the clocks change. A bare
   "UTC+12" choice cannot do that, which is why there is no longer a parallel set
   of fixed-offset rows -- they duplicated the offset column and quietly opted the
   user out of DST.

   Every whole hour from UTC-12 through UTC+14 is covered, plus the fractional
   offsets. Where one offset has regions that differ in whether they observe DST,
   both are listed (US Mountain and US Arizona, Adelaide and Darwin): that
   difference is the whole reason to pick one over the other.

   Baker Island is the exception that proves the rule. UTC-12 has no inhabited
   territory and so no IANA place id; Etc/GMT+12 is all there is. Note that the
   Etc ids carry the opposite sign to the offset they produce, which is how the
   IANA etcetera file defines them, and why timezone-test checks this row's sign
   rather than trusting it.

   UTC+3:30 is deliberately absent. Iran is the only region at that offset, and
   the MLP1 stock rootfs ships tzdata from around 2021, which still applies the
   daylight saving Iran abolished in tzdb 2022a. Listing it would have handed an
   Iranian user a clock that is an hour wrong for half the year, so the row is
   out until the data under it is current. Do not add Asia/Tehran back without
   running `make mlp1-device-timezone-test` against the firmware you intend to
   ship: the probe fails on exactly this, and that failure is why the row is
   missing rather than merely forgotten.

   Selection is persisted as the `tz` string, not the row index, so adding or
   moving rows needs no database migration.

   ASCII labels only: the launcher font subset has no extended-Latin glyphs. */
const jw_timezone_entry kJawakaTimeZones[] = {
    { "Baker Island",        "Etc/GMT+12",          "UTC-12"    },
    { "American Samoa",      "Pacific/Pago_Pago",   "UTC-11"    },
    { "US Hawaii",           "Pacific/Honolulu",    "UTC-10"    },
    { "Marquesas Islands",   "Pacific/Marquesas",   "UTC-9:30"  },
    { "US Alaska",           "America/Anchorage",   "UTC-9"     },
    { "US Pacific",          "America/Los_Angeles", "UTC-8"     },
    { "US Mountain",         "America/Denver",      "UTC-7"     },
    { "US Arizona",          "America/Phoenix",     "UTC-7"     },
    { "US Central",          "America/Chicago",     "UTC-6"     },
    { "US Eastern",          "America/New_York",    "UTC-5"     },
    { "Atlantic Canada",     "America/Halifax",     "UTC-4"     },
    { "Newfoundland",        "America/St_Johns",    "UTC-3:30"  },
    { "Brazil (East)",       "America/Sao_Paulo",   "UTC-3"     },
    { "Fernando de Noronha", "America/Noronha",     "UTC-2"     },
    { "Cape Verde",          "Atlantic/Cape_Verde", "UTC-1"     },
    { "UTC",                 "UTC",                 "UTC"       },
    { "UK / Ireland",        "Europe/London",       "UTC+0"     },
    { "Central Europe",      "Europe/Paris",        "UTC+1"     },
    { "Eastern Europe",      "Europe/Athens",       "UTC+2"     },
    { "Moscow",              "Europe/Moscow",       "UTC+3"     },
    { "Dubai",               "Asia/Dubai",          "UTC+4"     },
    { "Afghanistan",         "Asia/Kabul",          "UTC+4:30"  },
    { "Pakistan",            "Asia/Karachi",        "UTC+5"     },
    { "India",               "Asia/Kolkata",        "UTC+5:30"  },
    { "Nepal",               "Asia/Kathmandu",      "UTC+5:45"  },
    { "Bangladesh",          "Asia/Dhaka",          "UTC+6"     },
    { "Myanmar",             "Asia/Yangon",         "UTC+6:30"  },
    { "Bangkok",             "Asia/Bangkok",        "UTC+7"     },
    { "China",               "Asia/Shanghai",       "UTC+8"     },
    { "Eucla",               "Australia/Eucla",     "UTC+8:45"  },
    { "Japan / Korea",       "Asia/Tokyo",          "UTC+9"     },
    { "Adelaide",            "Australia/Adelaide",  "UTC+9:30"  },
    { "Darwin",              "Australia/Darwin",    "UTC+9:30"  },
    { "Sydney",              "Australia/Sydney",    "UTC+10"    },
    { "Lord Howe Island",    "Australia/Lord_Howe", "UTC+10:30" },
    { "Solomon Islands",     "Pacific/Guadalcanal", "UTC+11"    },
    { "New Zealand",         "Pacific/Auckland",    "UTC+12"    },
    { "Chatham Islands",     "Pacific/Chatham",     "UTC+12:45" },
    { "Tonga",               "Pacific/Tongatapu",   "UTC+13"    },
    { "Line Islands",        "Pacific/Kiritimati",  "UTC+14"    },
};

const int kJawakaTimeZoneCount =
    (int)(sizeof(kJawakaTimeZones) / sizeof(kJawakaTimeZones[0]));

int jw_timezone_utc_index(void) {
    for (int i = 0; i < kJawakaTimeZoneCount; ++i)
        if (strcmp(kJawakaTimeZones[i].tz, "UTC") == 0) return i;
    return 0;
}

const char *jw_timezone_label(const char *tz) {
    if (!tz || !tz[0]) return "System default";
    for (int i = 0; i < kJawakaTimeZoneCount; ++i)
        if (strcmp(kJawakaTimeZones[i].tz, tz) == 0) return kJawakaTimeZones[i].label;
    return tz;   /* unknown id: show the raw zone */
}

int jw_timezone_index_of(const char *tz) {
    if (tz && tz[0])
        for (int i = 0; i < kJawakaTimeZoneCount; ++i)
            if (strcmp(kJawakaTimeZones[i].tz, tz) == 0) return i;
    return jw_timezone_utc_index();
}
