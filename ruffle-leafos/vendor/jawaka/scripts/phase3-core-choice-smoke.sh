#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD:-build/phase3-core-choice}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-core-choice.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT

SD_ROOT="$TMP_ROOT/sd"
PLATFORM_ROOT="$SD_ROOT/.system/leaf/platforms/mac"
CORES_DIR="$PLATFORM_ROOT/cores"
DEFAULTS_DIR="$PLATFORM_ROOT/defaults"

mkdir -p \
    "$CORES_DIR" \
    "$DEFAULTS_DIR" \
    "$PLATFORM_ROOT/emulators/mupen64plus" \
    "$PLATFORM_ROOT/emulators/flycast" \
    "$PLATFORM_ROOT/emulators/yabasanshiro" \
    "$PLATFORM_ROOT/emulators/drastic" \
    "$PLATFORM_ROOT/emulators/fun-drastic"

cat >"$DEFAULTS_DIR/cores.json" <<'JSON'
{
  "version": 2,
  "platform": "mac",
  "cores": [
    {
      "id": "mupen64plus_standalone",
      "display_name": "Mupen64Plus Standalone",
      "type": "path",
      "libretro_name": null,
      "file_name": null,
      "config_folder": "Mupen64Plus Standalone",
      "info_name": null,
      "path": "emulators/mupen64plus/launch.sh",
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": false,
      "needs_swap": false,
      "status": "packaged"
    },
    {
      "id": "mupen64plus_next",
      "display_name": "Mupen64Plus Next",
      "type": "retroarch",
      "libretro_name": "mupen64plus_next",
      "file_name": "mupen64plus_next_libretro.dylib",
      "config_folder": "Mupen64Plus-Next",
      "info_name": "mupen64plus_next_libretro.info",
      "path": null,
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": false,
      "needs_swap": false,
      "status": "packaged"
    },
    {
      "id": "flycast_standalone",
      "display_name": "Flycast Standalone",
      "type": "path",
      "libretro_name": null,
      "file_name": null,
      "config_folder": "Flycast Standalone",
      "info_name": null,
      "path": "emulators/flycast/launch.sh",
      "supports_menu": false,
      "supports_savestate": false,
      "supports_disk_control": false,
      "needs_swap": false,
      "requires_direct_drm": true,
      "status": "packaged"
    },
    {
      "id": "flycast",
      "display_name": "Flycast",
      "type": "retroarch",
      "libretro_name": "flycast",
      "file_name": "flycast_libretro.dylib",
      "config_folder": "Flycast",
      "info_name": "flycast_libretro.info",
      "path": null,
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": true,
      "needs_swap": false,
      "requires_direct_drm": false,
      "status": "packaged"
    },
    {
      "id": "flycast_fast_umrk",
      "display_name": "FlyCast Fast UMRK",
      "type": "retroarch",
      "libretro_name": "flycast_fast_umrk",
      "file_name": "flycast_fast_umrk_libretro.dylib",
      "config_folder": "FlyCast Fast UMRK",
      "info_name": "flycast_fast_umrk_libretro.info",
      "path": null,
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": true,
      "needs_swap": false,
      "requires_direct_drm": false,
      "status": "packaged"
    },
    {
      "id": "km_flycast_xtreme",
      "display_name": "KM Flycast Xtreme",
      "type": "retroarch",
      "libretro_name": "km_flycast_xtreme",
      "file_name": "km_flycast_xtreme_libretro.dylib",
      "config_folder": "KM Flycast Xtreme",
      "info_name": "km_flycast_xtreme_libretro.info",
      "path": null,
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": false,
      "needs_swap": false,
      "requires_direct_drm": false,
      "status": "missing"
    },
    {
      "id": "yabasanshiro_standalone",
      "display_name": "YabaSanshiro Standalone",
      "type": "path",
      "libretro_name": null,
      "file_name": null,
      "config_folder": "YabaSanshiro Standalone",
      "info_name": null,
      "path": "emulators/yabasanshiro/launch.sh",
      "supports_menu": true,
      "supports_savestate": false,
      "supports_disk_control": false,
      "needs_swap": false,
      "requires_direct_drm": true,
      "status": "packaged"
    },
    {
      "id": "yabasanshiro",
      "display_name": "YabaSanshiro",
      "type": "retroarch",
      "libretro_name": "yabasanshiro",
      "file_name": "yabasanshiro_libretro.dylib",
      "config_folder": "YabaSanshiro",
      "info_name": "yabasanshiro_libretro.info",
      "path": null,
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": true,
      "needs_swap": false,
      "requires_direct_drm": false,
      "status": "packaged"
    },
    {
      "id": "drastic",
      "display_name": "DraStic",
      "type": "path",
      "libretro_name": null,
      "file_name": null,
      "config_folder": null,
      "info_name": null,
      "path": "emulators/drastic/launch.sh",
      "supports_menu": true,
      "supports_savestate": false,
      "supports_disk_control": false,
      "needs_swap": false,
      "requires_direct_drm": false,
      "status": "packaged"
    },
    {
      "id": "fun_drastic",
      "display_name": "Fun DraStic",
      "type": "path",
      "libretro_name": null,
      "file_name": null,
      "config_folder": null,
      "info_name": null,
      "path": "emulators/fun-drastic/launch.sh",
      "supports_menu": true,
      "supports_savestate": false,
      "supports_disk_control": false,
      "needs_swap": false,
      "requires_direct_drm": false,
      "status": "packaged"
    },
    {
      "id": "mgba",
      "display_name": "mGBA",
      "type": "retroarch",
      "libretro_name": "mgba",
      "file_name": "mgba_libretro.so",
      "config_folder": "mGBA",
      "info_name": "mgba_libretro.info",
      "path": null,
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": false,
      "needs_swap": false,
      "status": "packaged"
    },
    {
      "id": "gpsp",
      "display_name": "gpSP",
      "type": "retroarch",
      "libretro_name": "gpsp",
      "file_name": "gpsp_libretro.so",
      "config_folder": "gpSP",
      "info_name": "gpsp_libretro.info",
      "path": null,
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": false,
      "needs_swap": false,
      "status": "packaged"
    },
    {
      "id": "ghost_ra",
      "display_name": "Ghost RetroArch Core",
      "type": "retroarch",
      "libretro_name": "ghost_ra",
      "file_name": "ghost_ra_libretro.dylib",
      "config_folder": "Ghost",
      "info_name": "ghost_ra_libretro.info",
      "path": null,
      "supports_menu": true,
      "supports_savestate": true,
      "supports_disk_control": false,
      "needs_swap": false,
      "status": "packaged"
    }
  ]
}
JSON

