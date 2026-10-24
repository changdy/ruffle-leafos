#!/usr/bin/env bash
# Host orchestrator for the real-card P1 Pak Rat recovery matrix.
set -euo pipefail

ROOT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
TARGET_MOUNT="${P1_SDCARD_PATH:-/mnt/sdcard}"
BIND_ROOT="/tmp/jw-p1-fat"
DEVICE_ROOT="/tmp/jw-p1-run"
DEVICE_EVIDENCE="/tmp/jw-p1-device"
UNDERLAY="$TARGET_MOUNT/.p1-pakrat-fixture"
BUILD_DIR="${P1_BUILD_DIR:-build/mlp1}"
SMOKE_BIN="${P1_SMOKE_BIN:-$ROOT_DIR/$BUILD_DIR/bin/jawaka-pakrat-smoke}"
DAEMON_BIN="${P1_DAEMON_BIN:-$ROOT_DIR/$BUILD_DIR/bin/jawakad}"
HOST_TMP="$(mktemp -d "${TMPDIR:-/tmp}/jw-p1-host.XXXXXX")"
SERVER_PID=""
PORT=""
MODE="${P1_MODE:-matrix}"
KEEP_DEVICE="${P1_KEEP_DEVICE:-0}"
[ "$MODE" = removal-prepare ] && KEEP_DEVICE=1

if [ "${CONFIRM_P1_FAT_SMOKE:-0}" != 1 ]; then
    echo "Set CONFIRM_P1_FAT_SMOKE=1 after confirming the selected card is expendable." >&2
    exit 2
fi

case "$TARGET_MOUNT" in
    /mnt/sdcard|/media/sdcard1) ;;
    *) echo "refusing unsupported P1 card root: $TARGET_MOUNT" >&2; exit 2 ;;
esac

if [ -n "${ADB_SERIAL:-}" ]; then
    ADB=(adb -s "$ADB_SERIAL")
else
    serial="$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')"
    [ -n "${serial:-}" ] || { echo "No online adb device found." >&2; exit 1; }
    ADB=(adb -s "$serial")
fi

cleanup() {
    status=$?
    set +e
    if [ -n "$PORT" ]; then
        "${ADB[@]}" reverse --remove "tcp:$PORT" >/dev/null 2>&1 || true
    fi
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    if [ "$KEEP_DEVICE" != 1 ]; then
        "${ADB[@]}" shell "umount '$BIND_ROOT' >/dev/null 2>&1 || true; rm -rf '$DEVICE_ROOT' '$DEVICE_EVIDENCE' '$BIND_ROOT' '$UNDERLAY'" \
            >/dev/null 2>&1 || true
    fi
    rm -rf "$HOST_TMP"
    exit "$status"
}
trap cleanup EXIT HUP INT TERM

echo "Using adb device: $("${ADB[@]}" get-serialno)"
mount_line="$("${ADB[@]}" shell "awk -v target='$TARGET_MOUNT' '\$5 == target { print; exit }' /proc/self/mountinfo" | tr -d '\r')"
case "$mount_line" in
    *" - vfat "*|*" - msdos "*|*" - fat "*) ;;
    *) echo "selected P1 root is not an exact FAT mount: $mount_line" >&2; exit 1 ;;
esac
device_path="$(printf '%s\n' "$mount_line" | awk -F ' - ' '{print $2}' | awk '{print $2}')"
block="${device_path#/dev/}"
block="${block%p1}"
cid="$("${ADB[@]}" shell "cat '/sys/block/$block/device/cid'" | tr -d '\r')"
echo "Physical P1 card: mount=$TARGET_MOUNT device=$device_path cid=$cid"

if [ "${P1_SKIP_BUILD:-0}" != 1 ]; then
    make -C "$ROOT_DIR" mlp1-pakrat-smoke
    make -C "$ROOT_DIR" mlp1
fi
[ -x "$SMOKE_BIN" ] || { echo "missing MLP1 smoke binary: $SMOKE_BIN" >&2; exit 1; }
[ -x "$DAEMON_BIN" ] || { echo "missing MLP1 daemon binary: $DAEMON_BIN" >&2; exit 1; }

runtime_lib_dir="${P1_RUNTIME_LIB_DIR:-}"
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
feed="$HOST_TMP/feed"
BASE_URL="http://127.0.0.1:$PORT/"
P1_FEED_ROOT="$feed" P1_BASE_URL="$BASE_URL" \
P1_LARGE_BYTES="${P1_LARGE_BYTES:-0}" python3 - <<'PY'
import hashlib
import json
import os
import pathlib
import zipfile

root = pathlib.Path(os.environ["P1_FEED_ROOT"])
base = os.environ["P1_BASE_URL"]
large_bytes = int(os.environ["P1_LARGE_BYTES"])
store_id = "org.umrk.recovery-smoke"
artifact_name = "Recovery.mlp1.pak.zip"

