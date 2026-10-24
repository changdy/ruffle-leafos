#!/usr/bin/env bash
# Dreamcast-family governor policy smoke.
#
# Requirement: for any Dreamcast emulator -- the standalone Flycast and every
# Flycast libretro choice on DC, NAOMI and ATOMISWAVE -- the device runs in the
# performance profile, whatever the global, system, game or session setting
# asked for, and the previous mode comes back when the session ends or the
# launch fails before a child starts.
#
# Real jawakad on the mock platform, which records the governors it was asked
# for and serves them back over performance-status. A fixture catalog carries
# the four Flycast choices plus a non-Dreamcast control, and every fake child
# blocks on a release file so the assertions are deterministic rather than
# timed.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-dc-perf.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PLATFORM_ROOT="$PRIMARY/.system/leaf/platforms/mac"
DEFAULTS="$PLATFORM_ROOT/defaults"
CORES="$PLATFORM_ROOT/cores"
STANDALONE="$PLATFORM_ROOT/emulators/flycast/launch.sh"
FAKE_RA="$TMP_DIR/fake-retroarch.sh"
RELEASE="$TMP_DIR/child-release"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
DB_PATH="$STATE/library.db"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"
DAEMON_PID=""
MARK=0

cleanup() {
    status=$?
    set +e
    smoke_daemon_stop || status=1
    if [ "$status" -ne 0 ] && [ -f "$LOG" ]; then
        tail -120 "$LOG" >&2
    fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT

make -C "$ROOT_DIR" jawakad jawaka-platformctl >/dev/null

mkdir -p "$DEFAULTS" "$CORES" "$(dirname "$STANDALONE")" \
         "$STATE/retroarch" "$RUNTIME" "$USERDATA" "$LOGS" "$PRIMARY/Apps" \
         "$PRIMARY/Saves" "$PRIMARY/States" \
         "$PRIMARY/Roms/DC" "$PRIMARY/Roms/NAOMI" "$PRIMARY/Roms/ATOMISWAVE" \
         "$PRIMARY/Roms/GBA" \
         "$PRIMARY/Images/DC" "$PRIMARY/Images/NAOMI" \
         "$PRIMARY/Images/ATOMISWAVE" "$PRIMARY/Images/GBA"

# Fake children: stand in for the standalone launcher and for RetroArch. Both
# wait to be released so the smoke can inspect the device mode while the game is
# genuinely running.
cat >"$STANDALONE" <<EOF
#!/bin/sh
for _ in \$(seq 1 600); do
    [ -e "$RELEASE" ] && break
    sleep 0.05
done
exit 0
EOF
chmod 755 "$STANDALONE"
cp "$STANDALONE" "$FAKE_RA"

printf 'fixture-core\n' >"$CORES/flycast_libretro.so"
printf 'fixture-core\n' >"$CORES/flycast_fast_umrk_libretro.so"
printf 'fixture-core\n' >"$CORES/km_flycast_xtreme_libretro.so"
printf 'fixture-core\n' >"$CORES/mgba_libretro.so"

printf '%s\n' \
  '{"version":2,"platform":"mac","cores":['\
'{"id":"flycast_standalone","display_name":"Flycast Standalone","type":"path","libretro_name":null,"file_name":null,"config_folder":"Flycast Standalone","info_name":null,"path":"emulators/flycast/launch.sh","supports_menu":false,"supports_savestate":false,"supports_disk_control":false,"needs_swap":false,"requires_direct_drm":true,"status":"packaged"},'\
'{"id":"flycast","display_name":"Flycast","type":"retroarch","libretro_name":"flycast","file_name":"flycast_libretro.so","config_folder":"Flycast","info_name":"flycast_libretro.info","path":null,"supports_menu":true,"supports_savestate":true,"supports_disk_control":true,"needs_swap":false,"requires_direct_drm":false,"status":"packaged"},'\
'{"id":"flycast_fast_umrk","display_name":"FlyCast Fast UMRK","type":"retroarch","libretro_name":"flycast_fast_umrk","file_name":"flycast_fast_umrk_libretro.so","config_folder":"FlyCast Fast UMRK","info_name":"flycast_fast_umrk_libretro.info","path":null,"supports_menu":true,"supports_savestate":true,"supports_disk_control":true,"needs_swap":false,"requires_direct_drm":false,"status":"packaged"},'\
'{"id":"km_flycast_xtreme","display_name":"KM Flycast Xtreme","type":"retroarch","libretro_name":"km_flycast_xtreme","file_name":"km_flycast_xtreme_libretro.so","config_folder":"KM Flycast Xtreme","info_name":"km_flycast_xtreme_libretro.info","path":null,"supports_menu":true,"supports_savestate":true,"supports_disk_control":false,"needs_swap":false,"requires_direct_drm":false,"status":"packaged"},'\
'{"id":"mgba","display_name":"mGBA","type":"retroarch","libretro_name":"mgba","file_name":"mgba_libretro.so","config_folder":"mGBA","info_name":"mgba_libretro.info","path":null,"supports_menu":true,"supports_savestate":true,"supports_disk_control":false,"needs_swap":false,"requires_direct_drm":false,"status":"packaged"}'\
']}' \
  >"$DEFAULTS/cores.json"

printf '%s\n' \
  '{"version":2,"platform":"mac","systems":['\
'{"id":"DC","name":"Sega Dreamcast","patterns":["DC"],"extensions":["chd"],"archive_extensions":[],"archive_inner_extensions":["chd"],"archive_mode":"pass_through","file_names":[],"ignore_file_names":[],"playlist_extensions":[],"m3u_generation":"none","default_core":"flycast_standalone","alternate_cores":["flycast","flycast_fast_umrk","km_flycast_xtreme"],"rom_root":"Roms/DC","image_root":"Images/DC","bios_notes":[]},'\
'{"id":"NAOMI","name":"Sega Naomi","patterns":["NAOMI"],"extensions":["chd"],"archive_extensions":[],"archive_inner_extensions":["chd"],"archive_mode":"pass_through","file_names":[],"ignore_file_names":[],"playlist_extensions":[],"m3u_generation":"none","default_core":"flycast_standalone","alternate_cores":["flycast","flycast_fast_umrk","km_flycast_xtreme"],"rom_root":"Roms/NAOMI","image_root":"Images/NAOMI","bios_notes":[]},'\
'{"id":"ATOMISWAVE","name":"Atomiswave","patterns":["ATOMISWAVE"],"extensions":["chd"],"archive_extensions":[],"archive_inner_extensions":["chd"],"archive_mode":"pass_through","file_names":[],"ignore_file_names":[],"playlist_extensions":[],"m3u_generation":"none","default_core":"flycast_standalone","alternate_cores":["flycast","flycast_fast_umrk","km_flycast_xtreme"],"rom_root":"Roms/ATOMISWAVE","image_root":"Images/ATOMISWAVE","bios_notes":[]},'\
'{"id":"GBA","name":"Game Boy Advance","patterns":["GBA"],"extensions":["gba"],"archive_extensions":[],"archive_inner_extensions":["gba"],"archive_mode":"pass_through","file_names":[],"ignore_file_names":[],"playlist_extensions":[],"m3u_generation":"none","default_core":"mgba","alternate_cores":[],"rom_root":"Roms/GBA","image_root":"Images/GBA","bios_notes":[]}'\
']}' \
  >"$DEFAULTS/systems.json"

printf 'rom\n' >"$PRIMARY/Roms/DC/Game.chd"
printf 'rom\n' >"$PRIMARY/Roms/NAOMI/Game.chd"
printf 'rom\n' >"$PRIMARY/Roms/ATOMISWAVE/Game.chd"
printf 'rom\n' >"$PRIMARY/Roms/GBA/Game.gba"

smoke_daemon_start "$ROOT_DIR" "$LOG" \
    PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$PRIMARY/Apps" \
    USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
    SAVES_PATH="$PRIMARY/Saves" STATES_PATH="$PRIMARY/States" \
    UMRK_PLATFORM_PATH="$PLATFORM_ROOT" UMRK_RUNTIME_PATH="$RUNTIME" \
    UMRK_DAEMON_SOCKET="$SOCKET" UMRK_INTERNAL_DATA_PATH="$STATE" \
    UMRK_RETROARCH_BIN="$FAKE_RA" JAWAKA_SDCARD_ROOT="$PRIMARY" \
    "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only

for _ in $(seq 1 300); do
    [ -S "$SOCKET" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || exit 1
    sleep 0.02
done
[ -S "$SOCKET" ]

# The launch request resolves the game through the library, so wait for the
# first scan to finish before launching anything.
status=""
for _ in $(seq 1 600); do
    status="$("$CTL" --socket "$SOCKET" request '{"type":"library-status"}' 2>/dev/null || true)"
    if python3 -c 'import json,sys; d=json.load(sys.stdin); raise SystemExit(d.get("scan_running", True) or d.get("generation", 0) <= 0)' <<<"$status" 2>/dev/null; then
        break
    fi
    sleep 0.02
done
grep -F '"type":"library-status"' <<<"$status" >/dev/null

# Expect active profile plus all three governors, so both the daemon's
# bookkeeping and the values it actually wrote are checked.
matches_profile() {
    python3 -c '
import json, sys
want = sys.argv[1:5]
d = json.load(sys.stdin)
domains = d.get("domains", {})
got = [d.get("active_profile"),
       domains.get("cpu", {}).get("governor"),
       domains.get("gpu", {}).get("governor"),
       domains.get("dmc", {}).get("governor")]
raise SystemExit(0 if got == want else 1)
' "$@"
}

wait_profile() {
    local active="$1" cpu="$2" gpu="$3" dmc="$4" status=""
    for _ in $(seq 1 600); do
        status="$("$CTL" --socket "$SOCKET" request '{"type":"performance-status"}' 2>/dev/null || true)"
        if [ -n "$status" ] && printf '%s' "$status" | matches_profile "$active" "$cpu" "$gpu" "$dmc"; then
            return 0
        fi
        sleep 0.025
    done
    echo "performance-status never reached active=$active cpu=$cpu gpu=$gpu dmc=$dmc" >&2
    echo "last status: $status" >&2
    return 1
}

wait_frontend() {
    wait_profile frontend schedutil simple_ondemand dmc_ondemand
}

# Log assertions are scoped to the lines written since the case started, so an
# earlier case's launch can never satisfy them.
case_log() {
    tail -n +"$((MARK + 1))" "$LOG"
}

assert_log() {
    if ! case_log | grep -F "$1" >/dev/null; then
        echo "expected log line: $1" >&2
        return 1
    fi
}

assert_log_order() {
    local first_line second_line
    first_line="$(case_log | grep -n -F "$1" | head -1 | cut -d: -f1)"
    second_line="$(case_log | grep -n -F "$2" | head -1 | cut -d: -f1)"
    if [ -z "$first_line" ] || [ -z "$second_line" ] || [ "$first_line" -ge "$second_line" ]; then
        echo "expected '$1' before '$2'" >&2
        return 1
    fi
}

launch_game() {
    local system="$1" rom="$2"
    local request="{\"type\":\"launch-game\",\"system\":\"$system\",\"rom_path\":\"$rom\"}"
    "$CTL" --socket "$SOCKET" request "$request"
}

# The picker persists the chosen core per game; the launcher writes the same
# row. Using the real mechanism is the point of the smoke, so every libretro
# choice below is selected this way rather than through a one-shot request.
set_game_core_choice() {
    local system_id="$1" core="$2" game_id
    game_id="$(db_exec "SELECT id FROM games WHERE system='$system_id' ORDER BY id LIMIT 1;")"
    [ -n "$game_id" ]
    if [ -n "$core" ]; then
        db_exec "INSERT INTO game_settings (game_id, key, value, updated_at) VALUES ($game_id,'core_id','$core',strftime('%s','now')) ON CONFLICT(game_id,key) DO UPDATE SET value=excluded.value;"
    else
        db_exec "DELETE FROM game_settings WHERE game_id=$game_id AND key='core_id';"
    fi
}

# Launch a Dreamcast-family game, assert performance is applied before the
# child is spawned, then release the child and assert the frontend restore.
# $1 label, $2 system, $3 rom, $4 core ("" for the system default),
# $5 requested profile name, $6 launch reason.
run_dreamcast_case() {
    local label="$1" system_id="$2" rom="$3" core="$4" requested="$5" reason="$6"
    local line="performance: applied profile=performance requested=$requested reason=$reason system=$system_id"
    rm -f "$RELEASE"
    MARK="$(wc -l <"$LOG")"
    set_game_core_choice "$system_id" "$core"
    launch_game "$system_id" "$rom" | grep -F '"type":"ok"' >/dev/null
    wait_profile performance performance performance performance
    assert_log "$line"
    case "$reason" in
        retroarch-launch)
            assert_log_order "$line" "spawned RetroArch pid="
            ;;
        standalone-emulator-launch)
            assert_log_order "$line" "spawned standalone emulator pid="
            ;;
    esac
    touch "$RELEASE"
    wait_frontend
    assert_log "performance: applied profile=frontend requested=frontend reason="
    echo "case ok: $label"
}

