#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
COMMON_GIT_DIR="$(git -C "$ROOT_DIR" rev-parse --path-format=absolute --git-common-dir)"
WORKSPACE_ROOT="${WORKSPACE_ROOT:-$(cd "$COMMON_GIT_DIR/../.." && pwd)}"
BUILD_REL="${JAWAKA_MLP1_LIFE1_BUILD_REL:-build/mlp1}"
BUILD_DIR="$ROOT_DIR/$BUILD_REL"
BUNDLE_DIR="$ROOT_DIR/build/mlp1-life1-device/bundle"
REMOTE_DIR="${JAWAKA_MLP1_LIFE1_REMOTE_DIR:-/tmp/jawaka-a3b-life1}"
TOOLCHAIN_IMAGE="${MLP1_TOOLCHAIN_IMAGE:-ghcr.io/utility-muffin-research-kitchen/mlp1-toolchain:local}"
SERVICE_ID="org.umrk.test.life1device"
LIVE_PANGU_PID=""
TEST_DAEMON_PID=""
REMOTE_CARD_MOUNTED=0

if [[ ! "$REMOTE_DIR" =~ ^/tmp/jawaka-a3b-[A-Za-z0-9._/-]+$ ]] ||
   [[ "$REMOTE_DIR" == *".."* ]]; then
    echo "unsafe remote fixture root: $REMOTE_DIR" >&2
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

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    echo "Building A3b device binaries with $TOOLCHAIN_IMAGE"
    docker run --rm \
        -v "$ROOT_DIR:/workspace/Jawaka" \
        -v "$WORKSPACE_ROOT:/workspace" \
        -w /workspace/Jawaka \
        "$TOOLCHAIN_IMAGE" \
        bash -lc '
            set -euo pipefail
            make -f ports/mlp1/Makefile all
            make BUILD=build/mlp1 CC=aarch64-buildroot-linux-gnu-gcc \
                PLATFORM=mlp1 CSTD=-std=gnu11 WORKSPACE_ROOT=/workspace \
                build/mlp1/bin/life1-fixture-service \
                build/mlp1/bin/game-writer-fixture
        '
fi

for bin in jawakad jawaka-platformctl life1-fixture-service game-writer-fixture; do
    if [ ! -x "$BUILD_DIR/bin/$bin" ]; then
        echo "missing MLP1 binary: $BUILD_DIR/bin/$bin" >&2
        exit 1
    fi
done

rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR/bin" \
    "$BUNDLE_DIR/sd/Apps/mlp1/Life1Device.pak/bin" \
    "$BUNDLE_DIR/sd/.system/leaf/platforms/mlp1/defaults" \
    "$BUNDLE_DIR/sd/.system/leaf/platforms/mlp1/emulators/fixture" \
    "$BUNDLE_DIR/sd/Roms/N64" "$BUNDLE_DIR/sd/Images/N64" \
    "$BUNDLE_DIR/sd/BIOS" "$BUNDLE_DIR/sd/Saves" "$BUNDLE_DIR/sd/States" \
    "$BUNDLE_DIR/sd/Music" "$BUNDLE_DIR/sd/Videos" "$BUNDLE_DIR/sd/Cheats"
cp -f "$BUILD_DIR/bin/jawakad" "$BUILD_DIR/bin/jawaka-platformctl" \
    "$BUNDLE_DIR/bin/"
cp -f "$BUILD_DIR/bin/life1-fixture-service" \
    "$BUNDLE_DIR/sd/Apps/mlp1/Life1Device.pak/bin/"
cp -f "$BUILD_DIR/bin/game-writer-fixture" \
    "$BUNDLE_DIR/sd/.system/leaf/platforms/mlp1/emulators/fixture/"

printf '%s\n' \
    "{\"id\":\"$SERVICE_ID\",\"name\":\"LIFE-1 Device Fixture\",\"platform\":\"mlp1\",\"pak_version\":\"1.0.0\",\"service\":{\"schema\":1,\"id\":\"$SERVICE_ID\",\"run\":{\"path\":\"bin/life1-fixture-service\",\"args\":[]},\"default_enabled\":false,\"stop_grace_ms\":300,\"restart\":\"no\",\"lifecycle\":{\"game\":\"notify\"}}}" \
    >"$BUNDLE_DIR/sd/Apps/mlp1/Life1Device.pak/pak.json"
