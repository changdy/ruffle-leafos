#include "internal/platform/perf_policy.h"

#include <strings.h>

/* The Dreamcast family: three catalog systems that share the Flycast cores.
   DREAMCAST is the ROM-folder spelling the compatibility scanner stores when
   metadata is unavailable, so it is accepted alongside the DC id. Comparisons
   are case-insensitive because a user's folder name is not normalized. */
static bool jw__perf_system_is_dreamcast_family(const char *system) {
    if (!system || !system[0]) {
        return false;
    }
    return strcasecmp(system, "DC") == 0 ||
           strcasecmp(system, "DREAMCAST") == 0 ||
           strcasecmp(system, "NAOMI") == 0 ||
           strcasecmp(system, "ATOMISWAVE") == 0;
}

/* Systems whose games want the full performance profile even though they are
   not Dreamcast-family. */
static bool jw__perf_system_is_heavy(const char *system) {
    return strcasecmp(system, "N64") == 0 ||
           strcasecmp(system, "PSP") == 0 ||
           strcasecmp(system, "SATURN") == 0 ||
           strcasecmp(system, "NDS") == 0;
}

jw_platform_perf_profile jw_platform_perf_auto_profile_for_system(const char *system,
                                                                  int refresh_hz) {
    if (!system || !system[0]) {
        return JW_PLATFORM_PERF_PROFILE_BALANCED;
    }
    if (jw__perf_system_is_dreamcast_family(system) ||
        jw__perf_system_is_heavy(system)) {
        return JW_PLATFORM_PERF_PROFILE_PERFORMANCE;
    }
    /* Above 60 Hz a 60 fps game is presented twice per frame (a black frame
       with BFI, a repeat without), and every presentation waits on the
       compositor, so the frame's emulation and drawing must fit in one refresh:
       about 8 ms at 120 Hz instead of 16. Under balanced the governors
       sometimes ramp too late, the frame misses its refresh, and it shows as a
       black or bright flash with BFI and as an audio underrun either way.
       Switching a flickering 120 Hz BFI session to performance removed most of
       it (MLP1, 2026-09-26). An unknown rate (-1) keeps balanced. */
    if (refresh_hz > 60) {
        return JW_PLATFORM_PERF_PROFILE_PERFORMANCE;
    }
    return JW_PLATFORM_PERF_PROFILE_BALANCED;
}

jw_platform_perf_profile jw_platform_perf_game_profile(const char *system,
                                                       int refresh_hz,
                                                       jw_platform_perf_profile requested) {
    /* The Dreamcast family overrides every game-mode request. The frontend and
       sleep profiles are device modes, not game modes, and are applied with no
       system; leaving them out of the force keeps the exit path able to leave
       performance. */
    if (jw__perf_system_is_dreamcast_family(system) &&
        requested != JW_PLATFORM_PERF_PROFILE_FRONTEND &&
        requested != JW_PLATFORM_PERF_PROFILE_SLEEP) {
        return JW_PLATFORM_PERF_PROFILE_PERFORMANCE;
    }
    if (requested == JW_PLATFORM_PERF_PROFILE_AUTO) {
        return jw_platform_perf_auto_profile_for_system(system, refresh_hz);
    }
    return requested;
}
