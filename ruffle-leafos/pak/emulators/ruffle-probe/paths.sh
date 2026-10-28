#!/bin/sh
# Sourced by our launchers, never from a user-editable configuration file.
ruffle_die() { printf 'Ruffle: %s\n' "$*" >&2; exit 2; }

ruffle_paths() {
    [ "${PLATFORM:-mlp1}" = mlp1 ] || ruffle_die '仅支持 Miniloong Pocket 1（mlp1）'
    CARD_FROM_PAK="$(CDPATH= cd -- "$PAK_DIR/../../.." && pwd -P)"
    if [ "${RUFFLE_PROBE_APP_MODE:-0}" = 1 ]; then
        SDCARD_PATH="${SDCARD_PATH:-$CARD_FROM_PAK}"
    fi
    # Preserve paths already supplied for the selected ROM card, even if env.sh
    # describes the primary card. This matters when only one field is missing.
    SELECTED_ROMS="${ROMS_PATH:-}"
    SELECTED_USERDATA="${USERDATA_PATH:-}"
    SELECTED_LOGS="${LOGS_PATH:-}"
    SELECTED_SAVES="${SAVES_PATH:-}"
    if [ -z "$SELECTED_ROMS" ] || [ -z "$SELECTED_USERDATA" ] || [ -z "$SELECTED_LOGS" ]; then
        if [ -n "${UMRK_ENV_FILE:-}" ] && [ -f "$UMRK_ENV_FILE" ]; then
            . "$UMRK_ENV_FILE"
        elif [ -f "${SDCARD_PATH:-$CARD_FROM_PAK}/.system/leaf/platforms/mlp1/launcher/env.sh" ]; then
            . "${SDCARD_PATH:-$CARD_FROM_PAK}/.system/leaf/platforms/mlp1/launcher/env.sh"
        elif [ -f "${SDCARD_PATH:-$CARD_FROM_PAK}/.system/leaf/launcher/env.sh" ]; then
            . "${SDCARD_PATH:-$CARD_FROM_PAK}/.system/leaf/launcher/env.sh"
        fi
    fi
    if [ -n "$SELECTED_ROMS" ]; then ROMS_PATH="$SELECTED_ROMS"; fi
    if [ -n "$SELECTED_USERDATA" ]; then USERDATA_PATH="$SELECTED_USERDATA"; fi
    if [ -n "$SELECTED_LOGS" ]; then LOGS_PATH="$SELECTED_LOGS"; fi
    if [ -n "$SELECTED_SAVES" ]; then SAVES_PATH="$SELECTED_SAVES"; fi
    if [ "${RUFFLE_PROBE_APP_MODE:-0}" = 1 ]; then
        ROMS_PATH="${ROMS_PATH:-$SDCARD_PATH/Roms}"
        USERDATA_PATH="${USERDATA_PATH:-$SDCARD_PATH/.userdata/mlp1}"
        LOGS_PATH="${LOGS_PATH:-$USERDATA_PATH/logs}"
    fi
    [ -n "${ROMS_PATH:-}" ] || ruffle_die '缺少 ROMS_PATH，请从 Leaf 的 Flash 分类启动'
    [ -n "${USERDATA_PATH:-}" ] || ruffle_die '缺少 USERDATA_PATH'
    [ -n "${LOGS_PATH:-}" ] || ruffle_die '缺少 LOGS_PATH'
    for RUFFLE_ROOT in "$ROMS_PATH" "$USERDATA_PATH" "$LOGS_PATH"; do
        case "$RUFFLE_ROOT" in /*|?:/*) ;; *) ruffle_die 'Leaf 运行目录必须是绝对路径' ;; esac
    done
    # A misconfigured Leaf environment must never redirect user writes into pak.
    STATE_DIR="$USERDATA_PATH/ruffle-probe"
}

ruffle_write_check() {
    RUFFLE_PARENT="$1"
    while [ ! -d "$RUFFLE_PARENT" ]; do RUFFLE_PARENT="$(dirname -- "$RUFFLE_PARENT")"; done
    RUFFLE_CANON="$(CDPATH= cd -- "$RUFFLE_PARENT" && pwd -P)" || ruffle_die '无法解析数据目录'
    case "$RUFFLE_CANON/" in "$PAK_DIR/"*) ruffle_die '配置或日志目录不能位于安装包内' ;; esac
    mkdir -p "$1" || ruffle_die "无法创建本地目录：$1"
    RUFFLE_CANON="$(CDPATH= cd -- "$1" && pwd -P)" || ruffle_die "无法读取目录：$1"
    case "$RUFFLE_CANON/" in "$PAK_DIR/"*) ruffle_die '配置或日志目录不能位于安装包内' ;; esac
    RUFFLE_TEMP="$(mktemp "$1/.ruffle-write-XXXXXX")" || ruffle_die "目录不可写或存储卡已满：$1"
    if ! printf 'write-check\n' >"$RUFFLE_TEMP"; then
        rm -f -- "$RUFFLE_TEMP"
        ruffle_die "写入失败，请检查存储卡：$1"
    fi
    rm -f -- "$RUFFLE_TEMP" || ruffle_die "无法清理写入检查文件：$1"
}

# leaf-12 scans HOME/.local/share/fonts recursively, but selects its CJK
# fallback by the basename MiSans-Normal.ttf. The supplied menu font contains
# CJK glyphs; this compatibility alias does not change its internal font name.
ruffle_prepare_fonts() {
    RUFFLE_FONT_DIR="$HOME/.local/share/fonts/ruffle-leaf"
    for RUFFLE_FONT_PARENT in "$HOME" "$HOME/.local" "$HOME/.local/share" "$HOME/.local/share/fonts" "$RUFFLE_FONT_DIR"; do
        [ ! -L "$RUFFLE_FONT_PARENT" ] || ruffle_die '备用字体目录不能是符号链接'
    done
    ruffle_write_check "$RUFFLE_FONT_DIR"
    RUFFLE_FONT_FILE="$RUFFLE_FONT_DIR/MiSans-Normal.ttf"
    export RUFFLE_FONT_FILE
    [ ! -L "$RUFFLE_FONT_FILE" ] || ruffle_die '备用字体不能是符号链接'
    if [ -e "$RUFFLE_FONT_FILE" ] && [ ! -f "$RUFFLE_FONT_FILE" ]; then
        ruffle_die '备用字体路径不是普通文件'
    fi
    if ! cmp -s "$SELF_DIR/res/menu-font.ttf" "$RUFFLE_FONT_FILE"; then
        RUFFLE_FONT_TEMP="$(mktemp "$RUFFLE_FONT_DIR/.bundled-font-XXXXXX")" || ruffle_die '无法创建备用字体缓存'
        if ! cp -- "$SELF_DIR/res/menu-font.ttf" "$RUFFLE_FONT_TEMP" ||
           ! cmp -s "$SELF_DIR/res/menu-font.ttf" "$RUFFLE_FONT_TEMP" ||
           ! mv -f -- "$RUFFLE_FONT_TEMP" "$RUFFLE_FONT_FILE"; then
            rm -f -- "$RUFFLE_FONT_TEMP"
            ruffle_die '备用字体缓存更新失败，请检查存储空间'
        fi
    fi
}
