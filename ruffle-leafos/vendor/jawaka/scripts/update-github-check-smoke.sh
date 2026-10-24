#!/bin/bash
# Release-check behaviour against a local GitHub-shaped HTTPS server:
#   - the routine check stops at the newest compatible release, and reuses one
#     connection for the release list and every manifest it reads
#   - the release-list load fetches the manifests concurrently
#   - a transfer that stalls mid-response fails instead of hanging the check
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD:-build/update-github-check-smoke}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-update-github.XXXXXX")"
SERVER_PID=""
cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null || true
        wait "$SERVER_PID" 2>/dev/null || true
    fi
    rm -rf "$TMP_ROOT"
}
trap cleanup EXIT

STATE_DIR="$TMP_ROOT/state"
mkdir -p "$STATE_DIR"
cat >"$STATE_DIR/release.json" <<'JSON'
{
  "schema": 1,
  "product": "leaf",
  "platform": "mlp1",
  "version": "v0.1.0",
  "release_id": "v0.1.0"
}
JSON

openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj "/CN=127.0.0.1" \
    -keyout "$TMP_ROOT/key.pem" -out "$TMP_ROOT/cert.pem" >/dev/null 2>&1

cat >"$TMP_ROOT/server.py" <<'PY'
import http.server
import json
import ssl
import sys
import threading
import time

root = sys.argv[1]
log_path = root + "/requests.jsonl"
lock = threading.RLock()
in_flight = 0

# Newest first, like the GitHub API. v0.5.0 has no mlp1 build, so the routine
# check has to read past it; the draft, prerelease and asset-less releases are
# never fetched.
RELEASES = [
    ("v0.6.0-draft", {"draft": True}),
    ("v0.5.1-rc", {"prerelease": True}),
    ("v0.5.0", {}),
    ("v0.4.1-no-meta", {"no_manifest": True}),
    ("v0.4.0", {}),
    ("v0.3.0", {}),
    ("v0.2.0", {}),
]


def manifest(tag):
    platforms = {}
    if tag != "v0.5.0":
        platforms["mlp1"] = {
            "min_installed_schema": 1,
            "artifact": {
                "kind": "sd_root_zip",
                "name": "leaf-mlp1-sd-%s.zip" % tag,
                "size": 123,
                "installed_size": 456,
                "sha256": "0123456789abcdef" * 4,
            },
        }
    else:
        platforms["tg5040"] = {}
    return {"schema": 1, "product": "leaf", "release_id": tag,
            "version": tag, "platforms": platforms}


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def record(self, **fields):
        fields.update(path=self.path, port=self.client_address[1])
        with lock:
            with open(log_path, "a", encoding="utf-8") as fp:
                fp.write(json.dumps(fields) + "\n")

    def send_body(self, status, body, content_type="application/json"):
        data = body.encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self):
        global in_flight
        base = "https://127.0.0.1:%d" % self.server.server_address[1]
        if self.path == "/releases":
            self.record(kind="list")
            items = []
            for tag, flags in RELEASES:
                assets = [{"name": "leaf-mlp1-sd-%s.zip" % tag,
                           "browser_download_url": "%s/dl/%s/leaf-mlp1-sd-%s.zip" % (base, tag, tag)}]
                if not flags.get("no_manifest"):
                    assets.append({"name": "leaf-update.json",
                                   "browser_download_url": "%s/dl/%s/leaf-update.json" % (base, tag)})
                items.append({"tag_name": tag, "draft": flags.get("draft", False),
                              "prerelease": flags.get("prerelease", False),
                              "published_at": "2026-01-01T00:00:00Z",
                              "html_url": "%s/notes/%s" % (base, tag),
                              "assets": assets})
            self.send_body(200, json.dumps(items))
        elif self.path.startswith("/dl/"):
            # GitHub answers asset downloads with a redirect to its CDN.
            self.record(kind="redirect")
            self.send_response(302)
            self.send_header("Location", "/cdn/" + self.path[len("/dl/"):])
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif self.path.startswith("/cdn/") and self.path.endswith("/leaf-update.json"):
            tag = self.path.split("/")[2]
            with lock:
                in_flight += 1
                self.record(kind="manifest", tag=tag, in_flight=in_flight)
            time.sleep(0.3)
            with lock:
                in_flight -= 1
            self.send_body(200, json.dumps(manifest(tag)))
        elif self.path == "/stall":
            self.record(kind="stall")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", "1000")
            self.end_headers()
            self.wfile.write(b"[")
            self.wfile.flush()
            time.sleep(30)
        else:
            self.send_body(404, "{}")


server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
server.daemon_threads = True
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(root + "/cert.pem", root + "/key.pem")
server.socket = context.wrap_socket(server.socket, server_side=True)
with open(root + "/port", "w", encoding="utf-8") as fp:
    fp.write(str(server.server_address[1]))
server.serve_forever()
PY

python3 "$TMP_ROOT/server.py" "$TMP_ROOT" &
SERVER_PID=$!
for _ in $(seq 1 50); do
    [ -s "$TMP_ROOT/port" ] && break
    sleep 0.1
