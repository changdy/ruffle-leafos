#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-life1-override.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PLATFORM_ROOT="$PRIMARY/.system/leaf/platforms/mac"
DEFAULTS="$PLATFORM_ROOT/defaults"
EMULATOR="$PLATFORM_ROOT/emulators/fixture/game-writer-fixture"
PAK="$PRIMARY/Apps/mac/Life1Fixture.pak"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
SERVICE_ID="org.umrk.test.life1override"
RESULT="$RUNTIME/services/$SERVICE_ID/life1-fixture-result"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"

cleanup() {
    exit_status=$?
    set +e
    smoke_daemon_stop || exit_status=1
    if [ -n "${OLD_DAEMON_PID:-}" ]; then
        DAEMON_PID="$OLD_DAEMON_PID"
        smoke_daemon_stop KILL || exit_status=1
    fi
    if [ -n "${OLD_PGID:-}" ]; then
        kill -TERM "-$OLD_PGID" 2>/dev/null || true
    fi
    if [ "$exit_status" -ne 0 ] && [ -f "$LOG" ]; then cat "$LOG" >&2; fi
    rm -rf "$TMP_DIR"
    exit "$exit_status"
}
trap cleanup EXIT

make -C "$ROOT_DIR" jawakad jawaka-platformctl \
    "$BUILD_REL/bin/life1-fixture-service" \
    "$BUILD_REL/bin/game-writer-fixture" >/dev/null
mkdir -p "$PAK/bin" "$STATE" "$RUNTIME" "$USERDATA" "$LOGS" \
         "$DEFAULTS" "$(dirname "$EMULATOR")" \
         "$PRIMARY/Roms/N64" "$PRIMARY/Images/N64" \
         "$PRIMARY/Saves" "$PRIMARY/States"
cp "$ROOT_DIR/$BUILD_REL/bin/life1-fixture-service" \
   "$PAK/bin/life1-fixture-service"
cp "$ROOT_DIR/$BUILD_REL/bin/game-writer-fixture" "$EMULATOR"
chmod 755 "$PAK/bin/life1-fixture-service" "$EMULATOR"
printf 'rom\n' >"$PRIMARY/Roms/N64/Barrier.n64"
printf '%s\n' \
  "{\"id\":\"$SERVICE_ID\",\"name\":\"LIFE-1 Override Fixture\",\"platform\":\"mac\",\"pak_version\":\"1.0.0\",\"service\":{\"schema\":1,\"id\":\"$SERVICE_ID\",\"run\":{\"path\":\"bin/life1-fixture-service\",\"args\":[]},\"default_enabled\":false,\"stop_grace_ms\":300,\"restart\":\"no\",\"lifecycle\":{\"game\":\"notify\"}}}" \
  >"$PAK/pak.json"
printf '%s\n' \
  '{"version":2,"platform":"mac","cores":[{"id":"writer_fixture","display_name":"Writer Fixture","type":"path","libretro_name":null,"file_name":null,"config_folder":"Writer Fixture","info_name":null,"path":"emulators/fixture/game-writer-fixture","supports_menu":false,"supports_savestate":true,"supports_disk_control":false,"needs_swap":false,"status":"packaged"}]}' \
  >"$DEFAULTS/cores.json"
printf '%s\n' \
  '{"version":2,"platform":"mac","systems":[{"id":"N64","name":"Nintendo 64","patterns":["N64"],"extensions":["n64"],"archive_extensions":[],"archive_inner_extensions":["n64"],"archive_mode":"pass_through","file_names":[],"ignore_file_names":[],"playlist_extensions":[],"m3u_generation":"none","default_core":"writer_fixture","alternate_cores":[],"rom_root":"Roms/N64","image_root":"Images/N64","bios_notes":[]}]}' \
  >"$DEFAULTS/systems.json"

start_daemon() {
    smoke_daemon_start "$ROOT_DIR" "$LOG" \
        PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$PRIMARY/Apps" \
        USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
        SAVES_PATH="$PRIMARY/Saves" STATES_PATH="$PRIMARY/States" \
        UMRK_PLATFORM_PATH="$PLATFORM_ROOT" UMRK_RUNTIME_PATH="$RUNTIME" \
        UMRK_DAEMON_SOCKET="$SOCKET" UMRK_INTERNAL_DATA_PATH="$STATE" \
        JAWAKA_SDCARD_ROOT="$PRIMARY" \
        UMRK_LIFE1_FIXTURE_SERVICE_ID="$SERVICE_ID" \
        UMRK_LIFE1_FIXTURE_SCENARIO=game-never-subscribe \
        "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only
    for _ in $(seq 1 300); do
        response="$($CTL --socket "$SOCKET" request \
            '{"type":"library-status"}' 2>/dev/null || true)"
        if printf '%s' "$response" | grep -q '"generation":'; then return 0; fi
        kill -0 "$DAEMON_PID" 2>/dev/null || return 1
        sleep 0.02
    done
    return 1
}

