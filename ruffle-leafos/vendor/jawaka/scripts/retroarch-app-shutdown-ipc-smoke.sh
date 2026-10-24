#!/usr/bin/env bash
# Leaf#48 Branch C: only the RetroArch app tile gets a stop grace.
#
# The runner needs time on shutdown to talk RetroArch into saving and copy the
# config back. Every other .pak app must keep the old, immediate SIGTERM ->
# SIGKILL behavior -- Thing-File, SSH Server, PortMaster and the rest have no
# save handshake to wait for, and a shared grace would make every poweroff
# slower for nothing.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-ra-app.XXXXXX")"

cleanup() {
    status=$?
    set +e
    smoke_daemon_stop || status=1
    pkill -f "$TMP_DIR" 2>/dev/null
    if [ "$status" -ne 0 ]; then
        for log in "$TMP_DIR"/*.log; do [ -f "$log" ] && cat "$log" >&2; done
    fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT

fail() { echo "retroarch-app-shutdown-ipc-smoke: $1" >&2; exit 1; }

make -C "$ROOT_DIR" jawakad jawaka-platformctl >/dev/null

PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
mkdir -p "$STATE" "$USERDATA" "$LOGS"

# Two apps that differ only in the one thing the daemon keys off: the pak's
# name. Both hold the foreground until killed, so the shutdown path is what
# ends them.
make_pak() {
    pak="$PRIMARY/Apps/mac/$1.pak"
    mkdir -p "$pak"
    printf '{"name":"%s","platform":"mac","pak_version":"1.0.0"}\n' "$1" \
        >"$pak/pak.json"
    # shellcheck disable=SC2016
    printf '%s\n' '#!/bin/sh' \
        'printf "%s\n" "$$" >"$UMRK_RUNTIME_PATH/app-started"' \
        'while true; do sleep 0.1; done' >"$pak/launch.sh"
    chmod 755 "$pak/launch.sh"
}
make_pak RetroArch
make_pak GenericFixture

# Launch one app, then stop the daemon and hand back its log.
run_case() {
    case_name="$1"
    pak_rel="$2"
    RUNTIME="$TMP_DIR/runtime-$case_name"
    SOCKET="$RUNTIME/jawakad.sock"
    LOG="$TMP_DIR/$case_name.log"
    mkdir -p "$RUNTIME"

    # DAEMON_PID is jawakad itself: this test signals the daemon and reads
    # what its shutdown path does, which a surviving subshell would hide.
    smoke_daemon_start "$ROOT_DIR" "$LOG" \
        PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$PRIMARY/Apps" \
        USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
        UMRK_RUNTIME_PATH="$RUNTIME" UMRK_DAEMON_SOCKET="$SOCKET" \
        UMRK_INTERNAL_DATA_PATH="$STATE" JAWAKA_SDCARD_ROOT="$PRIMARY" \
        "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only

    for _ in $(seq 1 500); do
        [ -S "$SOCKET" ] && break
        kill -0 "$DAEMON_PID" 2>/dev/null || fail "$case_name: daemon died at startup"
        sleep 0.02
    done
    [ -S "$SOCKET" ] || fail "$case_name: daemon socket never appeared"

    "$CTL" --socket "$SOCKET" request \
        "{\"type\":\"launch-app\",\"pak_dir\":\"$pak_rel\"}" |
        grep -F '"type":"ok"' >/dev/null || fail "$case_name: launch-app refused"

    for _ in $(seq 1 500); do
        [ -f "$RUNTIME/app-started" ] && break
        kill -0 "$DAEMON_PID" 2>/dev/null || fail "$case_name: daemon died"
        sleep 0.02
    done
    [ -f "$RUNTIME/app-started" ] || fail "$case_name: app never started"

    kill -TERM "$DAEMON_PID"
    for _ in $(seq 1 1500); do
        kill -0 "$DAEMON_PID" 2>/dev/null || break
        sleep 0.02
    done
    if kill -0 "$DAEMON_PID" 2>/dev/null; then
        fail "$case_name: daemon did not shut down"
    fi
    wait "$DAEMON_PID" 2>/dev/null || true
    DAEMON_PID=
}

run_case retroarch "Apps/mac/RetroArch.pak"
grep -F 'shutdown: asked RetroArch app pid=' "$TMP_DIR/retroarch.log" >/dev/null ||
    fail "the RetroArch app tile was not given a stop grace"
# Group reserved, and reserved to the runner's own pid: a group signal aimed
# anywhere else could reach jawakad or an unrelated app.
ra_pid="$(grep -o 'spawned app pid=[0-9]* pgid=[0-9]*' "$TMP_DIR/retroarch.log" |
    tail -1 | sed 's/.*pid=\([0-9]*\) pgid=.*/\1/')"
ra_pgid="$(grep -o 'spawned app pid=[0-9]* pgid=[0-9]*' "$TMP_DIR/retroarch.log" |
    tail -1 | sed 's/.*pgid=//')"
[ -n "$ra_pid" ] && [ "$ra_pid" = "$ra_pgid" ] ||
    fail "RetroArch app pgid=$ra_pgid was not reserved to its own pid=$ra_pid"

run_case generic "Apps/mac/GenericFixture.pak"
grep -F 'shutdown: asked RetroArch app pid=' "$TMP_DIR/generic.log" >/dev/null &&
    fail "a generic .pak app inherited the RetroArch stop grace"
# No group of its own either: generic apps keep the plain single-pid shutdown.
grep -F 'pgid=-1' "$TMP_DIR/generic.log" >/dev/null ||
    fail "a generic .pak app was given a process group"

echo "PASS retroarch-app-shutdown-ipc-smoke"
