#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-imported-title.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
SECONDARY="$TMP_DIR/secondary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
PLATFORM="$TMP_DIR/platform"
SOCKET="$RUNTIME/jawakad.sock"
DB="$STATE/library.db"
LOG="$TMP_DIR/jawakad.log"
OUTSIDE="$TMP_DIR/outside.cue"

cleanup() {
    status=$?
    set +e
    smoke_daemon_stop || status=1
    if [ "$status" -ne 0 ] && [ -f "$LOG" ]; then
        cat "$LOG" >&2
    fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT

make -C "$ROOT_DIR" jawakad mockgen >/dev/null
cp -R "$ROOT_DIR/mock-sdcard" "$PRIMARY"
mkdir -p "$PRIMARY/Roms/PS" "$SECONDARY/Roms/PICO8" "$STATE" "$RUNTIME" "$PLATFORM/defaults"
printf 'FILE "track.bin" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n' >"$PRIMARY/Roms/PS/game.cue"
printf 'bin\n' >"$PRIMARY/Roms/PS/track.bin"
printf 'p8\n' >"$SECONDARY/Roms/PICO8/cart-a.p8"
printf 'p8\n' >"$SECONDARY/Roms/PICO8/cart-b.p8"
printf 'outside\n' >"$OUTSIDE"
printf '%s\n' '{"platform":"mac","cores":[{"id":"pcsx_rearmed","display_name":"PCSX ReARMed","type":"retroarch","file_name":"pcsx_rearmed_libretro.dylib","config_folder":"PCSX-ReARMed","status":"packaged"},{"id":"fake08","display_name":"FAKE-08","type":"retroarch","file_name":"fake08_libretro.dylib","config_folder":"fake-08","status":"packaged"}]}' >"$PLATFORM/defaults/cores.json"
printf '%s\n' '{"platform":"mac","systems":[{"id":"PS","name":"Sony PlayStation","patterns":["PS"],"extensions":["cue"],"archive_extensions":[],"archive_inner_extensions":["cue"],"archive_mode":"pass_through","file_names":[],"ignore_file_names":[],"playlist_extensions":[],"m3u_generation":"none","default_core":"pcsx_rearmed","alternate_cores":[],"rom_root":"Roms/PS","image_root":"Images/PS"},{"id":"PICO8","name":"Pico-8","patterns":["PICO8"],"extensions":["p8"],"archive_extensions":[],"archive_inner_extensions":["p8"],"archive_mode":"pass_through","file_names":[],"ignore_file_names":[],"playlist_extensions":[],"m3u_generation":"none","default_core":"fake08","alternate_cores":[],"rom_root":"Roms/PICO8","image_root":"Images/PICO8"}]}' >"$PLATFORM/defaults/systems.json"

# Keep the first scan active long enough that the second request exercises the
# queued-title merge path rather than only two sequential idle scans.
mkdir -p "$PRIMARY/Roms/PICO8"
for n in $(seq 1 600); do
    printf 'p8\n' >"$PRIMARY/Roms/PICO8/queue-fixture-$n.p8"
done

smoke_daemon_start "$ROOT_DIR" "$LOG" \
    UMRK_RUNTIME_PATH="$RUNTIME" \
    UMRK_DAEMON_SOCKET="$SOCKET" \
    UMRK_INTERNAL_DATA_PATH="$STATE" \
    UMRK_PLATFORM_PATH="$PLATFORM" \
    JAWAKA_SDCARD_ROOT="$PRIMARY" \
    SDCARD_PATHS="$PRIMARY:$SECONDARY" \
    build/bin/jawakad --daemon-only

for _ in $(seq 1 500); do
    [ -S "$SOCKET" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || { cat "$LOG" >&2; exit 1; }
    sleep 0.02
done
[ -S "$SOCKET" ] || { echo "jawakad socket did not appear" >&2; cat "$LOG" >&2; exit 1; }

# The assertions only hold for the metadata scanner. If the catalog loader
# rejects the fixture above, the compatibility scanner indexes every file
# (track.bin included) and the failure surfaces as a misleading title mismatch.
for _ in $(seq 1 500); do
    grep -q "RetroArch discovery: scanning with" "$LOG" && break
    sleep 0.02
done
if grep -F "using compatibility scanner" "$LOG" >&2; then
    echo "fixture catalog was rejected; update it to the current catalog schema" >&2
    exit 1
fi

python3 "$ROOT_DIR/scripts/imported-title-ipc-smoke.py" \
    "$SOCKET" "$DB" "$PRIMARY" "$SECONDARY" "$OUTSIDE"
grep -F "scan-library imported-titles accepted=2 applied=1 unmatched=1" "$LOG" >/dev/null
grep -F "scan-library imported-titles accepted=2 applied=2 unmatched=0" "$LOG" >/dev/null
