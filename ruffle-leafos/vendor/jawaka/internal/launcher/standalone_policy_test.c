#include "internal/launcher/standalone_policy.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>

static void expect(const char *label, bool actual, bool expected) {
    if (actual == expected) {
        return;
    }
    fprintf(stderr, "%s: got %s, expected %s\n", label,
            actual ? "true" : "false", expected ? "true" : "false");
    exit(1);
}

static jw_standalone_policy release(const char *core_id, const char *path) {
    return jw_standalone_policy_resolve(core_id, path, NULL);
}

static bool is(const char *core_id, const char *path,
               jw_standalone_release expected) {
    return release(core_id, path).release == expected;
}

static bool direct_drm(const char *core_id, const char *path, bool metadata) {
    jw_standalone_policy policy = release(core_id, path);
    return jw_standalone_policy_requires_direct_drm(&policy, metadata);
}

static bool calibrated(const char *core_id, const char *path) {
    jw_standalone_policy policy = release(core_id, path);
    return jw_standalone_policy_uses_calibrated_virtual_input(&policy);
}

static bool content(const char *core_id, const char *path, const char *rom) {
    jw_standalone_policy policy = release(core_id, path);
    return jw_standalone_policy_supports_content(&policy, rom);
}

/* A content-pak core that copies a release emulator's id and install path
   must resolve to generic policy for every consumer. */
static void expect_provider_bound_generic(const char *label,
                                          const char *core_id,
                                          const char *path) {
    jw_standalone_policy policy =
        jw_standalone_policy_resolve(core_id, path, "mlp1/Spoof.pak");
    char full[256];

    snprintf(full, sizeof(full), "%s: provider bound", label);
    expect(full, policy.provider_bound, true);
    snprintf(full, sizeof(full), "%s: no release identity", label);
    expect(full, policy.release == JW_STANDALONE_RELEASE_NONE, true);
    snprintf(full, sizeof(full), "%s: no direct DRM by name", label);
    expect(full, jw_standalone_policy_requires_direct_drm(&policy, false), false);
    snprintf(full, sizeof(full), "%s: calibrated input by provider", label);
    expect(full, jw_standalone_policy_uses_calibrated_virtual_input(&policy),
           true);
    snprintf(full, sizeof(full), "%s: ZIP not rejected by name", label);
    expect(full, jw_standalone_policy_supports_content(
                     &policy, "/sd/Roms/SATURN/Rampage.zip"),
           true);
    snprintf(full, sizeof(full), "%s: M3U not rejected by name", label);
    expect(full, jw_standalone_policy_supports_content(
                     &policy, "/sd/Roms/SATURN/Enemy Zero.M3U"),
           true);
}

