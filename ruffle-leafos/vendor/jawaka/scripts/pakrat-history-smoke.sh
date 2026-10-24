#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(CDPATH= cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD:-build/pakrat-history-smoke}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-pakrat-history.XXXXXX")"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP_ROOT"
}
trap cleanup EXIT

for tool in python3 zip curl; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$tool is required for pakrat-history-smoke" >&2
        exit 2
    }
done

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        sha256sum "$1" | awk '{print $1}'
    fi
}

FEED_ROOT="$TMP_ROOT/feed"
SD_ROOT="$TMP_ROOT/sd"
STATE_DIR="$SD_ROOT/.umrk/mlp1"
PLATFORM_ROOT="$SD_ROOT/.system/leaf/platforms/mlp1"
STORE_ID="org.umrk.history-smoke"
INSTALL_PATH="$SD_ROOT/Apps/mlp1/History.pak"
ARTIFACT_NAME="History.mlp1.pak.zip"

mkdir -p "$FEED_ROOT/artifacts/0.1.0" "$FEED_ROOT/artifacts/0.2.0" \
    "$STATE_DIR/store" "$PLATFORM_ROOT"
printf '{ "managed_apps": [] }\n' >"$PLATFORM_ROOT/manifest.json"

make_artifact() {
    local version="$1"
    local min_leaf="$2"
    local stage="$TMP_ROOT/stage-$version"
    local pak="$stage/History.pak"
    local archive="$FEED_ROOT/artifacts/$version/$ARTIFACT_NAME"
    mkdir -p "$pak"
    if [ -n "$min_leaf" ]; then
        cat >"$pak/pak.json" <<JSON
{ "name": "History Smoke", "platform": "mlp1", "pak_version": "$version", "min_leaf_version": "$min_leaf" }
JSON
    else
        cat >"$pak/pak.json" <<JSON
{ "name": "History Smoke", "platform": "mlp1", "pak_version": "$version" }
JSON
    fi
    printf '#!/bin/sh\nexit 0\n' >"$pak/launch.sh"
    chmod +x "$pak/launch.sh"
    (cd "$stage" && zip -qr "$archive" History.pak)
}

make_artifact "0.1.0" ""
make_artifact "0.2.0" "0.7.0"
FLOOR_ARCHIVE="$FEED_ROOT/artifacts/0.1.0/$ARTIFACT_NAME"
NEW_ARCHIVE="$FEED_ROOT/artifacts/0.2.0/$ARTIFACT_NAME"
FLOOR_SHA="$(sha256_file "$FLOOR_ARCHIVE")"
NEW_SHA="$(sha256_file "$NEW_ARCHIVE")"
FLOOR_SIZE="$(wc -c <"$FLOOR_ARCHIVE" | tr -d ' ')"
NEW_SIZE="$(wc -c <"$NEW_ARCHIVE" | tr -d ' ')"

PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
BASE_URL="http://127.0.0.1:$PORT/"

write_catalog() {
    local include_new="$1"
    local new_min_leaf="${2:-0.7.0}"
    local versions
    if [ "$include_new" = "yes" ]; then
        versions=$(cat <<JSON
          {
            "version": "0.2.0",
            "min_leaf_version": "$new_min_leaf",
            "artifact": {
              "url": "${BASE_URL}artifacts/0.2.0/$ARTIFACT_NAME",
              "name": "$ARTIFACT_NAME",
              "archive": "zip",
              "size": $NEW_SIZE,
              "installed_size": 64,
              "sha256": "$NEW_SHA"
            }
          },
JSON
)
    else
        versions=""
    fi
    cat >"$FEED_ROOT/storefront.json" <<JSON
{
  "schema": 1,
  "product": "pak-rat",
  "apps": [{
    "id": "$STORE_ID",
    "name": "History Smoke",
    "summary": "Exact historical repair fixture",
    "version": "0.1.0",
    "packages": [{
      "platform": "mlp1",
      "runtime": "leaf",
      "version": "0.1.0",
      "install_name": "History.pak",
      "runtime_manifest_path": "pak.json",
      "artifact": {
        "url": "${BASE_URL}artifacts/0.1.0/$ARTIFACT_NAME",
        "name": "$ARTIFACT_NAME",
        "archive": "zip",
        "size": $FLOOR_SIZE,
        "installed_size": 64,
        "sha256": "$FLOOR_SHA"
      },
      "versions": [
$versions
        {
          "version": "0.1.0",
          "artifact": {
            "url": "${BASE_URL}artifacts/0.1.0/$ARTIFACT_NAME",
            "name": "$ARTIFACT_NAME",
            "archive": "zip",
            "size": $FLOOR_SIZE,
            "installed_size": 64,
            "sha256": "$FLOOR_SHA"
          }
        }
      ]
    }]
  }]
}
JSON
}

