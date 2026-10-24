#ifndef JW_PLATFORM_INPUT_ROSTER_H
#define JW_PLATFORM_INPUT_ROSTER_H

/* Launch-time controller roster + private /dev/input namespace for emulator
   children (the contract in umrk-workspace/plans/paired-wireless-controllers-mlp1.md).

   The roster freezes the player slot order for one launch: up to three
   external gamepads in numeric eventN order, then Jawaka's calibrated virtual
   Loong controller (always last, never displaced). Identity is by exact event
   path plus st_rdev so the two identically-named "Loong Gamepad" devices are
   never confused. Children get a mount-namespace snapshot of /dev/input
   holding the roster plus the non-gamepad nodes, so the grabbed physical pad,
   excess controllers, and late-connect devices cannot be opened by the
   emulator at all.

   Everything here is fail-closed for real launches: when the roster backend is
   supported, a failed build or namespace setup must abort the launch before
   exec rather than fall back to direct physical input. Desktop (mock) builds
   report unsupported so host development keeps its direct-input behavior. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "internal/platform/input_proxy.h"

#define JW_INPUT_ROSTER_MAX_CONTROLLERS 4
#define JW_INPUT_ROSTER_MAX_EXTERNALS 3
#define JW_INPUT_ROSTER_MAX_IGNORED 4
#define JW_INPUT_ROSTER_MAX_PASSTHROUGH 16
#define JW_INPUT_ROSTER_PATH_MAX 256
#define JW_INPUT_ROSTER_NAME_MAX 128

#define JW_INPUT_ROSTER_STATE_DIR "/run/jawaka"

/* Stable error codes for launch diagnostics (log-searchable). */
#define JW_INPUT_ROSTER_ERR_UNSUPPORTED "ERR_ROSTER_UNSUPPORTED"
#define JW_INPUT_ROSTER_ERR_PROXY "ERR_ROSTER_PROXY_INACTIVE"
#define JW_INPUT_ROSTER_ERR_WATCH "ERR_ROSTER_WATCH_ONLY"
#define JW_INPUT_ROSTER_ERR_PATHS "ERR_ROSTER_DEVICE_PATHS"
#define JW_INPUT_ROSTER_ERR_SCAN "ERR_ROSTER_SCAN"
#define JW_INPUT_ROSTER_ERR_PREPARE "ERR_INPUT_NAMESPACE_PREPARE"
#define JW_INPUT_ROSTER_ERR_NAMESPACE "ERR_INPUT_NAMESPACE"

typedef struct {
    char path[JW_INPUT_ROSTER_PATH_MAX];
    dev_t rdev;
    bool is_virtual; /* calibrated virtual Loong (always the last roster entry) */
    char name[JW_INPUT_ROSTER_NAME_MAX];
    uint16_t vendor;
    uint16_t product;
} jw_input_roster_entry;

typedef struct {
    /* SDL/player-slot order: externals first, virtual last. */
    jw_input_roster_entry controllers[JW_INPUT_ROSTER_MAX_CONTROLLERS];
    int count;          /* 1..4 populated entries */
    int external_count; /* count - 1 */

    /* The grabbed physical Loong device — kept for diagnostics, never
       exposed to the child. */
    char physical_path[JW_INPUT_ROSTER_PATH_MAX];
    dev_t physical_rdev;

    /* Externals dropped by the three-external limit, for the launch log. */
    char ignored[JW_INPUT_ROSTER_MAX_IGNORED][JW_INPUT_ROSTER_PATH_MAX];
    int ignored_count;

    /* Non-gamepad /dev/input nodes (power keys, CEC, headphone jack) carried
       into the child view unchanged. Hiding them buys nothing — a device with
       no gamepad bits cannot become a phantom player — while their absence
       breaks emulators that expect a normal /dev/input. Frozen with the
       roster, so late-connected devices still cannot appear. */
    char passthrough[JW_INPUT_ROSTER_MAX_PASSTHROUGH][JW_INPUT_ROSTER_PATH_MAX];
    int passthrough_count;
} jw_input_roster;

/* True on platforms where roster + namespace isolation is implemented and
   launches must go through it (MLP1). False on desktop/mock builds. */
bool jw_input_roster_supported(void);

/* True when the event node exposes gamepad capability bits (gamepad button
   block or d-pad buttons). Shared by the roster scan and the external-input
   monitor. */
bool jw_input_device_is_gamepad(const char *path);

/* Freeze the roster for a launch. Requires the full grab-and-forward input
   proxy (a watch-only proxy or missing/duplicated device paths fail closed).
   Returns 0 and fills roster on success, -1 with a stable error code + message
   in error on failure. */
int jw_input_roster_build(const jw_input_proxy *proxy, jw_input_roster *roster,
                          char *error, size_t error_size);

/* Player index of the calibrated virtual controller in the roster. */
static inline int jw_input_roster_virtual_index(const jw_input_roster *roster) {
    return roster ? roster->external_count : 0;
}

/* Colon-separated SDL_JOYSTICK_DEVICE value in roster order. out always ends
   NUL; returns the needed length excluding NUL. */
size_t jw_input_roster_sdl_devices(const jw_input_roster *roster,
                                   char *out, size_t out_size);

/* One bounded launch-log block: physical (excluded), virtual (mandatory),
   P1-P4 members, ignored externals. */
void jw_input_roster_log(const jw_input_roster *roster, const char *tag);

/* Parent side, immediately after fork: create
   /run/jawaka/input-<child-pid> with one empty placeholder per exposed node
   (roster members and passthrough nodes, under their original eventN
   basename) for the child to bind the real nodes onto, plus a by-path
   directory holding only the links that resolve to an exposed node — the
   stock by-path carries a link straight to the physical Loong.
   Fills dir_out; on failure returns -1 after removing what it created. */
int jw_input_namespace_prepare(const jw_input_roster *roster, pid_t child_pid,
                               char *dir_out, size_t dir_out_size);

/* Child side, between fork and exec: unshare a private mount namespace and
   bind-mount the roster's /dev/input view built by _prepare. Any failure is a
   launch-fatal error (-1 + error); the caller must _exit, never exec. */
int jw_input_namespace_enter(const char *dir, const jw_input_roster *roster,
                             char *error, size_t error_size);

/* Parent side, after the child exits: remove the placeholder files and the
   per-launch directory. */
void jw_input_namespace_cleanup_dir(const char *dir);

/* jawakad startup: remove stale empty input-* directories left behind by an
   unclean shutdown of a previous jawakad. */
void jw_input_namespace_startup_sweep(void);

#endif /* JW_PLATFORM_INPUT_ROSTER_H */
