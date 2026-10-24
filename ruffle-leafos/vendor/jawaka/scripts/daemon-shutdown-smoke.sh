#!/usr/bin/env bash
# jawakad has to exit on SIGTERM while it supervises a launcher. The shutdown
# branch never reaches the loop's poll, so this guards that child exits are
# still noticed there (a launcher left unreaped kept the daemon, and a reboot,
# waiting forever).
#
# The daemon is started directly, never inside a ( ... ) subshell, so
# DAEMON_PID is jawakad itself and cleanup can always kill it; JAWAKA_OSD=0
# keeps it from opening an OSD window on a desktop host.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-daemon-shutdown.XXXXXX")"
BIN="$TMP_DIR/bin"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
LOG="$TMP_DIR/jawakad.log"
LAUNCHER_PID_FILE="$TMP_DIR/launcher.pid"

cleanup() {
    status=$?
    set +e
    if [ -n "${DAEMON_PID:-}" ] && kill -0 "$DAEMON_PID" 2>/dev/null; then
        kill -KILL "$DAEMON_PID" 2>/dev/null
        wait "$DAEMON_PID" 2>/dev/null
    fi
    if [ -f "$LAUNCHER_PID_FILE" ]; then
        kill -KILL "$(cat "$LAUNCHER_PID_FILE")" 2>/dev/null
    fi
    if [ "$status" -ne 0 ] && [ -f "$LOG" ]; then tail -40 "$LOG" >&2; fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT

make -C "$ROOT_DIR" jawakad >/dev/null
mkdir -p "$BIN" "$STATE" "$RUNTIME" "$LOGS" "$PRIMARY/Apps"
cp "$ROOT_DIR/$BUILD_REL/bin/jawakad" "$BIN/jawakad"
cat >"$BIN/jawaka-launcher" <<'SCRIPT'
#!/bin/sh
echo "$$" >"$LAUNCHER_PID_FILE"
trap 'exit 0' TERM INT
while :; do sleep 1; done
SCRIPT
chmod 755 "$BIN/jawaka-launcher"

cd "$TMP_DIR"
PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$PRIMARY/Apps" \
USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" UMRK_RUNTIME_PATH="$RUNTIME" \
UMRK_DAEMON_SOCKET="$RUNTIME/jawakad.sock" UMRK_INTERNAL_DATA_PATH="$STATE" \
JAWAKA_SDCARD_ROOT="$PRIMARY" JAWAKA_OSD=0 LAUNCHER_PID_FILE="$LAUNCHER_PID_FILE" \
    "$BIN/jawakad" >>"$LOG" 2>&1 &
DAEMON_PID=$!

for _ in $(seq 1 200); do
    [ -s "$LAUNCHER_PID_FILE" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || { echo "jawakad exited before spawning the launcher" >&2; exit 1; }
    sleep 0.05
done
[ -s "$LAUNCHER_PID_FILE" ] || { echo "launcher never started" >&2; exit 1; }
launcher_pid="$(cat "$LAUNCHER_PID_FILE")"

kill -TERM "$DAEMON_PID"
for _ in $(seq 1 100); do
    kill -0 "$DAEMON_PID" 2>/dev/null || break
    sleep 0.1
done
if kill -0 "$DAEMON_PID" 2>/dev/null; then
    echo "FAIL daemon-shutdown-smoke: jawakad still running 10 s after SIGTERM" >&2
    exit 1
fi
wait "$DAEMON_PID" 2>/dev/null || true
DAEMON_PID=
if kill -0 "$launcher_pid" 2>/dev/null; then
    echo "FAIL daemon-shutdown-smoke: launcher outlived the daemon" >&2
    exit 1
fi
echo "PASS daemon-shutdown-smoke"
