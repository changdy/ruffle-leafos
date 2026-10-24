#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
UMRK_ROOT="${WORKSPACE_ROOT:-$(cd "$JAWAKA_DIR/.." && pwd)}"
BUILD_DIR="${BUILD:-build/phase3-fixture}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-phase3-scan.XXXXXX")"
if [[ "${KEEP_TEMP:-0}" != "1" ]]; then
    trap 'rm -rf "$TMP_ROOT"' EXIT
else
    echo "phase3 fixture temp: $TMP_ROOT" >&2
fi

SD_ROOT="$TMP_ROOT/sd"
SECONDARY_ROOT="$TMP_ROOT/secondary-sd"
DB_PATH="$TMP_ROOT/library.db"
COMPAT_DB_PATH="$TMP_ROOT/library-compat.db"
OUT_PATH="$TMP_ROOT/scan.tsv"
OUT_COMPAT_PATH="$TMP_ROOT/scan-compat.tsv"
OUT_RESCAN_PATH="$TMP_ROOT/scan-rescan.tsv"
OUT_PRUNE_PATH="$TMP_ROOT/scan-prune.tsv"

# Art lookup must hold on case-sensitive cards (ext4). Point TMPDIR at a
# case-sensitive volume to run the distinct-case fixtures; on a
# case-insensitive one they are skipped and extension spellings are compared
# without case.
: >"$TMP_ROOT/CaseProbe"
if [[ -e "$TMP_ROOT/caseprobe" ]]; then
    CASE_SENSITIVE=0
    echo "phase3 fixture: case-insensitive volume, skipping distinct-case art fixtures" >&2
else
    CASE_SENSITIVE=1
fi
rm -f "$TMP_ROOT/CaseProbe"

mkdir -p \
    "$SD_ROOT/.system/leaf/platforms/mlp1/defaults" \
    "$SD_ROOT/.system/leaf/platforms/mlp1/emulators/ports" \
    "$SD_ROOT/Roms/MD" \
    "$SD_ROOT/Roms/GBA/Imgs" \
    "$SD_ROOT/Roms/ARCADE" \
    "$SD_ROOT/Roms/AMIGA" \
    "$SD_ROOT/Roms/ATOMISWAVE" \
    "$SD_ROOT/Roms/NAOMI" \
    "$SD_ROOT/Roms/NAOMI/direct" \
    "$SD_ROOT/Roms/NAOMI/vstrik3" \
    "$SD_ROOT/Roms/PC98" \
    "$SD_ROOT/Roms/NES" \
    "$SD_ROOT/Roms/FC" \
    "$SD_ROOT/Roms/PS" \
    "$SD_ROOT/Roms/PORTS" \
    "$SD_ROOT/Roms/UNKNOWN" \
    "$SD_ROOT/Images/MD" \
    "$SD_ROOT/Images/GENESIS" \
    "$SD_ROOT/Images/GBA" \
    "$SD_ROOT/Apps/mlp1/FixtureNative.pak" \
    "$SD_ROOT/Apps/shared/FixtureShared.pak" \
    "$SD_ROOT/Apps/tg5040/WrongDevice.pak" \
    "$SD_ROOT/Apps/FlatLegacy.pak" \
    "$SECONDARY_ROOT/Apps/mlp1/SecondaryNative.pak" \
    "$SECONDARY_ROOT/Roms/FC" \
    "$SECONDARY_ROOT/Roms/GBA/Imgs" \
    "$SECONDARY_ROOT/Images/GBA"

cp "$UMRK_ROOT/miniloong-launcher-switcher/device/mlp1/defaults/cores.json" \
   "$SD_ROOT/.system/leaf/platforms/mlp1/defaults/cores.json"
cp "$UMRK_ROOT/miniloong-launcher-switcher/device/mlp1/defaults/systems.json" \
   "$SD_ROOT/.system/leaf/platforms/mlp1/defaults/systems.json"
cp "$UMRK_ROOT/miniloong-launcher-switcher/device/mlp1/defaults/arcade_names.txt" \
   "$SD_ROOT/.system/leaf/platforms/mlp1/defaults/arcade_names.txt"
printf '#!/bin/sh\nexec "$1"\n' >"$SD_ROOT/.system/leaf/platforms/mlp1/emulators/ports/launch.sh"
chmod 755 "$SD_ROOT/.system/leaf/platforms/mlp1/emulators/ports/launch.sh"

