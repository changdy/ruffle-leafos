#include "internal/launcher/standalone_policy.h"

#include <string.h>
#include <strings.h>

static bool jw__string_equals(const char *value, const char *expected) {
    return value && expected && strcmp(value, expected) == 0;
}

static bool jw__path_contains(const char *path, const char *component) {
    return path && component && strstr(path, component) != NULL;
}

/* Release identity predicates. Private: only jw_standalone_policy_resolve()
   may call them, after it has ruled out a provider-bound core. */

static bool jw__is_mupen64plus(const char *core_id, const char *launcher_path) {
    return jw__string_equals(core_id, "mupen64plus_standalone") ||
           jw__string_equals(core_id, "mupen64plus") ||
           jw__path_contains(launcher_path, "/mupen64plus/") ||
           jw__path_contains(launcher_path, "/Mupen64Plus");
}

static bool jw__is_flycast(const char *core_id, const char *launcher_path) {
    return jw__string_equals(core_id, "flycast_standalone") ||
           jw__string_equals(core_id, "flycast") ||
           jw__path_contains(launcher_path, "/flycast/") ||
           jw__path_contains(launcher_path, "/Flycast/");
}

static bool jw__is_ppsspp(const char *core_id, const char *launcher_path) {
    return jw__string_equals(core_id, "ppsspp") ||
           jw__string_equals(core_id, "ppsspp_gles") ||
           jw__path_contains(launcher_path, "/ppsspp/") ||
           jw__path_contains(launcher_path, "/PPSSPP");
}

static bool jw__is_drastic(const char *core_id, const char *launcher_path) {
    return jw__string_equals(core_id, "drastic") ||
           jw__path_contains(launcher_path, "/drastic/") ||
           jw__path_contains(launcher_path, "/DraStic");
}

static bool jw__is_yabasanshiro(const char *core_id, const char *launcher_path) {
    /* The bare yabasanshiro id belongs to the RetroArch core. */
    return jw__string_equals(core_id, "yabasanshiro_standalone") ||
           jw__path_contains(launcher_path, "/emulators/yabasanshiro/");
}

static bool jw__is_fun_drastic(const char *core_id, const char *launcher_path) {
    /* Match the exact id and the exact deployment directory. The DraStic
       predicate looks for "/drastic/", which does not match "/fun-drastic/",
       so neither one catches the other. */
    return jw__string_equals(core_id, "fun_drastic") ||
           jw__path_contains(launcher_path, "/emulators/fun-drastic/");
}

static bool jw__is_ports(const char *core_id, const char *launcher_path) {
    return jw__string_equals(core_id, "ports") ||
           jw__path_contains(launcher_path, "/emulators/ports/") ||
           jw__path_contains(launcher_path, "/Roms/PORTS");
}

jw_standalone_policy jw_standalone_policy_resolve(const char *core_id,
                                                  const char *launcher_path,
                                                  const char *provider) {
    jw_standalone_policy policy = { 0 };
    if (provider && provider[0]) {
        policy.provider_bound = true;
        return policy;
    }

    /* Same order the daemon's Menu routing checked these in before the
       identity was resolved once. */
    if (jw__is_ppsspp(core_id, launcher_path)) {
        policy.release = JW_STANDALONE_RELEASE_PPSSPP;
    } else if (jw__is_drastic(core_id, launcher_path)) {
        policy.release = JW_STANDALONE_RELEASE_DRASTIC;
    } else if (jw__is_fun_drastic(core_id, launcher_path)) {
        policy.release = JW_STANDALONE_RELEASE_FUN_DRASTIC;
    } else if (jw__is_mupen64plus(core_id, launcher_path)) {
        policy.release = JW_STANDALONE_RELEASE_MUPEN64PLUS;
    } else if (jw__is_flycast(core_id, launcher_path)) {
        policy.release = JW_STANDALONE_RELEASE_FLYCAST;
    } else if (jw__is_yabasanshiro(core_id, launcher_path)) {
        policy.release = JW_STANDALONE_RELEASE_YABASANSHIRO;
    } else if (jw__is_ports(core_id, launcher_path)) {
        policy.release = JW_STANDALONE_RELEASE_PORTS;
    }
    return policy;
}

static bool jw__path_extension_is(const char *path, const char *extension) {
    if (!path || !extension) {
        return false;
    }
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    const char *dot = strrchr(base, '.');
    return dot && strcasecmp(dot, extension) == 0;
}

bool jw_standalone_policy_supports_content(const jw_standalone_policy *policy,
                                           const char *content_path) {
    if (!policy || policy->release != JW_STANDALONE_RELEASE_YABASANSHIRO) {
        return true;
    }
    return !jw__path_extension_is(content_path, ".zip") &&
           !jw__path_extension_is(content_path, ".m3u");
}

bool jw_standalone_policy_requires_direct_drm(
        const jw_standalone_policy *policy,
        bool metadata_requires_direct_drm) {
    return metadata_requires_direct_drm ||
           (policy && policy->release == JW_STANDALONE_RELEASE_FLYCAST);
}

bool jw_standalone_policy_uses_calibrated_virtual_input(
        const jw_standalone_policy *policy) {
    return policy && (policy->provider_bound ||
                      policy->release != JW_STANDALONE_RELEASE_NONE);
}

jw_standalone_menu_route jw_standalone_policy_menu(
        const jw_standalone_policy *policy, bool supports_menu, bool external) {
    if (!policy || !policy->provider_bound) return JW_STANDALONE_MENU_RELEASE;
    if (!supports_menu) return JW_STANDALONE_MENU_CONFIRM_QUIT;
    return external ? JW_STANDALONE_MENU_EXTERNAL_HANDLED
                    : JW_STANDALONE_MENU_FORWARD;
}
