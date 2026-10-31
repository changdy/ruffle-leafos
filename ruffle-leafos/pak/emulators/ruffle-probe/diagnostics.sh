#!/bin/sh
# Bounded local report. Never dumps the full environment or executes a driver
# utility. GL information comes from the context used by the actual player.
set -eu
SELF_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
PAK_DIR="$(CDPATH= cd -- "$SELF_DIR/../.." && pwd -P)"
. "$SELF_DIR/paths.sh"
ruffle_paths
[ "$#" -eq 0 ] || ruffle_die '环境采集不接收游戏参数'
ruffle_write_check "$LOGS_PATH"
REPORT="$LOGS_PATH/ruffle-environment.txt"
[ ! -L "$REPORT" ] && [ ! -L "$REPORT.previous" ] || ruffle_die '诊断目标不能是符号链接'
TEMP_REPORT="$(mktemp "$LOGS_PATH/.ruffle-environment-XXXXXX")" || ruffle_die '无法创建诊断报告'
trap 'rm -f -- "$TEMP_REPORT"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

section() { printf '\n[%s]\n' "$1"; }
field() { printf '%s=%s\n' "$1" "$2"; }
read_bounded() {
    if [ -f "$1" ] && [ -r "$1" ]; then
        printf '\n--- %s (up to 65536 bytes) ---\n' "$1"
        head -c 65536 "$1" || printf '\nread_error\n'
        printf '\n'
    fi
}
{
    field report_schema 1
    field pak_version 0.8.2
    field collected_utc "$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
    field device_validation 'not performed by collector'
    section versions
    field leaf_version_export "${LEAF_VERSION:-unknown}"
    field loongos_version_export "${LOONGOS_VERSION:-unknown}"
    field declared_min_leaf '0.13.0 (package metadata; NOT detected device version)'
    field kernel "$(uname -sr 2>/dev/null || printf unknown)"
    field abi "$(uname -m 2>/dev/null || printf unknown)"
    LIBC_VERSION="$(getconf GNU_LIBC_VERSION 2>/dev/null || true)"
    if [ -z "$LIBC_VERSION" ] && [ -x /lib/libc.so.6 ]; then
        LIBC_VERSION="$(/lib/libc.so.6 2>/dev/null | head -n 1)"
    fi
    field libc "${LIBC_VERSION:-unknown}"
    read_bounded /etc/os-release
    read_bounded /etc/version
    # Record candidates verbatim, with provenance. No guessed version parsing.
    for VERSION_FILE in VERSION version version.txt version.json release.json build-info.json; do
        read_bounded "$CARD_FROM_PAK/.system/leaf/$VERSION_FILE"
    done
    # Leaf 0.13 records the installed release here. A platform manifest's
    # 0.0.1-poc is not the Leaf release version; preserve both with provenance.
    read_bounded "$CARD_FROM_PAK/.umrk/${PLATFORM:-mlp1}/release.json"
    read_bounded "${SYSTEM_PATH:-$CARD_FROM_PAK/.system/leaf/platforms/mlp1}/manifest.json"
    section runtime_paths
    field pak "$PAK_DIR"
    field roms "$ROMS_PATH"
    field userdata "$USERDATA_PATH"
    field saves "${SAVES_PATH:-not exported; progress backend unresolved}"
    field logs "$LOGS_PATH"
    field home "$STATE_DIR/home"
    field virtual_input "${JAWAKA_INPUT_VIRTUAL_EVENT:-not exported}"
    field wayland_display "${WAYLAND_DISPLAY:-wayland-0}"
    field xdg_runtime "${XDG_RUNTIME_DIR:-/var/run}"
    field menu_contract 'supports_menu=true; Leaf 0.13 Guide tap opens internal pause menu; hold escape remains system-owned'
    section catalog
    read_bounded "$CARD_FROM_PAK/.umrk/mlp1/catalog/diagnostics.json"
    read_bounded "$CARD_FROM_PAK/.umrk/mlp1/catalog/current"
    section memory
    read_bounded /proc/meminfo
    section mounts
    read_bounded /proc/mounts
    section input_devices
    read_bounded /proc/bus/input/devices
    section audio_devices
    field audio_route "${RUFFLE_AUDIO_ROUTE:-not selected by this standalone collection}"
    field alsa_config "${ALSA_CONFIG_PATH:-system default}"
    read_bounded /proc/asound/cards
    read_bounded /proc/asound/pcm
    section thermal
    for THERMAL_FILE in /sys/class/thermal/thermal_zone*/type /sys/class/thermal/thermal_zone*/temp; do
        read_bounded "$THERMAL_FILE"
    done
    section renderer
    field evidence 'See leaf-graphics actual= and leaf-vulkan in ruffle-probe.log; GL strings identify the presentation context, not necessarily the Flash renderer'
    field performance 'Present intervals and swap-call wall time only; NOT GPU execution time or power measurements'
    section source
    field runtime 'source-built Ruffle Flash; see build-manifest.json for binary SHA-256'
    field source_status 'pinned public base and local Leaf frontend; see provenance/leaf-source.lock.json'
} >"$TEMP_REPORT" || ruffle_die '诊断写入失败，上一份报告仍保留'
if [ -f "$REPORT" ]; then
    cp -f -- "$REPORT" "$REPORT.previous" || ruffle_die '无法保留上一份诊断报告'
fi
mv -f -- "$TEMP_REPORT" "$REPORT" || ruffle_die '无法提交诊断报告'
trap - EXIT HUP INT TERM
printf 'Ruffle 环境报告：%s\n' "$REPORT"
