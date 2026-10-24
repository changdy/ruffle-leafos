#!/bin/sh
set -eu

PAK_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SDCARD_PATH=${SDCARD_PATH:-/mnt/sdcard}
PLATFORM=${PLATFORM:-mlp1}

if [ -n "${UMRK_ENV_FILE:-}" ] && [ -f "$UMRK_ENV_FILE" ]; then
    . "$UMRK_ENV_FILE"
elif [ -f "$SDCARD_PATH/.system/leaf/platforms/$PLATFORM/launcher/env.sh" ]; then
    . "$SDCARD_PATH/.system/leaf/platforms/$PLATFORM/launcher/env.sh"
elif [ -f "$SDCARD_PATH/.system/leaf/launcher/env.sh" ]; then
    . "$SDCARD_PATH/.system/leaf/launcher/env.sh"
fi

STATE_DIR="${USERDATA_PATH:-$SDCARD_PATH/.userdata/$PLATFORM}/ruffle"
LOG_DIR="${LOGS_PATH:-${USERDATA_PATH:-$SDCARD_PATH/.userdata/$PLATFORM}/logs}"
mkdir -p "$STATE_DIR/home" "$STATE_DIR/config" "$STATE_DIR/cache" "$STATE_DIR/data" "$LOG_DIR"

export HOME="$STATE_DIR/home"
export XDG_CONFIG_HOME="$STATE_DIR/config"
export XDG_CACHE_HOME="$STATE_DIR/cache"
export XDG_DATA_HOME="$STATE_DIR/data"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export SDL_VIDEODRIVER="${RUFFLE_SDL_VIDEODRIVER:-wayland}"
export SDL_AUDIODRIVER="${RUFFLE_SDL_AUDIODRIVER:-alsa}"
export RUFFLE_DISPLAY_ROTATION="${RUFFLE_DISPLAY_ROTATION:-0}"
export RUFFLE_SWAP_ABXY="${RUFFLE_SWAP_ABXY:-1}"
export RUFFLE_START_IN_MOUSE_MODE="${RUFFLE_START_IN_MOUSE_MODE:-1}"
export RUFFLE_FLASH_DIR="${RUFFLE_FLASH_DIR:-$SDCARD_PATH/Roms/flash}"
export RUFFLE_MENU_FONT="${RUFFLE_MENU_FONT:-$PAK_DIR/res/menu-font.ttf}"
export RUST_LOG="${RUST_LOG:-info}"
export LD_LIBRARY_PATH="$PAK_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_PRELOAD="${RUFFLE_SDL_PRELOAD:-/usr/lib/libSDL2-2.0.so.0}${LD_PRELOAD:+:$LD_PRELOAD}"

if [ -n "${JAWAKA_INPUT_VIRTUAL_EVENT:-}" ]; then
    export SDL_JOYSTICK_DEVICE="$JAWAKA_INPUT_VIRTUAL_EVENT"
fi

LOG_FILE="$LOG_DIR/ruffle.log"
printf '\n=== ruffle launch %s ===\n' "$(date '+%Y-%m-%d %H:%M:%S')" >>"$LOG_FILE"
printf 'version=leaf-12 video=%s audio=%s rotation=%s swap_abxy=%s mouse_mode=%s runtime=%s wayland=%s\n' \
    "$SDL_VIDEODRIVER" "$SDL_AUDIODRIVER" "$RUFFLE_DISPLAY_ROTATION" "$RUFFLE_SWAP_ABXY" "$RUFFLE_START_IN_MOUSE_MODE" "$XDG_RUNTIME_DIR" \
    "$WAYLAND_DISPLAY" \
    >>"$LOG_FILE"

if [ "$#" -gt 0 ]; then
    printf 'swf=%s\n' "$1" >>"$LOG_FILE"
    cd "$PAK_DIR"
    exec "$PAK_DIR/bin/ruffle" "$@" --fullscreen >>"$LOG_FILE" 2>&1
else
    printf 'menu_root=%s\n' "$RUFFLE_FLASH_DIR" >>"$LOG_FILE"
fi

RETURN_TO_MENU_FILE="$STATE_DIR/return-to-menu"
export RUFFLE_RETURN_TO_MENU_FILE="$RETURN_TO_MENU_FILE"
cd "$PAK_DIR"
while :; do
    rm -f "$RETURN_TO_MENU_FILE"
    if "$PAK_DIR/bin/ruffle" --fullscreen >>"$LOG_FILE" 2>&1; then
        status=0
    else
        status=$?
    fi
    if [ -f "$RETURN_TO_MENU_FILE" ]; then
        rm -f "$RETURN_TO_MENU_FILE"
        continue
    fi
    exit "$status"
done
