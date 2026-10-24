#!/usr/bin/env bash
# Host orchestrator for B4a's real two-card FAT mutation/recovery matrix.
set -euo pipefail

ROOT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PRIMARY_MOUNT="${B4A_PRIMARY_MOUNT:-/mnt/sdcard}"
SECONDARY_MOUNT="${B4A_SECONDARY_MOUNT:-/media/sdcard1}"
PRIMARY_BIND="/tmp/jw-b4a-primary"
SECONDARY_BIND="/tmp/jw-b4a-secondary"
DEVICE_ROOT="/tmp/jw-b4a-run"
DEVICE_EVIDENCE="/tmp/jw-b4a-evidence"
PRIMARY_UNDERLAY="$PRIMARY_MOUNT/.b4a-pakrat-fixture"
SECONDARY_UNDERLAY="$SECONDARY_MOUNT/.b4a-pakrat-fixture"
BUILD_DIR="${B4A_BUILD_DIR:-build/mlp1}"
HOST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/jw-b4a-host.XXXXXX")"
SERVER_PID=""
PORT=""

if [ "${CONFIRM_B4A_FAT_SMOKE:-0}" != 1 ]; then
    echo "Set CONFIRM_B4A_FAT_SMOKE=1 after confirming both selected cards are expendable." >&2
    exit 2
fi
case "$PRIMARY_MOUNT:$SECONDARY_MOUNT" in
    /mnt/sdcard:/media/sdcard1|/media/sdcard1:/mnt/sdcard) ;;
    *) echo "refusing unsupported B4a card roots" >&2; exit 2 ;;
esac
[ "$PRIMARY_MOUNT" != "$SECONDARY_MOUNT" ] || exit 2

if [ -n "${ADB_SERIAL:-}" ]; then
    ADB=(adb -s "$ADB_SERIAL")
else
    serial="$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')"
    [ -n "${serial:-}" ] || { echo "No online adb device found." >&2; exit 1; }
    ADB=(adb -s "$serial")
fi

kill_fixture_processes() {
    # Expanded by the device shell, not by this host-side script.
    # shellcheck disable=SC2016
    "${ADB[@]}" shell '
        fixture_pids=""
        for proc in /proc/[0-9]*; do
            pid="${proc##*/}"
            [ "$pid" = "$$" ] && continue
            arg0="$(tr "\000" "\n" <"$proc/cmdline" 2>/dev/null | sed -n "1p")"
            arg1="$(tr "\000" "\n" <"$proc/cmdline" 2>/dev/null | sed -n "2p")"
            case "$arg0:$arg1" in
                /tmp/jw-b4a-run/bin/jawakad:*|\
                /tmp/jw-b4a-run/bin/jawaka-pakrat-smoke:*|\
                /tmp/jw-b4a-run/bin/jawaka-platformctl:*|\
                bash:/tmp/jw-b4a-run/run.sh|\
                /bin/bash:/tmp/jw-b4a-run/run.sh|\
                /bin/sh:/tmp/jw-b4a-primary/Apps/mlp1/TxnService.pak/bin/run.sh|\
                /bin/sh:/tmp/jw-b4a-secondary/Apps/mlp1/TxnService.pak/bin/run.sh)
                    fixture_pids="$fixture_pids $pid"
                    ;;
            esac
        done
        for pid in $fixture_pids; do
            kill -CONT "$pid" >/dev/null 2>&1 || true
            kill -TERM "$pid" >/dev/null 2>&1 || true
        done
        sleep 0.1
        for pid in $fixture_pids; do
            kill -KILL "$pid" >/dev/null 2>&1 || true
        done
    ' >/dev/null 2>&1 || true
}

cleanup() {
    local status=$?
    set +e
    if [ -n "$PORT" ]; then
        "${ADB[@]}" reverse --remove "tcp:$PORT" >/dev/null 2>&1 || true
    fi
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    kill_fixture_processes
    "${ADB[@]}" shell \
        "umount -l '$PRIMARY_BIND' >/dev/null 2>&1 || true; umount -l '$SECONDARY_BIND' >/dev/null 2>&1 || true; rm -rf '$DEVICE_ROOT' '$DEVICE_EVIDENCE' '$PRIMARY_BIND' '$SECONDARY_BIND' '$PRIMARY_UNDERLAY' '$SECONDARY_UNDERLAY'" \
        >/dev/null 2>&1 || true
    rm -rf "$HOST_TMP"
    exit "$status"
}
trap cleanup EXIT HUP INT TERM