start_daemon
"$CTL" --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"run\",\"id\":\"run\",\"service_id\":\"$SERVICE_ID\"}" |
    grep -F '"ok":true' >/dev/null
for _ in $(seq 1 300); do
    grep -Fx 'ready=1' "$RESULT" >/dev/null 2>&1 && break
    sleep 0.02
done
grep -Fx 'ready=1' "$RESULT" >/dev/null
service_status="$($CTL --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"status\",\"id\":\"old\",\"service_id\":\"$SERVICE_ID\"}")"
OLD_PGID="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["ownership_identity"]["pgid"])' \
    <<<"$service_status")"

# Lose the daemon but keep its service. A SIGKILL would not do that on Linux,
# where a service dies with jawakad (SVC-1 arms PR_SET_PDEATHSIG), so freeze
# it instead: a stopped daemon supervises nothing, and cleanup kills it.
OLD_DAEMON_PID="$DAEMON_PID"
DAEMON_PID=""
kill -STOP "$OLD_DAEMON_PID"
kill -0 "$OLD_PGID"

start_daemon
for _ in $(seq 1 300); do
    service_status="$($CTL --socket "$SOCKET" request \
        "{\"v\":1,\"op\":\"status\",\"id\":\"stale\",\"service_id\":\"$SERVICE_ID\"}" \
        2>/dev/null || true)"
    printf '%s' "$service_status" | grep -q '"effective_state":"stale-generation"' && break
    sleep 0.02
done
printf '%s' "$service_status" | grep -q '"effective_state":"stale-generation"'

launch_reply="$($CTL --socket "$SOCKET" request \
    '{"type":"launch-game","system":"N64","rom_path":"Roms/N64/Barrier.n64"}')"
printf '%s' "$launch_reply" | grep -q '"type":"error"'
[ ! -e "$RUNTIME/active-game.json" ]
[ ! -e "$RUNTIME/game-writer-live" ]

blocked="$($CTL --socket "$SOCKET" request \
    '{"type":"game-launch-blocked-status"}')"
printf '%s' "$blocked" | grep -q '"blocked":true'
printf '%s' "$blocked" | grep -q '"override_allowed":true'
printf '%s' "$blocked" | grep -q "\"service_id\":\"$SERVICE_ID\""
printf '%s' "$blocked" | grep -q '"reason":"stale-service-generation"'
# Retry belongs to the RAOfflineProxy prompt only; this block keeps its
# two-way Cancel / Play Anyway contract and a refused Retry leaves it intact.
printf '%s' "$blocked" | grep -q '"retry_allowed":false'
"$CTL" --socket "$SOCKET" request '{"type":"game-launch-retry"}' |
    grep -F '"type":"error"' >/dev/null
blocked="$($CTL --socket "$SOCKET" request \
    '{"type":"game-launch-blocked-status"}')"
printf '%s' "$blocked" | grep -q '"blocked":true'
printf '%s' "$blocked" | grep -q '"reason":"stale-service-generation"'
[ ! -e "$RUNTIME/game-writer-live" ]

"$CTL" --socket "$SOCKET" request '{"type":"game-launch-override"}' |
    grep -F '"type":"ok"' >/dev/null
for _ in $(seq 1 300); do
    [ -e "$RUNTIME/game-writer-live" ] && break
    sleep 0.01
done
[ -e "$RUNTIME/game-writer-live" ]
[ -e "$RUNTIME/active-game.json" ]
kill -0 "$OLD_PGID"
for _ in $(seq 1 500); do
    [ -e "$RUNTIME/game-writer-done" ] && \
        [ ! -e "$RUNTIME/active-game.json" ] && break
    sleep 0.01
done
[ -e "$RUNTIME/game-writer-done" ]
[ ! -e "$RUNTIME/active-game.json" ]
grep -F "explicit override bypassing stale service generation service=$SERVICE_ID" \
    "$LOG" >/dev/null
echo "PASS life1-game-override-ipc-smoke"
