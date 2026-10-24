#!/bin/sh
# CONTENT-1 path core for the source-built Leaf frontend.
set -eu
SELF_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
PAK_DIR="$(CDPATH= cd -- "$SELF_DIR/../.." && pwd -P)"
. "$SELF_DIR/paths.sh"
ruffle_paths
IS_DEMO=0
if [ "$#" -eq 0 ] && [ "${RUFFLE_PROBE_APP_MODE:-0}" = 1 ]; then
    SWF="$SELF_DIR/res/Leaf-GPU-Probe.swf"
    IS_DEMO=1
else
    [ "$#" -eq 1 ] || ruffle_die '请传入一个选中的 SWF 文件路径'
    SWF="$1"
    case "$SWF" in /*) ;; *) ruffle_die '游戏路径必须为绝对路径' ;; esac
fi
case "$SWF" in *.[sS][wW][fF]) ;; *) ruffle_die '选中的文件不是 SWF' ;; esac
[ -f "$SWF" ] && [ -r "$SWF" ] || ruffle_die '游戏文件不存在或不可读'
# Resolve the file itself, not only its parent: reject symlink escapes.
SWF="$(readlink -f -- "$SWF")" || ruffle_die '无法解析游戏路径'
if [ "$IS_DEMO" = 1 ]; then
    FLASH_DIR="$ROMS_PATH/flash"
    DEFAULT_MODE=native
else
    ROM_ROOT="$(CDPATH= cd -- "$ROMS_PATH" && pwd -P)" || ruffle_die '游戏目录不存在'
    case "$SWF" in
        "$ROM_ROOT"/flash/*) FLASH_DIR="$ROM_ROOT/flash" ;;
        "$ROM_ROOT"/FLASH/*) FLASH_DIR="$ROM_ROOT/FLASH" ;; # Read legacy paths only.
        *) ruffle_die '游戏不在所选卡的 Roms/flash 目录内' ;;
    esac
    DEFAULT_MODE=native
fi
SESSION_MODE="${RUFFLE_SESSION_MODE:-$DEFAULT_MODE}"
case "$SESSION_MODE" in native|probe) ;; *) ruffle_die 'RUFFLE_SESSION_MODE 仅支持 native 或 probe' ;; esac
case "${RUFFLE_INPUT_MODE:-hybrid}" in hybrid|keyboard|mouse) ;; *) ruffle_die '控制方式必须为 hybrid、keyboard 或 mouse' ;; esac
case "${RUFFLE_PROBE_TIMING:-1}" in 0|1) ;; *) ruffle_die '帧间隔采样必须为 0 或 1' ;; esac
BIN="$SELF_DIR/bin/ruffle"
SHIM="$SELF_DIR/lib/libruffle_probe.so"
[ -f "$BIN" ] || ruffle_die '缺少播放器，请重新解压完整安装包'
[ -f "$SHIM" ] || ruffle_die '缺少诊断适配库，请重新解压完整安装包'
[ -f "$SELF_DIR/res/menu-font.ttf" ] || ruffle_die '缺少中文菜单字体'
SDL_LIB=/usr/lib/libSDL2-2.0.so.0
if [ ! -f "$SDL_LIB" ]; then SDL_LIB="$SELF_DIR/lib/libSDL2-2.0.so.0"; fi
[ -f "$SDL_LIB" ] || ruffle_die '缺少 SDL2 动态库'
[ -f "$SELF_DIR/lib/libasound.so.2" ] || ruffle_die '缺少 ALSA 音频动态库'
LOG_FILE="$LOGS_PATH/ruffle-probe.log"
if [ "${RUFFLE_PROBE_DRY_RUN:-0}" = 1 ]; then
    printf 'binary=%s\nargument=%s\nargument=--fullscreen\nflash_root=%s\nstate=%s\nlog=%s\npreload=%s:%s\nsession_mode=%s\nany_key_exit=%s\nhome=%s/home\nsaves=%s\n' \
        "$BIN" "$SWF" "$FLASH_DIR" "$STATE_DIR" "$LOG_FILE" "$SHIM" "$SDL_LIB" \
        "$SESSION_MODE" "$(if [ "$SESSION_MODE" = probe ]; then printf 1; else printf 0; fi)" \
        "$STATE_DIR" "${SAVES_PATH:-unresolved}"
    exit 0
fi
# Keep writes outside the updateable package. Confirm actual writes, not just -w.
for LOCAL_DIR in "$STATE_DIR/home" "$STATE_DIR/config" "$STATE_DIR/cache" "$STATE_DIR/data" "$LOGS_PATH"; do
    ruffle_write_check "$LOCAL_DIR"
done
export HOME="$STATE_DIR/home"
export XDG_CONFIG_HOME="$STATE_DIR/config" XDG_CACHE_HOME="$STATE_DIR/cache" XDG_DATA_HOME="$STATE_DIR/data"
export RUFFLE_CONTROLS_DIR="$STATE_DIR/config/controls"
ruffle_prepare_fonts
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/var/run}" WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export SDL_VIDEODRIVER=wayland SDL_AUDIODRIVER=alsa
# CPAL uses ALSA independently of SDL_AUDIODRIVER. The stock ALSA default is
# HDMI (card 0); Leaf's PulseAudio server routes to the internal RK817 sink.
# Respect an explicit ALSA configuration or an existing per-user .asoundrc.
RUFFLE_AUDIO_ROUTE=system-default
if [ -n "${ALSA_CONFIG_PATH:-}" ] || [ -e "$HOME/.asoundrc" ]; then
    RUFFLE_AUDIO_ROUTE=custom-alsa-config
elif [ -S /tmp/pulse-socket ] && [ -r /usr/lib/alsa-lib/libasound_module_pcm_pulse.so ] &&
     [ -r "$SELF_DIR/res/alsa-pulse.conf" ]; then
    export ALSA_CONFIG_PATH="$SELF_DIR/res/alsa-pulse.conf"
    export PULSE_SERVER="${PULSE_SERVER:-unix:/tmp/pulse-socket}"
    RUFFLE_AUDIO_ROUTE=leaf-pulseaudio
fi
export RUFFLE_AUDIO_ROUTE
export RUFFLE_DISPLAY_ROTATION=0 RUFFLE_SWAP_AB=1
export RUFFLE_INPUT_MODE="${RUFFLE_INPUT_MODE:-hybrid}"
export RUFFLE_FLASH_DIR="$FLASH_DIR" RUFFLE_MENU_FONT="$SELF_DIR/res/menu-font.ttf"
. "$SELF_DIR/theme.sh"
ruffle_load_theme
export RUFFLE_PROBE_DIAGNOSTICS=1 RUFFLE_PROBE_TIMING="${RUFFLE_PROBE_TIMING:-1}"
export RUFFLE_PROBE_ANY_KEY_EXIT=0
[ "$SESSION_MODE" = native ] || ruffle_die '新播放器只支持 native 会话'
export RUST_LOG="${RUST_LOG:-info}"
if [ -n "${JAWAKA_INPUT_VIRTUAL_EVENT:-}" ]; then export SDL_JOYSTICK_DEVICE="$JAWAKA_INPUT_VIRTUAL_EVENT"; fi
unset RUFFLE_RETURN_TO_MENU_FILE
[ ! -L "$LOG_FILE" ] && [ ! -L "$LOG_FILE.previous" ] || ruffle_die '日志目标不能是符号链接'
if [ -f "$LOG_FILE" ]; then mv -f -- "$LOG_FILE" "$LOG_FILE.previous" || ruffle_die '无法轮换日志'; fi
printf 'ruffle-leaf 0.6.3\nswf=%s\nmode=%s\nvideo=%s\nwayland=%s\nsdl=%s\n' \
    "$SWF" "$SESSION_MODE" "$SDL_VIDEODRIVER" "$WAYLAND_DISPLAY" "$SDL_LIB" >"$LOG_FILE" || ruffle_die '无法写入启动日志'
printf 'bundled_cjk_font=%s\n' "$RUFFLE_FONT_FILE" >>"$LOG_FILE"
printf 'audio_route=%s\n' "$RUFFLE_AUDIO_ROUTE" >>"$LOG_FILE"
# Collection failure must be visible, but cannot make a working game unlaunchable.
if ! "$SELF_DIR/diagnostics.sh" >>"$LOG_FILE" 2>&1; then
    printf 'environment_collection=failed; previous report retained\n' >>"$LOG_FILE"
fi
[ -n "${SAVES_PATH:-}" ] || ruffle_die 'Leaf 未提供存档目录'
case "$SAVES_PATH" in /*|?:/*) ;; *) ruffle_die '存档目录必须为绝对路径' ;; esac
export RUFFLE_STORAGE_DIR="$SAVES_PATH/ruffle-leaf-preview/shared_objects"
ruffle_write_check "$RUFFLE_STORAGE_DIR"
printf 'native_storage=%s/slots-v1; profiles=5; format=RLS1; native_progress_only=1\n' "$RUFFLE_STORAGE_DIR" >>"$LOG_FILE"
cd "$STATE_DIR"
export LD_LIBRARY_PATH="$SELF_DIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
# Keep preload out of all diagnostic utilities.
export LD_PRELOAD="$SHIM:$SDL_LIB${LD_PRELOAD:+:$LD_PRELOAD}"
if [ "$IS_DEMO" = 1 ]; then set -- --app-settings; else set --; fi
exec "$BIN" "$SWF" --fullscreen "$@" >>"$LOG_FILE" 2>&1
