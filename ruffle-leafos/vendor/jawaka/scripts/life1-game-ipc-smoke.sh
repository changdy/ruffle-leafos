#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-life1-game.XXXXXX")"
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
SERVICE_ID="org.umrk.test.life1game"
SCENARIO="${UMRK_LIFE1_SMOKE_SCENARIO:-game-exchange}"
SUCCESS_SCENARIO=0
UNSUBSCRIBED_SCENARIO=0
UNMANAGED_SCENARIO=0
case "$SCENARIO" in
    game-exchange|game-slow-ready|game-reconnect|game-waiting|game-wait-expiry|game-start-now) SUCCESS_SCENARIO=1 ;;
    game-never-subscribe|game-late-subscribe) UNSUBSCRIBED_SCENARIO=1 ;;
    game-disabled|game-no-pak) UNMANAGED_SCENARIO=1 ;;
esac
RESULT="$RUNTIME/services/$SERVICE_ID/life1-fixture-result"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"

cleanup() {
    status=$?
    set +e
    smoke_daemon_stop || status=1
    if [ "$status" -ne 0 ] && [ -f "$LOG" ]; then cat "$LOG" >&2; fi
    rm -rf "$TMP_DIR"
    exit "$status"
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
printf 'rom\n' >"$PRIMARY/Roms/N64/Barrier.n64"
printf '%s\n' \
  "{\"id\":\"$SERVICE_ID\",\"name\":\"LIFE-1 Game Fixture\",\"platform\":\"mac\",\"pak_version\":\"1.0.0\",\"service\":{\"schema\":1,\"id\":\"$SERVICE_ID\",\"run\":{\"path\":\"bin/life1-fixture-service\",\"args\":[]},\"default_enabled\":false,\"stop_grace_ms\":300,\"restart\":\"no\",\"lifecycle\":{\"game\":\"notify\"}}}" \
  >"$PAK/pak.json"
if [ "$SCENARIO" = game-no-pak ]; then
    rm -rf "$PAK"
fi
printf '%s\n' \
  '{"version":2,"platform":"mac","cores":[{"id":"writer_fixture","display_name":"Writer Fixture","type":"path","libretro_name":null,"file_name":null,"config_folder":"Writer Fixture","info_name":null,"path":"emulators/fixture/game-writer-fixture","supports_menu":false,"supports_savestate":true,"supports_disk_control":false,"needs_swap":false,"status":"packaged"}]}' \
  >"$DEFAULTS/cores.json"
printf '%s\n' \
  '{"version":2,"platform":"mac","systems":[{"id":"N64","name":"Nintendo 64","patterns":["N64"],"extensions":["n64"],"archive_extensions":[],"archive_inner_extensions":["n64"],"archive_mode":"pass_through","file_names":[],"ignore_file_names":[],"playlist_extensions":[],"m3u_generation":"none","default_core":"writer_fixture","alternate_cores":[],"rom_root":"Roms/N64","image_root":"Images/N64","bios_notes":[]}]}' \
  >"$DEFAULTS/systems.json"

smoke_daemon_start "$ROOT_DIR" "$LOG" \
    PLATFORM=mac \
    SDCARD_PATH="$PRIMARY" \
    APPS_PATH="$PRIMARY/Apps" \
    USERDATA_PATH="$USERDATA" \
    LOGS_PATH="$LOGS" \
    SAVES_PATH="$PRIMARY/Saves" \
    STATES_PATH="$PRIMARY/States" \
    UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    UMRK_RUNTIME_PATH="$RUNTIME" \
    UMRK_DAEMON_SOCKET="$SOCKET" \
    UMRK_INTERNAL_DATA_PATH="$STATE" \
    JAWAKA_SDCARD_ROOT="$PRIMARY" \
    UMRK_LIFE1_FIXTURE_SERVICE_ID="$SERVICE_ID" \
    UMRK_LIFE1_FIXTURE_SCENARIO="$SCENARIO" \
    "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only
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

if [ "$UNMANAGED_SCENARIO" -eq 0 ]; then
    "$CTL" --socket "$SOCKET" request \
        "{\"v\":1,\"op\":\"run\",\"id\":\"run\",\"service_id\":\"$SERVICE_ID\"}" |
        grep -F '"ok":true' >/dev/null
    for _ in $(seq 1 300); do
        grep -Fx 'ready=1' "$RESULT" >/dev/null 2>&1 && break
        kill -0 "$DAEMON_PID" 2>/dev/null || exit 1
        sleep 0.02
    done
    grep -Fx 'ready=1' "$RESULT" >/dev/null
fi

LAUNCH_START_NS="$(python3 -c 'import time; print(time.monotonic_ns())')"
"$CTL" --socket "$SOCKET" request \
    '{"type":"launch-game","system":"N64","rom_path":"Roms/N64/Barrier.n64"}' |
    grep -F '"type":"ok"' >/dev/null

if [ "$SCENARIO" = game-start-now ]; then
    for _ in $(seq 1 300); do
        grep -Fx 'waiting=1' "$RESULT" >/dev/null 2>&1 && break
        kill -0 "$DAEMON_PID" 2>/dev/null || exit 1
        sleep 0.01
    done
    grep -Fx 'waiting=1' "$RESULT" >/dev/null
    "$CTL" --socket "$SOCKET" request '{"type":"game-start-now"}' |
        grep -F '"type":"ok"' >/dev/null
fi

for _ in $(seq 1 300); do
    [ -f "$RUNTIME/game-writer-live" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || exit 1
    sleep 0.01
done
[ -f "$RUNTIME/game-writer-live" ]
WRITER_LIVE_NS="$(python3 -c 'import time; print(time.monotonic_ns())')"
LAUNCH_TO_WRITER_MS="$(( (WRITER_LIVE_NS - LAUNCH_START_NS) / 1000000 ))"
[ -f "$RUNTIME/active-game.json" ]
if [ "$SUCCESS_SCENARIO" -eq 1 ] && \
   grep -Fx 'finish=1' "$RESULT" >/dev/null 2>&1; then
    echo "game.finish arrived before the writer descendant exited" >&2
    exit 1
fi

for _ in $(seq 1 500); do
    finished=0
    if [ "$SUCCESS_SCENARIO" -eq 1 ]; then
        grep -Fx 'finish=1' "$RESULT" >/dev/null 2>&1 && finished=1
    elif [ "$UNMANAGED_SCENARIO" -eq 1 ]; then
        finished=1
    else
        status="$($CTL --socket "$SOCKET" request \
            "{\"v\":1,\"op\":\"status\",\"id\":\"status\",\"service_id\":\"$SERVICE_ID\"}" 2>/dev/null || true)"
        grep -F '"effective_state":"running"' <<<"$status" >/dev/null 2>&1 && finished=1
    fi
    [ -f "$RUNTIME/game-writer-done" ] && [ ! -f "$RUNTIME/active-game.json" ] && \
        [ "$finished" -eq 1 ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || exit 1
    sleep 0.01
done
[ -f "$RUNTIME/game-writer-done" ]
[ ! -e "$RUNTIME/active-game.json" ]
if [ "$SUCCESS_SCENARIO" -eq 1 ]; then
    grep -Fx 'start=1' "$RESULT" >/dev/null
    grep -Fx 'finish=1' "$RESULT" >/dev/null
    case "$SCENARIO" in
        game-exchange|game-wait-expiry|game-start-now)
            grep -Fx 'cancel=1' "$RESULT" >/dev/null
            ;;
    esac
    case "$SCENARIO" in
        game-waiting|game-wait-expiry|game-start-now)
            grep -Fx 'waiting=1' "$RESULT" >/dev/null
            ;;
    esac
    if [ "$SCENARIO" = game-reconnect ]; then
        grep -Fx 'reconnected=1' "$RESULT" >/dev/null
    fi
elif [ "$UNMANAGED_SCENARIO" -eq 0 ]; then
    grep -Fx 'ready=1' "$RESULT" >/dev/null
    grep -F 'life1: verified service stop service=' "$LOG" >/dev/null
    case "$SCENARIO" in
        game-malformed) grep -F 'reason=malformed-exchange-message' "$LOG" >/dev/null ;;
        game-drop) grep -F 'reason=peer-closed' "$LOG" >/dev/null ;;
        game-timeout) grep -F 'reason=ready-ack-timeout' "$LOG" >/dev/null ;;
        game-stalled)
            grep -F 'reason=waiting-stalled' "$LOG" >/dev/null
            grep -F 'reason=ready-ack-timeout' "$LOG" >/dev/null
            ;;
        game-never-subscribe|game-late-subscribe)
            grep -F 'reason=notify-unsubscribed' "$LOG" >/dev/null
            ;;
    esac
