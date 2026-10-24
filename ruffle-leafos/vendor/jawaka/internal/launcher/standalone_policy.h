#ifndef JW_STANDALONE_POLICY_H
#define JW_STANDALONE_POLICY_H

#include <stdbool.h>

/* The release-owned standalone a core is recognized as. Recognition uses the
   core id and executable path, so it is only meaningful for release cores. */
typedef enum {
    JW_STANDALONE_RELEASE_NONE = 0,
    JW_STANDALONE_RELEASE_PPSSPP,
    /* DraStic is one of the supported roster standalones: it runs under the
       full calibrated proxy (paired wireless controllers plan) even though its
       bundled SDL backend reads a raw event path instead of standard joystick
       enums. */
    JW_STANDALONE_RELEASE_DRASTIC,
    /* Fun DraStic is a second Nintendo DS standalone: tenlevels' frontend over
       the same closed-source drastic64 binary the DraStic package ships. It is
       its own identity rather than a wider DraStic match, the way YabaSanshiro
       is: the ids collide on substring, every session would otherwise log the
       wrong emulator's name, and the two packages must be able to diverge on
       policy. */
    JW_STANDALONE_RELEASE_FUN_DRASTIC,
    JW_STANDALONE_RELEASE_MUPEN64PLUS,
    JW_STANDALONE_RELEASE_FLYCAST,
    JW_STANDALONE_RELEASE_YABASANSHIRO,
    JW_STANDALONE_RELEASE_PORTS,
} jw_standalone_release;

/* Resolved once per launch and carried by the launch target and the running
   session. Consumers read this instead of matching ids or paths again. */
typedef struct {
    /* The catalog core has a provider: a content pak contributed it. */
    bool provider_bound;
    /* Always JW_STANDALONE_RELEASE_NONE when provider_bound. */
    jw_standalone_release release;
} jw_standalone_policy;

/* The single gate between catalog identity and release emulator behavior. A
   provider-bound core's id and install path are chosen by its author, so they
   are never compared with release names: it gets generic policy whatever it is
   called or wherever it lives. `provider` is the catalog core's provider; NULL
   or empty means a release-owned core. Native PICO-8 is not decided here; it
   keeps its explicit jw_pico8_core_matches() identity. */
jw_standalone_policy jw_standalone_policy_resolve(const char *core_id,
                                                  const char *launcher_path,
                                                  const char *provider);

/* False only for content formats proven unsupported by a recognized release
   standalone. Other cores and untested formats remain eligible. */
bool jw_standalone_policy_supports_content(const jw_standalone_policy *policy,
                                           const char *content_path);

/* `metadata_requires_direct_drm` is the merged catalog's own flag and is
   honored for every core; release identity can only add to it. */
bool jw_standalone_policy_requires_direct_drm(const jw_standalone_policy *policy,
                                              bool metadata_requires_direct_drm);
bool jw_standalone_policy_uses_calibrated_virtual_input(
    const jw_standalone_policy *policy);

typedef enum {
    JW_STANDALONE_MENU_RELEASE = 0,
    JW_STANDALONE_MENU_FORWARD,
    /* The core has no menu of its own, so Menu ends it. Quitting loses
       anything unsaved, so the daemon asks "Return to Leaf?" first. */
    JW_STANDALONE_MENU_CONFIRM_QUIT,
    JW_STANDALONE_MENU_EXTERNAL_HANDLED,
} jw_standalone_menu_route;

/* Native PICO-8 and LIFE-1 run before this provider gate. External "handled"
   means no synthetic tap; it cannot consume the ungrabbed physical event. */
jw_standalone_menu_route jw_standalone_policy_menu(
    const jw_standalone_policy *policy, bool supports_menu, bool external);

#endif
