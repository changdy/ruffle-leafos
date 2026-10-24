#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-life1-check.XXXXXX")"
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
SERVICE_ID="org.umrk.test.life1check"
SCENARIO="${UMRK_LIFE1_SMOKE_SCENARIO:-game-check-current}"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"

cleanup() {
    exit_status=$?
    set +e
    smoke_daemon_stop || exit_status=1
    if [ "$exit_status" -ne 0 ] && [ -f "$LOG" ]; then
        sed -n '1,240p' "$LOG" >&2
    fi
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
cp "$ROOT_DIR/$BUILD_REL/bin/life1-fixture-service" "$PAK/bin/life1-fixture-service"
cp "$ROOT_DIR/$BUILD_REL/bin/game-writer-fixture" "$EMULATOR"
chmod 755 "$PAK/bin/life1-fixture-service" "$EMULATOR"
# The launch-status stages this smoke reads are logged as jawakad sends them to
# the OSD, so OSD requests stay on. A copy of the daemon with no jawaka-osd
# beside it keeps any OSD from starting.
DAEMON_BIN="$TMP_DIR/bin/jawakad"
mkdir -p "$TMP_DIR/bin"
cp "$ROOT_DIR/$BUILD_REL/bin/jawakad" "$DAEMON_BIN"
printf 'rom\n' >"$PRIMARY/Roms/N64/Barrier.n64"
printf '%s\n' \
  "{\"id\":\"$SERVICE_ID\",\"name\":\"LIFE-1 Check Fixture\",\"platform\":\"mac\",\"pak_version\":\"1.0.0\",\"service\":{\"schema\":1,\"id\":\"$SERVICE_ID\",\"run\":{\"path\":\"bin/life1-fixture-service\",\"args\":[]},\"default_enabled\":false,\"stop_grace_ms\":300,\"restart\":\"no\",\"lifecycle\":{\"game\":\"stop\"}}}" \
  >"$PAK/pak.json"
printf '%s\n' \
  '{"version":2,"platform":"mac","cores":[{"id":"writer_fixture","display_name":"Writer Fixture","type":"path","libretro_name":null,"file_name":null,"config_folder":"Writer Fixture","info_name":null,"path":"emulators/fixture/game-writer-fixture","supports_menu":false,"supports_savestate":true,"supports_disk_control":false,"needs_swap":false,"status":"packaged"}]}' \
  >"$DEFAULTS/cores.json"
printf '%s\n' \
  '{"version":2,"platform":"mac","systems":[{"id":"N64","name":"Nintendo 64","patterns":["N64"],"extensions":["n64"],"archive_extensions":[],"archive_inner_extensions":["n64"],"archive_mode":"pass_through","file_names":[],"ignore_file_names":[],"playlist_extensions":[],"m3u_generation":"none","default_core":"writer_fixture","alternate_cores":[],"rom_root":"Roms/N64","image_root":"Images/N64","bios_notes":[]}]}' \
  >"$DEFAULTS/systems.json"

smoke_daemon_start "$ROOT_DIR" "$LOG" \
    PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$PRIMARY/Apps" \
    USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
    SAVES_PATH="$PRIMARY/Saves" STATES_PATH="$PRIMARY/States" \
    UMRK_PLATFORM_PATH="$PLATFORM_ROOT" UMRK_RUNTIME_PATH="$RUNTIME" \
    UMRK_DAEMON_SOCKET="$SOCKET" UMRK_INTERNAL_DATA_PATH="$STATE" \
    JAWAKA_SDCARD_ROOT="$PRIMARY" \
    UMRK_LIFE1_FIXTURE_SERVICE_ID="$SERVICE_ID" \
    UMRK_LIFE1_FIXTURE_SCENARIO="$SCENARIO" JAWAKA_OSD=1 \
    "$DAEMON_BIN" --daemon-only

for _ in $(seq 1 300); do
    [ -S "$SOCKET" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || exit 1
    sleep 0.02
done
[ -S "$SOCKET" ]
for _ in $(seq 1 500); do
    status="$($CTL --socket "$SOCKET" request '{"type":"library-status"}' 2>/dev/null || true)"
    if python3 -c 'import json,sys; d=json.load(sys.stdin); raise SystemExit(d.get("scan_running", True) or d.get("generation", 0) <= 0)' <<<"$status" 2>/dev/null; then
        break
    fi
    sleep 0.02
done

"$CTL" --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"run\",\"id\":\"run\",\"service_id\":\"$SERVICE_ID\"}" |
    grep -F '"ok":true' >/dev/null
for _ in $(seq 1 300); do
    grep -Fx 'ready=1' "$RUNTIME/services/$SERVICE_ID/life1-fixture-result" >/dev/null 2>&1 && break
    sleep 0.02
done
grep -Fx 'ready=1' "$RUNTIME/services/$SERVICE_ID/life1-fixture-result" >/dev/null

"$CTL" --socket "$SOCKET" request \
    '{"type":"launch-game","system":"N64","rom_path":"Roms/N64/Barrier.n64"}' |
    grep -F '"type":"ok"' >/dev/null

case "$SCENARIO" in
    game-check-wait|game-check-play|game-check-cancel|game-check-expiry)
        for _ in $(seq 1 300); do
            decision="$($CTL --socket "$SOCKET" request '{"type":"game-launch-blocked-status"}' 2>/dev/null || true)"
            printf '%s' "$decision" | grep -q '"sync_pending":true' && break
            sleep 0.02
        done
        printf '%s' "$decision" | grep -q '"pending_items":3'
        printf '%s' "$decision" | grep -q '"pending_bytes":49152'
        ;;
esac

case "$SCENARIO" in
    game-check-wait)
        "$CTL" --socket "$SOCKET" request '{"type":"game-check-wait"}' |
            grep -F '"type":"ok"' >/dev/null
        : >"$RUNTIME/services/$SERVICE_ID/check-wait-selected"
        ;;
    game-check-play)
        "$CTL" --socket "$SOCKET" request '{"type":"game-check-play-anyway"}' |
            grep -F '"type":"ok"' >/dev/null
        ;;
    game-check-cancel)
        "$CTL" --socket "$SOCKET" request '{"type":"game-check-cancel"}' |
            grep -F '"type":"ok"' >/dev/null
        sleep 0.2
        [ ! -e "$RUNTIME/active-game.json" ]
        [ ! -e "$RUNTIME/game-writer-live" ]
        for _ in $(seq 1 100); do
            status="$($CTL --socket "$SOCKET" request \
                "{\"v\":1,\"op\":\"status\",\"id\":\"status\",\"service_id\":\"$SERVICE_ID\"}" 2>/dev/null || true)"
            printf '%s' "$status" | grep -q '"effective_state":"running"' && break
            sleep 0.02
        done
        printf '%s' "$status" | grep -q '"effective_state":"running"'
        grep -F 'life1: user cancelled check-before-stop' "$LOG" >/dev/null
        grep -F 'life1: launch status stage=checking' "$LOG" >/dev/null
        grep -F 'life1: launch status stage=syncing pending_items=3' "$LOG" >/dev/null
        if grep -F 'life1: launch status stage=starting' "$LOG" >/dev/null; then
            echo "launch status showed stage=starting" >&2
            exit 1
        fi
        echo "PASS life1-game-check-ipc-smoke ($SCENARIO)"
        exit 0
        ;;
    game-check-expiry)
        "$CTL" --socket "$SOCKET" request '{"type":"game-check-wait"}' |
            grep -F '"type":"ok"' >/dev/null
        ;;
