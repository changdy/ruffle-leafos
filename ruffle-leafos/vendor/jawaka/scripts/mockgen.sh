#!/usr/bin/env bash
set -euo pipefail

PLATFORM="${PLATFORM:-mac}"
ROOT="${SDCARD_PATH:-${JAWAKA_SDCARD_ROOT:-./mock-sdcard}}"
DEFAULT_SYSTEM_ROOT="$ROOT/.system/leaf/platforms/$PLATFORM"
SYSTEM_ROOT="${UMRK_PLATFORM_PATH:-${SYSTEM_PATH:-$DEFAULT_SYSTEM_ROOT}}"
ROMS_ROOT="${ROMS_PATH:-$ROOT/Roms}"
IMAGES_ROOT="${IMAGES_PATH:-$ROOT/Images}"
MUSIC_ROOT="${MUSIC_PATH:-$ROOT/Music}"
BIOS_ROOT="${BIOS_PATH:-$ROOT/BIOS}"
APPS_ROOT="${APPS_PATH:-$ROOT/Apps}"
PLATFORM_APPS_ROOT="$APPS_ROOT/$PLATFORM"
SHARED_APPS_ROOT="$APPS_ROOT/shared"
SAVES_ROOT="${SAVES_PATH:-$ROOT/Saves}"
STATES_ROOT="${STATES_PATH:-$ROOT/States}"
INTERNAL_DATA="${UMRK_INTERNAL_DATA_PATH:-$ROOT/.umrk/$PLATFORM}"
USERDATA_ROOT="${USERDATA_PATH:-$ROOT/.userdata/$PLATFORM}"
SHARED_USERDATA_ROOT="${SHARED_USERDATA_PATH:-$ROOT/.userdata/shared}"
FORCE="${FORCE:-0}"
ROM_COUNT=0
SYSTEM_COUNT=0

if [[ -d "$ROOT" ]]; then
    if [[ "$FORCE" == "1" ]]; then
        rm -rf "$ROOT"
    else
        echo "mockgen: updating in place"
    fi
fi

mkdir -p "$ROMS_ROOT" "$IMAGES_ROOT" "$MUSIC_ROOT" "$BIOS_ROOT" "$PLATFORM_APPS_ROOT" "$SHARED_APPS_ROOT" "$SAVES_ROOT" "$STATES_ROOT"
mkdir -p "$SYSTEM_ROOT/defaults" "$INTERNAL_DATA" "$USERDATA_ROOT" "$SHARED_USERDATA_ROOT"

cat >"$SYSTEM_ROOT/manifest.json" <<'JSON'
{
  "platform": "mac",
  "version": "0.0.1",
  "artifact_format": "directory",
  "retroarch_bin_relpath": "retroarch/RetroArch.app/Contents/MacOS/RetroArch",
  "cores_dir_relpath": "cores",
  "info_dir_relpath": "info",
  "defaults_dir_relpath": "defaults",
  "theme_dir_relpath": "themes",
  "managed_apps": [],
  "source_repos": {
    "launcher": "Jawaka",
    "retroarch": "retroarch-builds",
    "cores": "Cores-spruce"
  }
}
JSON

cat >"$SYSTEM_ROOT/defaults/cores.json" <<'JSON'
{
  "platform": "mac",
  "systems": {
    "FC": { "core": "fceumm_libretro.dylib" },
    "NES": { "core": "fceumm_libretro.dylib" },
    "GB": { "core": "gambatte_libretro.dylib" },
    "GBC": { "core": "gambatte_libretro.dylib" },
    "GBA": { "core": "mgba_libretro.dylib" },
    "MD": { "core": "genesis_plus_gx_libretro.dylib" },
    "GG": { "core": "genesis_plus_gx_libretro.dylib" },
    "MS": { "core": "genesis_plus_gx_libretro.dylib" },
    "PS": { "core": "swanstation_libretro.dylib" }
  }
}
JSON

cat >"$SYSTEM_ROOT/defaults/retroarch.cfg" <<'CFG'
config_save_on_exit = "false"
libretro_directory = "cores"
libretro_info_path = "info"
system_directory = "BIOS"
savefile_directory = "Saves"
savestate_directory = "States"
CFG

