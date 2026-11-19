#!/bin/sh
set -eu

roms=/storage/roms
[ "$(stat -c %d "$roms")" != "$(stat -c %d /storage)" ] || exit 1
app="$roms/applyCenter/.screen-color"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run/0-runtime-dir}"
if [ -z "${WAYLAND_DISPLAY:-}" ]; then
    for display in wayland-1 wayland-0; do
        if [ -S "$XDG_RUNTIME_DIR/$display" ]; then WAYLAND_DISPLAY=$display; break; fi
    done
fi
export WAYLAND_DISPLAY SDL_VIDEODRIVER=wayland SDL_RENDER_DRIVER=opengles2 PYSDL2_DLL_PATH=/usr/lib
export SDL_GAMECONTROLLERCONFIG='19000f6a706c61795f6a6f7973746900,play_joystick,a:b1,b:b0,x:b3,y:b2,back:b8,guide:b12,start:b9,leftstick:b11,leftshoulder:b4,rightshoulder:b5,dpup:b14,dpdown:b15,dpleft:b16,dpright:b17,leftx:a0,lefty:a1,rightx:a3,righty:a4,lefttrigger:a2,righttrigger:a5,platform:Linux,'
exec /usr/bin/python3 -B "$app/screen_color_ui.py"