echo "Using adb device: $("${ADB[@]}" get-serialno)"
kill_fixture_processes
mount_evidence=""
for mount in "$PRIMARY_MOUNT" "$SECONDARY_MOUNT"; do
    line="$("${ADB[@]}" shell "awk -v target='$mount' '\$5 == target {print; exit}' /proc/self/mountinfo" | tr -d '\r')"
    case "$line" in
        *" - vfat "*|*" - msdos "*|*" - fat "*) ;;
        *) echo "selected B4a root is not an exact FAT mount: $line" >&2; exit 1 ;;
    esac
    mount_evidence="$mount_evidence$line
"
done

if [ "${B4A_SKIP_BUILD:-0}" != 1 ]; then
    make -C "$ROOT_DIR" mlp1-pakrat-smoke
    make -C "$ROOT_DIR" mlp1
fi
SMOKE_BIN="$ROOT_DIR/$BUILD_DIR/bin/jawaka-pakrat-smoke"
DAEMON_BIN="$ROOT_DIR/$BUILD_DIR/bin/jawakad"
CTL_BIN="$ROOT_DIR/$BUILD_DIR/bin/jawaka-platformctl"
for binary in "$SMOKE_BIN" "$DAEMON_BIN" "$CTL_BIN"; do
    [ -x "$binary" ] || { echo "missing MLP1 binary: $binary" >&2; exit 1; }
done

runtime_lib_dir="${B4A_RUNTIME_LIB_DIR:-}"
if [ -z "$runtime_lib_dir" ]; then
    for candidate in \
        /mnt/sdcard/.system/leaf/platforms/mlp1/launcher/lib \
        /media/sdcard1/.system/leaf/platforms/mlp1/launcher/lib; do
        if "${ADB[@]}" shell "test -d '$candidate'"; then
            runtime_lib_dir="$candidate"
            break
        fi
    done
fi
[ -n "$runtime_lib_dir" ] || { echo "MLP1 launcher runtime libraries not found" >&2; exit 1; }

PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
FEED="$HOST_TMP/feed"
BASE_URL="http://127.0.0.1:$PORT/"
B4A_FEED_ROOT="$FEED" B4A_BASE_URL="$BASE_URL" python3 - <<'PY'
import hashlib
import json
import os
from pathlib import Path
import zipfile

root = Path(os.environ["B4A_FEED_ROOT"])
base = os.environ["B4A_BASE_URL"]
store_id = "org.umrk.test.txnservice"
artifact_name = "TxnService.mlp1.pak.zip"

def artifact(version: str, service: bool):
    out = root / "artifacts" / version / artifact_name
    out.parent.mkdir(parents=True, exist_ok=True)
    manifest = {"id": store_id, "name": "TXN Service", "platform": "mlp1",
                "pak_version": version}
    if service:
        manifest["min_leaf_version"] = "1.0.0"
        manifest["service"] = {
            "schema": 1, "id": store_id,
            "run": {"path": "bin/run.sh", "args": []},
            "restart": "no", "default_enabled": False,
            "stop_grace_ms": 300,
        }
        manifest["state"] = {
            "root": "Syncthing",
            "revoke_on_uninstall": ["leaf/trusted.json"],
            "retained_roots": ["Syncthing"],
        }
    else:
        manifest["name"] = "TXN Service Floor"
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("TxnService.pak/pak.json",
                    json.dumps(manifest, separators=(",", ":")))
        launch = zipfile.ZipInfo("TxnService.pak/launch.sh")
        launch.external_attr = 0o100755 << 16
        zf.writestr(launch, "#!/bin/sh\nexit 0\n")
        if service:
            run = zipfile.ZipInfo("TxnService.pak/bin/run.sh")
            run.external_attr = 0o100755 << 16
            zf.writestr(run, "#!/bin/sh\ntrap 'exit 0' TERM INT\nwhile :; do sleep 1; done\n")
    data = out.read_bytes()
    return {"url": f"{base}artifacts/{version}/{artifact_name}",
            "name": artifact_name, "archive": "zip", "size": len(data),
            "installed_size": 4096, "sha256": hashlib.sha256(data).hexdigest()}

floor_art = artifact("0.0.1", False)
v1_art = artifact("1.0.0", True)
v2_art = artifact("2.0.0", True)
floor = {"version": "0.0.1", "artifact": floor_art}
v1 = {"version": "1.0.0", "min_leaf_version": "1.0.0", "artifact": v1_art}
v2 = {"version": "2.0.0", "min_leaf_version": "1.0.0", "artifact": v2_art}