int main(void) {
    const char *flycast_path =
        "/sd/.system/leaf/platforms/mlp1/emulators/flycast/launch.sh";

    expect("Flycast identity",
           is("flycast_standalone", flycast_path, JW_STANDALONE_RELEASE_FLYCAST),
           true);
    expect("Flycast direct DRM by identity",
           direct_drm("flycast_standalone", flycast_path, false), true);
    expect("Flycast calibrated input",
           calibrated("flycast_standalone", flycast_path), true);
    expect("Flycast path compatibility",
           is("legacy_path_core", flycast_path, JW_STANDALONE_RELEASE_FLYCAST),
           true);

    expect("metadata direct DRM",
           direct_drm("ppsspp", "/sd/emulators/ppsspp/launch.sh", true), true);
    expect("PPSSPP identity",
           is("ppsspp", "/sd/emulators/ppsspp/launch.sh",
              JW_STANDALONE_RELEASE_PPSSPP),
           true);
    expect("PPSSPP calibrated input",
           calibrated("ppsspp", "/sd/emulators/ppsspp/launch.sh"), true);
    expect("PPSSPP GLES calibrated input by path",
           calibrated("ppsspp_gles", "/sd/emulators/ppsspp/launch-gles.sh"),
           true);

    expect("Mupen identity",
           is("mupen64plus_standalone", "/sd/emulators/mupen64plus/launch.sh",
              JW_STANDALONE_RELEASE_MUPEN64PLUS),
           true);
    expect("Mupen identity by legacy id",
           is("mupen64plus", NULL, JW_STANDALONE_RELEASE_MUPEN64PLUS), true);
    expect("Mupen calibrated input",
           calibrated("mupen64plus_standalone",
                      "/sd/emulators/mupen64plus/launch.sh"),
           true);
    expect("Mupen no implicit direct DRM",
           direct_drm("mupen64plus_standalone",
                      "/sd/emulators/mupen64plus/launch.sh", false),
           false);

    expect("Ports identity",
           is("ports", "/sd/emulators/ports/launch.sh",
              JW_STANDALONE_RELEASE_PORTS),
           true);
    expect("Ports calibrated input",
           calibrated("ports", "/sd/emulators/ports/launch.sh"), true);
    expect("Ports no blanket direct DRM",
           direct_drm("ports", "/sd/emulators/ports/launch.sh", false), false);

    expect("DraStic identity",
           is("drastic", "/sd/emulators/drastic/launch.sh",
              JW_STANDALONE_RELEASE_DRASTIC),
           true);
    expect("DraStic no implicit direct DRM",
           direct_drm("drastic", "/sd/emulators/drastic/launch.sh", false),
           false);
    expect("DraStic calibrated input (paired wireless controllers)",
           calibrated("drastic", "/sd/emulators/drastic/launch.sh"), true);
    expect("DraStic calibrated input by path only",
           calibrated("unrelated_core", "/sd/emulators/DraStic/launch.sh"),
           true);

    /* Fun DraStic is a second NDS standalone sharing the DraStic binary. The
       two must not recognize each other: a collision would make every Fun
       DraStic session log the wrong emulator and would entangle any future
       policy divergence between the packages. */
    expect("Fun DraStic identity",
           is("fun_drastic", "/sd/emulators/fun-drastic/launch.sh",
              JW_STANDALONE_RELEASE_FUN_DRASTIC),
           true);
    expect("Fun DraStic path compatibility",
           is("legacy_path_core",
              "/sd/.system/leaf/platforms/mlp1/emulators/fun-drastic/launch.sh",
              JW_STANDALONE_RELEASE_FUN_DRASTIC),
           true);
    expect("Fun DraStic exact identity",
           is("fun_drastic_preview", NULL, JW_STANDALONE_RELEASE_NONE), true);
    expect("Fun DraStic exact package path",
           is("legacy_path_core", "/sd/emulators/fun-drastic-preview/launch.sh",
              JW_STANDALONE_RELEASE_NONE),
           true);
    expect("DraStic does not answer for Fun DraStic",
           is("fun_drastic", "/sd/emulators/fun-drastic/launch.sh",
              JW_STANDALONE_RELEASE_DRASTIC),
           false);
    expect("Fun DraStic does not answer for DraStic",
           is("drastic", "/sd/emulators/drastic/launch.sh",
              JW_STANDALONE_RELEASE_FUN_DRASTIC),
           false);
    expect("Fun DraStic calibrated input (paired wireless controllers)",
           calibrated("fun_drastic", "/sd/emulators/fun-drastic/launch.sh"),
           true);
    expect("Fun DraStic no implicit direct DRM",
           direct_drm("fun_drastic", "/sd/emulators/fun-drastic/launch.sh",
                      false),
           false);
    /* drastic64 is byte-identical in both packages, so content support cannot
       diverge; archive handling goes through the hook's system() interposition
       and the unzip_roms key, not through a Jawaka restriction. */
    expect("Fun DraStic ZIP supported",
           content("fun_drastic", "/sd/emulators/fun-drastic/launch.sh",
                   "/sd/Roms/NDS/Game.zip"),
           true);
    expect("Fun DraStic 7z supported",
           content("fun_drastic", "/sd/emulators/fun-drastic/launch.sh",
                   "/sd/Roms/NDS/Game.7z"),
           true);

    expect("YabaSanshiro standalone identity",
           is("yabasanshiro_standalone", NULL,
              JW_STANDALONE_RELEASE_YABASANSHIRO),
           true);
    expect("YabaSanshiro standalone path compatibility",
           is("legacy_path_core",
              "/sd/.system/leaf/platforms/mlp1/emulators/yabasanshiro/launch.sh",
              JW_STANDALONE_RELEASE_YABASANSHIRO),
           true);
    expect("YabaSanshiro exact standalone identity",
           is("yabasanshiro_standalone_preview", NULL,
              JW_STANDALONE_RELEASE_NONE),
           true);
    expect("YabaSanshiro exact package path",
           is("legacy_path_core", "/sd/emulators/yabasanshiro-preview/launch.sh",
              JW_STANDALONE_RELEASE_NONE),
           true);
    expect("RetroArch YabaSanshiro is not standalone",
           is("yabasanshiro", "/sd/cores/yabasanshiro_libretro.so",
              JW_STANDALONE_RELEASE_NONE),
           true);
    expect("RetroArch YabaSanshiro keeps normal input",
           calibrated("yabasanshiro", "/sd/cores/yabasanshiro_libretro.so"),
           false);
    expect("YabaSanshiro standalone calibrated input",
           calibrated("yabasanshiro_standalone", NULL), true);
    expect("YabaSanshiro standalone path calibrated input",
           calibrated("legacy_path_core", "/sd/emulators/yabasanshiro/launch.sh"),
           true);
    expect("YabaSanshiro direct DRM remains metadata-driven",
           direct_drm("yabasanshiro_standalone",
                      "/sd/emulators/yabasanshiro/launch.sh", false),
           false);
    expect("YabaSanshiro CHD supported",
           content("yabasanshiro_standalone", NULL,
                   "/sd/Roms/SATURN/Shining Force III.chd"),
           true);
    expect("YabaSanshiro ZIP rejected",
           content("yabasanshiro_standalone", NULL,
                   "/sd/Roms/SATURN/Rampage.ZIP"),
           false);
    expect("YabaSanshiro M3U rejected by package path",
           content("legacy_path_core", "/sd/emulators/yabasanshiro/launch.sh",
                   "Roms/SATURN/Enemy Zero.m3u"),
           false);
    expect("RetroArch YabaSanshiro keeps ZIP support",
           content("yabasanshiro", "/sd/cores/yabasanshiro_libretro.so",
                   "/sd/Roms/SATURN/Rampage.zip"),
           true);
    expect("Other standalone content unaffected",
           content("flycast_standalone", "/sd/emulators/flycast/launch.sh",
                   "/sd/Roms/DC/Game.zip"),
           true);
    expect("missing identity calibrated input", calibrated(NULL, NULL), false);
    expect("missing identity is not provider bound",
           release(NULL, NULL).provider_bound, false);
    expect("empty provider is release-owned",
           jw_standalone_policy_resolve("drastic", NULL, "").release ==
               JW_STANDALONE_RELEASE_DRASTIC,
           true);
    expect("NULL policy is generic",
           jw_standalone_policy_uses_calibrated_virtual_input(NULL) ||
               jw_standalone_policy_requires_direct_drm(NULL, false) ||
               !jw_standalone_policy_supports_content(NULL, "Game.zip"),
           false);

    /* Provider-bound (content-pak) cores: the id and the install path each
       copy a release emulator's, alone and together. None may inherit that
       emulator's route. */
    static const struct {
        const char *label;
        const char *core_id;
        const char *path_component;
    } spoofs[] = {
        { "PPSSPP", "ppsspp", "emulators/ppsspp" },
        { "PPSSPP GLES", "ppsspp_gles", "emulators/PPSSPP" },
        { "DraStic", "drastic", "emulators/drastic" },
        { "Fun DraStic", "fun_drastic", "emulators/fun-drastic" },
        { "Mupen64Plus", "mupen64plus_standalone", "emulators/mupen64plus" },
        { "Flycast", "flycast_standalone", "emulators/flycast" },
        { "YabaSanshiro", "yabasanshiro_standalone", "emulators/yabasanshiro" },
        { "Ports", "ports", "emulators/ports" },
    };
    for (size_t i = 0; i < sizeof(spoofs) / sizeof(spoofs[0]); i++) {
        char path[256];
        char label[128];
        snprintf(path, sizeof(path),
                 "/sd/Apps/mlp1/Spoof.pak/%s/launch.sh",
                 spoofs[i].path_component);

        snprintf(label, sizeof(label), "content-pak %s id and path",
                 spoofs[i].label);
        expect_provider_bound_generic(label, spoofs[i].core_id, path);
        snprintf(label, sizeof(label), "content-pak %s id only",
                 spoofs[i].label);
        expect_provider_bound_generic(label, spoofs[i].core_id,
                                      "/sd/Apps/mlp1/Spoof.pak/bin/run.sh");
        snprintf(label, sizeof(label), "content-pak %s path only",
                 spoofs[i].label);
        expect_provider_bound_generic(label, "spoof_core", path);

        /* The release identity these inputs spoof is still recognized for a
           release core, so the provider is what made the difference. */
        snprintf(label, sizeof(label), "release %s still recognized",
                 spoofs[i].label);
        expect(label, release(spoofs[i].core_id, path).release !=
                          JW_STANDALONE_RELEASE_NONE,
               true);
    }
    expect_provider_bound_generic("content-pak PORTS content path",
                                  "spoof_core", "/sd/Roms/PORTS/game.sh");
    expect_provider_bound_generic("content-pak unrelated identity",
                                  "fixture_core", "/sd/Apps/mlp1/Fixture.pak/run");

    {
        jw_standalone_policy policy = jw_standalone_policy_resolve(
            "spoof_core", "/sd/Apps/mlp1/Spoof.pak/emulators/flycast/launch.sh",
            "mlp1/Spoof.pak");
        expect("content-pak core still honors catalog direct DRM metadata",
               jw_standalone_policy_requires_direct_drm(&policy, true), true);
    }

    puts("Standalone launch policy checks passed");
    return 0;
}