esac

case "$SCENARIO" in
    game-check-expiry|game-check-timeout|game-check-malformed|game-check-unsafe-card)
        for _ in $(seq 1 300); do
            blocked="$($CTL --socket "$SOCKET" request '{"type":"game-launch-blocked-status"}' 2>/dev/null || true)"
            printf '%s' "$blocked" | grep -q '"override_allowed":true' && break
            sleep 0.02
        done
        [ ! -e "$RUNTIME/active-game.json" ]
        [ ! -e "$RUNTIME/game-writer-live" ]
        printf '%s' "$blocked" | grep -q '"requires_verified_stop":true'
        if [ "$SCENARIO" = game-check-expiry ]; then
            printf '%s' "$blocked" | grep -q '"reason":"sync-wait-expired"'
            printf '%s' "$blocked" | grep -q '"pending_items":3'
        elif [ "$SCENARIO" = game-check-unsafe-card ]; then
            printf '%s' "$blocked" | grep -q '"reason":"unsafe-card-binding"'
        else
            printf '%s' "$blocked" | grep -q '"reason":"check-before-stop-failed"'
        fi
        "$CTL" --socket "$SOCKET" request '{"type":"game-launch-override"}' |
            grep -F '"type":"ok"' >/dev/null
        grep -F 'skip_check=true allow_unverified_stop=false' "$LOG" >/dev/null
        if [ "$SCENARIO" = game-check-unsafe-card ]; then
            grep -Fx 'unsafe_card=1' "$RUNTIME/services/$SERVICE_ID/life1-fixture-result" >/dev/null
        fi
        ;;
