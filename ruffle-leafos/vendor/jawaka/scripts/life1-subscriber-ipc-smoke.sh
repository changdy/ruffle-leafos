#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-life1-subscriber.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PAK="$PRIMARY/Apps/mac/Life1Fixture.pak"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
SERVICE_ID="org.umrk.test.life1"
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
    "$BUILD_REL/bin/life1-fixture-service" >/dev/null
mkdir -p "$PAK/bin" "$STATE" "$RUNTIME" "$USERDATA" "$LOGS"
cp "$ROOT_DIR/$BUILD_REL/bin/life1-fixture-service" "$PAK/bin/life1-fixture-service"
chmod 755 "$PAK/bin/life1-fixture-service"
printf '%s\n' \
  "{\"id\":\"$SERVICE_ID\",\"name\":\"LIFE-1 Fixture\",\"platform\":\"mac\",\"pak_version\":\"1.0.0\",\"service\":{\"schema\":1,\"id\":\"$SERVICE_ID\",\"run\":{\"path\":\"bin/life1-fixture-service\",\"args\":[]},\"default_enabled\":false,\"stop_grace_ms\":300,\"restart\":\"no\",\"lifecycle\":{\"game\":\"notify\"}}}" \
  >"$PAK/pak.json"

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
    UMRK_LIFE1_FIXTURE_SERVICE_ID="$SERVICE_ID" \
    "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only
for _ in $(seq 1 250); do
    [ -S "$SOCKET" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || exit 1
    sleep 0.02
done
[ -S "$SOCKET" ]

"$CTL" --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"run\",\"id\":\"run\",\"service_id\":\"$SERVICE_ID\"}" |
    grep -F '"ok":true' >/dev/null
for _ in $(seq 1 250); do
    [ -f "$RESULT" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || exit 1
    sleep 0.02
done
grep -Fx 'subscribed=2' "$RESULT" >/dev/null
grep -Fx 'reconciled=2' "$RESULT" >/dev/null
grep -Fx 'replaced=1' "$RESULT" >/dev/null

# Real kernel credentials, no test injection: a foreground process declaring
# the service id is rejected. A second slow client then proves a partial frame
# cannot stall an ordinary one-shot CTL request and is closed at one second.
python3 - "$SOCKET" "$SERVICE_ID" <<'PY'
import json, socket, struct, sys, time

path, service_id = sys.argv[1:]

def frame(payload):
    raw = json.dumps(payload, separators=(",", ":")).encode()
    return struct.pack("!I", len(raw)) + raw

def recv_frame(sock):
    prefix = sock.recv(4)
    assert len(prefix) == 4, prefix
    length = struct.unpack("!I", prefix)[0]
    chunks = bytearray()
    while len(chunks) < length:
        chunk = sock.recv(length - len(chunks))
        assert chunk
        chunks.extend(chunk)
    return json.loads(chunks)

impostor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
impostor.settimeout(1.0)
impostor.connect(path)
impostor.sendall(frame({"v": 1, "op": "subscribe", "id": "bad",
    "events": ["game"], "service_id": service_id, "mode": "notify",
    "ack_ms": 250, "wait_ms": 0}))
reply = recv_frame(impostor)
assert reply["error"]["code"] == "foreground-app-peer", reply
assert impostor.recv(1) == b""
impostor.close()

slow = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
slow.settimeout(2.0)
slow.connect(path)
slow.sendall(b"\x00\x00")

ordinary = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
ordinary.settimeout(0.5)
ordinary.connect(path)
ordinary.sendall(frame({"v": 1, "op": "status", "id": "status",
                        "service_id": service_id}))
status = recv_frame(ordinary)
assert status["service_id"] == service_id, status
ordinary.close()

time.sleep(1.1)
assert slow.recv(1) == b""
slow.close()
PY

grep -F "life1: subscribed service=$SERVICE_ID" "$LOG" >/dev/null
grep -F "life1: reconciled service=$SERVICE_ID active=false" "$LOG" >/dev/null
grep -F 'reason=replaced-by-resubscribe' "$LOG" >/dev/null
echo "PASS life1-subscriber-ipc-smoke"
