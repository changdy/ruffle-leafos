#ifndef JW_PLATFORM_PERF_POLICY_H
#define JW_PLATFORM_PERF_POLICY_H

#include "internal/platform/device.h"

/* The AUTO game profile for a system: which profile the device picks when
 * nothing has overridden it.
 *
 * `system` is the launch's system id (or, when metadata is unavailable, the
 * ROM folder name the compatibility scanner stored). NULL or empty means no
 * preference, which resolves to the balanced profile.
 *
 * `refresh_hz` is the live display rate, -1 when unknown. Above 60 Hz every
 * game resolves to performance: each frame gets one refresh of time instead of
 * two, and balanced's governors do not always ramp in time. */
jw_platform_perf_profile jw_platform_perf_auto_profile_for_system(const char *system,
                                                                  int refresh_hz);

/* The profile a game actually runs with, once the session, game, system and
 * global settings have all had their say. `requested` is their resolved answer,
 * AUTO when none of them chose one.
 *
 * The rule is keyed on the system rather than the core, because every emulator
 * choice a system offers runs the same content on the same hardware.
 *
 * Dreamcast-family games always run in performance. DC, NAOMI and ATOMISWAVE
 * each offer the standalone Flycast plus the Flycast libretro choices, and the
 * device is expected to hold full speed for all of them; that is a contract
 * for the Dreamcast family, not a default the player can override per session,
 * game, system or global setting. The forced value therefore wins over an
 * explicit request for another profile.
 *
 * Every other system keeps the player's choice: a Balanced, Battery Saver,
 * Performance or Custom override is honored, and AUTO falls back to
 * jw_platform_perf_auto_profile_for_system(), which also applies the refresh
 * rule. The refresh rule is a default, not a contract: it never overrides an
 * explicit choice.
 *
 * The frontend and sleep profiles describe the idle device, not a running
 * game, and the daemon applies them with no system at all. A NULL or empty
 * system therefore keeps whatever was requested, so a Dreamcast game cannot
 * drag the idle device into performance after it exits. */
jw_platform_perf_profile jw_platform_perf_game_profile(const char *system,
                                                       int refresh_hz,
                                                       jw_platform_perf_profile requested);

#endif