done
PORT="$(cat "$TMP_ROOT/port")"
BASE="https://127.0.0.1:$PORT"

make -C "$JAWAKA_DIR" BUILD="$BUILD_DIR" jawaka-update-smoke >/dev/null
BIN="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-update-smoke"
export JAWAKA_UPDATE_INSECURE_TLS=1

# Routine check: stops at v0.4.0, the newest release with an mlp1 build.
: >"$TMP_ROOT/requests.jsonl"
"$BIN" --state-dir "$STATE_DIR" --platform mlp1 \
    --releases-url "$BASE/releases" >"$TMP_ROOT/latest.out"
python3 - "$TMP_ROOT/latest.out" "$TMP_ROOT/requests.jsonl" <<'PY'
import json
import sys
status = json.load(open(sys.argv[1], encoding="utf-8"))
requests = [json.loads(line) for line in open(sys.argv[2], encoding="utf-8")]
assert status["state"] == "available", status
assert status["candidate"]["release_id"] == "v0.4.0", status
assert [o["release_id"] for o in status["options"]] == ["v0.4.0"], status["options"]
assert status["options_complete"] is False, status
manifests = [r["tag"] for r in requests if r["kind"] == "manifest"]
assert manifests == ["v0.5.0", "v0.4.0"], manifests
ports = {r["port"] for r in requests}
assert len(ports) == 1, "expected one reused connection, saw %d: %s" % (len(ports), requests)
PY

# Release-list load: every manifest, fetched concurrently.
: >"$TMP_ROOT/requests.jsonl"
"$BIN" --state-dir "$STATE_DIR" --platform mlp1 \
    --releases-url "$BASE/releases" --all >"$TMP_ROOT/all.out"
python3 - "$TMP_ROOT/all.out" "$TMP_ROOT/requests.jsonl" <<'PY'
import json
import sys
status = json.load(open(sys.argv[1], encoding="utf-8"))
requests = [json.loads(line) for line in open(sys.argv[2], encoding="utf-8")]
assert status["candidate"]["release_id"] == "v0.4.0", status
assert [o["release_id"] for o in status["options"]] == ["v0.4.0", "v0.3.0", "v0.2.0"], status["options"]
assert status["options_complete"] is True, status
manifests = sorted(r["tag"] for r in requests if r["kind"] == "manifest")
assert manifests == ["v0.2.0", "v0.3.0", "v0.4.0", "v0.5.0"], manifests
peak = max(r["in_flight"] for r in requests if r["kind"] == "manifest")
assert peak >= 2, "manifests were fetched one at a time"
PY

# The daemon's async job: a list load asked for mid-check queues behind it, and
# a reload keeps the chosen release and its download.
JAWAKA_UPDATE_RELEASES_URL="$BASE/releases" \
    "$BIN" --state-dir "$STATE_DIR" --platform mlp1 --job-scenario >"$TMP_ROOT/job.out"
python3 - "$TMP_ROOT/job.out" <<'PY'
import json
import sys
out = json.load(open(sys.argv[1], encoding="utf-8"))
assert out["state_while_checking"] == "checking", out
assert out["loading_after_queue"] is True, out
first = out["after_queue"]
assert first["state"] == "available", first
assert first["candidate"]["release_id"] == "v0.4.0", first
assert len(first["options"]) == 3 and first["options_complete"] is True, first
assert first["selected_option"] == 0 and first["options_loading"] is False, first
# A list load never flips the page into "checking".
assert out["state_while_loading"] == "available", out
again = out["after_reload"]
assert again["candidate"]["release_id"] == "v0.2.0", again
assert again["downloaded"] is True, again
assert again["selected_option"] == out["picked"] == 2, again
assert again["options_complete"] is True and again["options_loading"] is False, again
# A routine check drops the old list's "complete" until it lands, so a list
# request during it queues a fresh load.
assert out["complete_while_rechecking"] is False, out
assert out["loading_after_second_queue"] is True, out
last = out["after_recheck"]
assert last["candidate"]["release_id"] == "v0.4.0", last
assert len(last["options"]) == 3 and last["options_complete"] is True, last
assert last["options_loading"] is False, last
PY

# A response that stops arriving fails once the stall window passes.
export JAWAKA_UPDATE_STALL_SECONDS=2
started=$(date +%s)
if "$BIN" --state-dir "$STATE_DIR" --platform mlp1 \
    --releases-url "$BASE/stall" >"$TMP_ROOT/stall.out"; then
    echo "stalled release list unexpectedly succeeded" >&2
    exit 1
fi
elapsed=$(( $(date +%s) - started ))
if [ "$elapsed" -gt 10 ]; then
    echo "stalled transfer took ${elapsed}s to fail" >&2
    exit 1
fi
python3 - "$TMP_ROOT/stall.out" <<'PY'
import json
import sys
status = json.load(open(sys.argv[1], encoding="utf-8"))
assert status["state"] == "error", status
assert "libcurl download failed" in status["message"], status
PY

echo "update-github-check-smoke: ok"
