#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMMON_GIT_DIR="$(git -C "$ROOT_DIR" rev-parse --path-format=absolute --git-common-dir)"
DEFAULT_WORKSPACE_ROOT="$(cd "$COMMON_GIT_DIR/../.." && pwd)"
WORKSPACE_ROOT="${WORKSPACE_ROOT:-$DEFAULT_WORKSPACE_ROOT}"
BUILD_REL="build/mlp1-service-fixture-device"
BUILD_DIR="$ROOT_DIR/$BUILD_REL"
REMOTE_DIR="${JAWAKA_MLP1_SERVICE_FIXTURE_REMOTE_DIR:-/tmp/jawaka-a2-service-fixtures}"
REMOTE_ARCHIVE="${REMOTE_DIR}.tgz"
TOOLCHAIN_IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local}"

if [[ ! "$REMOTE_DIR" =~ ^/tmp/jawaka-a2-[A-Za-z0-9._/-]+$ ]] ||
   [[ "$REMOTE_DIR" == *".."* ]]; then
    echo "unsafe remote fixture root: $REMOTE_DIR" >&2
    exit 1
fi

CANONICAL_INVALID="$WORKSPACE_ROOT/leaf-contracts/contracts/leaf-services/manifests/invalid"
if [ ! -d "$CANONICAL_INVALID" ]; then
    echo "canonical A0 fixture tree not found: $CANONICAL_INVALID" >&2
    exit 1
fi

if [ -n "${ADB_SERIAL:-}" ]; then
    ADB=(adb -s "$ADB_SERIAL")
else
    serial="$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')"
    if [ -z "${serial:-}" ]; then
        echo "No online adb device found." >&2
        exit 1
    fi
    ADB=(adb -s "$serial")
fi

echo "Using adb device: $("${ADB[@]}" get-serialno)"
echo "Building A2 fixture suite with $TOOLCHAIN_IMAGE"
docker run --rm \
    -e DEVICE_FIXTURE_ROOT="$REMOTE_DIR" \
    -v "$ROOT_DIR:/src" \
    -v "$WORKSPACE_ROOT:/workspace:ro" \
    -w /src \
    "$TOOLCHAIN_IMAGE" \
    bash -lc '
        set -euo pipefail
        build_rel="build/mlp1-service-fixture-device"
        make \
            BUILD="$build_rel" \
            CC=aarch64-buildroot-linux-gnu-gcc \
            PLATFORM=mlp1 \
            CSTD=-std=gnu11 \
            WORKSPACE_ROOT=/workspace \
            SERVICE_FIXTURE_TEST_ROOT="$DEVICE_FIXTURE_ROOT/service-fixtures" \
            "$build_rel/bin/ownership-test" \
            "$build_rel/bin/service-fixture-test"
    '

ARCHIVE="$BUILD_DIR/jawaka-a2-service-fixtures.tgz"
tar -C "$BUILD_DIR" -czf "$ARCHIVE" \
    bin/ownership-test bin/service-fixture-test service-fixtures

cleanup() {
    "${ADB[@]}" shell "rm -rf '$REMOTE_DIR' '$REMOTE_ARCHIVE'" >/dev/null 2>&1 || true
}
trap cleanup EXIT

cleanup
"${ADB[@]}" shell "mkdir -p '$REMOTE_DIR'"
"${ADB[@]}" push "$ARCHIVE" "$REMOTE_ARCHIVE" >/dev/null
"${ADB[@]}" shell "tar -xzf '$REMOTE_ARCHIVE' -C '$REMOTE_DIR' && \
    chmod 755 '$REMOTE_DIR/bin/'* \
        '$REMOTE_DIR/service-fixtures/valid/Apps/'*.pak/bin/service-fixture"

echo "Running focused ownership proof on MLP1"
"${ADB[@]}" shell "cd '$REMOTE_DIR' && ./bin/ownership-test"

echo "Running five behavior and 27 invalid service paks on MLP1"
"${ADB[@]}" shell "cd '$REMOTE_DIR' && ./bin/service-fixture-test"

remaining="$("${ADB[@]}" shell \
    "ps -eo pid,ppid,pgid,sid,stat,comm,args 2>/dev/null | \
     grep -E 'service-fixture|service-fixture-test' | grep -v grep || true" | tr -d '\r')"
if [ -n "$remaining" ]; then
    echo "fixture processes remain after the device suite:" >&2
    echo "$remaining" >&2
    exit 1
fi

echo "PASS MLP1 A2 service-fixture smoke (no fixture processes remain)"