fi
if [ "$SCENARIO" = game-start-now ]; then
    grep -F 'reason=start-now-request' "$LOG" >/dev/null
fi

if [ "$UNMANAGED_SCENARIO" -eq 0 ]; then
    "$CTL" --socket "$SOCKET" request \
        "{\"v\":1,\"op\":\"status\",\"id\":\"status\",\"service_id\":\"$SERVICE_ID\"}" |
        grep -F '"effective_state":"running"' >/dev/null
elif [ "$SCENARIO" = game-disabled ]; then
    "$CTL" --socket "$SOCKET" request \
        "{\"v\":1,\"op\":\"status\",\"id\":\"status\",\"service_id\":\"$SERVICE_ID\"}" |
        grep -F '"effective_state":"disabled"' >/dev/null
fi
grep -F 'life1: active launch committed' "$LOG" >/dev/null
if [ "$UNSUBSCRIBED_SCENARIO" -eq 1 ] || [ "$UNMANAGED_SCENARIO" -eq 1 ]; then
    if grep -F 'life1: game.start service=' "$LOG" >/dev/null; then
        echo "unsubscribed service unexpectedly received game.start" >&2
        exit 1
    fi
else
    grep -F 'life1: game.start service=' "$LOG" >/dev/null
fi
grep -F 'life1: writer started' "$LOG" >/dev/null
grep -F 'life1: game.finish' "$LOG" >/dev/null
if grep -F 'life1: launch status stage=starting' "$LOG" >/dev/null; then
    echo "launch status showed stage=starting" >&2
    exit 1
fi
if [ "$UNMANAGED_SCENARIO" -eq 1 ] &&
   grep -F 'life1: launch status stage=' "$LOG" >/dev/null; then
    echo "unmanaged launch showed a launch status stage" >&2
    exit 1
fi
echo "PASS life1-game-ipc-smoke ($SCENARIO launch_to_writer_ms=$LAUNCH_TO_WRITER_MS)"
