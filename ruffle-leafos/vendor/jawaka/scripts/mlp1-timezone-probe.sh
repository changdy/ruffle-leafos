#!/usr/bin/env bash
# Push the cross-built time-zone probe to an MLP1 and run it there.
#
# The probe converts fixed UTC instants with TZ + tzset() + localtime_r(). It
# neither reads nor sets the system clock and writes nothing outside /tmp, so it
# is safe to run with the launcher up.
#
# Transport: adb by default, matching the other device scripts here. Set
# MLP1_SSH_HOST to use SSH instead, which is what you want when the device is
# only on the network, or when adb is busy:
#
#   MLP1_SSH_HOST=sshadmin@192.168.0.220 MLP1_SSH_PORT=2222 \
#     scripts/mlp1-timezone-probe.sh
#
# MLP1_SSH_PASS uses sshpass; leave it unset to authenticate normally.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT_DIR/build/mlp1/bin/timezone-probe"
REMOTE="/tmp/timezone-probe"
ARGS="${MLP1_TIMEZONE_PROBE_ARGS:--v}"

if [ ! -x "$BIN" ]; then
    echo "missing $BIN; run: make mlp1-device-timezone-test" >&2
    exit 1
fi

if [ -n "${MLP1_SSH_HOST:-}" ]; then
    port="${MLP1_SSH_PORT:-22}"
    ssh_cmd=(ssh -o StrictHostKeyChecking=no -p "$port" "$MLP1_SSH_HOST")
    scp_cmd=(scp -o StrictHostKeyChecking=no -P "$port")
    if [ -n "${MLP1_SSH_PASS:-}" ]; then
        command -v sshpass >/dev/null || { echo "MLP1_SSH_PASS needs sshpass" >&2; exit 1; }
        ssh_cmd=(sshpass -p "$MLP1_SSH_PASS" "${ssh_cmd[@]}")
        scp_cmd=(sshpass -p "$MLP1_SSH_PASS" "${scp_cmd[@]}")
    fi
    echo "Using ssh device: $MLP1_SSH_HOST:$port"
    "${scp_cmd[@]}" "$BIN" "$MLP1_SSH_HOST:$REMOTE" >/dev/null
    trap '"${ssh_cmd[@]}" "rm -f '"'$REMOTE'"'" >/dev/null 2>&1 || true' EXIT
    # ssh does forward the remote exit status, so no sentinel is needed here.
    "${ssh_cmd[@]}" "chmod 755 '$REMOTE' && '$REMOTE' $ARGS"
    exit 0
fi

if [ -n "${ADB_SERIAL:-}" ]; then
    ADB=(adb -s "$ADB_SERIAL")
else
    serial="$(adb devices | awk 'NR>1 && $2=="device" {print $1; exit}')"
    if [ -z "${serial:-}" ]; then
        echo "No online adb device found. Set MLP1_SSH_HOST to use SSH instead." >&2
        exit 1
    fi
    ADB=(adb -s "$serial")
fi

echo "Using adb device: $("${ADB[@]}" get-serialno)"
"${ADB[@]}" push "$BIN" "$REMOTE" >/dev/null
"${ADB[@]}" shell "chmod 755 '$REMOTE'"
# adb shell's exit status is unreliable on this firmware; echo a sentinel and
# grep for it instead of trusting $?.
out="$("${ADB[@]}" shell "'$REMOTE' $ARGS; echo rc=\$?" | tr -d '\r')"
printf '%s\n' "$out"
"${ADB[@]}" shell "rm -f '$REMOTE'"
grep -q '^rc=0$' <<<"$out"
