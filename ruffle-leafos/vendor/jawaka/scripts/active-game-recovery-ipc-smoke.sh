#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-active-recovery.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PAK="$PRIMARY/Apps/mac/RecoveryFixture.pak"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
SERVICE_ID="org.umrk.test.recovery"
STARTED="$RUNTIME/recovery-service-started"
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

make -C "$ROOT_DIR" jawakad jawaka-platformctl >/dev/null
mkdir -p "$PAK/bin" "$STATE" "$RUNTIME" "$USERDATA" "$LOGS" \
         "$PRIMARY/Saves" "$PRIMARY/States"
# The generated service script expands this when Jawaka launches it.
# shellcheck disable=SC2016
printf '%s\n' '#!/bin/sh' \
    'printf "1\n" >"$UMRK_RUNTIME_PATH/recovery-service-started"' \
    'while :; do sleep 1; done' >"$PAK/bin/run.sh"
chmod 755 "$PAK/bin/run.sh"
printf '%s\n' \
  "{\"id\":\"$SERVICE_ID\",\"name\":\"Recovery Fixture\",\"platform\":\"mac\",\"pak_version\":\"1.0.0\",\"service\":{\"schema\":1,\"id\":\"$SERVICE_ID\",\"run\":{\"path\":\"bin/run.sh\",\"args\":[]},\"default_enabled\":false,\"stop_grace_ms\":300,\"restart\":\"no\",\"lifecycle\":{\"game\":\"notify\"}}}" \
  >"$PAK/pak.json"

start_daemon() {
    rm -f "$SOCKET"
    smoke_daemon_start "$ROOT_DIR" "$LOG" \
        PLATFORM=mac \
        SDCARD_PATH="$PRIMARY" \
        APPS_PATH="$PRIMARY/Apps" \
        USERDATA_PATH="$USERDATA" \
        LOGS_PATH="$LOGS" \
        UMRK_RUNTIME_PATH="$RUNTIME" \
        UMRK_DAEMON_SOCKET="$SOCKET" \
        UMRK_INTERNAL_DATA_PATH="$STATE" \
        JAWAKA_SDCARD_ROOT="$PRIMARY" \
        "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only
    for _ in $(seq 1 300); do
        [ -S "$SOCKET" ] && return 0
        kill -0 "$DAEMON_PID" 2>/dev/null || return 1
        sleep 0.02
    done
    return 1
}

start_daemon
"$CTL" --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"enable\",\"id\":\"enable\",\"service_id\":\"$SERVICE_ID\"}" |
    grep -F '"ok":true' >/dev/null
smoke_daemon_stop

printf '%s' \
  "{\"launch_id\":\"recovered-launch\",\"source_id\":\"primary\",\"saves_path\":\"$PRIMARY/Saves\",\"states_path\":\"$PRIMARY/States\"}" \
  >"$RUNTIME/active-game.json"
start_daemon

# Give normal autostart more than enough ticks; the recovered gate must retain
# the pending intent without ever forking the game-sensitive service.
sleep 1.5
[ ! -e "$STARTED" ]
status="$($CTL --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"status\",\"id\":\"status\",\"service_id\":\"$SERVICE_ID\"}")"
grep -F '"desired_enabled":true' <<<"$status" >/dev/null
grep -F '"pgid":null' <<<"$status" >/dev/null

"$CTL" --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"run\",\"id\":\"run\",\"service_id\":\"$SERVICE_ID\"}" \
    >"$TMP_DIR/run.out" 2>"$TMP_DIR/run.err" || true
grep -F 'lifecycle-in-progress' "$TMP_DIR/run.out" >/dev/null
[ -f "$RUNTIME/active-game.json" ]
[ ! -e "$STARTED" ]
grep -F 'life1: recovered active launch id=recovered-launch' "$LOG" >/dev/null
grep -F 'game-sensitive starts suppressed' "$LOG" >/dev/null
echo "PASS active-game-recovery-ipc-smoke"