launch_and_expect_profile() {
    local label="$1" system_id="$2" rom="$3" core="$4" line="$5"
    local active="$6" cpu="$7" gpu="$8" dmc="$9"
    rm -f "$RELEASE"
    MARK="$(wc -l <"$LOG")"
    set_game_core_choice "$system_id" "$core"
    launch_game "$system_id" "$rom" | grep -F '"type":"ok"' >/dev/null
    wait_profile "$active" "$cpu" "$gpu" "$dmc"
    assert_log "$line"
    touch "$RELEASE"
    wait_frontend
    echo "case ok: $label"
}

set_global_profile() {
    "$CTL" --socket "$SOCKET" request \
        "{\"type\":\"performance-set-profile\",\"profile\":\"$1\",\"scope\":\"global\"}" |
        grep -F '"type":"ok"' >/dev/null
}

set_session_profile() {
    "$CTL" --socket "$SOCKET" request \
        "{\"type\":\"performance-set-profile\",\"profile\":\"$1\",\"scope\":\"session\"}" |
        grep -F '"type":"ok"' >/dev/null
}

db_exec() {
    # The dot-command sets the busy timeout silently; "PRAGMA busy_timeout"
    # would echo its value into every command substitution.
    sqlite3 "$DB_PATH" ".timeout 5000" "$1"
}

