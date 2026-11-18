#!/bin/sh
set -eu

roms=/storage/roms
app="$roms/applyCenter/.ruffle-flash"
# RGBox can rebind /storage/roms to internal storage when the card disappears.
[ "$(stat -c %d "$roms")" != "$(stat -c %d /storage)" ] || exit 1
cd "$app"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run/0-runtime-dir}"
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    for display in wayland-1 wayland-0; do
        if [ -S "$XDG_RUNTIME_DIR/$display" ]; then WAYLAND_DISPLAY=$display; break; fi
    done
fi
export WAYLAND_DISPLAY SDL_VIDEODRIVER=wayland PYSDL2_DLL_PATH=/usr/lib
export SDL_RENDER_DRIVER=opengles2

# Match AisOS face-button labels and expose the system pad's Guide button.
export SDL_GAMECONTROLLERCONFIG='19000f6a706c61795f6a6f7973746900,play_joystick,a:b1,b:b0,x:b3,y:b2,back:b8,guide:b10,start:b9,leftstick:b11,rightstick:b12,leftshoulder:b4,rightshoulder:b5,dpup:b14,dpdown:b15,dpleft:b16,dpright:b17,leftx:a0,lefty:a1,rightx:a3,righty:a4,lefttrigger:a2,righttrigger:a5,platform:Linux,'
export RUFFLE_SWAP_AB=0 RUFFLE_SYSTEM_LANGUAGE=zh
export RUFFLE_FLASH_DIR="$roms/flash" RUFFLE_MENU_FONT="$app/MiSans-Normal.ttf"
export RUFFLE_FONT_FILE="$RUFFLE_MENU_FONT" SDCARD_PATH="$roms"
data="$roms/gamedata/ruffle-flash"
export HOME="$data/home" XDG_CACHE_HOME="$data/cache" XDG_CONFIG_HOME="$data/config"
export XDG_DATA_HOME="$data/data" RUFFLE_CONTROLS_DIR="$XDG_CONFIG_HOME/controls"
export RUFFLE_STORAGE_DIR="$data/shared_objects"
mkdir -p "$HOME" "$XDG_CACHE_HOME" "$RUFFLE_CONTROLS_DIR" "$XDG_DATA_HOME" "$RUFFLE_STORAGE_DIR" "$RUFFLE_FLASH_DIR"
if [ -f "$data/session.log" ]; then mv -f "$data/session.log" "$data/session.previous.log"; fi
exec /usr/bin/python3 -B -u "$app/launcher.py" >"$data/session.log" 2>&1