cat >"$DEFAULTS_DIR/systems.json" <<'JSON'
{
  "version": 2,
  "platform": "mac",
  "systems": [
    {
      "id": "N64",
      "name": "Nintendo 64",
      "patterns": ["N64"],
      "extensions": ["n64", "v64", "z64"],
      "archive_extensions": ["zip", "7z"],
      "archive_inner_extensions": ["n64", "v64", "z64"],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": [],
      "m3u_generation": "none",
      "default_core": "mupen64plus_standalone",
      "alternate_cores": ["mupen64plus_next", "ghost_ra"],
      "rom_root": "Roms/N64",
      "image_root": "Images/N64",
      "bios_notes": []
    },
    {
      "id": "N64ALT",
      "name": "Nintendo 64 Alternate",
      "patterns": ["N64ALT"],
      "extensions": ["n64", "v64", "z64"],
      "archive_extensions": ["zip", "7z"],
      "archive_inner_extensions": ["n64", "v64", "z64"],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": [],
      "m3u_generation": "none",
      "default_core": "mupen64plus_next",
      "alternate_cores": ["mupen64plus_standalone"],
      "rom_root": "Roms/N64",
      "image_root": "Images/N64",
      "bios_notes": []
    },
    {
      "id": "DC",
      "name": "Sega Dreamcast",
      "patterns": ["DC"],
      "extensions": ["chd", "cdi", "gdi"],
      "archive_extensions": [],
      "archive_inner_extensions": ["chd", "cdi", "gdi"],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": ["m3u"],
      "m3u_generation": "manual",
      "default_core": "flycast_standalone",
      "alternate_cores": ["flycast", "flycast_fast_umrk", "km_flycast_xtreme"],
      "rom_root": "Roms/DC",
      "image_root": "Images/DC",
      "bios_notes": []
    },
    {
      "id": "GBA",
      "name": "Game Boy Advance",
      "patterns": ["GBA"],
      "extensions": ["gba"],
      "archive_extensions": ["zip"],
      "archive_inner_extensions": ["gba"],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": [],
      "m3u_generation": "none",
      "default_core": "mgba",
      "alternate_cores": ["gpsp"],
      "rom_root": "Roms/GBA",
      "image_root": "Images/GBA",
      "bios_notes": []
    },
    {
      "id": "NDS",
      "name": "Nintendo DS",
      "patterns": ["NDS"],
      "extensions": ["nds"],
      "archive_extensions": ["7z", "zip"],
      "archive_inner_extensions": ["nds"],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": [],
      "m3u_generation": "none",
      "default_core": "drastic",
      "alternate_cores": ["fun_drastic"],
      "rom_root": "Roms/NDS",
      "image_root": "Images/NDS",
      "bios_notes": []
    },
    {
      "id": "SATURN",
      "name": "Sega Saturn",
      "patterns": ["SATURN"],
      "extensions": ["ccd", "chd", "cue", "iso", "mds"],
      "archive_extensions": ["zip"],
      "archive_inner_extensions": ["ccd", "chd", "cue", "iso", "mds"],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": ["m3u"],
      "m3u_generation": "manual",
      "default_core": "yabasanshiro",
      "alternate_cores": ["yabasanshiro_standalone"],
      "rom_root": "Roms/SATURN",
      "image_root": "Images/SATURN",
      "bios_notes": []
    },
    {
      "id": "MIXPATH",
      "name": "Missing RetroArch default, path first",
      "patterns": ["MIXPATH"],
      "extensions": ["n64", "v64", "z64"],
      "archive_extensions": [],
      "archive_inner_extensions": [],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": [],
      "m3u_generation": "none",
      "default_core": "ghost_ra",
      "alternate_cores": ["mupen64plus_standalone", "mupen64plus_next"],
      "rom_root": "Roms/MIXPATH",
      "image_root": "Images/MIXPATH",
      "bios_notes": []
    },
    {
      "id": "MIXRA",
      "name": "Missing RetroArch default, RetroArch first",
      "patterns": ["MIXRA"],
      "extensions": ["n64", "v64", "z64"],
      "archive_extensions": [],
      "archive_inner_extensions": [],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": [],
      "m3u_generation": "none",
      "default_core": "ghost_ra",
      "alternate_cores": ["mupen64plus_next", "mupen64plus_standalone"],
      "rom_root": "Roms/MIXRA",
      "image_root": "Images/MIXRA",
      "bios_notes": []
    },
    {
      "id": "SATSTANDALONE",
      "name": "Saturn with a standalone default",
      "patterns": ["SATSTANDALONE"],
      "extensions": ["chd", "cue"],
      "archive_extensions": ["zip"],
      "archive_inner_extensions": ["chd", "cue"],
      "archive_mode": "pass_through",
      "file_names": [],
      "ignore_file_names": [],
      "playlist_extensions": ["m3u"],
      "m3u_generation": "manual",
      "default_core": "yabasanshiro_standalone",
      "alternate_cores": ["yabasanshiro"],
      "rom_root": "Roms/SATSTANDALONE",
      "image_root": "Images/SATSTANDALONE",
      "bios_notes": []
    }
  ]
}
JSON

