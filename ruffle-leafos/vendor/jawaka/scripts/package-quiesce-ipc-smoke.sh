#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-package-ipc.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PAK="$PRIMARY/Apps/mac/PackageFixture.pak"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
CTL="$ROOT_DIR/build/bin/jawaka-platformctl"
SERVICE_ID="org.umrk.test.packageipc"

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
mkdir -p "$PAK/bin" "$STATE" "$RUNTIME" "$USERDATA" "$LOGS"
cat >"$PAK/bin/run.sh" <<'EOF'
#!/bin/sh
trap 'exit 0' TERM INT
while :; do sleep 1; done
EOF
chmod 755 "$PAK/bin/run.sh"

write_manifest() {
    version="$1"
    cat >"$PAK/pak.json" <<EOF
{"id":"$SERVICE_ID","name":"Package Fixture","platform":"mac","pak_version":"$version","service":{"schema":1,"id":"$SERVICE_ID","run":{"path":"bin/run.sh","args":[]},"default_enabled":false,"stop_grace_ms":300,"restart":"no"}}
EOF
}
write_manifest 1.0.0

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
    build/bin/jawakad --daemon-only
for _ in $(seq 1 200); do
    [ -S "$SOCKET" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || exit 1
    sleep 0.02
done
[ -S "$SOCKET" ]

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

ctl1 enable | grep -F '"ok":true' >/dev/null
ctl1 run | grep -F '"ok":true' >/dev/null
wait_state running

request '{"type":"package-quiesce-begin","operation_id":"ipc-smoke"}' |
    grep -F '"type":"ok"' >/dev/null
[ "$(status_field "['lifecycle_policy_stop']['reason']")" = package ]
[ "$(status_field "['desired_enabled']")" = True ]

ctl1 run | grep -F '"code":"package-in-progress"' >/dev/null
request '{"type":"launch-app","pak_dir":"Apps/mac/Anything.pak"}' |
    grep -F 'package operation in progress' >/dev/null

write_manifest 2.0.0
request '{"type":"package-quiesce-end","operation_id":"ipc-smoke"}' |
    grep -F '"type":"ok"' >/dev/null
wait_state running
[ "$(status_field "['installed_package']['version']")" = 2.0.0 ]
[ "$(status_field "['lifecycle_policy_stop']['active']")" = False ]

echo "PASS package-quiesce-ipc-smoke"