printf 'rom\n' >"$SD_ROOT/Roms/MD/Sonic.md"
printf 'archive\n' >"$SD_ROOT/Roms/MD/Sonic.md.zip"
printf 'archive\n' >"$SD_ROOT/Roms/GBA/Example.gba.zip"
printf 'bios\n' >"$SD_ROOT/Roms/ARCADE/neogeo.zip"
printf 'rom\n' >"$SD_ROOT/Roms/ARCADE/mslug.zip"
printf 'disk one\n' >"$SD_ROOT/Roms/AMIGA/Workbench Disk 1.adf"
printf 'archive\n' >"$SD_ROOT/Roms/AMIGA/Workbench Disk 2.zip"
printf 'installed game\n' >"$SD_ROOT/Roms/AMIGA/Unrelated.lha"
printf 'Workbench Disk 1.adf|Disk 1\nWorkbench Disk 2.zip#Disk2.adf\n#SAVEDISK:Save Disk\n' \
    >"$SD_ROOT/Roms/AMIGA/Workbench.m3u"
printf 'rom\n' >"$SD_ROOT/Roms/ATOMISWAVE/mslug6.zip"
printf 'rom\n' >"$SD_ROOT/Roms/NAOMI/mvsc2.zip"
printf 'rom\n' >"$SD_ROOT/Roms/NAOMI/vstrik3.zip"
printf 'disc\n' >"$SD_ROOT/Roms/NAOMI/vstrik3/gds-0006.chd"
printf 'disc\n' >"$SD_ROOT/Roms/NAOMI/direct/direct.chd"
printf 'disk a\n' >"$SD_ROOT/Roms/PC98/Dragon Knight 4 Special Disk (Disk 1 of 2)(Disk A).fdd"
printf 'disk b\n' >"$SD_ROOT/Roms/PC98/Dragon Knight 4 Special Disk (Disk 2 of 2)(Disk B).fdd"
printf 'standalone\n' >"$SD_ROOT/Roms/PC98/Standalone.fdd"
printf 'np2kai "Dragon Knight 4 Special Disk (Disk 1 of 2)(Disk A).fdd" "Dragon Knight 4 Special Disk (Disk 2 of 2)(Disk B).fdd"\n' \
    >"$SD_ROOT/Roms/PC98/Dragon Knight 4 Special Disk.cmd"
# Folder folding: Roms/NES and Roms/FC both resolve to system FC. The same
# title under both must collapse to one entry, preferring the canonical public
# folder (Roms/NES). An alias-only title (no canonical twin) must be kept.
printf 'rom\n' >"$SD_ROOT/Roms/NES/Mario.nes"
printf 'rom\n' >"$SD_ROOT/Roms/FC/Mario.nes"
printf 'rom\n' >"$SD_ROOT/Roms/FC/AliasOnly.nes"
printf 'disc\n' >"$SD_ROOT/Roms/PS/Game.m3u"
printf 'echo test\n' >"$SD_ROOT/Roms/PORTS/Test.sh"
printf 'ignore me\n' >"$SD_ROOT/Roms/UNKNOWN/readme.txt"
: >"$SD_ROOT/Images/MD/Sonic.png"
# JPEG box art. MD's canonical image root is Images/GENESIS, so Images/MD is
# the physical-folder fallback; GBA's canonical root is Images/GBA.
printf 'rom\n' >"$SD_ROOT/Roms/MD/Streets.md"
: >"$SD_ROOT/Images/MD/Streets.jpg"
printf 'rom\n' >"$SD_ROOT/Roms/MD/Canon.md"
: >"$SD_ROOT/Images/GENESIS/Canon.jpg"
: >"$SD_ROOT/Images/MD/Canon.png"
printf 'rom\n' >"$SD_ROOT/Roms/GBA/Other.gba"
: >"$SD_ROOT/Roms/GBA/Imgs/Other.JPEG"
printf 'rom\n' >"$SD_ROOT/Roms/GBA/Pair.gba"
: >"$SD_ROOT/Images/GBA/Pair.jpg"
: >"$SD_ROOT/Images/GBA/Pair.png"
printf 'rom\n' >"$SD_ROOT/Roms/GBA/Where.gba"
: >"$SD_ROOT/Images/GBA/Where.jpg"
: >"$SD_ROOT/Roms/GBA/Imgs/Where.png"
printf 'rom\n' >"$SD_ROOT/Roms/GBA/Mixed.gba"
: >"$SD_ROOT/Images/GBA/Mixed.JpG"
EXPECTED_GAMES=26
if [[ "$CASE_SENSITIVE" == "1" ]]; then
    # Distinct stems on ext4: neither may borrow the other's art.
    printf 'rom\n' >"$SD_ROOT/Roms/GBA/Case.gba"
    printf 'rom\n' >"$SD_ROOT/Roms/GBA/case.gba"
    : >"$SD_ROOT/Images/GBA/Case.png"
    : >"$SD_ROOT/Images/GBA/case.jpg"
    printf 'rom\n' >"$SD_ROOT/Roms/GBA/Lonely.gba"
    : >"$SD_ROOT/Images/GBA/lonely.jpg"
    EXPECTED_GAMES=29
