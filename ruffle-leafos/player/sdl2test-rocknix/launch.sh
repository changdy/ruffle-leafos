#!/bin/sh
# Example launcher for ROCKNIX (sway / Wayland)

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run/0-runtime-dir}"
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-1}"
export SDL_VIDEODRIVER="${SDL_VIDEODRIVER:-wayland}"
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-pulseaudio}"

cd "$(dirname "$0")"
exec ./sdl2test-rocknix "$@" --fullscreen
