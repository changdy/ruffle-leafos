#define _POSIX_C_SOURCE 200809L

#include "internal/platform/perf_policy.h"

#include <stdio.h>
#include <string.h>

static const char *profile_name(jw_platform_perf_profile profile) {
    switch (profile) {
        case JW_PLATFORM_PERF_PROFILE_PERFORMANCE: return "performance";
        case JW_PLATFORM_PERF_PROFILE_BALANCED:    return "balanced";
        case JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER: return "battery-saver";
        case JW_PLATFORM_PERF_PROFILE_SLEEP:       return "sleep";
        case JW_PLATFORM_PERF_PROFILE_FRONTEND:    return "frontend";
        case JW_PLATFORM_PERF_PROFILE_CUSTOM:      return "custom";
        case JW_PLATFORM_PERF_PROFILE_AUTO:
        default:                                   return "auto";
    }
}

/* The system-rule checks below run at 60 Hz, where only the system decides. */
static int expect_profile_at(const char *system, int refresh_hz,
                             jw_platform_perf_profile wanted) {
    jw_platform_perf_profile got =
        jw_platform_perf_auto_profile_for_system(system, refresh_hz);
    if (got == wanted) {
        return 0;
    }
    fprintf(stderr, "perf-policy-test: system=%s refresh=%d resolved to %s, expected %s\n",
            system ? system : "(null)", refresh_hz, profile_name(got),
            profile_name(wanted));
    return 1;
}

static int expect_profile(const char *system, jw_platform_perf_profile wanted) {
    return expect_profile_at(system, 60, wanted);
}

static int expect_game_profile_at(const char *system, int refresh_hz,
                                  jw_platform_perf_profile requested,
                                  jw_platform_perf_profile wanted) {
    jw_platform_perf_profile got =
        jw_platform_perf_game_profile(system, refresh_hz, requested);
    if (got == wanted) {
        return 0;
    }
    fprintf(stderr,
            "perf-policy-test: system=%s refresh=%d requested=%s resolved to %s, expected %s\n",
            system ? system : "(null)", refresh_hz, profile_name(requested),
            profile_name(got), profile_name(wanted));
    return 1;
}

static int expect_game_profile(const char *system,
                               jw_platform_perf_profile requested,
                               jw_platform_perf_profile wanted) {
    return expect_game_profile_at(system, 60, requested, wanted);
}

