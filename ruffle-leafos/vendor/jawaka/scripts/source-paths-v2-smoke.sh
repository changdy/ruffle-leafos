#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_DIR="${BUILD:-build/source-paths-v2-smoke}"
TMP_ROOT="$(mktemp -d /tmp/jw-path2.XXXXXX)"
DAEMON_PID=""

cleanup() {
    status=$?
    set +e
    smoke_daemon_stop || status=1
    rm -rf "$TMP_ROOT"
    exit "$status"
}
trap cleanup EXIT

make -C "$ROOT_DIR" -s BUILD="$BUILD_DIR" jawakad jawaka-platformctl
DAEMON="$ROOT_DIR/$BUILD_DIR/bin/jawakad"
CTL="$ROOT_DIR/$BUILD_DIR/bin/jawaka-platformctl"

run_case() {
    local name="$1" expected="$2"
    local root="$TMP_ROOT/$name"
    local sd="$root/sd" runtime="$root/runtime" state="$root/state"
    local platform="$root/platform" socket="$runtime/jawakad.sock"
    local log="$root/jawakad.log" output="$root/capabilities.json"
    mkdir -p "$sd/Apps/mac" "$sd/Apps/shared" "$sd/Roms" \
             "$runtime" "$state" "$platform/defaults"

    if [ "$name" = "invalid" ]; then
        smoke_daemon_start "$PWD" "$log" \
            UMRK_ENV_VERSION=2 USERDATA_PATHS=/wrong-card/.userdata/mac \
            UMRK_RUNTIME_PATH="$runtime" UMRK_DAEMON_SOCKET="$socket" \
            UMRK_INTERNAL_DATA_PATH="$state" UMRK_PLATFORM_PATH="$platform" \
            JAWAKA_SDCARD_ROOT="$sd" \
            "$DAEMON" --daemon-only
    else
        smoke_daemon_start "$PWD" "$log" \
            UMRK_RUNTIME_PATH="$runtime" UMRK_DAEMON_SOCKET="$socket" \
            UMRK_INTERNAL_DATA_PATH="$state" UMRK_PLATFORM_PATH="$platform" \
            JAWAKA_SDCARD_ROOT="$sd" \
            "$DAEMON" --daemon-only
    fi
    for _ in $(seq 1 500); do
        [ -S "$socket" ] && break
        kill -0 "$DAEMON_PID" 2>/dev/null || {
            cat "$log" >&2
            return 1
        }
        sleep 0.02
    done
    [ -S "$socket" ] || { cat "$log" >&2; return 1; }
    "$CTL" --socket "$socket" request \
        '{"type":"hello","role":"source-paths-v2-smoke"}' >"$output"
    if [ "$expected" = "true" ]; then
        grep -F '"source-paths-v2"' "$output" >/dev/null || {
            cat "$output" >&2
            return 1
        }
    elif grep -F '"source-paths-v2"' "$output" >/dev/null; then
        cat "$output" >&2
        return 1
    fi
    smoke_daemon_stop
}

run_case valid true
run_case invalid false
echo "source-paths-v2 capability smoke: PASS"
