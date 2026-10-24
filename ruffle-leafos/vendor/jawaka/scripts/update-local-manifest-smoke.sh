#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD:-build/update-local-manifest-smoke}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-update-manifest.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT

STATE_DIR="$TMP_ROOT/state"
mkdir -p "$STATE_DIR"
cat >"$STATE_DIR/release.json" <<'JSON'
{
  "schema": 1,
  "product": "leaf",
  "platform": "mlp1",
  "version": "v0.5.2",
  "release_id": "v0.5.2"
}
JSON

write_manifest() {
    local path="$1"
    local url_line="$2"
    cat >"$path" <<JSON
{
  "schema": 1,
  "product": "leaf",
  "channel": "stable",
  "version": "v0.5.3-rc.1",
  "release_id": "v0.5.3-rc.1",
  "platforms": {
    "mlp1": {
      "min_installed_schema": 1,
      "managed_apps": [],
      "migrations": [],
      "handoff": {
        "type": "stock_loong_upgrade",
        "completion": "reboot",
        "trigger_file": "loong_upgrade"
      },
      "artifact": {
        "kind": "sd_root_zip",
        "name": "leaf-mlp1-sd-v0.5.3-rc.1.zip",
        "size": 123,
        "installed_size": 456,
        "sha256": "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"$url_line
      }
    }
  }
}
JSON
}

make -C "$JAWAKA_DIR" BUILD="$BUILD_DIR" jawaka-update-smoke >/dev/null
BIN="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-update-smoke"

HTTPS_MANIFEST="$TMP_ROOT/https.json"
NO_URL_MANIFEST="$TMP_ROOT/no-url.json"
HTTP_MANIFEST="$TMP_ROOT/http.json"
write_manifest "$HTTPS_MANIFEST" ',
        "url": "https://github.com/Utility-Muffin-Research-Kitchen/Leaf/releases/download/v0.5.3-rc.1/leaf-mlp1-sd-v0.5.3-rc.1.zip"'
write_manifest "$NO_URL_MANIFEST" ''
write_manifest "$HTTP_MANIFEST" ',
        "url": "http://example.invalid/leaf.zip"'

"$BIN" --state-dir "$STATE_DIR" --platform mlp1 --manifest "$HTTPS_MANIFEST" \
    >"$TMP_ROOT/https.out"
python3 - "$TMP_ROOT/https.out" <<'PY'
import json
import sys
status = json.load(open(sys.argv[1], encoding="utf-8"))
assert status["state"] == "available", status
assert status["artifact_url"].startswith("https://github.com/"), status
PY

"$BIN" --state-dir "$STATE_DIR" --platform mlp1 --manifest "$NO_URL_MANIFEST" \
    >"$TMP_ROOT/no-url.out"
python3 - "$TMP_ROOT/no-url.out" <<'PY'
import json
import sys
status = json.load(open(sys.argv[1], encoding="utf-8"))
assert status["state"] == "available", status
assert status["artifact_url"] is None, status
PY

if "$BIN" --state-dir "$STATE_DIR" --platform mlp1 --manifest "$HTTP_MANIFEST" \
    >"$TMP_ROOT/http.out"; then
    echo "HTTP artifact URL unexpectedly accepted" >&2
    exit 1
fi
python3 - "$TMP_ROOT/http.out" <<'PY'
import json
import sys
status = json.load(open(sys.argv[1], encoding="utf-8"))
assert status["state"] == "error", status
assert status["message"] == "Local manifest artifact URL must use HTTPS", status
PY

echo "Jawaka local update manifest smoke passed"