def write_catalog(name: str, versions):
    package = {"platform": "mlp1", "runtime": "leaf", "version": "0.0.1",
               "install_name": "TxnService.pak",
               "runtime_manifest_path": "pak.json", "artifact": floor_art,
               "versions": versions}
    catalog = {"schema": 1, "product": "pak-rat", "apps": [{
        "id": store_id, "name": "TXN Service", "summary": "B4a FAT fixture",
        "version": "0.0.1", "packages": [package],
    }]}
    directory = root / name
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "storefront.json").write_text(
        json.dumps(catalog, separators=(",", ":")), encoding="utf-8")

write_catalog("v1", [v1, floor])
write_catalog("v2", [v2, v1, floor])
(root / "floor.sha256").write_text(floor_art["sha256"] + "\n")
PY
FLOOR_SHA="$(tr -d '\n' <"$FEED/floor.sha256")"

python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$FEED" \
    >"$HOST_TMP/http.log" 2>&1 &
SERVER_PID=$!
"${ADB[@]}" reverse "tcp:$PORT" "tcp:$PORT" >/dev/null
for _ in $(seq 1 50); do
    if "${ADB[@]}" shell "wget -q -O /dev/null '${BASE_URL}v1/storefront.json'" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done
"${ADB[@]}" shell "wget -q -O /dev/null '${BASE_URL}v2/storefront.json'"

"${ADB[@]}" shell \
    "umount -l '$PRIMARY_BIND' >/dev/null 2>&1 || true; umount -l '$SECONDARY_BIND' >/dev/null 2>&1 || true; rm -rf '$DEVICE_ROOT' '$DEVICE_EVIDENCE' '$PRIMARY_BIND' '$SECONDARY_BIND' '$PRIMARY_UNDERLAY' '$SECONDARY_UNDERLAY'; mkdir -p '$DEVICE_ROOT/bin' '$DEVICE_EVIDENCE' '$PRIMARY_BIND' '$SECONDARY_BIND' '$PRIMARY_UNDERLAY' '$SECONDARY_UNDERLAY'; mount --bind '$PRIMARY_UNDERLAY' '$PRIMARY_BIND'; mount --bind '$SECONDARY_UNDERLAY' '$SECONDARY_BIND'"
"${ADB[@]}" push "$SMOKE_BIN" "$DEVICE_ROOT/bin/jawaka-pakrat-smoke" >/dev/null
"${ADB[@]}" push "$DAEMON_BIN" "$DEVICE_ROOT/bin/jawakad" >/dev/null
"${ADB[@]}" push "$CTL_BIN" "$DEVICE_ROOT/bin/jawaka-platformctl" >/dev/null
"${ADB[@]}" push "$ROOT_DIR/scripts/mlp1-service-mutation-device.sh" "$DEVICE_ROOT/run.sh" >/dev/null
"${ADB[@]}" shell "chmod 755 '$DEVICE_ROOT/bin/'* '$DEVICE_ROOT/run.sh'"

set +e
"${ADB[@]}" shell \
    "B4A_PRIMARY='$PRIMARY_BIND' B4A_SECONDARY='$SECONDARY_BIND' B4A_BASE_URL='$BASE_URL' B4A_FLOOR_SHA='$FLOOR_SHA' B4A_BIN_ROOT='$DEVICE_ROOT/bin' B4A_RUNTIME_LIB_DIR='$runtime_lib_dir' B4A_EVIDENCE='$DEVICE_EVIDENCE' bash '$DEVICE_ROOT/run.sh'"
status=$?
set -e

evidence_dir="$ROOT_DIR/build/b4a-mlp1-evidence"
rm -rf "$evidence_dir"
mkdir -p "$evidence_dir"
"${ADB[@]}" pull "$DEVICE_EVIDENCE/." "$evidence_dir/" >/dev/null 2>&1 || true
mkdir -p "$evidence_dir/device-tmp"
"${ADB[@]}" pull "/tmp/jw-b4a-device/." "$evidence_dir/device-tmp/" \
    >/dev/null 2>&1 || true
printf '%s' "$mount_evidence" >"$evidence_dir/mountinfo.txt"
printf '%s\n' "$("${ADB[@]}" get-serialno)" >"$evidence_dir/adb-serial.txt"
[ "$status" -eq 0 ] || exit "$status"
echo "B4a MLP1 FAT smoke evidence: $evidence_dir"