write_release() {
    cat >"$STATE_DIR/release.json" <<JSON
{ "schema": 1, "product": "leaf", "platform": "mlp1", "version": "$1", "release_id": "$1" }
JSON
}

write_catalog yes "0.6.0"
printf '%s\n' "$BASE_URL" >"$STATE_DIR/store/dev-catalog-url"
python3 -m http.server "$PORT" --bind 127.0.0.1 \
    --directory "$FEED_ROOT" >"$TMP_ROOT/http.log" 2>&1 &
SERVER_PID="$!"
for _ in $(seq 1 30); do
    if curl -fsS "${BASE_URL}storefront.json" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done
curl -fsS "${BASE_URL}storefront.json" >/dev/null

make -C "$JAWAKA_DIR" -s BUILD="$BUILD_DIR" jawaka-pakrat-smoke
BIN="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-pakrat-smoke"
run_smoke() {
    "$BIN" --platform mlp1 --sdcard-root "$SD_ROOT" "$@"
}

write_release "v0.7.0"
if run_smoke install "$STORE_ID" >"$TMP_ROOT/runtime-mismatch.out" 2>&1; then
    echo "install unexpectedly accepted mismatched runtime min_leaf_version" >&2
    exit 1
fi
grep -F "artifact extraction/validation failed" \
    "$TMP_ROOT/runtime-mismatch.out" >/dev/null
test ! -e "$INSTALL_PATH"

write_catalog yes "0.7.0"
run_smoke install "$STORE_ID" >"$TMP_ROOT/install-new.out"
grep -F "installed: $STORE_ID 0.2.0" "$TMP_ROOT/install-new.out" >/dev/null

write_release "v0.6.1"
run_smoke list >"$TMP_ROOT/old-leaf-installed.out"
grep -F $'installed\t'"$STORE_ID"$'\t0.1.0\tinstalled=0.2.0' \
    "$TMP_ROOT/old-leaf-installed.out" >/dev/null
grep -F $'action=1\ttarget=0.2.0\thistory=1\tmissing_history=0' \
    "$TMP_ROOT/old-leaf-installed.out" >/dev/null

if run_smoke install-target "$STORE_ID" 0.2.0 \
    >"$TMP_ROOT/gated-target.out" 2>&1; then
    echo "normal install unexpectedly accepted a gated target on old Leaf" >&2
    exit 1
fi
grep -F "requested version 0.2.0 is no longer compatible" \
    "$TMP_ROOT/gated-target.out" >/dev/null

rm -rf "$INSTALL_PATH"
run_smoke rescan >/dev/null
run_smoke list >"$TMP_ROOT/stale-with-history.out"
grep -F $'stale\t'"$STORE_ID"$'\t0.1.0\tinstalled=0.2.0' \
    "$TMP_ROOT/stale-with-history.out" >/dev/null
grep -F $'action=1\ttarget=0.2.0\thistory=1\tmissing_history=0' \
    "$TMP_ROOT/stale-with-history.out" >/dev/null
run_smoke repair "$STORE_ID" 0.2.0 >"$TMP_ROOT/repair.out"
test -d "$INSTALL_PATH"
grep -F "installed: $STORE_ID 0.2.0" "$TMP_ROOT/repair.out" >/dev/null

rm -rf "$INSTALL_PATH"
run_smoke rescan >/dev/null
write_catalog no
run_smoke list >"$TMP_ROOT/stale-missing-history.out"
grep -F $'action=0\ttarget=-\thistory=0\tmissing_history=1' \
    "$TMP_ROOT/stale-missing-history.out" >/dev/null
if run_smoke repair "$STORE_ID" 0.2.0 \
    >"$TMP_ROOT/missing-repair.out" 2>&1; then
    echo "repair unexpectedly succeeded without exact catalog history" >&2
    exit 1
fi
grep -F "installed version 0.2.0 is missing from catalog history" \
    "$TMP_ROOT/missing-repair.out" >/dev/null
test ! -e "$INSTALL_PATH"

if run_smoke install "$STORE_ID" >"$TMP_ROOT/downgrade.out" 2>&1; then
    echo "normal install unexpectedly downgraded the owned version" >&2
    exit 1
fi
grep -F "catalog selection would downgrade the installed package" \
    "$TMP_ROOT/downgrade.out" >/dev/null
test ! -e "$INSTALL_PATH"

echo "Pak Rat historical repair smoke passed"