fi
printf '#!/bin/sh\n' >"$SD_ROOT/Apps/mlp1/FixtureNative.pak/launch.sh"
printf '{ "name": "Fixture Native", "icon": "icon.png", "platform": "mlp1", "pak_version": "1.2.3", "min_jawaka_version": "0", "min_leaf_version": "0.7.0" }\n' \
    >"$SD_ROOT/Apps/mlp1/FixtureNative.pak/pak.json"
printf '#!/bin/sh\n' >"$SD_ROOT/Apps/shared/FixtureShared.pak/launch.sh"
printf '{ "name": "Fixture Shared", "icon": "icon.png", "platform": "shared", "pak_version": "1", "min_jawaka_version": "0" }\n' \
    >"$SD_ROOT/Apps/shared/FixtureShared.pak/pak.json"
printf '#!/bin/sh\n' >"$SD_ROOT/Apps/tg5040/WrongDevice.pak/launch.sh"
printf '{ "name": "Wrong Device", "icon": "icon.png", "platform": "tg5040", "pak_version": "1", "min_jawaka_version": "0" }\n' \
    >"$SD_ROOT/Apps/tg5040/WrongDevice.pak/pak.json"
printf '#!/bin/sh\n' >"$SD_ROOT/Apps/FlatLegacy.pak/launch.sh"
printf '{ "name": "Flat Legacy", "icon": "icon.png", "platform": "mlp1", "pak_version": "1", "min_jawaka_version": "0" }\n' \
    >"$SD_ROOT/Apps/FlatLegacy.pak/pak.json"
printf 'secondary duplicate\n' >"$SECONDARY_ROOT/Roms/GBA/Example.gba.zip"
printf 'secondary\n' >"$SECONDARY_ROOT/Roms/GBA/Secondary.gba"
# Same title as the primary canonical NES copy, but on another storage source.
# It must not be collapsed across sources.
printf 'secondary alias\n' >"$SECONDARY_ROOT/Roms/FC/Mario.nes"
: >"$SECONDARY_ROOT/Roms/GBA/Imgs/Secondary.png"
printf 'secondary\n' >"$SECONDARY_ROOT/Roms/GBA/SecondaryJpeg.gba"
: >"$SECONDARY_ROOT/Images/GBA/SecondaryJpeg.jpeg"
printf '#!/bin/sh\n' >"$SECONDARY_ROOT/Apps/mlp1/SecondaryNative.pak/launch.sh"
printf '{ "name": "Secondary Native", "icon": "icon.png", "platform": "mlp1", "pak_version": "1", "min_jawaka_version": "0" }\n' \
    >"$SECONDARY_ROOT/Apps/mlp1/SecondaryNative.pak/pak.json"

make -C "$JAWAKA_DIR" \
    BUILD="$BUILD_DIR" \
    PLATFORM=mlp1 \
    LDFLAGS_PLATFORM= \
    jawaka-scan-smoke >/dev/null

run_scan() {
    local db="$1"
    UMRK_PLATFORM_PATH="$SD_ROOT/.system/leaf/platforms/mlp1" \
    SDCARD_PATHS="$SD_ROOT:$SECONDARY_ROOT" \
        "$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-scan-smoke" "$SD_ROOT" "$db"
}

fail() {
    echo "$1" >&2
    exit 1
}

# Compare art paths exactly on a case-sensitive volume; elsewhere the lookup
# may return another spelling of the same file, so compare without case.
same_art_path() {
    if [[ "$CASE_SENSITIVE" == "1" ]]; then
        [[ "$1" == "$2" ]]
    else
        [[ "$(printf '%s' "$1" | tr '[:upper:]' '[:lower:]')" == \
           "$(printf '%s' "$2" | tr '[:upper:]' '[:lower:]')" ]]
    fi
}