printf '%s\n' \
    '{"version":2,"platform":"mlp1","cores":[{"id":"writer_fixture","display_name":"Writer Fixture","type":"path","libretro_name":null,"file_name":null,"config_folder":"Writer Fixture","info_name":null,"path":"emulators/fixture/game-writer-fixture","supports_menu":false,"supports_savestate":true,"supports_disk_control":false,"needs_swap":false,"status":"packaged"}]}' \
    >"$BUNDLE_DIR/sd/.system/leaf/platforms/mlp1/defaults/cores.json"
printf '%s\n' \
    '{"version":2,"platform":"mlp1","systems":[{"id":"N64","name":"Nintendo 64","patterns":["N64"],"extensions":["n64"],"archive_extensions":[],"archive_inner_extensions":["n64"],"archive_mode":"pass_through","file_names":[],"ignore_file_names":[],"playlist_extensions":[],"m3u_generation":"none","default_core":"writer_fixture","alternate_cores":[],"rom_root":"Roms/N64","image_root":"Images/N64","bios_notes":[]}]}' \
    >"$BUNDLE_DIR/sd/.system/leaf/platforms/mlp1/defaults/systems.json"
printf 'rom\n' >"$BUNDLE_DIR/sd/Roms/N64/Barrier.n64"

cleanup_test_daemon() {
    if [ -n "${TEST_DAEMON_PID:-}" ]; then
        "${ADB[@]}" shell "kill '$TEST_DAEMON_PID' 2>/dev/null || true"
        for _ in $(seq 1 50); do
            if ! "${ADB[@]}" shell "kill -0 '$TEST_DAEMON_PID' 2>/dev/null"; then
                break
            fi
            sleep 0.05
        done
        "${ADB[@]}" shell "kill -KILL '$TEST_DAEMON_PID' 2>/dev/null || true"
        TEST_DAEMON_PID=""
    fi
}

cleanup() {
    status=$?
    set +e
    cleanup_test_daemon
    if [ "$status" -ne 0 ]; then
        echo "A3b device logs after failure:" >&2
        "${ADB[@]}" shell "find '$REMOTE_DIR/cases' -name jawakad.log -type f \
            -exec sh -c 'echo --- \"\$1\"; tail -120 \"\$1\"' sh {} \; \
            2>/dev/null || true" >&2
    fi
    if [ -n "${LIVE_PANGU_PID:-}" ]; then
        echo "Resuming live loong_pangu pid $LIVE_PANGU_PID"
        "${ADB[@]}" shell "kill -CONT '$LIVE_PANGU_PID' 2>/dev/null || true" >/dev/null
    fi
    if [ "$REMOTE_CARD_MOUNTED" -eq 1 ]; then
        "${ADB[@]}" shell "umount '$REMOTE_DIR/sd' 2>/dev/null || true" >/dev/null
    fi
    "${ADB[@]}" shell "rm -rf '$REMOTE_DIR'" >/dev/null 2>&1 || true
    rm -rf "$BUNDLE_DIR"
    exit "$status"
}
trap cleanup EXIT

echo "Deploying isolated A3b bundle to $REMOTE_DIR"
"${ADB[@]}" shell "umount '$REMOTE_DIR/sd' 2>/dev/null || true; \
    rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR'"
"${ADB[@]}" push "$BUNDLE_DIR/." "$REMOTE_DIR/" >/dev/null
"${ADB[@]}" shell "mv '$REMOTE_DIR/sd' '$REMOTE_DIR/sd-source' && \
    mkdir -p '$REMOTE_DIR/sd' && mount --bind '$REMOTE_DIR/sd-source' '$REMOTE_DIR/sd'"
REMOTE_CARD_MOUNTED=1
"${ADB[@]}" shell "chmod 755 '$REMOTE_DIR/bin/'* \
    '$REMOTE_DIR/sd/Apps/mlp1/Life1Device.pak/bin/'* \
    '$REMOTE_DIR/sd/.system/leaf/platforms/mlp1/emulators/fixture/'*"

LIVE_PANGU_PID="$("${ADB[@]}" shell 'pidof loong_pangu 2>/dev/null || true' |
    tr -d '\r' | awk '{print $1}')"