def artifact(version: str, payload: str, min_leaf: str | None = None):
    out = root / "artifacts" / version / artifact_name
    out.parent.mkdir(parents=True, exist_ok=True)
    manifest = {"name": "Recovery Smoke", "platform": "mlp1", "pak_version": version}
    if min_leaf:
        manifest["min_leaf_version"] = min_leaf
    with zipfile.ZipFile(out, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("Recovery.pak/pak.json", json.dumps(manifest, separators=(",", ":")))
        info = zipfile.ZipInfo("Recovery.pak/launch.sh")
        info.external_attr = 0o100755 << 16
        zf.writestr(info, "#!/bin/sh\nexit 0\n")
        zf.writestr("Recovery.pak/payload.txt", payload + "\n")
        if large_bytes:
            bulk = zipfile.ZipInfo("Recovery.pak/bulk.bin")
            bulk.compress_type = zipfile.ZIP_STORED
            with zf.open(bulk, "w") as target:
                remaining = large_bytes
                while remaining:
                    chunk = os.urandom(min(1024 * 1024, remaining))
                    target.write(chunk)
                    remaining -= len(chunk)
    data = out.read_bytes()
    return {"url": f"{base}artifacts/{version}/{artifact_name}",
            "name": artifact_name, "archive": "zip", "size": len(data),
            "installed_size": max(64, large_bytes + 1024),
            "sha256": hashlib.sha256(data).hexdigest()}

old_art = artifact("0.1.0", "old-payload")
new_art = artifact("0.2.0", "new-payload", "0.7.0")

def package(versions):
    return {"platform": "mlp1", "runtime": "leaf", "version": "0.1.0",
            "install_name": "Recovery.pak", "runtime_manifest_path": "pak.json",
            "artifact": old_art, "versions": versions}

old_version = {"version": "0.1.0", "artifact": old_art}
new_version = {"version": "0.2.0", "min_leaf_version": "0.7.0", "artifact": new_art}
for mode, versions in (
    ("old", [old_version]),
    ("new", [new_version, old_version]),
):
    catalog = {"schema": 1, "product": "pak-rat", "apps": [{
        "id": store_id, "name": "Recovery Smoke",
        "summary": "Physical FAT promote-recovery fixture",
        "version": "0.1.0",
        "packages": [package(versions)],
    }]}
    directory = root / mode
    directory.mkdir(parents=True, exist_ok=True)
    (directory / "storefront.json").write_text(json.dumps(catalog, separators=(",", ":")))
PY

python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$feed" \
    >"$HOST_TMP/http.log" 2>&1 &
SERVER_PID=$!
"${ADB[@]}" reverse "tcp:$PORT" "tcp:$PORT" >/dev/null
for _ in $(seq 1 50); do
    if "${ADB[@]}" shell "wget -q -O /dev/null '${BASE_URL}old/storefront.json'" \
        >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done
"${ADB[@]}" shell "wget -q -O /dev/null '${BASE_URL}old/storefront.json'"

if [ "$MODE" = removal-recover ]; then
    "${ADB[@]}" shell "test -d '$UNDERLAY' && rm -rf '$DEVICE_ROOT' '$BIND_ROOT' && mkdir -p '$DEVICE_ROOT/bin' '$BIND_ROOT'"
else
    "${ADB[@]}" shell "rm -rf '$DEVICE_ROOT' '$BIND_ROOT' '$UNDERLAY'; mkdir -p '$DEVICE_ROOT/bin' '$BIND_ROOT' '$UNDERLAY'"
fi
"${ADB[@]}" push "$SMOKE_BIN" "$DEVICE_ROOT/bin/jawaka-pakrat-smoke" >/dev/null
"${ADB[@]}" push "$DAEMON_BIN" "$DEVICE_ROOT/bin/jawakad" >/dev/null
"${ADB[@]}" push "$ROOT_DIR/scripts/mlp1-pakrat-recovery-device.sh" "$DEVICE_ROOT/run.sh" >/dev/null
"${ADB[@]}" shell "chmod 755 '$DEVICE_ROOT/bin/jawaka-pakrat-smoke' '$DEVICE_ROOT/bin/jawakad' '$DEVICE_ROOT/run.sh'; mount --bind '$UNDERLAY' '$BIND_ROOT'"

set +e
"${ADB[@]}" shell \
    "P1_MODE='$MODE' P1_SD_ROOT='$BIND_ROOT' P1_UNDERLAY='$UNDERLAY' P1_BASE_URL='$BASE_URL' P1_SMOKE_BIN='$DEVICE_ROOT/bin/jawaka-pakrat-smoke' P1_DAEMON_BIN='$DEVICE_ROOT/bin/jawakad' P1_RUNTIME_LIB_DIR='$runtime_lib_dir' P1_TMP_ROOT='$DEVICE_EVIDENCE' bash '$DEVICE_ROOT/run.sh'"
status=$?
set -e

evidence_dir="$ROOT_DIR/build/p1-mlp1-evidence-$MODE"
rm -rf "$evidence_dir"
mkdir -p "$evidence_dir"
"${ADB[@]}" pull "$DEVICE_EVIDENCE/." "$evidence_dir/" >/dev/null 2>&1 || true
printf '%s\n' "$mount_line" >"$evidence_dir/mountinfo.txt"
printf '%s\n' "$cid" >"$evidence_dir/card-cid.txt"

[ "$status" -eq 0 ] || exit "$status"
if [ "$MODE" = removal-prepare ]; then
    echo "P1 removal fixture intentionally retained at $UNDERLAY for the physical pull."
fi
echo "P1 MLP1 FAT smoke evidence: $evidence_dir"
