#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORKSPACE_ROOT="$(cd "$ROOT_DIR/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/mlp1"
LOCAL_LOG_DIR="$ROOT_DIR/build/mlp1-ra-command-smoke/$(date +%Y%m%d-%H%M%S)"
REMOTE_DIR="${JAWAKA_RA_SMOKE_REMOTE_DIR:-/tmp/umrk-ra-command-smoke}"
RETROARCH_BIN="${RETROARCH_BIN:-$WORKSPACE_ROOT/retroarch-builds/output/mlp1/bin/retroarch}"
RETROARCH_LIB_DIR="${RETROARCH_LIB_DIR:-$WORKSPACE_ROOT/retroarch-builds/output/mlp1/ffmpeg/flat}"
CORE_SO="${CORE_SO:-$WORKSPACE_ROOT/Cores-spruce/output/mlp1/cores/2048_libretro.so}"
CORE_INFO="${CORE_INFO:-$WORKSPACE_ROOT/Cores-spruce/output/mlp1/info/2048_libretro.info}"
RUN_LOG="$REMOTE_DIR/logs/retroarch.log"
CTL="$REMOTE_DIR/bin/jawaka-retroarchctl"
PORT="${JAWAKA_RA_COMMAND_PORT:-55355}"
VIDEO_DRIVER="${VIDEO_DRIVER:-gl}"
VIDEO_CONTEXT_DRIVER="${VIDEO_CONTEXT_DRIVER:-}"
CONTENT_PATH="${1:-${CONTENT_PATH:-}}"
CONTENT_WAIT_SECONDS="${CONTENT_WAIT_SECONDS:-8}"
SYSTEM_DIR="${SYSTEM_DIR:-}"

if [ "$#" -gt 1 ]; then
    echo "usage: $0 [content-file]" >&2
    exit 2
fi
case "$REMOTE_DIR" in
    /tmp/?*) ;;
    *)
        echo "JAWAKA_RA_SMOKE_REMOTE_DIR must be a dedicated path below /tmp" >&2
        exit 2
        ;;
esac
case "$REMOTE_DIR" in
    *[!A-Za-z0-9._/-]*|*//*|*/../*|*/..|*/./*|*/.)
        echo "JAWAKA_RA_SMOKE_REMOTE_DIR contains unsupported path characters or traversal" >&2
        exit 2
        ;;
esac

CORE_FILE="$(basename "$CORE_SO")"
CORE_INFO_FILE="$(basename "$CORE_INFO")"
CORE_LABEL="${CORE_FILE%_libretro.so}"
for artifact_name in "$CORE_FILE" "$CORE_INFO_FILE"; do
    case "$artifact_name" in
        *[!A-Za-z0-9._-]*)
            echo "core artifact filenames must use only letters, digits, dot, dash, and underscore" >&2
            exit 2
            ;;
    esac
done
case "$CONTENT_WAIT_SECONDS" in
    ''|*[!0-9]*)
        echo "CONTENT_WAIT_SECONDS must be a non-negative integer" >&2
        exit 2
        ;;
esac

REMOTE_CORE="$REMOTE_DIR/cores/$CORE_FILE"
REMOTE_INFO="$REMOTE_DIR/info/$CORE_INFO_FILE"
REMOTE_CONTENT=""
if [ -n "$CONTENT_PATH" ]; then
    if [ ! -f "$CONTENT_PATH" ]; then
        echo "content file is missing: $CONTENT_PATH" >&2
        exit 1
    fi
    content_name="$(basename "$CONTENT_PATH")"
    content_extension="${content_name##*.}"
    if [ "$content_extension" = "$content_name" ]; then
        content_extension="bin"
    fi
    case "$content_extension" in
        *[!A-Za-z0-9]*)
            echo "content extension must be alphanumeric: $content_extension" >&2
            exit 2
            ;;
    esac
    REMOTE_CONTENT="$REMOTE_DIR/content/smoke-content.$content_extension"
fi
if [ -n "$SYSTEM_DIR" ] && [ ! -d "$SYSTEM_DIR" ]; then
    echo "RetroArch system directory is missing: $SYSTEM_DIR" >&2
    exit 1