if [ -n "$LIVE_PANGU_PID" ]; then
    echo "Pausing live loong_pangu pid $LIVE_PANGU_PID"
    "${ADB[@]}" shell "kill -STOP '$LIVE_PANGU_PID'"
fi

wait_for_remote_test() {
    local command="$1"
    local attempts="${2:-200}"
    for _ in $(seq 1 "$attempts"); do
        if "${ADB[@]}" shell "$command" >/dev/null 2>&1; then
            return 0
        fi
        if [ -n "${TEST_DAEMON_PID:-}" ] &&
           ! "${ADB[@]}" shell "kill -0 '$TEST_DAEMON_PID' 2>/dev/null"; then
            echo "test daemon exited while waiting for: $command" >&2
            return 1
        fi
        sleep 0.05
    done
    echo "timed out waiting for: $command" >&2
    return 1
}

run_case() {
    local scenario="$1"
    local case_dir="$REMOTE_DIR/cases/$scenario"
    local runtime="$case_dir/runtime"
    local state="$case_dir/state"
    local userdata="$case_dir/userdata"
    local logs="$case_dir/logs"
    local result="$runtime/services/$SERVICE_ID/life1-fixture-result"
    local socket="$runtime/jawakad.sock"
    local log="$logs/jawakad.log"
    local response
    local start_ms
    local live_ms

    echo "Running MLP1 LIFE-1 case: $scenario"
    "${ADB[@]}" shell "mkdir -p '$runtime' '$state' '$userdata' '$logs'"
    "${ADB[@]}" shell "( \
        cd '$REMOTE_DIR' && exec env \
            PLATFORM=mlp1 \
            SDCARD_PATH='$REMOTE_DIR/sd' \
            SDCARD_PATHS='$REMOTE_DIR/sd' \
            ROMS_PATH='$REMOTE_DIR/sd/Roms' \
            ROMS_PATHS='$REMOTE_DIR/sd/Roms' \
            IMAGES_PATH='$REMOTE_DIR/sd/Images' \
            IMAGES_PATHS='$REMOTE_DIR/sd/Images' \
            APPS_PATH='$REMOTE_DIR/sd/Apps' \
            APPS_PATHS='$REMOTE_DIR/sd/Apps' \
            USERDATA_PATH='$userdata' \
            USERDATA_PATHS='$userdata' \
            SHARED_USERDATA_PATH='$userdata/shared' \
            SHARED_USERDATA_PATHS='$userdata/shared' \
            LOGS_PATH='$logs' \
            MUSIC_PATH='$REMOTE_DIR/sd/Music' \
            MUSIC_PATHS='$REMOTE_DIR/sd/Music' \
            VIDEO_PATH='$REMOTE_DIR/sd/Videos' \
            VIDEO_PATHS='$REMOTE_DIR/sd/Videos' \
            BIOS_PATH='$REMOTE_DIR/sd/BIOS' \
            BIOS_PATHS='$REMOTE_DIR/sd/BIOS' \
            SAVES_PATH='$REMOTE_DIR/sd/Saves' \
            SAVES_PATHS='$REMOTE_DIR/sd/Saves' \
            STATES_PATH='$REMOTE_DIR/sd/States' \
            STATES_PATHS='$REMOTE_DIR/sd/States' \
            CHEATS_PATH='$REMOTE_DIR/sd/Cheats' \
            CHEATS_PATHS='$REMOTE_DIR/sd/Cheats' \
            UMRK_PLATFORM_PATH='$REMOTE_DIR/sd/.system/leaf/platforms/mlp1' \
            UMRK_RUNTIME_PATH='$runtime' \
            JAWAKA_RUNTIME_DIR='$runtime' \
            UMRK_DAEMON_SOCKET='$socket' \
            UMRK_INTERNAL_DATA_PATH='$state' \
            JAWAKA_SDCARD_ROOT='$REMOTE_DIR/sd' \
            UMRK_LIFE1_FIXTURE_SERVICE_ID='$SERVICE_ID' \
            UMRK_LIFE1_FIXTURE_SCENARIO='$scenario' \
            '$REMOTE_DIR/bin/jawakad' --daemon-only \
        ) </dev/null >'$log' 2>&1 & \
        echo \$! >'$case_dir/daemon.pid'"
    TEST_DAEMON_PID="$("${ADB[@]}" shell "cat '$case_dir/daemon.pid'" | tr -d '\r')"
    wait_for_remote_test "test -S '$socket'"

    for _ in $(seq 1 200); do
        response="$("${ADB[@]}" shell "'$REMOTE_DIR/bin/jawaka-platformctl' \
            --socket '$socket' request '{\"type\":\"library-status\"}'" 2>/dev/null |
            tr -d '\r' || true)"
        if grep -F '"scan_running":false' <<<"$response" >/dev/null &&
           ! grep -F '"generation":0' <<<"$response" >/dev/null; then
            break
        fi
        if grep -F '"scan_error":"scan failed' <<<"$response" >/dev/null; then
            break
        fi
        sleep 0.05
    done
    grep -F '"scan_running":false' <<<"$response" >/dev/null
    if grep -F '"generation":0' <<<"$response" >/dev/null; then
        echo "library scan did not produce a generation: $response" >&2
        return 1
    fi

    response="$("${ADB[@]}" shell "'$REMOTE_DIR/bin/jawaka-platformctl' \
        --socket '$socket' request \
        '{\"v\":1,\"op\":\"run\",\"id\":\"run\",\"service_id\":\"$SERVICE_ID\"}'" |
        tr -d '\r')"
    grep -F '"ok":true' <<<"$response" >/dev/null
    wait_for_remote_test "grep -Fx 'ready=1' '$result'" 300

    start_ms="$(python3 -c 'import time; print(time.monotonic_ns() // 1000000)')"
    response="$("${ADB[@]}" shell "'$REMOTE_DIR/bin/jawaka-platformctl' \
        --socket '$socket' request \
        '{\"type\":\"launch-game\",\"system\":\"N64\",\"rom_path\":\"Roms/N64/Barrier.n64\"}'" |
        tr -d '\r')"
    grep -F '"type":"ok"' <<<"$response" >/dev/null
    wait_for_remote_test "test -f '$runtime/game-writer-live'" 300
    live_ms="$(python3 -c 'import time; print(time.monotonic_ns() // 1000000)')"

    "${ADB[@]}" shell "test -f '$runtime/active-game.json'"
    if [ "$scenario" = "game-exchange" ] &&
       "${ADB[@]}" shell "grep -Fx 'finish=1' '$result'" >/dev/null 2>&1; then
        echo "game.finish arrived before the writer descendant exited" >&2
        return 1
    fi

    if [ "$scenario" = "game-exchange" ]; then
        wait_for_remote_test "test -f '$runtime/game-writer-done' && \
            test ! -e '$runtime/active-game.json' && \
            grep -Fx 'finish=1' '$result'" 500
    else
        wait_for_remote_test "test -f '$runtime/game-writer-done' && \
            test ! -e '$runtime/active-game.json'" 500
    fi
    "${ADB[@]}" shell "grep -F 'life1: active launch committed' '$log' && \
        grep -F 'life1: writer started' '$log' && \
        grep -F 'life1: game.finish' '$log'" >/dev/null

    if [ "$scenario" = "game-exchange" ]; then
        "${ADB[@]}" shell "grep -Fx 'start=1' '$result' && \
            grep -Fx 'cancel=1' '$result' && grep -Fx 'finish=1' '$result'" >/dev/null
    else
        "${ADB[@]}" shell "grep -F 'reason=malformed-exchange-message' '$log' && \
            grep -F 'life1: verified service stop' '$log'" >/dev/null
    fi

    cleanup_test_daemon
    remaining="$("${ADB[@]}" shell \
        "ps -eo pid,ppid,pgid,stat,args 2>/dev/null | \
         grep '$REMOTE_DIR' | grep -v grep || true" | tr -d '\r')"
    if [ -n "$remaining" ]; then
        echo "A3b fixture processes remain after $scenario:" >&2
        echo "$remaining" >&2
        return 1
    fi

    echo "PASS MLP1 LIFE-1 $scenario (ADB-inclusive launch_to_writer_ms=$((live_ms - start_ms)))"
}

run_case game-exchange
run_case game-malformed

echo "PASS MLP1 A3b LIFE-1 smoke (success + fallback, no fixture processes remain)"
