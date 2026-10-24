#!/usr/bin/env bash
# Push the cross-built chord state-machine test to the MLP1 and run it.
#
# The test drives the proxy's static handler with synthetic events; its "virtual
# pad" is a temp file and its physical fd is -1, so it neither grabs nor emits
# anything on a real device and is safe to run with the launcher up.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/build/mlp1/bin/input-proxy-chord-test"
REMOTE="/tmp/input-proxy-chord-test"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run: make mlp1-adb-chord-test" >&2
    exit 1
fi

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
"${ADB[@]}" push "$BIN" "$REMOTE" >/dev/null
"${ADB[@]}" shell "chmod 755 '$REMOTE'"
# adb shell's exit status is unreliable on this firmware; echo a sentinel and
# grep for it instead of trusting $?.
out="$("${ADB[@]}" shell "'$REMOTE'; echo rc=\$?" | tr -d '\r')"
printf '%s\n' "$out"
"${ADB[@]}" shell "rm -f '$REMOTE'"
grep -q '^rc=0$' <<<"$out"
