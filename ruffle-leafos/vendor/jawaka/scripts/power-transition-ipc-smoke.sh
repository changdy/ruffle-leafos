#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-power-transition.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PAK="$PRIMARY/Apps/mac/PowerFixture.pak"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
SERVICE_PID_FILE="$TMP_DIR/service.pid"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"
SERVICE_ID="org.umrk.test.powertransition"

cleanup() {
    status=$?
    set +e
    smoke_daemon_stop || status=1
    if [ -f "$SERVICE_PID_FILE" ]; then
        service_pid="$(cat "$SERVICE_PID_FILE" 2>/dev/null || true)"
        case "$service_pid" in
            ''|*[!0-9]*) ;;
            *) kill "$service_pid" 2>/dev/null || true ;;
        esac
    fi
    if [ "$status" -ne 0 ] && [ -f "$LOG" ]; then cat "$LOG" >&2; fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT

make -C "$ROOT_DIR" jawakad jawaka-platformctl >/dev/null
mkdir -p "$PAK/bin" "$STATE" "$RUNTIME" "$USERDATA" "$LOGS"
cat >"$PAK/bin/run.sh" <<'EOF'
#!/bin/sh
echo "$$" >"$SERVICE_PID_FILE"
trap 'exit 0' TERM INT
while :; do sleep 1; done
EOF
chmod 755 "$PAK/bin/run.sh"
cat >"$PAK/pak.json" <<EOF
{"id":"$SERVICE_ID","name":"Power Fixture","platform":"mac","pak_version":"1.0.0","service":{"schema":1,"id":"$SERVICE_ID","run":{"path":"bin/run.sh","args":[]},"default_enabled":false,"stop_grace_ms":300,"restart":"no"}}
EOF

start_daemon() {
    rm -f "$SOCKET" "$SERVICE_PID_FILE"
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
        SERVICE_PID_FILE="$SERVICE_PID_FILE" \
        "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only
    for _ in $(seq 1 200); do
        [ -S "$SOCKET" ] && return 0
        kill -0 "$DAEMON_PID" 2>/dev/null || return 1
        sleep 0.02
    done
    return 1
}

request() {
    "$CTL" --socket "$SOCKET" request "$1"
}
ctl1() {
    op="$1"
    request "{\"v\":1,\"op\":\"$op\",\"id\":\"$op\",\"service_id\":\"$SERVICE_ID\"}"
}
status_field() {
    field="$1"
    ctl1 status | python3 -c "import json,sys; d=json.load(sys.stdin); print(d$field)"
}
wait_state() {
    want="$1"
    for _ in $(seq 1 250); do
        [ "$(status_field "['effective_state']")" = "$want" ] && return 0
        sleep 0.02
    done
    return 1
}
wait_daemon_exit() {
    for _ in $(seq 1 250); do
        kill -0 "$DAEMON_PID" 2>/dev/null || {
            wait "$DAEMON_PID"
            DAEMON_PID=""
            return 0
        }
        sleep 0.02
    done
    return 1
}

inode_of() {
    case "$(uname -s)" in
        Darwin) stat -f '%i' "$1" ;;
        *) stat -c '%i' "$1" ;;
    esac
}

start_daemon
ctl1 enable | grep -F '"ok":true' >/dev/null
ctl1 run | grep -F '"ok":true' >/dev/null
wait_state running
test -s "$SERVICE_PID_FILE"
lease="$RUNTIME/services/$SERVICE_ID/generation.lease"
test -f "$lease"
lease_inode="$(inode_of "$lease")"

for action in reboot poweroff; do
    if [ "$action" = poweroff ]; then
        start_daemon
        wait_state running
        test -s "$SERVICE_PID_FILE"
    fi
    service_pid="$(cat "$SERVICE_PID_FILE")"
    request "{\"type\":\"platform-action\",\"action\":\"$action\"}" |
        grep -F '"code":"ok"' >/dev/null
    wait_daemon_exit
    if kill -0 "$service_pid" 2>/dev/null; then
        echo "$action left service pid $service_pid running" >&2
        exit 1
    fi
    test -f "$lease"
    test "$(inode_of "$lease")" = "$lease_inode"
done

echo "PASS power-transition-ipc-smoke"