create_rom() {
    local sys="$1"
    local ext="$2"
    local title="$3"
    local rom_path="$ROMS_ROOT/$sys/$title.$ext"
    local image_path="$IMAGES_ROOT/$sys/$title.png"

    mkdir -p "$ROMS_ROOT/$sys" "$IMAGES_ROOT/$sys"

    if [[ ! -f "$rom_path" ]]; then
        printf 'mock rom for %s\n' "$title" >"$rom_path"
        ROM_COUNT=$((ROM_COUNT + 1))
    fi

    if [[ ! -f "$image_path" ]]; then
        python3 -c "
import sys, zlib, struct
def c(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
png = (b'\x89PNG\r\n\x1a\n'
    + c(b'IHDR', struct.pack('>IIBBBBB', 1, 1, 8, 2, 0, 0, 0))
    + c(b'IDAT', zlib.compress(b'\x00\xd0\x80\x00'))
    + c(b'IEND', b''))
open(sys.argv[1], 'wb').write(png)
" "$image_path"
    fi
}

write_tiny_png() {
    local image_path="$1"
    mkdir -p "$(dirname "$image_path")"
    python3 - "$image_path" <<'PY'
import struct
import sys
import zlib

def chunk(t, data):
    return struct.pack(">I", len(data)) + t + data + struct.pack(">I", zlib.crc32(t + data) & 0xffffffff)

w = h = 32
pixel = bytes((0x99, 0x3b, 0x41))
rows = b"".join(b"\x00" + pixel * w for _ in range(h))
png = (
    b"\x89PNG\r\n\x1a\n"
    + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    + chunk(b"IDAT", zlib.compress(rows))
    + chunk(b"IEND", b"")
)
open(sys.argv[1], "wb").write(png)
PY
}

create_app() {
    local pak_dir="$1"
    local app_name="$2"
    local message="$3"
    local icon="${4:-}"
    local platform="${5:-$PLATFORM}"
    local app_root="$APPS_ROOT/$platform"

    mkdir -p "$app_root/$pak_dir"
    printf '#!/bin/sh\nprintf "%s\\n"\n' "$message" >"$app_root/$pak_dir/launch.sh"
    chmod +x "$app_root/$pak_dir/launch.sh"
    if [[ -n "$icon" ]]; then
        write_tiny_png "$app_root/$pak_dir/$icon"
    fi
    printf '{ "name": "%s", "icon": "%s", "platform": "%s", "pak_version": "0.0.1", "min_jawaka_version": "0.0.1" }\n' "$app_name" "$icon" "$platform" >"$app_root/$pak_dir/pak.json"
}

while IFS=: read -r sys ext titles; do
    [[ -n "$sys" ]] || continue
    SYSTEM_COUNT=$((SYSTEM_COUNT + 1))

    IFS='|' read -r -a title_array <<<"$titles"
    created_for_system=0
    for title in "${title_array[@]}"; do
        create_rom "$sys" "$ext" "$title"
        created_for_system=$((created_for_system + 1))
    done

    target_count=20
    if [[ "$sys" == "PORTS" ]]; then
        target_count=5
    fi

    filler=1
    while [[ $created_for_system -lt $target_count ]]; do
        create_rom "$sys" "$ext" "$sys Filler $filler"
        created_for_system=$((created_for_system + 1))
        filler=$((filler + 1))
    done
done <<'EOF'
GB:zip:Tetris|Super Mario Land|Pokemon Red|Kirby's Dream Land|Metroid II|Wario Land|Final Fantasy Adventure
GBA:zip:Advance Wars|Golden Sun|Mario Kart Super Circuit|Metroid Fusion|Pokemon Emerald|Castlevania Aria of Sorrow|F-Zero Maximum Velocity
GBC:zip:Pokemon Crystal|Zelda Oracle of Ages|Wario Land 3|Dragon Warrior Monsters|Survival Kids
SFC:zip:Super Mario World|Chrono Trigger|F-Zero|Earthbound|Super Metroid|Final Fantasy VI|Star Fox
FC:zip:Super Mario Bros 3|Metroid|Castlevania|Mega Man 2|Contra|Kirby's Adventure|Final Fantasy
MD:zip:Sonic the Hedgehog|Streets of Rage 2|Gunstar Heroes|Phantasy Star IV|Shining Force|Rocket Knight Adventures
GG:zip:Sonic Triple Trouble|Shinobi|Columns|Wonder Boy
MS:zip:Phantasy Star|Wonder Boy III|Alex Kidd in Miracle World
PCE:zip:Bonk's Adventure|R-Type|Castlevania Rondo of Blood
NEOGEO:zip:Metal Slug|King of Fighters 98|Samurai Shodown II|Garou Mark of the Wolves
LYNX:zip:California Games|Blue Lightning|Chip's Challenge
PS:chd:Castlevania Symphony of the Night|Metal Gear Solid|Final Fantasy VII|Chrono Cross|Silent Hill|Resident Evil 2|Ridge Racer Type 4
NDS:nds:Mario Kart DS|Castlevania Dawn of Sorrow|Advance Wars Dual Strike|Phoenix Wright Ace Attorney
ARCADE:zip:1942|Street Fighter II|Final Fight|Cadillacs and Dinosaurs|Sunset Riders
PORTS:sh:DOOM|Quake|OpenLara
EOF

create_app "HelloApp.pak" "Hello App" "Hello from a Jawaka mock pak!" "res/icon.png" "$PLATFORM"
create_app "Tools.pak" "Tools" "Tools placeholder" "" "shared"

echo "mockgen: generated $ROM_COUNT fake ROMs across $SYSTEM_COUNT systems in $ROOT"