# expect_game_art <tsv> <system> <title> <rom_path> <image_path>
expect_game_art() {
    local tsv="$1" system="$2" title="$3" rom="$4" want="$5" got
    got="$(awk -F '\t' -v s="$system" -v t="$title" -v r="$rom" \
        '$1 == "game" && $2 == s && $3 == t && $4 == r { print $5 }' "$tsv")"
    same_art_path "$got" "$want" ||
        { cat "$tsv" >&2; fail "art for $system/$title ($rom): want '$want', got '$got'"; }
    if [[ -n "$want" ]]; then
        local abs="$got"
        [[ "$abs" == /* ]] || abs="$SD_ROOT/$got"
        [[ -f "$abs" ]] || fail "stored art path for $rom does not resolve: $abs"
    fi
}

# expect_db_art <db> <source_id> <rom_relpath> <image_root_kind|image_relpath>
expect_db_art() {
    local db="$1" source_id="$2" rom="$3" want="$4" got
    got="$(sqlite3 "$db" \
        "SELECT COALESCE(image_root_kind,'')||'|'||COALESCE(image_relpath,'')
         FROM games WHERE source_id='$source_id' AND rom_relpath='$rom';")"
    same_art_path "$got" "$want" ||
        fail "db art for $source_id/$rom: want '$want', got '$got'"
}

run_scan "$DB_PATH" >"$OUT_PATH"

grep -F $'summary\tgames='"$EXPECTED_GAMES"$'\tsystems=10\tapps=3' "$OUT_PATH" >/dev/null ||
    { cat "$OUT_PATH" >&2; fail "unexpected scan summary"; }
# Alias dedup: primary has one FC "Mario", and it is the canonical Roms/NES copy, not Roms/FC.
# A secondary source alias copy with the same title remains separate.
[ "$(grep -cF $'game\tFC\tMario\t' "$OUT_PATH")" = "2" ]
grep -F $'game\tFC\tMario\tRoms/NES/Mario.nes\t' "$OUT_PATH" >/dev/null
grep -F $'game\tFC\tMario\t'"$SECONDARY_ROOT"$'/Roms/FC/Mario.nes\t' "$OUT_PATH" >/dev/null
if grep -F $'game\tFC\tMario\tRoms/FC/Mario.nes\t' "$OUT_PATH" >/dev/null; then
    cat "$OUT_PATH" >&2
    echo "alias dedup kept the legacy Roms/FC copy instead of canonical Roms/NES" >&2
    exit 1
fi
# Alias-only title (no canonical twin) is preserved, still under system FC.
grep -F $'game\tFC\tAliasOnly\tRoms/FC/AliasOnly.nes\t' "$OUT_PATH" >/dev/null
grep -F $'game\tMD\tSonic\tRoms/MD/Sonic.md\tImages/MD/Sonic.png' "$OUT_PATH" >/dev/null
grep -F $'game\tMD\tSonic\tRoms/MD/Sonic.md.zip\tImages/MD/Sonic.png' "$OUT_PATH" >/dev/null
grep -F $'game\tGBA\tExample\tRoms/GBA/Example.gba.zip\t' "$OUT_PATH" >/dev/null
grep -F "game"$'\t'"GBA"$'\t'"Example"$'\t'"$SECONDARY_ROOT/Roms/GBA/Example.gba.zip"$'\t' "$OUT_PATH" >/dev/null
grep -F "game"$'\t'"GBA"$'\t'"Secondary"$'\t'"$SECONDARY_ROOT/Roms/GBA/Secondary.gba"$'\t'"$SECONDARY_ROOT/Roms/GBA/Imgs/Secondary.png" "$OUT_PATH" >/dev/null
grep -F $'game\tARCADE\tMetal Slug\tRoms/ARCADE/mslug.zip\t' "$OUT_PATH" >/dev/null
grep -F $'game\tAMIGA\tWorkbench\tRoms/AMIGA/Workbench.m3u\t' "$OUT_PATH" >/dev/null
grep -F $'game\tAMIGA\tUnrelated\tRoms/AMIGA/Unrelated.lha\t' "$OUT_PATH" >/dev/null
if grep -F $'game\tAMIGA\tWorkbench Disk ' "$OUT_PATH" >/dev/null; then
    cat "$OUT_PATH" >&2
    echo "phase3 fixture scan exposed an Amiga m3u dependency as a separate game" >&2
    exit 1
fi
grep -F $'game\tATOMISWAVE\tMetal Slug 6\tRoms/ATOMISWAVE/mslug6.zip\t' "$OUT_PATH" >/dev/null
grep -F $'game\tNAOMI\tMarvel vs. Capcom 2 New Age of Heroes (Export, Korea)\tRoms/NAOMI/mvsc2.zip\t' "$OUT_PATH" >/dev/null
grep -F $'game\tNAOMI\tVirtua Striker 3\tRoms/NAOMI/vstrik3.zip\t' "$OUT_PATH" >/dev/null
grep -F $'game\tNAOMI\tdirect\tRoms/NAOMI/direct/direct.chd\t' "$OUT_PATH" >/dev/null
if grep -F 'gds-0006.chd' "$OUT_PATH" >/dev/null; then
    cat "$OUT_PATH" >&2
    echo "phase3 fixture scan exposed an archive dependency as a separate game" >&2
    exit 1
fi
grep -F $'game\tPC98\tDragon Knight 4 Special Disk\tRoms/PC98/Dragon Knight 4 Special Disk.cmd\t' "$OUT_PATH" >/dev/null
grep -F $'game\tPC98\tStandalone\tRoms/PC98/Standalone.fdd\t' "$OUT_PATH" >/dev/null
if grep -F 'Dragon Knight 4 Special Disk (Disk ' "$OUT_PATH" >/dev/null; then
    cat "$OUT_PATH" >&2
    echo "phase3 fixture scan exposed a cmd dependency as a separate game" >&2
    exit 1
fi
grep -F $'game\tPS\tGame\tRoms/PS/Game.m3u\t' "$OUT_PATH" >/dev/null
grep -F $'game\tPORTS\tTest\tRoms/PORTS/Test.sh\t' "$OUT_PATH" >/dev/null
grep -F $'app\tFixture Native\tApps/mlp1/FixtureNative.pak\tmlp1\ticon.png\t0.7.0' "$OUT_PATH" >/dev/null
grep -F $'app\tFixture Shared\tApps/shared/FixtureShared.pak\tshared\ticon.png' "$OUT_PATH" >/dev/null
grep -F "app"$'\t'"Secondary Native"$'\t'"$SECONDARY_ROOT/Apps/mlp1/SecondaryNative.pak"$'\t'"mlp1"$'\t'"icon.png" "$OUT_PATH" >/dev/null
grep -F "secondary_sd|GBA/Secondary.gba|roms|GBA/Imgs/Secondary.png" \
    < <(sqlite3 "$DB_PATH" \
        "SELECT source_id||'|'||rom_relpath||'|'||image_root_kind||'|'||image_relpath
         FROM games WHERE name='Secondary';") >/dev/null
grep -F "primary|MD/Sonic.md|images|MD/Sonic.png" \
    < <(sqlite3 "$DB_PATH" \
        "SELECT source_id||'|'||rom_relpath||'|'||image_root_kind||'|'||image_relpath
         FROM games WHERE name='Sonic' AND rom_relpath='MD/Sonic.md';") >/dev/null
grep -F "1.2.3|0|0.7.0" \
    < <(sqlite3 "$DB_PATH" \
        "SELECT pak_version||'|'||min_jawaka_version||'|'||min_leaf_version
         FROM apps WHERE name='Fixture Native';") >/dev/null

# JPEG art, metadata scanner.
expect_game_art "$OUT_PATH" MD Streets Roms/MD/Streets.md Images/MD/Streets.jpg
expect_db_art "$DB_PATH" primary MD/Streets.md "images|MD/Streets.jpg"
expect_game_art "$OUT_PATH" GBA Other Roms/GBA/Other.gba Roms/GBA/Imgs/Other.JPEG
expect_db_art "$DB_PATH" primary GBA/Other.gba "roms|GBA/Imgs/Other.JPEG"
# PNG beats JPG in the same folder.
expect_game_art "$OUT_PATH" GBA Pair Roms/GBA/Pair.gba Images/GBA/Pair.png
expect_db_art "$DB_PATH" primary GBA/Pair.gba "images|GBA/Pair.png"
# Location order beats format: JPEG in Images/ over PNG in Roms/<folder>/Imgs/.
expect_game_art "$OUT_PATH" GBA Where Roms/GBA/Where.gba Images/GBA/Where.jpg
expect_db_art "$DB_PATH" primary GBA/Where.gba "images|GBA/Where.jpg"
# Canonical image root beats the physical-folder fallback regardless of format.
expect_game_art "$OUT_PATH" MD Canon Roms/MD/Canon.md Images/GENESIS/Canon.jpg
expect_db_art "$DB_PATH" primary MD/Canon.md "images|GENESIS/Canon.jpg"
# The extension spelling that exists is what gets stored.
expect_game_art "$OUT_PATH" GBA Mixed Roms/GBA/Mixed.gba Images/GBA/Mixed.JpG
expect_db_art "$DB_PATH" primary GBA/Mixed.gba "images|GBA/Mixed.JpG"
# Secondary storage source.
expect_game_art "$OUT_PATH" GBA SecondaryJpeg "$SECONDARY_ROOT/Roms/GBA/SecondaryJpeg.gba" \
    "$SECONDARY_ROOT/Images/GBA/SecondaryJpeg.jpeg"
expect_db_art "$DB_PATH" secondary_sd GBA/SecondaryJpeg.gba "images|GBA/SecondaryJpeg.jpeg"
if [[ "$CASE_SENSITIVE" == "1" ]]; then
    expect_game_art "$OUT_PATH" GBA Case Roms/GBA/Case.gba Images/GBA/Case.png
    expect_game_art "$OUT_PATH" GBA case Roms/GBA/case.gba Images/GBA/case.jpg
    expect_db_art "$DB_PATH" primary GBA/case.gba "images|GBA/case.jpg"
    expect_game_art "$OUT_PATH" GBA Lonely Roms/GBA/Lonely.gba ""
fi

if grep -E 'neogeo|UNKNOWN|readme|WrongDevice|FlatLegacy' "$OUT_PATH" >/dev/null; then
    cat "$OUT_PATH" >&2
    echo "phase3 fixture scan included an ignored, unknown, unsupported, or wrong-platform file" >&2
    exit 1
fi

# Compatibility scanner (no catalog metadata): Images/<folder> then
# Roms/<folder>/Imgs, titles keep everything but the last extension.
JAWAKA_DISABLE_RETROARCH_V2=1 run_scan "$COMPAT_DB_PATH" >"$OUT_COMPAT_PATH"
expect_game_art "$OUT_COMPAT_PATH" MD Streets Roms/MD/Streets.md Images/MD/Streets.jpg
expect_game_art "$OUT_COMPAT_PATH" MD Canon Roms/MD/Canon.md Images/MD/Canon.png
expect_game_art "$OUT_COMPAT_PATH" GBA Other Roms/GBA/Other.gba Roms/GBA/Imgs/Other.JPEG
expect_db_art "$COMPAT_DB_PATH" primary GBA/Other.gba "roms|GBA/Imgs/Other.JPEG"
expect_game_art "$OUT_COMPAT_PATH" GBA Pair Roms/GBA/Pair.gba Images/GBA/Pair.png
expect_game_art "$OUT_COMPAT_PATH" GBA Where Roms/GBA/Where.gba Images/GBA/Where.jpg
expect_game_art "$OUT_COMPAT_PATH" GBA Mixed Roms/GBA/Mixed.gba Images/GBA/Mixed.JpG
expect_game_art "$OUT_COMPAT_PATH" GBA SecondaryJpeg "$SECONDARY_ROOT/Roms/GBA/SecondaryJpeg.gba" \
    "$SECONDARY_ROOT/Images/GBA/SecondaryJpeg.jpeg"
expect_db_art "$COMPAT_DB_PATH" secondary_sd GBA/SecondaryJpeg.gba "images|GBA/SecondaryJpeg.jpeg"

# A rescan picks up a PNG dropped next to existing JPEG art.
: >"$SD_ROOT/Images/MD/Streets.png"
run_scan "$DB_PATH" >"$OUT_RESCAN_PATH"
expect_game_art "$OUT_RESCAN_PATH" MD Streets Roms/MD/Streets.md Images/MD/Streets.png
expect_db_art "$DB_PATH" primary MD/Streets.md "images|MD/Streets.png"

rm -rf "$SECONDARY_ROOT"
run_scan "$DB_PATH" >"$OUT_PRUNE_PATH"

grep -F $'summary\tgames='"$EXPECTED_GAMES"$'\tsystems=10\tapps=3' "$OUT_PRUNE_PATH" >/dev/null
if ! grep -F "$SECONDARY_ROOT" "$OUT_PRUNE_PATH" >/dev/null ||
   ! grep -F $'game\tGBA\tSecondary' "$OUT_PRUNE_PATH" >/dev/null ||
   ! grep -F $'app\tSecondary Native' "$OUT_PRUNE_PATH" >/dev/null; then
    cat "$OUT_PRUNE_PATH" >&2
    echo "phase3 fixture scan pruned unavailable secondary SD rows" >&2
    exit 1
fi

cat "$OUT_PATH"
cat "$OUT_PRUNE_PATH"