esac

for _ in $(seq 1 500); do
    [ -e "$RUNTIME/game-writer-live" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || exit 1
    sleep 0.01
done
[ -e "$RUNTIME/game-writer-live" ]
grep -F 'life1: verified service stop service=' "$LOG" >/dev/null
grep -F 'life1: writer started' "$LOG" >/dev/null
grep -F 'life1: launch status stage=checking' "$LOG" >/dev/null
grep -F 'life1: launch status stage=stopping' "$LOG" >/dev/null
if grep -F 'life1: launch status stage=starting' "$LOG" >/dev/null; then
    echo "launch status showed stage=starting" >&2
    exit 1
fi
stop_line="$(grep -n -F 'life1: verified service stop service=' "$LOG" | tail -1 | cut -d: -f1)"
writer_line="$(grep -n -F 'life1: writer started' "$LOG" | tail -1 | cut -d: -f1)"
checking_line="$(grep -n -F 'life1: launch status stage=checking' "$LOG" | head -1 | cut -d: -f1)"
stopping_line="$(grep -n -F 'life1: launch status stage=stopping' "$LOG" | tail -1 | cut -d: -f1)"
[ "$checking_line" -lt "$stopping_line" ]
[ "$stopping_line" -le "$stop_line" ]
[ "$stop_line" -le "$writer_line" ]

case "$SCENARIO" in
    game-check-wait|game-check-play|game-check-expiry)
        grep -F 'life1: launch status stage=syncing pending_items=3' "$LOG" >/dev/null
        syncing_line="$(grep -n -F 'life1: launch status stage=syncing pending_items=3' "$LOG" | head -1 | cut -d: -f1)"
        [ "$checking_line" -lt "$syncing_line" ]
        [ "$syncing_line" -lt "$stopping_line" ]
        ;;
esac
case "$SCENARIO" in
    game-check-current|game-check-wait|game-check-play)
        grep -E 'life1: verified service stop .*coordinator_first=true .*group_fallback=false' "$LOG" >/dev/null
        ;;
esac

for _ in $(seq 1 600); do
    status="$($CTL --socket "$SOCKET" request \
        "{\"v\":1,\"op\":\"status\",\"id\":\"status\",\"service_id\":\"$SERVICE_ID\"}" 2>/dev/null || true)"
    [ -e "$RUNTIME/game-writer-done" ] && [ ! -e "$RUNTIME/active-game.json" ] && \
        printf '%s' "$status" | grep -q '"effective_state":"running"' && break
    sleep 0.02
done
[ -e "$RUNTIME/game-writer-done" ]
[ ! -e "$RUNTIME/active-game.json" ]
printf '%s' "$status" | grep -q '"effective_state":"running"'
grep -F 'life1: game.check service=' "$LOG" >/dev/null
echo "PASS life1-game-check-ipc-smoke ($SCENARIO)"