printf '#!/bin/sh\nexit 0\n' >"$PLATFORM_ROOT/emulators/mupen64plus/launch.sh"
chmod 755 "$PLATFORM_ROOT/emulators/mupen64plus/launch.sh"
printf '#!/bin/sh\nexit 0\n' >"$PLATFORM_ROOT/emulators/flycast/launch.sh"
chmod 755 "$PLATFORM_ROOT/emulators/flycast/launch.sh"
printf '#!/bin/sh\nexit 0\n' >"$PLATFORM_ROOT/emulators/yabasanshiro/launch.sh"
chmod 755 "$PLATFORM_ROOT/emulators/yabasanshiro/launch.sh"
printf '#!/bin/sh\nexit 0\n' >"$PLATFORM_ROOT/emulators/drastic/launch.sh"
chmod 755 "$PLATFORM_ROOT/emulators/drastic/launch.sh"
printf '#!/bin/sh\nexit 0\n' >"$PLATFORM_ROOT/emulators/fun-drastic/launch.sh"
chmod 755 "$PLATFORM_ROOT/emulators/fun-drastic/launch.sh"
: >"$CORES_DIR/mupen64plus_next_libretro.dylib"
: >"$CORES_DIR/flycast_libretro.dylib"
: >"$CORES_DIR/flycast_fast_umrk_libretro.dylib"
: >"$CORES_DIR/yabasanshiro_libretro.dylib"
: >"$CORES_DIR/mgba_libretro.so"
: >"$CORES_DIR/gpsp_libretro.so"

