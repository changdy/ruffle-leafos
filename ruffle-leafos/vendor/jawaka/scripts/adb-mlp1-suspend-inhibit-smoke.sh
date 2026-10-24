#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/mlp1/bin"
REMOTE_DIR="${JAWAKA_INHIBIT_SMOKE_REMOTE_DIR:-/tmp/umrk-suspend-inhibit-smoke}"
SOCKET="${UMRK_DAEMON_SOCKET:-/tmp/jawaka-runtime/jawakad.sock}"
HOLD_SECONDS="${JAWAKA_INHIBIT_HOLD_SECONDS:-45}"

if [ "${CONFIRM_SUSPEND_SMOKE:-0}" != "1" ]; then
    echo "This smoke intentionally deep-suspends the MLP1 twice." >&2
    echo "Set CONFIRM_SUSPEND_SMOKE=1 and keep the power button accessible." >&2
    exit 2
fi

if [ "${SKIP_BUILD:-0}" != "1" ]; then
    make -C "$ROOT_DIR" mlp1 mlp1-inhibit-smoke
fi

for binary in jawaka-inhibitctl jawaka-platformctl; do
    test -f "$BUILD_DIR/$binary" || { echo "missing $BUILD_DIR/$binary" >&2; exit 1; }
done

if [ -n "${ADB_SERIAL:-}" ]; then
    ADB=(adb -s "$ADB_SERIAL")
else
    serial="$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')"
    test -n "${serial:-}" || { echo "No online adb device found." >&2; exit 1; }
    ADB=(adb -s "$serial")
fi

echo "Using adb device: $("${ADB[@]}" get-serialno)"

echo "Staging the candidate jawakad and restarting Leaf"
"${ADB[@]}" push "$BUILD_DIR/jawakad" /tmp/jawakad.phase3 >/dev/null
"${ADB[@]}" shell '
set -e
target=""
for root in /mnt/sdcard /media/sdcard1; do
    for name in loong_pangu jawakad; do
        candidate="$root/.system/leaf/platforms/mlp1/launcher/bin/$name"
        if [ -f "$candidate" ]; then target="$candidate"; break 2; fi
    done
done
[ -n "$target" ] || { echo "installed jawakad not found" >&2; exit 1; }
chmod 755 /tmp/jawakad.phase3
mv /tmp/jawakad.phase3 "$target"
'
ADB_SERIAL="$("${ADB[@]}" get-serialno)" "$ROOT_DIR/../Leaf/scripts/adb-restart-loong.sh"

"${ADB[@]}" shell "rm -rf '$REMOTE_DIR' && mkdir -p '$REMOTE_DIR'"
"${ADB[@]}" push "$BUILD_DIR/jawaka-inhibitctl" "$REMOTE_DIR/jawaka-inhibitctl" >/dev/null
"${ADB[@]}" push "$BUILD_DIR/jawaka-platformctl" "$REMOTE_DIR/jawaka-platformctl" >/dev/null
"${ADB[@]}" shell "chmod 755 '$REMOTE_DIR/jawaka-inhibitctl' '$REMOTE_DIR/jawaka-platformctl'"

wait_for_active() {
    local expected="$1"
    for _ in $(seq 1 30); do
        status="$("${ADB[@]}" shell "$REMOTE_DIR/jawaka-inhibitctl --socket '$SOCKET' status" 2>/dev/null || true)"
        if [[ "$status" == *"\"active_count\":$expected"* ]]; then
            echo "$status"
            return 0
        fi
        sleep 0.2
    done
    echo "suspend inhibitor did not reach active_count=$expected" >&2
    return 1
}

wait_for_zero_or_suspend() {
    for _ in $(seq 1 120); do
        status="$("${ADB[@]}" shell "$REMOTE_DIR/jawaka-inhibitctl --socket '$SOCKET' status" 2>/dev/null || true)"
        [[ "$status" == *'"active_count":0'* || -z "$status" ]] && return 0
        sleep 0.5
    done
    echo "lease neither cleared nor triggered pending suspend" >&2
    return 1
}

request_sleep() {
    "${ADB[@]}" shell "$REMOTE_DIR/jawaka-platformctl --socket '$SOCKET' request '{\"type\":\"platform-action\",\"action\":\"sleep\"}'"
}

wait_for_suspend_and_wake() {
    echo "Waiting for pending deep suspend..."
    suspended=0
    for _ in $(seq 1 30); do
        if ! "${ADB[@]}" shell true >/dev/null 2>&1; then
            suspended=1
            break
        fi
        sleep 0.5
    done
    if [ "$suspended" != "1" ]; then
        echo "device did not become unreachable after pending sleep" >&2
        return 1
    fi
    echo "Device suspended; waiting for RTC auto-wake."
    for _ in $(seq 1 80); do
        if "${ADB[@]}" shell true >/dev/null 2>&1; then
            "${ADB[@]}" shell "echo 0 > /sys/class/rtc/rtc0/wakealarm 2>/dev/null || true"
            sleep 1
            return 0
        fi
        sleep 0.5
    done
    echo "RTC wake did not return ADB; tap power once to resume the device." >&2
    return 1
}

arm_auto_wake() {
    "${ADB[@]}" shell "echo 0 > /sys/class/rtc/rtc0/wakealarm && echo +20 > /sys/class/rtc/rtc0/wakealarm"
}

echo "== Explicit release path =="
"${ADB[@]}" shell "start-stop-daemon -S -b -m -p '$REMOTE_DIR/holder.pid' -x '$REMOTE_DIR/jawaka-inhibitctl' -- --socket '$SOCKET' hold --reason 'mlp1 release smoke' --seconds '$HOLD_SECONDS' --heartbeat '$REMOTE_DIR/heartbeat'"
wait_for_active 1
request_sleep
first="$("${ADB[@]}" shell "cat '$REMOTE_DIR/heartbeat'")"
sleep 35
second="$("${ADB[@]}" shell "cat '$REMOTE_DIR/heartbeat'")"
if [ "$second" -le "$first" ]; then
    echo "helper did not advance while suspend was inhibited" >&2
    exit 1
fi
echo "Helper advanced beyond suspend grace: $first -> $second"
arm_auto_wake
wait_for_zero_or_suspend
wait_for_suspend_and_wake

echo "== Dead holder reap path =="
"${ADB[@]}" shell "rm -f '$REMOTE_DIR/heartbeat' && start-stop-daemon -S -b -m -p '$REMOTE_DIR/holder.pid' -x '$REMOTE_DIR/jawaka-inhibitctl' -- --socket '$SOCKET' hold --reason 'mlp1 reap smoke' --seconds 120 --heartbeat '$REMOTE_DIR/heartbeat'"
wait_for_active 1
request_sleep
sleep 3
arm_auto_wake
"${ADB[@]}" shell "kill \$(cat '$REMOTE_DIR/holder.pid')"
wait_for_zero_or_suspend
wait_for_suspend_and_wake

echo "MLP1 suspend inhibitor smoke passed."