fi

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    make -C "$ROOT_DIR" mlp1
fi

for path in \
    "$BUILD_DIR/bin/jawaka-retroarchctl" \
    "$RETROARCH_BIN" \
    "$CORE_SO" \
    "$CORE_INFO"; do
    if [ ! -f "$path" ]; then
        echo "missing required smoke artifact: $path" >&2
        exit 1
    fi
done
if [ ! -d "$RETROARCH_LIB_DIR" ]; then
    echo "missing RetroArch runtime library directory: $RETROARCH_LIB_DIR" >&2
    exit 1
fi
for library in libavcodec.so.60 libavformat.so.60 libavutil.so.58; do
    if [ ! -f "$RETROARCH_LIB_DIR/$library" ]; then
        echo "missing required RetroArch runtime library: $RETROARCH_LIB_DIR/$library" >&2
        exit 1
    fi
done

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

mkdir -p "$LOCAL_LOG_DIR"

cleanup() {
    set +e
    "${ADB[@]}" shell "
REMOTE='$REMOTE_DIR'
if [ -f \"\$REMOTE/retroarch.pid\" ]; then
    pid=\$(cat \"\$REMOTE/retroarch.pid\" 2>/dev/null || true)
    if [ -n \"\$pid\" ]; then
        kill \"\$pid\" 2>/dev/null || true
    fi
fi
if [ -f \"\$REMOTE/paused-pids\" ]; then
    while read pid; do
        [ -n \"\$pid\" ] && kill -CONT \"\$pid\" 2>/dev/null || true
    done < \"\$REMOTE/paused-pids\"
fi
" >/dev/null 2>&1 || true
    if [ ! -s "$LOCAL_LOG_DIR/retroarch.log" ]; then
        "${ADB[@]}" pull "$REMOTE_DIR/logs/." "$LOCAL_LOG_DIR/" >/dev/null 2>&1 || true
    fi
    "${ADB[@]}" shell "rm -rf '$REMOTE_DIR'" >/dev/null 2>&1 || true
    set -e
}
trap cleanup EXIT

echo "Staging RetroArch command smoke to $REMOTE_DIR"
"${ADB[@]}" shell "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR/bin' '$REMOTE_DIR/cores' '$REMOTE_DIR/info' '$REMOTE_DIR/lib' '$REMOTE_DIR/content' '$REMOTE_DIR/home/.config/retroarch' '$REMOTE_DIR/logs' '$REMOTE_DIR/system' '$REMOTE_DIR/saves' '$REMOTE_DIR/states'"
"${ADB[@]}" push "$RETROARCH_BIN" "$REMOTE_DIR/bin/retroarch" >/dev/null
"${ADB[@]}" push "$RETROARCH_LIB_DIR/." "$REMOTE_DIR/lib/" >/dev/null
"${ADB[@]}" push "$BUILD_DIR/bin/jawaka-retroarchctl" "$REMOTE_DIR/bin/jawaka-retroarchctl" >/dev/null
"${ADB[@]}" push "$CORE_SO" "$REMOTE_CORE" >/dev/null
"${ADB[@]}" push "$CORE_INFO" "$REMOTE_INFO" >/dev/null
if [ -n "$REMOTE_CONTENT" ]; then
    "${ADB[@]}" push "$CONTENT_PATH" "$REMOTE_CONTENT" >/dev/null
fi
if [ -n "$SYSTEM_DIR" ]; then
    "${ADB[@]}" push "$SYSTEM_DIR/." "$REMOTE_DIR/system/" >/dev/null
fi
"${ADB[@]}" shell "chmod 755 '$REMOTE_DIR/bin/retroarch' '$REMOTE_DIR/bin/jawaka-retroarchctl' '$REMOTE_CORE'"

"${ADB[@]}" shell "cat > '$REMOTE_DIR/retroarch.cfg' <<'CFG'
config_save_on_exit = \"false\"
video_driver = \"$VIDEO_DRIVER\"
video_context_driver = \"$VIDEO_CONTEXT_DRIVER\"
audio_driver = \"alsa\"
input_driver = \"sdl2\"
input_joypad_driver = \"sdl2\"
input_autodetect_enable = \"true\"
network_cmd_enable = \"true\"
network_cmd_port = \"$PORT\"
libretro_directory = \"$REMOTE_DIR/cores\"
libretro_info_path = \"$REMOTE_DIR/info\"
system_directory = \"$REMOTE_DIR/system\"
savefile_directory = \"$REMOTE_DIR/saves\"
savestate_directory = \"$REMOTE_DIR/states\"
rgui_browser_directory = \"$REMOTE_DIR\"
menu_driver = \"rgui\"
video_fullscreen = \"true\"
video_windowed_fullscreen = \"false\"
pause_nonactive = \"false\"
pause_on_disconnect = \"false\"
quit_press_twice = \"false\"
stdin_cmd_enable = \"false\"
log_verbosity = \"true\"
CFG"

"${ADB[@]}" shell "cat > '$REMOTE_DIR/launch-retroarch.sh' <<'SH'
#!/bin/sh
REMOTE='$REMOTE_DIR'
CONTENT='$REMOTE_CONTENT'
cd \"\$REMOTE\" || exit 1
export XDG_RUNTIME_DIR=/var/run
export WAYLAND_DISPLAY=wayland-0
export SDL_VIDEODRIVER=wayland
export HOME=\"\$REMOTE/home\"
export LD_LIBRARY_PATH=\"\$REMOTE/lib\${LD_LIBRARY_PATH:+:\$LD_LIBRARY_PATH}\"
if [ -n \"\$CONTENT\" ]; then
    exec \"\$REMOTE/bin/retroarch\" --config \"\$REMOTE/retroarch.cfg\" -L '$REMOTE_CORE' \"\$CONTENT\" --verbose > '$RUN_LOG' 2>&1 < /dev/null
else
    exec \"\$REMOTE/bin/retroarch\" --config \"\$REMOTE/retroarch.cfg\" -L '$REMOTE_CORE' --verbose > '$RUN_LOG' 2>&1 < /dev/null
fi
SH
chmod 755 '$REMOTE_DIR/launch-retroarch.sh'"

echo "Pausing current launcher/Jawaka pids for screen ownership"
"${ADB[@]}" shell "
REMOTE='$REMOTE_DIR'
: > \"\$REMOTE/paused-pids\"
for name in loong_pangu jawaka-launcher jawakad; do
    for pid in \$(pidof \"\$name\" 2>/dev/null || true); do
        echo \"\$pid\" >> \"\$REMOTE/paused-pids\"
        kill -STOP \"\$pid\" 2>/dev/null || true
    done
done
kill \$(pidof retroarch 2>/dev/null) 2>/dev/null || true
"

echo "Launching RetroArch $CORE_LABEL smoke core"
"${ADB[@]}" shell "rm -f '$REMOTE_DIR/retroarch.pid' && start-stop-daemon -S -b -m -p '$REMOTE_DIR/retroarch.pid' -x '$REMOTE_DIR/launch-retroarch.sh'"
SMOKE_PID="$("${ADB[@]}" shell "cat '$REMOTE_DIR/retroarch.pid'" | tr -d '\r')"
case "$SMOKE_PID" in
    ''|*[!0-9]*)
        echo "RetroArch smoke PID is invalid: $SMOKE_PID" >&2
        exit 1
        ;;
esac

echo "Waiting for RetroArch command port $PORT"
ready=0
for _ in $(seq 1 20); do
    if "${ADB[@]}" shell "grep -q 'bringing_up_command_interface_at_port $PORT' '$RUN_LOG' 2>/dev/null"; then
        ready=1
        break
    fi
    sleep 0.5
done

if [ "$ready" != "1" ]; then
    echo "RetroArch command interface did not become ready." >&2
    "${ADB[@]}" shell "tail -120 '$RUN_LOG' 2>/dev/null || true"
    exit 1
fi

if [ -n "$REMOTE_CONTENT" ]; then
    echo "Waiting $CONTENT_WAIT_SECONDS seconds for content rendering"
    sleep "$CONTENT_WAIT_SECONDS"
    if ! "${ADB[@]}" shell "pidof retroarch >/dev/null 2>&1"; then
        echo "RetroArch exited while loading content." >&2
        "${ADB[@]}" shell "tail -160 '$RUN_LOG' 2>/dev/null || true"
        exit 1
    fi
fi

echo "Capturing rendered frame"
"${ADB[@]}" shell "
cd '$REMOTE_DIR/logs' || exit 1
rm -f wayland-screenshot-*.png gameplay.png
XDG_RUNTIME_DIR=/var/run WAYLAND_DISPLAY=wayland-0 weston-screenshooter
shot=\$(ls -1t wayland-screenshot-*.png 2>/dev/null | head -1)
[ -n \"\$shot\" ] || exit 1
mv \"\$shot\" gameplay.png
test -s gameplay.png
"

run_ctl() {
    echo "== $* =="
    "${ADB[@]}" shell "$CTL --timeout-ms 1500 --port '$PORT' $*"
}

INITIAL_STATUS="$(run_ctl status)"
printf '%s\n' "$INITIAL_STATUS"
run_ctl pause
run_ctl status
run_ctl resume
run_ctl status
run_ctl menu-toggle
run_ctl menu-toggle
run_ctl save-state
sleep 1
"${ADB[@]}" shell "state_files=\$(find '$REMOTE_DIR/states' -type f -print 2>/dev/null | sed -n '1,20p'); if [ -n \"\$state_files\" ]; then printf '%s\n' \"\$state_files\"; else echo 'state_file=not_observed; smoke verifies the save/load command path'; fi"
run_ctl load-state
run_ctl quit

stopped=0
for _ in $(seq 1 8); do
    if ! "${ADB[@]}" shell "kill -0 '$SMOKE_PID' >/dev/null 2>&1"; then
        stopped=1
        break
    fi
    sleep 0.5
done
if [ "$stopped" != "1" ]; then
    echo "Warning: QUIT stopped the core but RetroArch did not exit; terminating smoke PID $SMOKE_PID." >&2
    "${ADB[@]}" shell "kill -KILL '$SMOKE_PID' 2>/dev/null || true"
    sleep 0.5
fi

echo "RetroArch command smoke log tail:"
"${ADB[@]}" shell "tail -120 '$RUN_LOG' 2>/dev/null || true"

if ! "${ADB[@]}" shell "grep -Fq '$REMOTE_CORE' '$RUN_LOG'"; then
    echo "RetroArch log does not identify the intended core library: $REMOTE_CORE" >&2
    exit 1
fi
if [ -n "$REMOTE_CONTENT" ] \
    && ! "${ADB[@]}" shell "grep -Fq '$REMOTE_CONTENT' '$RUN_LOG'"; then
    if ! grep -Fq 'content=smoke-content' <<<"$INITIAL_STATUS"; then
        echo "Neither the RetroArch log nor command status identifies the staged content: $REMOTE_CONTENT" >&2
        exit 1
    fi
fi

failure_pattern='(dynarec|gles|egl|loader|glibc).*(error|failed|not found|undefined)|(error|failed|not found|undefined).*(dynarec|gles|egl|loader|glibc)|failed to load (content|core|libretro)|undefined symbol'
if "${ADB[@]}" shell "grep -Eiq '$failure_pattern' '$RUN_LOG'"; then
    echo "RetroArch log contains a dynarec, graphics, loader, or GLIBC failure." >&2
    "${ADB[@]}" shell "grep -Ein '$failure_pattern' '$RUN_LOG' || true"
    exit 1
fi

"${ADB[@]}" pull "$REMOTE_DIR/logs/." "$LOCAL_LOG_DIR/" >/dev/null
if [ ! -s "$LOCAL_LOG_DIR/retroarch.log" ] || [ ! -s "$LOCAL_LOG_DIR/gameplay.png" ]; then
    echo "Failed to retain the smoke log and gameplay screenshot." >&2
    exit 1
fi

echo "RetroArch command smoke completed. Logs: $LOCAL_LOG_DIR"
