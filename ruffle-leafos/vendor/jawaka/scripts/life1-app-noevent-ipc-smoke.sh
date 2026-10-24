#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-life1-app.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
SERVICE_PAK="$PRIMARY/Apps/mac/Life1Fixture.pak"
APP_PAK="$PRIMARY/Apps/mac/AppFixture.pak"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
SERVICE_ID="org.umrk.test.life1app"
RESULT="$RUNTIME/services/$SERVICE_ID/life1-fixture-result"
APP_DONE="$RUNTIME/app-fixture-done"
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
    "$BUILD_REL/bin/life1-fixture-service" >/dev/null
mkdir -p "$SERVICE_PAK/bin" "$APP_PAK" "$STATE" "$RUNTIME" \
         "$USERDATA" "$LOGS"
cp "$ROOT_DIR/$BUILD_REL/bin/life1-fixture-service" \
   "$SERVICE_PAK/bin/life1-fixture-service"
chmod 755 "$SERVICE_PAK/bin/life1-fixture-service"
printf '%s\n' \
  "{\"id\":\"$SERVICE_ID\",\"name\":\"LIFE-1 App Fixture\",\"platform\":\"mac\",\"pak_version\":\"1.0.0\",\"service\":{\"schema\":1,\"id\":\"$SERVICE_ID\",\"run\":{\"path\":\"bin/life1-fixture-service\",\"args\":[]},\"default_enabled\":false,\"stop_grace_ms\":300,\"restart\":\"no\",\"lifecycle\":{\"game\":\"notify\"}}}" \
  >"$SERVICE_PAK/pak.json"
printf '%s\n' \
  '{"name":"Foreground App Fixture","platform":"mac","pak_version":"1.0.0"}' \
  >"$APP_PAK/pak.json"
# The generated launcher expands this at runtime, not while the fixture is made.
# shellcheck disable=SC2016
printf '%s\n' '#!/bin/sh' 'sleep 0.15' \
  'printf "1\n" >"$UMRK_RUNTIME_PATH/app-fixture-done"' \
  >"$APP_PAK/launch.sh"
chmod 755 "$APP_PAK/launch.sh"

smoke_daemon_start "$ROOT_DIR" "$LOG" \
    PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$PRIMARY/Apps" \
    USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
    UMRK_RUNTIME_PATH="$RUNTIME" UMRK_DAEMON_SOCKET="$SOCKET" \
    UMRK_INTERNAL_DATA_PATH="$STATE" JAWAKA_SDCARD_ROOT="$PRIMARY" \
    UMRK_LIFE1_FIXTURE_SERVICE_ID="$SERVICE_ID" \
    UMRK_LIFE1_FIXTURE_SCENARIO=game-exchange \
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

"$CTL" --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"run\",\"id\":\"run\",\"service_id\":\"$SERVICE_ID\"}" |
    grep -F '"ok":true' >/dev/null
for _ in $(seq 1 300); do
    if grep -Fx 'ready=1' "$RESULT" >/dev/null 2>&1; then
        service_status="$($CTL --socket "$SOCKET" request \
            "{\"v\":1,\"op\":\"status\",\"id\":\"settle\",\"service_id\":\"$SERVICE_ID\"}" \
            2>/dev/null || true)"
        grep -F '"effective_state":"running"' <<<"$service_status" >/dev/null 2>&1 && break
    fi
    sleep 0.02
done
grep -Fx 'ready=1' "$RESULT" >/dev/null
before="$($CTL --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"status\",\"id\":\"before\",\"service_id\":\"$SERVICE_ID\"}")"
before_pgid="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["ownership_identity"]["pgid"])' <<<"$before")"

"$CTL" --socket "$SOCKET" request \
    '{"type":"launch-app","pak_dir":"Apps/mac/AppFixture.pak"}' |
    grep -F '"type":"ok"' >/dev/null
for _ in $(seq 1 300); do
    [ -f "$APP_DONE" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || exit 1
    sleep 0.01
done
[ -f "$APP_DONE" ]
after="$($CTL --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"status\",\"id\":\"after\",\"service_id\":\"$SERVICE_ID\"}")"
after_pgid="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["ownership_identity"]["pgid"])' <<<"$after")"
[ "$before_pgid" = "$after_pgid" ]
grep -F '"effective_state":"running"' <<<"$after" >/dev/null
grep -F '"coordination":"subscribed"' <<<"$after" >/dev/null
[ ! -e "$RUNTIME/active-game.json" ]
if grep -F 'life1: game.start service=' "$LOG" >/dev/null ||
   grep -F 'life1: verified service stop service=' "$LOG" >/dev/null; then
    echo ".pak launch disturbed LIFE-1 service" >&2
    exit 1
fi
echo "PASS life1-app-noevent-ipc-smoke"
