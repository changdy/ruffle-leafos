#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/mlp1"
BUNDLE_DIR="$ROOT_DIR/build/mlp1-adb-smoke/bundle"
REMOTE_DIR="${JAWAKA_MLP1_REMOTE_DIR:-/tmp/jawaka-mlp1-smoke}"
REMOTE_SDCARD_PATH="${REMOTE_SDCARD_PATH:-$REMOTE_DIR/sd}"
REMOTE_RUNTIME_PATH="${REMOTE_RUNTIME_PATH:-$REMOTE_DIR/run}"
REMOTE_LOGS_PATH="${REMOTE_LOGS_PATH:-$REMOTE_DIR/logs}"
RUN_SECONDS="${JAWAKA_MLP1_SMOKE_SECONDS:-12}"
RECOVERY_STAGE_PATH="$REMOTE_SDCARD_PATH/Apps/mlp1/.pakrat-stage-org.umrk.adb-smoke"

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    make -C "$ROOT_DIR" mlp1
fi

for bin in jawakad jawaka-launcher jawaka-menu; do
    if [ ! -x "$BUILD_DIR/bin/$bin" ]; then
        echo "missing MLP1 binary: $BUILD_DIR/bin/$bin" >&2
        exit 1
    fi
done

rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR/bin" "$BUNDLE_DIR/res"
cp -f "$BUILD_DIR/bin/jawakad" "$BUNDLE_DIR/bin/jawakad"
cp -f "$BUILD_DIR/bin/jawaka-launcher" "$BUNDLE_DIR/bin/jawaka-launcher"
cp -f "$BUILD_DIR/bin/jawaka-menu" "$BUNDLE_DIR/bin/jawaka-menu"
cp -Rf "$ROOT_DIR/res/themes" "$BUNDLE_DIR/res/"
if [ -d "$ROOT_DIR/res/system_icons" ]; then
    cp -Rf "$ROOT_DIR/res/system_icons" "$BUNDLE_DIR/res/"
fi
cp -Rf "$WORKSPACE_ROOT/Catastrophe/res/fonts" "$BUNDLE_DIR/res/"
cp -f "$WORKSPACE_ROOT/Catastrophe/res/font.ttf" "$BUNDLE_DIR/res/font.ttf"

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

pangu_pid="$("${ADB[@]}" shell 'pidof loong_pangu 2>/dev/null || true' | tr -d '\r' | awk '{print $1}')"

cleanup() {
    if [ -n "${pangu_pid:-}" ]; then
        echo "Resuming stock loong_pangu pid $pangu_pid"
        "${ADB[@]}" shell "kill -CONT '$pangu_pid' 2>/dev/null || true" >/dev/null || true
    fi
}
trap cleanup EXIT

echo "Deploying Jawaka smoke bundle to $REMOTE_DIR"
"${ADB[@]}" shell "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR/bundle' '$REMOTE_SDCARD_PATH/Roms/FC' '$REMOTE_SDCARD_PATH/Images/FC' '$REMOTE_SDCARD_PATH/Apps' '$REMOTE_SDCARD_PATH/BIOS' '$REMOTE_SDCARD_PATH/Saves' '$REMOTE_SDCARD_PATH/States' '$REMOTE_SDCARD_PATH/.umrk/mlp1' '$REMOTE_RUNTIME_PATH' '$REMOTE_LOGS_PATH' '$RECOVERY_STAGE_PATH'"
"${ADB[@]}" push "$BUNDLE_DIR/." "$REMOTE_DIR/bundle/" >/dev/null
"${ADB[@]}" shell "chmod 755 '$REMOTE_DIR/bundle/bin/'* && printf 'mock rom\\n' > '$REMOTE_SDCARD_PATH/Roms/FC/Smoke Test.zip'"

if [ -n "${pangu_pid:-}" ]; then
    echo "Pausing stock loong_pangu pid $pangu_pid"
    "${ADB[@]}" shell "kill -STOP '$pangu_pid'"
else
    echo "No stock loong_pangu pid found; running smoke without pause."
fi

echo "Running Jawaka autodemo smoke for up to ${RUN_SECONDS}s"
set +e
"${ADB[@]}" shell "
cd '$REMOTE_DIR/bundle' &&
XDG_RUNTIME_DIR=/var/run \
SDL_VIDEODRIVER=wayland \
CAT_WINDOW_WIDTH=960 \
CAT_WINDOW_HEIGHT=720 \
CAT_THEMES_DIR='$REMOTE_DIR/bundle/res/themes' \
CAT_FONTS_DIR='$REMOTE_DIR/bundle/res' \
SDCARD_PATH='$REMOTE_SDCARD_PATH' \
UMRK_RUNTIME_PATH='$REMOTE_RUNTIME_PATH' \
UMRK_DAEMON_SOCKET='$REMOTE_RUNTIME_PATH/jawakad.sock' \
UMRK_INTERNAL_DATA_PATH='$REMOTE_SDCARD_PATH/.umrk/mlp1' \
JAWAKA_SDCARD_ROOT='$REMOTE_SDCARD_PATH' \
JAWAKA_RUNTIME_DIR='$REMOTE_RUNTIME_PATH' \
JAWAKA_AUTODEMO=1 \
JAWAKA_AUTODEMO_DELAY_MS=800 \
JAWAKA_THEME=Jawaka-Tabs \
timeout '$RUN_SECONDS' '$REMOTE_DIR/bundle/bin/jawakad' > '$REMOTE_LOGS_PATH/jawaka.log' 2>&1
"
rc=$?
set -e

echo "Jawaka smoke log:"
"${ADB[@]}" shell "tail -160 '$REMOTE_LOGS_PATH/jawaka.log' 2>/dev/null || true"

if [ "$rc" -ne 0 ]; then
    echo "Jawaka smoke exited with status $rc" >&2
    exit "$rc"
fi

if ! "${ADB[@]}" shell "test ! -e '$RECOVERY_STAGE_PATH'"; then
    echo "jawakad did not sweep the startup Pak Rat recovery fixture" >&2
    exit 1
fi
echo "Pak Rat startup recovery fixture swept on device."

echo "Jawaka MLP1 ADB smoke completed."
