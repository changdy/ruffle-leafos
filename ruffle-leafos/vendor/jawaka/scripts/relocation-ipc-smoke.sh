#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-relocation-ipc.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
SECONDARY="$TMP_DIR/secondary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
CTL="$ROOT_DIR/build/bin/jawaka-platformctl"

cleanup() {
    status=$?
    set +e
    smoke_daemon_stop || status=1
    if [ "$status" -ne 0 ] && [ -f "$LOG" ]; then
        cat "$LOG" >&2
    fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT

make -C "$ROOT_DIR" jawakad jawaka-platformctl >/dev/null
mkdir -p "$PRIMARY/Roms/PORTS" "$PRIMARY/Images/PORTS" \
         "$SECONDARY/Roms/PORTS" "$SECONDARY/Images/PORTS" \
         "$STATE" "$RUNTIME"
for n in $(seq 1 40); do
    printf '#!/bin/sh\nexit 0\n' >"$PRIMARY/Roms/PORTS/Relocate$n.sh"
    chmod +x "$PRIMARY/Roms/PORTS/Relocate$n.sh"
done

start_daemon() {
    smoke_daemon_start "$ROOT_DIR" "$LOG" \
        UMRK_RUNTIME_PATH="$RUNTIME" \
        UMRK_DAEMON_SOCKET="$SOCKET" \
        UMRK_INTERNAL_DATA_PATH="$STATE" \
        JAWAKA_SDCARD_ROOT="$PRIMARY" \
        SDCARD_PATHS="$PRIMARY:$SECONDARY" \
        JAWAKA_SCAN_TEST_DELAY_MS=100 \
        build/bin/jawakad --daemon-only
    for _ in $(seq 1 500); do
        [ -S "$SOCKET" ] && return 0
        kill -0 "$DAEMON_PID" 2>/dev/null || return 1
        sleep 0.02
    done
    return 1
}

library_status() {
    "$CTL" --socket "$SOCKET" request '{"type":"library-status"}'
}

wait_for_idle_scan() {
    for _ in $(seq 1 500); do
        if ! status="$(library_status 2>/dev/null)"; then
            kill -0 "$DAEMON_PID" 2>/dev/null || return 1
            sleep 0.02
            continue
        fi
        if python3 -c 'import json,sys; d=json.load(sys.stdin); raise SystemExit(d.get("scan_running", True) or d.get("generation", 0) <= 0)' <<<"$status"; then
            printf '%s' "$status"
            return 0
        fi
        sleep 0.02
    done
    return 1
}

start_daemon
status="$(wait_for_idle_scan)"
generation="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["generation"])' <<<"$status")"
"$CTL" --socket "$SOCKET" capabilities |
    grep -F '"relocate-games-v1":true' >/dev/null

items="$(python3 -c 'import json; print(json.dumps([
    {"old":{"source_id":"primary","rom_relpath":f"PORTS/Relocate{i}.sh"},
     "new":{"source_id":"secondary_sd","rom_relpath":f"PORTS/Relocate{i}.sh"}}
    for i in range(1,41)], separators=(",",":")))')"
[ "${#items}" -gt 2048 ]

"$CTL" --socket "$SOCKET" request '{"type":"scan-library"}' >/dev/null
if "$CTL" --socket "$SOCKET" relocate-prepare busy-probe "$generation" "$items" \
    >"$TMP_DIR/busy.out" 2>"$TMP_DIR/busy.err"; then
    echo "prepare during active scan unexpectedly succeeded" >&2
    exit 1
fi
grep -F 'library scan is active' "$TMP_DIR/busy.err" >/dev/null
status="$(wait_for_idle_scan)"
generation="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["generation"])' <<<"$status")"

prepare="$("$CTL" --socket "$SOCKET" relocate-prepare move-large "$generation" "$items")"
grep -F '"state":"prepared"' <<<"$prepare" >/dev/null

launch="$("$CTL" --socket "$SOCKET" request \
    '{"type":"launch-game","system":"PORTS","rom_path":"Roms/PORTS/Relocate1.sh"}')"
grep -F '"message":"game is relocating"' <<<"$launch" >/dev/null

commit="$("$CTL" --socket "$SOCKET" relocate-commit move-large)"
commit_generation="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["mapping_generation"])' <<<"$commit")"
retry="$("$CTL" --socket "$SOCKET" relocate-commit move-large)"
retry_generation="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["mapping_generation"])' <<<"$retry")"
[ "$commit_generation" = "$retry_generation" ]

revert="$("$CTL" --socket "$SOCKET" relocate-revert move-large)"
revert_generation="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["mapping_generation"])' <<<"$revert")"
[ "$revert_generation" -eq $((commit_generation + 1)) ]
retry="$("$CTL" --socket "$SOCKET" relocate-revert move-large)"
retry_generation="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["mapping_generation"])' <<<"$retry")"
[ "$revert_generation" = "$retry_generation" ]

smoke_daemon_stop
rm -f "$SOCKET"
start_daemon
"$CTL" --socket "$SOCKET" relocate-status move-large |
    grep -F '"state":"reverted"' >/dev/null

finish="$("$CTL" --socket "$SOCKET" relocate-finish move-large)"
ticket="$(python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["scan_ticket_generation"]); assert d["scan_ticket_generation"] > d["mapping_generation"]' <<<"$finish")"
for _ in $(seq 1 500); do
    final="$("$CTL" --socket "$SOCKET" relocate-status move-large)"
    state="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["state"])' <<<"$final")"
    [ "$state" = "finished" ] && break
    sleep 0.02
done
[ "$state" = "finished" ]
final_generation="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["library_generation"])' <<<"$final")"
[ "$final_generation" -ge "$ticket" ]
status="$(wait_for_idle_scan)"
generation="$(python3 -c 'import json,sys; print(json.load(sys.stdin)["generation"])' <<<"$status")"
abort_prepare="$("$CTL" --socket "$SOCKET" relocate-prepare abort-op "$generation" "$items")"
grep -F '"state":"prepared"' <<<"$abort_prepare" >/dev/null
"$CTL" --socket "$SOCKET" relocate-abort abort-op |
    grep -F '"state":"aborted"' >/dev/null
"$CTL" --socket "$SOCKET" relocate-abort abort-op |
    grep -F '"state":"aborted"' >/dev/null

if "$CTL" --socket "$SOCKET" relocate-prepare stale-op 0 "$items" \
    >"$TMP_DIR/stale.out" 2>"$TMP_DIR/stale.err"; then
    echo "stale relocation unexpectedly succeeded" >&2
    exit 1
fi
grep -F 'stale library generation' "$TMP_DIR/stale.err" >/dev/null

echo "PASS relocation-ipc-smoke"