int main(void) {
    /* Every Dreamcast-family system must enter the performance profile: the
       standalone Flycast and each Flycast libretro choice all inherit it from
       the system, because the AUTO rule is not per-core. */
    static const char *const dreamcast_family[] = {
        "DC", "dc", "Dc",
        "DREAMCAST", "Dreamcast", "dreamcast",
        "NAOMI", "Naomi", "naomi",
        "ATOMISWAVE", "Atomiswave", "atomiswave",
    };
    for (size_t i = 0; i < sizeof(dreamcast_family) / sizeof(dreamcast_family[0]); i++) {
        if (expect_profile(dreamcast_family[i],
                           JW_PLATFORM_PERF_PROFILE_PERFORMANCE) != 0) {
            return 1;
        }
    }

    /* The other systems that already preferred performance keep it. */
    static const char *const heavy[] = { "N64", "n64", "PSP", "SATURN", "NDS" };
    for (size_t i = 0; i < sizeof(heavy) / sizeof(heavy[0]); i++) {
        if (expect_profile(heavy[i], JW_PLATFORM_PERF_PROFILE_PERFORMANCE) != 0) {
            return 1;
        }
    }

    /* Everything else stays balanced: this rule is a short list, not "games
       are fast". A missing or unknown system must not silently claim
       performance either. */
    static const char *const balanced[] = {
        "GBA", "gba", "SNES", "PSX", "NDS2X", "MEGA_DRIVE", "PICO8",
    };
    for (size_t i = 0; i < sizeof(balanced) / sizeof(balanced[0]); i++) {
        if (expect_profile(balanced[i], JW_PLATFORM_PERF_PROFILE_BALANCED) != 0) {
            return 1;
        }
    }
    if (expect_profile("", JW_PLATFORM_PERF_PROFILE_BALANCED) != 0 ||
        expect_profile(NULL, JW_PLATFORM_PERF_PROFILE_BALANCED) != 0) {
        return 1;
    }

    /* The Dreamcast family is a contract, not a default: a Balanced, Battery
       Saver or Custom request from the global, system, game or session setting
       still runs the game in performance. Every one of those settings resolves
       into this same call, so the matrix below is the whole override story. */
    const jw_platform_perf_profile game_requests[] = {
        JW_PLATFORM_PERF_PROFILE_AUTO,
        JW_PLATFORM_PERF_PROFILE_PERFORMANCE,
        JW_PLATFORM_PERF_PROFILE_BALANCED,
        JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER,
        JW_PLATFORM_PERF_PROFILE_CUSTOM,
    };
    for (size_t i = 0; i < sizeof(dreamcast_family) / sizeof(dreamcast_family[0]); i++) {
        for (size_t j = 0; j < sizeof(game_requests) / sizeof(game_requests[0]); j++) {
            if (expect_game_profile(dreamcast_family[i], game_requests[j],
                                    JW_PLATFORM_PERF_PROFILE_PERFORMANCE) != 0) {
                return 1;
            }
        }
        /* Frontend and sleep describe the idle device. They are applied with no
           system, and a Dreamcast game must not drag the idle device into
           performance after it exits. */
        if (expect_game_profile(dreamcast_family[i],
                                JW_PLATFORM_PERF_PROFILE_FRONTEND,
                                JW_PLATFORM_PERF_PROFILE_FRONTEND) != 0 ||
            expect_game_profile(dreamcast_family[i],
                                JW_PLATFORM_PERF_PROFILE_SLEEP,
                                JW_PLATFORM_PERF_PROFILE_SLEEP) != 0) {
            return 1;
        }
    }

    /* Every other system keeps the player's choice. */
    static const struct {
        const char *system;
        jw_platform_perf_profile requested;
        jw_platform_perf_profile wanted;
    } other_systems[] = {
        { "GBA",  JW_PLATFORM_PERF_PROFILE_AUTO,          JW_PLATFORM_PERF_PROFILE_BALANCED },
        { "GBA",  JW_PLATFORM_PERF_PROFILE_BALANCED,      JW_PLATFORM_PERF_PROFILE_BALANCED },
        { "GBA",  JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER, JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER },
        { "GBA",  JW_PLATFORM_PERF_PROFILE_CUSTOM,        JW_PLATFORM_PERF_PROFILE_CUSTOM },
        { "GBA",  JW_PLATFORM_PERF_PROFILE_PERFORMANCE,   JW_PLATFORM_PERF_PROFILE_PERFORMANCE },
        { "SNES", JW_PLATFORM_PERF_PROFILE_BALANCED,      JW_PLATFORM_PERF_PROFILE_BALANCED },
        { "N64",  JW_PLATFORM_PERF_PROFILE_AUTO,          JW_PLATFORM_PERF_PROFILE_PERFORMANCE },
        { "N64",  JW_PLATFORM_PERF_PROFILE_BALANCED,      JW_PLATFORM_PERF_PROFILE_BALANCED },
        { NULL,   JW_PLATFORM_PERF_PROFILE_BALANCED,      JW_PLATFORM_PERF_PROFILE_BALANCED },
        { NULL,   JW_PLATFORM_PERF_PROFILE_FRONTEND,      JW_PLATFORM_PERF_PROFILE_FRONTEND },
        { NULL,   JW_PLATFORM_PERF_PROFILE_SLEEP,         JW_PLATFORM_PERF_PROFILE_SLEEP },
        { NULL,   JW_PLATFORM_PERF_PROFILE_AUTO,          JW_PLATFORM_PERF_PROFILE_BALANCED },
        { "",     JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER, JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER },
    };
    for (size_t i = 0; i < sizeof(other_systems) / sizeof(other_systems[0]); i++) {
        if (expect_game_profile(other_systems[i].system,
                                other_systems[i].requested,
                                other_systems[i].wanted) != 0) {
            return 1;
        }
    }

    /* Above 60 Hz each frame gets one refresh of time instead of two, so AUTO
       boosts every game. 60 Hz and an unknown rate (-1, or 0 from a status
       that never read the mode) keep the per-system answer. */
    static const struct {
        const char *system;
        int refresh_hz;
        jw_platform_perf_profile wanted;
    } refresh_auto[] = {
        { "MD",   120, JW_PLATFORM_PERF_PROFILE_PERFORMANCE },
        { "GBA",  100, JW_PLATFORM_PERF_PROFILE_PERFORMANCE },
        { "SNES", 120, JW_PLATFORM_PERF_PROFILE_PERFORMANCE },
        { "N64",  120, JW_PLATFORM_PERF_PROFILE_PERFORMANCE },
        { "MD",    60, JW_PLATFORM_PERF_PROFILE_BALANCED },
        { "MD",    -1, JW_PLATFORM_PERF_PROFILE_BALANCED },
        { "MD",     0, JW_PLATFORM_PERF_PROFILE_BALANCED },
        { "",     120, JW_PLATFORM_PERF_PROFILE_BALANCED },
        { NULL,   120, JW_PLATFORM_PERF_PROFILE_BALANCED },
    };
    for (size_t i = 0; i < sizeof(refresh_auto) / sizeof(refresh_auto[0]); i++) {
        if (expect_profile_at(refresh_auto[i].system, refresh_auto[i].refresh_hz,
                              refresh_auto[i].wanted) != 0) {
            return 1;
        }
    }

    /* The refresh rule is a default, not a contract: an explicit choice wins
       at 120 Hz, and the idle-device profiles are never touched. */
    static const struct {
        const char *system;
        jw_platform_perf_profile requested;
        jw_platform_perf_profile wanted;
    } refresh_game[] = {
        { "MD",  JW_PLATFORM_PERF_PROFILE_AUTO,          JW_PLATFORM_PERF_PROFILE_PERFORMANCE },
        { "MD",  JW_PLATFORM_PERF_PROFILE_BALANCED,      JW_PLATFORM_PERF_PROFILE_BALANCED },
        { "MD",  JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER, JW_PLATFORM_PERF_PROFILE_BATTERY_SAVER },
        { "MD",  JW_PLATFORM_PERF_PROFILE_CUSTOM,        JW_PLATFORM_PERF_PROFILE_CUSTOM },
        { "DC",  JW_PLATFORM_PERF_PROFILE_BALANCED,      JW_PLATFORM_PERF_PROFILE_PERFORMANCE },
        { NULL,  JW_PLATFORM_PERF_PROFILE_FRONTEND,      JW_PLATFORM_PERF_PROFILE_FRONTEND },
        { NULL,  JW_PLATFORM_PERF_PROFILE_SLEEP,         JW_PLATFORM_PERF_PROFILE_SLEEP },
        { NULL,  JW_PLATFORM_PERF_PROFILE_AUTO,          JW_PLATFORM_PERF_PROFILE_BALANCED },
        { "MD",  JW_PLATFORM_PERF_PROFILE_FRONTEND,      JW_PLATFORM_PERF_PROFILE_FRONTEND },
    };
    for (size_t i = 0; i < sizeof(refresh_game) / sizeof(refresh_game[0]); i++) {
        if (expect_game_profile_at(refresh_game[i].system, 120,
                                   refresh_game[i].requested,
                                   refresh_game[i].wanted) != 0) {
            return 1;
        }
    }

    printf("perf-policy-test: ok\n");
    return 0;
}