make -C "$JAWAKA_DIR" BUILD="$BUILD_DIR" \
    jawaka-catalog-smoke jawaka-core-override-smoke >/dev/null
SMOKE="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-catalog-smoke"
OVERRIDE_SMOKE="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-core-override-smoke"

UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" N64 "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/n64.tsv"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" N64ALT "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/n64alt.tsv"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" DC "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/dc.tsv"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" GBA "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/gba.tsv"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" SATURN "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/saturn.tsv"
# Two path cores over the same emulator binary. DraStic stays the NDS default
# and Fun DraStic is the ordered alternate; the fifth argument reports what a
# persisted per-game choice of fun_drastic resolves to.
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" NDS "$CORES_DIR" "$PLATFORM_ROOT" fun_drastic \
    >"$TMP_ROOT/nds.tsv"

grep -F $'count\t2' "$TMP_ROOT/n64.tsv" >/dev/null
grep -F $'choice\t0\tmupen64plus_standalone\tpath\tdefault\tMupen64Plus Standalone\temulators/mupen64plus/launch.sh' "$TMP_ROOT/n64.tsv" >/dev/null
grep -F $'choice\t1\tmupen64plus_next\tretroarch\talternate\tMupen64Plus Next\tmupen64plus_next_libretro.dylib' "$TMP_ROOT/n64.tsv" >/dev/null

grep -F $'count\t2' "$TMP_ROOT/n64alt.tsv" >/dev/null
grep -F $'choice\t0\tmupen64plus_next\tretroarch\tdefault\tMupen64Plus Next\tmupen64plus_next_libretro.dylib' "$TMP_ROOT/n64alt.tsv" >/dev/null
grep -F $'choice\t1\tmupen64plus_standalone\tpath\talternate\tMupen64Plus Standalone\temulators/mupen64plus/launch.sh' "$TMP_ROOT/n64alt.tsv" >/dev/null

# The shipped Dreamcast trio: standalone default, then the current libretro
# Flycast, then FlyCast Fast UMRK. km_flycast_xtreme stays a missing reference
# and must not become a fourth entry.
grep -F $'count\t3' "$TMP_ROOT/dc.tsv" >/dev/null
grep -F $'choice\t0\tflycast_standalone\tpath\tdefault\tFlycast Standalone\temulators/flycast/launch.sh\tdirect-drm' "$TMP_ROOT/dc.tsv" >/dev/null
grep -F $'choice\t1\tflycast\tretroarch\talternate\tFlycast\tflycast_libretro.dylib\tshared-drm' "$TMP_ROOT/dc.tsv" >/dev/null
grep -F $'choice\t2\tflycast_fast_umrk\tretroarch\talternate\tFlyCast Fast UMRK\tflycast_fast_umrk_libretro.dylib\tshared-drm' "$TMP_ROOT/dc.tsv" >/dev/null
if grep -F 'km_flycast_xtreme' "$TMP_ROOT/dc.tsv" >/dev/null; then
    cat "$TMP_ROOT/dc.tsv" >&2
    echo "a missing catalog reference appeared in Dreamcast core choices" >&2
    exit 1