# Baseline: the idle device sits in the frontend profile.
wait_frontend

# -- No override: AUTO already resolves the Dreamcast family to performance --
run_dreamcast_case "NAOMI standalone Flycast" \
    NAOMI Roms/NAOMI/Game.chd "" auto standalone-emulator-launch
run_dreamcast_case "DC Flycast libretro" \
    DC Roms/DC/Game.chd flycast auto retroarch-launch

# -- Global Balanced cannot downgrade a Dreamcast game, and still applies to a
#    non-Dreamcast one --
set_global_profile balanced
run_dreamcast_case "ATOMISWAVE KM Flycast Xtreme with global Balanced" \
    ATOMISWAVE Roms/ATOMISWAVE/Game.chd km_flycast_xtreme balanced retroarch-launch

launch_and_expect_profile "GBA keeps the global Balanced override" \
    GBA Roms/GBA/Game.gba mgba \
    "performance: applied profile=balanced requested=balanced reason=retroarch-launch system=GBA" \
    balanced schedutil simple_ondemand dmc_ondemand
set_global_profile auto

# -- Session Battery Saver cannot downgrade a Dreamcast game either, and the
#    override is gone once that session ends --
set_session_profile battery-saver
run_dreamcast_case "DC Flycast with session Battery Saver" \
    DC Roms/DC/Game.chd flycast battery-saver retroarch-launch
