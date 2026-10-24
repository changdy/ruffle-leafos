#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/mlp1-input-capture"
RUN_SECONDS="${JAWAKA_MLP1_INPUT_SECONDS:-75}"
REMOTE_LOG="${JAWAKA_MLP1_INPUT_REMOTE_LOG:-/tmp/mlp1-input-capture.evtest.log}"
EVENT_DEV="${JAWAKA_MLP1_INPUT_DEV:-}"
GRAB="${JAWAKA_MLP1_INPUT_GRAB:-1}"

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

if [ -z "$EVENT_DEV" ]; then
    devices="$("${ADB[@]}" shell 'cat /proc/bus/input/devices' | tr -d '\r')"
    event_name="$(printf '%s\n' "$devices" | awk '
        BEGIN { RS = ""; FS = "\n" }
        /Name="Loong Gamepad"/ {
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^H:/ && match($i, /event[0-9]+/)) {
                    print substr($i, RSTART, RLENGTH)
                    exit
                }
            }
        }
    ')"
    if [ -z "$event_name" ]; then
        echo "Could not find Loong Gamepad event device." >&2
        exit 1
    fi
    EVENT_DEV="/dev/input/$event_name"
fi

mkdir -p "$BUILD_DIR"
stamp="$(date '+%Y%m%d-%H%M%S')"
LOCAL_LOG="$BUILD_DIR/$stamp-evtest.log"
SUMMARY_LOG="$BUILD_DIR/$stamp-summary.txt"

grab_arg=""
if [ "$GRAB" != "0" ]; then
    grab_arg="--grab"
fi

echo "Capturing $EVENT_DEV for ${RUN_SECONDS}s with evtest ${grab_arg:-without --grab}."
echo "Sequence:"
echo "  dpad up, dpad down, dpad left, dpad right"
echo "  stick up, stick down, stick left, stick right"
echo "  A, B, X, Y"
echo "  L1, R1, L2, R2"
echo "  Select, Start, Menu, stick-click, Volume-, Volume+"
echo "Starting in 5 seconds..."
sleep 5

"${ADB[@]}" shell "
rm -f '$REMOTE_LOG'
{
    echo 'MLP1 input capture'
    echo 'device=$EVENT_DEV'
    echo 'seconds=$RUN_SECONDS'
    echo 'grab=$GRAB'
    echo 'sequence=dpad_udlr stick_udlr abxy l1r1l2r2 select start menu stickclick voldown volup'
    echo
} > '$REMOTE_LOG'
timeout '$RUN_SECONDS' evtest $grab_arg '$EVENT_DEV' >> '$REMOTE_LOG' 2>&1
rc=\$?
echo >> '$REMOTE_LOG'
echo \"evtest_rc=\$rc\" >> '$REMOTE_LOG'
exit 0
"

"${ADB[@]}" pull "$REMOTE_LOG" "$LOCAL_LOG" >/dev/null

perl -ne '
    if (/Event: time ([0-9.]+), type \d+ \(EV_KEY\), code (\d+) \(([^)]+)\), value 1/) {
        print "$1 KEY $3 ($2) press\n";
    } elsif (/Event: time ([0-9.]+), type \d+ \(EV_KEY\), code (\d+) \(([^)]+)\), value 0/) {
        print "$1 KEY $3 ($2) release\n";
    } elsif (/Event: time ([0-9.]+), type \d+ \(EV_ABS\), code (\d+) \(([^)]+)\), value (-?\d+)/) {
        print "$1 ABS $3 ($2) value $4\n";
    }
' "$LOCAL_LOG" > "$SUMMARY_LOG"

echo
echo "Raw log: $LOCAL_LOG"
echo "Summary: $SUMMARY_LOG"
echo
echo "Captured event summary:"
sed -n '1,220p' "$SUMMARY_LOG"