fi

grep -F $'count\t2' "$TMP_ROOT/gba.tsv" >/dev/null
grep -F $'choice\t0\tmgba\tretroarch\tdefault\tmGBA\tmgba_libretro.so' "$TMP_ROOT/gba.tsv" >/dev/null
grep -F $'choice\t1\tgpsp\tretroarch\talternate\tgpSP\tgpsp_libretro.so' "$TMP_ROOT/gba.tsv" >/dev/null

grep -F $'count\t2' "$TMP_ROOT/saturn.tsv" >/dev/null
grep -F $'choice\t0\tyabasanshiro\tretroarch\tdefault\tYabaSanshiro\tyabasanshiro_libretro.dylib\tshared-drm' "$TMP_ROOT/saturn.tsv" >/dev/null
grep -F $'choice\t1\tyabasanshiro_standalone\tpath\talternate\tYabaSanshiro Standalone\temulators/yabasanshiro/launch.sh\tdirect-drm' "$TMP_ROOT/saturn.tsv" >/dev/null

grep -F $'count\t2' "$TMP_ROOT/nds.tsv" >/dev/null
grep -F $'choice\t0\tdrastic\tpath\tdefault\tDraStic\temulators/drastic/launch.sh\tshared-drm' "$TMP_ROOT/nds.tsv" >/dev/null
grep -F $'choice\t1\tfun_drastic\tpath\talternate\tFun DraStic\temulators/fun-drastic/launch.sh\tshared-drm' "$TMP_ROOT/nds.tsv" >/dev/null
grep -F $'preferred\tfun_drastic\tpath\talternate\temulators/fun-drastic/launch.sh' "$TMP_ROOT/nds.tsv" >/dev/null

# Launch resolution: the exact core a library launch selects, with real
# per-core availability on this fixture card. Empty choices mean no saved
# choice at that scope.
: >"$TMP_ROOT/launch.tsv"
expect_launch() {
    local label="$1" system="$2" rom="$3" game_choice="$4" system_choice="$5"
    local expected="$6" actual
    actual="$(UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
        "$SMOKE" --launch "$SD_ROOT" "$system" "$CORES_DIR" "$PLATFORM_ROOT" \
        "$rom" "$game_choice" "$system_choice")"
    printf '%s\t%s\n' "$label" "$actual" >>"$TMP_ROOT/launch.tsv"
    if [ "$actual" != "$expected" ]; then
        cat "$TMP_ROOT/launch.tsv" >&2
        echo "launch resolution '$label': got '$actual', expected '$expected'" >&2
        exit 1
    fi
}

expect_launch "RetroArch default beats installed path alternate" \
    SATURN "Roms/SATURN/Astal.chd" "" "" \
    $'launch\tyabasanshiro\tretroarch\tdefault'
expect_launch "path default" \
    N64 "Roms/N64/smoke.z64" "" "" \
    $'launch\tmupen64plus_standalone\tpath\tdefault'
expect_launch "RetroArch default of a mixed system" \
    N64ALT "Roms/N64/smoke.z64" "" "" \
    $'launch\tmupen64plus_next\tretroarch\tdefault'
expect_launch "missing RetroArch default, path alternate first" \
    MIXPATH "Roms/MIXPATH/smoke.z64" "" "" \
    $'launch\tmupen64plus_standalone\tpath\talternate'
expect_launch "missing RetroArch default, RetroArch alternate first" \
    MIXRA "Roms/MIXRA/smoke.z64" "" "" \
    $'launch\tmupen64plus_next\tretroarch\talternate'
expect_launch "saved standalone choice" \
    SATURN "Roms/SATURN/Astal.chd" "" yabasanshiro_standalone \
    $'launch\tyabasanshiro_standalone\tpath\tsaved'
expect_launch "saved FlyCast Fast UMRK choice" \
    DC "Roms/DC/smoke.chd" "" flycast_fast_umrk \
    $'launch\tflycast_fast_umrk\tretroarch\tsaved'
expect_launch "saved choice for an available allowed core" \
    GBA "Roms/GBA/smoke.gba" "" gpsp \
    $'launch\tgpsp\tretroarch\tsaved'