session_status="$("$CTL" --socket "$SOCKET" request '{"type":"performance-status"}')"
if ! printf '%s' "$session_status" | python3 -c '
import json, sys
d = json.load(sys.stdin)
raise SystemExit(0 if not d.get("session_override") and d.get("session_profile") == "auto" else 1)
'; then
    echo "session override survived the session" >&2
    exit 1
fi

# -- System and game overrides, written the way the launcher writes them --
db_exec "INSERT INTO system_settings (system, key, value, updated_at) VALUES ('DC','performance_profile','balanced',strftime('%s','now')) ON CONFLICT(system,key) DO UPDATE SET value=excluded.value;"
run_dreamcast_case "DC Flycast with system Balanced" \
    DC Roms/DC/Game.chd flycast balanced retroarch-launch

dc_game_id="$(db_exec "SELECT id FROM games WHERE system='DC' ORDER BY id LIMIT 1;")"
[ -n "$dc_game_id" ]
db_exec "INSERT INTO game_settings (game_id, key, value, updated_at) VALUES ($dc_game_id,'performance_profile','battery-saver',strftime('%s','now')) ON CONFLICT(game_id,key) DO UPDATE SET value=excluded.value;"
run_dreamcast_case "DC Flycast with game Battery Saver" \
    DC Roms/DC/Game.chd flycast battery-saver retroarch-launch

# -- A launch that fails after the profile was applied must restore it. The Fast
#    core's option seed runs after the apply and fails without its release-owned
#    template --
rm -f "$RELEASE"
MARK="$(wc -l <"$LOG")"
set_game_core_choice DC flycast_fast_umrk
reply="$(launch_game DC Roms/DC/Game.chd)"
printf '%s' "$reply" | grep -F '"type":"error"' >/dev/null
assert_log "performance: applied profile=performance requested=battery-saver reason=retroarch-launch system=DC"
assert_log "could not install the core option profile for flycast_fast_umrk"
assert_log "performance: applied profile=frontend requested=frontend reason=launch-failed"
wait_frontend
echo "case ok: failed launch restored the frontend profile"

if grep -F 'performance: apply failed' "$LOG" >/dev/null; then
    echo "a governor write failed during the smoke" >&2
    exit 1
fi

echo "PASS performance-dreamcast-ipc-smoke"