expect_launch "game choice takes precedence over system choice" \
    GBA "Roms/GBA/smoke.gba" gpsp mgba \
    $'launch\tgpsp\tretroarch\tsaved'
expect_launch "invalid game choice masks available system choice" \
    GBA "Roms/GBA/smoke.gba" bogus gpsp \
    $'launch\tmgba\tretroarch\tdefault'
expect_launch "disallowed saved choice" \
    GBA "Roms/GBA/smoke.gba" "" flycast \
    $'launch\tmgba\tretroarch\tdefault'
expect_launch "unavailable saved RetroArch choice, available path default" \
    N64 "Roms/N64/smoke.z64" "" ghost_ra \
    $'launch\tmupen64plus_standalone\tpath\tdefault'
expect_launch "path default accepts its content" \
    SATSTANDALONE "Roms/SATSTANDALONE/Astal.chd" "" "" \
    $'launch\tyabasanshiro_standalone\tpath\tdefault'
expect_launch "path default rejecting ZIP falls through to RetroArch" \
    SATSTANDALONE "Roms/SATSTANDALONE/Rampage.zip" "" "" \
    $'launch\tyabasanshiro\tretroarch\talternate'
expect_launch "saved standalone rejecting M3U falls through to default" \
    SATURN "Roms/SATURN/Enemy Zero.m3u" "" yabasanshiro_standalone \
    $'launch\tyabasanshiro\tretroarch\tdefault'

UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$OVERRIDE_SMOKE" "$SD_ROOT" GBA "$CORES_DIR" "$PLATFORM_ROOT" \
    "$TMP_ROOT/library.db" "Roms/GBA/smoke.gba" gpsp >"$TMP_ROOT/gba-override.tsv"
grep -F $'choices\tmgba\tgpsp' "$TMP_ROOT/gba-override.tsv" >/dev/null
grep -F $'persisted\tsystem\tgpsp' "$TMP_ROOT/gba-override.tsv" >/dev/null
grep -F $'persisted\tgame\tgpsp' "$TMP_ROOT/gba-override.tsv" >/dev/null
grep -F $'fallback\tmgba' "$TMP_ROOT/gba-override.tsv" >/dev/null
grep -F 'PASS core-override-smoke' "$TMP_ROOT/gba-override.tsv" >/dev/null

chmod 644 "$PLATFORM_ROOT/emulators/mupen64plus/launch.sh"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" N64 "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/n64-noexec.tsv"
grep -F $'count\t1' "$TMP_ROOT/n64-noexec.tsv" >/dev/null
grep -F $'choice\t0\tmupen64plus_next\tretroarch\talternate\tMupen64Plus Next\tmupen64plus_next_libretro.dylib' "$TMP_ROOT/n64-noexec.tsv" >/dev/null
if grep -F 'mupen64plus_standalone' "$TMP_ROOT/n64-noexec.tsv" >/dev/null; then
    cat "$TMP_ROOT/n64-noexec.tsv" >&2
    echo "non-executable path core appeared in core choices" >&2
    exit 1
fi

expect_launch "non-executable path default, RetroArch alternate" \
    N64 "Roms/N64/smoke.z64" "" "" \
    $'launch\tmupen64plus_next\tretroarch\talternate'
expect_launch "saved non-executable path core falls through" \
    N64ALT "Roms/N64/smoke.z64" "" mupen64plus_standalone \
    $'launch\tmupen64plus_next\tretroarch\tdefault'
mv "$CORES_DIR/mupen64plus_next_libretro.dylib" "$TMP_ROOT/mupen64plus_next_libretro.dylib"
expect_launch "all catalog candidates unavailable" \
    MIXPATH "Roms/MIXPATH/smoke.z64" "" "" \
    $'launch\tnone'
mv "$TMP_ROOT/mupen64plus_next_libretro.dylib" "$CORES_DIR/mupen64plus_next_libretro.dylib"

chmod 644 "$PLATFORM_ROOT/emulators/flycast/launch.sh"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" DC "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/dc-noexec.tsv"
grep -F $'count\t2' "$TMP_ROOT/dc-noexec.tsv" >/dev/null
grep -F $'choice\t0\tflycast\tretroarch\talternate\tFlycast\tflycast_libretro.dylib\tshared-drm' "$TMP_ROOT/dc-noexec.tsv" >/dev/null
grep -F $'choice\t1\tflycast_fast_umrk\tretroarch\talternate\tFlyCast Fast UMRK\tflycast_fast_umrk_libretro.dylib\tshared-drm' "$TMP_ROOT/dc-noexec.tsv" >/dev/null
if grep -F 'flycast_standalone' "$TMP_ROOT/dc-noexec.tsv" >/dev/null; then
    cat "$TMP_ROOT/dc-noexec.tsv" >&2
    echo "non-executable Flycast path core appeared in core choices" >&2
    exit 1
fi

chmod 644 "$PLATFORM_ROOT/emulators/yabasanshiro/launch.sh"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" SATURN "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/saturn-noexec.tsv"
grep -F $'count\t1' "$TMP_ROOT/saturn-noexec.tsv" >/dev/null
grep -F $'choice\t0\tyabasanshiro\tretroarch\tdefault\tYabaSanshiro\tyabasanshiro_libretro.dylib\tshared-drm' "$TMP_ROOT/saturn-noexec.tsv" >/dev/null
if grep -F 'yabasanshiro_standalone' "$TMP_ROOT/saturn-noexec.tsv" >/dev/null; then
    cat "$TMP_ROOT/saturn-noexec.tsv"
cat "$TMP_ROOT/nds.tsv"
cat "$TMP_ROOT/nds-noexec.tsv"
cat "$TMP_ROOT/nds-nodefault.tsv" >&2
    echo "non-executable YabaSanshiro path core appeared in core choices" >&2
    exit 1
fi

chmod 644 "$PLATFORM_ROOT/emulators/fun-drastic/launch.sh"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" NDS "$CORES_DIR" "$PLATFORM_ROOT" fun_drastic \
    >"$TMP_ROOT/nds-noexec.tsv"
grep -F $'count\t1' "$TMP_ROOT/nds-noexec.tsv" >/dev/null
grep -F $'choice\t0\tdrastic\tpath\tdefault\tDraStic\temulators/drastic/launch.sh\tshared-drm' "$TMP_ROOT/nds-noexec.tsv" >/dev/null
grep -F $'preferred\tfun_drastic\tunavailable' "$TMP_ROOT/nds-noexec.tsv" >/dev/null
chmod 755 "$PLATFORM_ROOT/emulators/fun-drastic/launch.sh"

# The default must survive a missing alternate: DraStic is the NDS default and
# an unstaged Fun DraStic package cannot take Nintendo DS down with it.
chmod 644 "$PLATFORM_ROOT/emulators/drastic/launch.sh"
UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$SMOKE" "$SD_ROOT" NDS "$CORES_DIR" "$PLATFORM_ROOT" >"$TMP_ROOT/nds-nodefault.tsv"
grep -F $'count\t1' "$TMP_ROOT/nds-nodefault.tsv" >/dev/null
grep -F $'choice\t0\tfun_drastic\tpath\talternate\tFun DraStic\temulators/fun-drastic/launch.sh\tshared-drm' "$TMP_ROOT/nds-nodefault.tsv" >/dev/null
chmod 755 "$PLATFORM_ROOT/emulators/drastic/launch.sh"

cat "$TMP_ROOT/n64.tsv"
cat "$TMP_ROOT/n64alt.tsv"
cat "$TMP_ROOT/n64-noexec.tsv"
cat "$TMP_ROOT/dc.tsv"
cat "$TMP_ROOT/dc-noexec.tsv"
cat "$TMP_ROOT/gba.tsv"
cat "$TMP_ROOT/gba-override.tsv"
cat "$TMP_ROOT/launch.tsv"
cat "$TMP_ROOT/saturn.tsv"
cat "$TMP_ROOT/saturn-noexec.tsv"
cat "$TMP_ROOT/nds.tsv"
cat "$TMP_ROOT/nds-noexec.tsv"
cat "$TMP_ROOT/nds-nodefault.tsv"
