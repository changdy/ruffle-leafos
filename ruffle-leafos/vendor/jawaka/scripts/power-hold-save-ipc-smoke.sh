#!/usr/bin/env bash
# Save Before Power Off, driven on the host: a real jawakad on the mock
# platform, a fake RetroArch that speaks the MLP1 synchronous-save protocol
# (scripts/fake-retroarch.py with FAKE_RA_SYNC_SAVE_BYTES), and power-key edges
# scripted through the mock input proxy (JAWAKA_MOCK_POWER_EDGES: one
# "down <stamp>" / "up <stamp>" per line, <stamp> absolute CLOCK_MONOTONIC ms or
# "+N" / "-N" ms relative to when the line is read; each edge is delivered no
# earlier than its stamp, in file order).
#
# Two test hooks exist only in host builds of jawakad (never under
# -DPLATFORM_MLP1), read once at start-up:
#   JAWAKA_TEST_POWER_HARD_CUT_S=10  the mock platform cannot apply a Force Off
#                                    Hold, so the policy would see the stock
#                                    5,700 ms cut and decline at once; this
#                                    makes it plan against a 10 s cut
#                                    (release boundary press + 5,100 ms).
#   JAWAKA_TEST_POWER_HOLD_SAVE_ANY_PLATFORM=1  opens the platform_id == "mlp1"
#                                    gate so the mock can stand in for the MLP1.
# Rows "off", "on-mac" and "stock-cut" run without those hooks and show the
# gates themselves; the rest exercise the policy wiring behind them.
#
# Every row asserts its log tokens and that jawakad exits on its own. Daemon
# smokes run in a Linux container (gcc:14 --init), not on the Mac.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"
FAKE_RA_PY="$ROOT_DIR/scripts/fake-retroarch.py"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-power-hold-save.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PLATFORM_ROOT="$PRIMARY/.system/leaf/platforms/mac"
DEFAULTS="$PLATFORM_ROOT/defaults"
CORES="$PLATFORM_ROOT/cores"
FAKE_RA="$TMP_DIR/retroarch"
CORE_FOLDER="Fixture RA"
ROM="Roms/N64/Hold Game.n64"
STATES_DIR="$PRIMARY/States/$CORE_FOLDER"
FINAL_STATE="$STATES_DIR/Hold Game.state99"
STATE_BYTES=1048576
CASE=""
LOG=""

cleanup() {
    status=$?
    set +e
    smoke_daemon_stop || status=1
    pkill -f "$TMP_DIR" 2>/dev/null
    if [ "$status" -ne 0 ]; then
        for log in "$TMP_DIR"/*.log; do
            [ -f "$log" ] || continue
            echo "---- $log" >&2
            sed -n '1,400p' "$log" >&2
        done
    fi
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT

fail() { echo "FAIL power-hold-save-ipc-smoke${CASE:+ ($CASE)}: $*" >&2; exit 1; }

# The fake binds RetroArch's command port; a stray listener would answer the
# probe in its place.
if python3 - <<'PY'
import socket, sys
s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    s.bind(("127.0.0.1", 55355))
except OSError:
    sys.exit(0)
sys.exit(1)
PY
then
    fail "UDP 127.0.0.1:55355 is already in use"
fi

make -C "$ROOT_DIR" BUILD="$BUILD_REL" jawakad jawaka-platformctl >/dev/null

mkdir -p "$USERDATA" "$LOGS" "$DEFAULTS" "$CORES" "$PRIMARY/Roms/N64" \
         "$PRIMARY/Images/N64" "$PRIMARY/Saves" "$PRIMARY/States"
printf 'core\n' >"$CORES/fixture_libretro.so"
printf 'rom\n' >"$PRIMARY/$ROM"
cat >"$FAKE_RA" <<EOF
#!/bin/sh
exec python3 "$FAKE_RA_PY" "\$@"
EOF
chmod 755 "$FAKE_RA"

python3 - "$DEFAULTS" "$CORE_FOLDER" <<'CATALOG'
import json, pathlib, sys
defaults = pathlib.Path(sys.argv[1])
core = {"id": "fixture_ra", "display_name": "Fixture RetroArch",
        "type": "retroarch", "libretro_name": "fixture",
        "file_name": "fixture_libretro.so", "config_folder": sys.argv[2],
        "info_name": "fixture_libretro.info", "path": None,
        "supports_menu": True, "supports_savestate": True,
        "supports_disk_control": False, "needs_swap": False,
        "status": "packaged"}
(defaults / "cores.json").write_text(json.dumps(
    {"version": 2, "platform": "mac", "cores": [core]}))
(defaults / "systems.json").write_text(json.dumps(
    {"version": 2, "platform": "mac", "systems": [{
        "id": "N64", "name": "Nintendo 64", "patterns": ["N64"],
        "extensions": ["n64"], "archive_extensions": [],
        "archive_inner_extensions": ["n64"], "archive_mode": "pass_through",
        "file_names": [], "ignore_file_names": [], "playlist_extensions": [],
        "m3u_generation": "none", "default_core": "fixture_ra",
        "alternate_cores": [], "rom_root": "Roms/N64",
        "image_root": "Images/N64", "bios_notes": []}]}))
CATALOG

request() {
    "$CTL" --socket "$SOCKET" request "$1" 2>/dev/null || true
}

wait_log() { # pattern, tenths of a second
    local i
    for i in $(seq 1 "$2"); do
        grep -F -q -- "$1" "$LOG" && return 0
        sleep 0.1
    done
    return 1
}

expect() { grep -F -q -- "$1" "$LOG" || fail "missing log line: $1"; }
expect_not() { ! grep -F -q -- "$1" "$LOG" || fail "unexpected log line: $1"; }
# Lines a power-hold save attempt would leave, as opposed to the setting cache
# line that the poll logs on its own.
ATTEMPT_RE='power-hold save: (waiting|saving|saved|unsupported|still-held|budget-insufficient|storage-error|timeout|child-exited|durability-uncertain|interrupted|quit grace|RetroArch cannot|session has no|could not)'
expect_no_attempt() { ! grep -E -q -- "$ATTEMPT_RE" "$LOG" || fail "a save attempt was made"; }
expect_daemon_gone() {
    local i
    for i in $(seq 1 300); do
        if ! kill -0 "$DAEMON_PID" 2>/dev/null; then
            wait "$DAEMON_PID" 2>/dev/null || true
            DAEMON_PID=""
            return 0
        fi
        sleep 0.1
    done
    fail "jawakad did not exit after the long press"
}

# run_case NAME SETTING(0|1) [NAME=VALUE ...] -- EDGE ...
#   Fresh daemon, library and States per row. Launches the fixture game,
#   waits for the fake RetroArch to bind its port, writes the edges, then
#   waits for jawakad to exit on its own.
run_case() {
    CASE="$1"
    local setting="$2"
    shift 2
    local extra=()
    while [ "$#" -gt 0 ] && [ "$1" != "--" ]; do
        extra+=("$1")
        shift
    done
    [ "${1:-}" = "--" ] || fail "run_case: no edges"
    shift
    RUNTIME="$TMP_DIR/runtime-$CASE"
    SOCKET="$RUNTIME/jawakad.sock"
    LOG="$TMP_DIR/$CASE.log"
    EDGES="$TMP_DIR/$CASE.edges"
    RA_COMMANDS="$TMP_DIR/$CASE.ra-commands"
    RA_READY="$RUNTIME/ra-ready"
    rm -rf "$RUNTIME" "$STATE" "$PRIMARY/States"
    mkdir -p "$RUNTIME" "$STATE" "$PRIMARY/States"
    : >"$EDGES"
    : >"$RA_COMMANDS"

    smoke_daemon_start "$ROOT_DIR" "$LOG" \
        PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$PRIMARY/Apps" \
        USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
        SAVES_PATH="$PRIMARY/Saves" STATES_PATH="$PRIMARY/States" \
        UMRK_PLATFORM_PATH="$PLATFORM_ROOT" UMRK_RUNTIME_PATH="$RUNTIME" \
        UMRK_DAEMON_SOCKET="$SOCKET" UMRK_INTERNAL_DATA_PATH="$STATE" \
        JAWAKA_SDCARD_ROOT="$PRIMARY" CORES_PATH="$CORES" \
        UMRK_RETROARCH_BIN="$FAKE_RA" \
        JAWAKA_MOCK_POWER_EDGES="$EDGES" \
        FAKE_RA_QUIT_LOG="$RA_COMMANDS" FAKE_RA_READY="$RA_READY" \
        FAKE_RA_STATES_DIR="$STATES_DIR" \
        "${extra[@]}" \
        "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only

    local i status
    for i in $(seq 1 300); do
        [ -S "$SOCKET" ] && break
        kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon exited during startup"
        sleep 0.02
    done
    [ -S "$SOCKET" ] || fail "daemon socket never appeared"
    for i in $(seq 1 500); do
        status="$(request '{"type":"library-status"}')"
        if python3 -c 'import json,sys; d=json.load(sys.stdin); raise SystemExit(d.get("scan_running", True) or d.get("generation", 0) <= 0)' <<<"$status" 2>/dev/null; then
            break
        fi
        sleep 0.02
    done

    if [ "$setting" = 1 ]; then
        python3 - "$STATE/library.db" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("INSERT OR REPLACE INTO settings(key,value) VALUES ('save_state_on_power_hold','1')")
db.commit()
PY
        # The daemon re-reads the setting on its 2 s poll once the file changed.
        wait_log 'power-hold save: setting on' 80 || fail "setting never read as on"
    fi

    request "{\"type\":\"launch-game\",\"system\":\"N64\",\"rom_path\":\"$ROM\"}" |
        grep -F '"type":"ok"' >/dev/null || fail "launch-game refused"
    wait_log 'RetroArch session started pid=' 100 || fail "RetroArch session never started"
    for i in $(seq 1 200); do
        [ -s "$RA_READY" ] && break
        sleep 0.02
    done
    [ -s "$RA_READY" ] || fail "fake RetroArch never bound its port"
    sleep 0.2

    printf '%s\n' "$@" >>"$EDGES"
    expect_daemon_gone
}

# Common to every row: the long press shuts down and the game child is gone.
expect_shutdown() {
    expect 'power: long-press'
    expect 'RetroArch session'
}

# --- Setting Off: existing shutdown, no attempt, no probe. --------------------
run_case off 0 -- "down +0"
expect_shutdown
expect_not 'power-hold save: setting on'
expect_no_attempt
grep -q 'GET_STATE_SAVE_INFO' "$RA_COMMANDS" && fail "RetroArch was probed with the setting Off"
echo "row off: long press -> shutdown, no save attempt"

# --- Setting On on a platform that is not the MLP1: gated, no attempt. --------
run_case on-mac 1 -- "down +0"
expect_shutdown
expect 'power-hold save: setting on'
expect_no_attempt
grep -q 'GET_STATE_SAVE_INFO' "$RA_COMMANDS" && fail "RetroArch was probed on a non-MLP1 platform"
echo "row on-mac: platform gate holds"

# --- Gate open, no Force Off Hold applied: the stock cut leaves no release ---
# --- wait, so the policy declines before any prompt. ------------------------
run_case stock-cut 1 JAWAKA_TEST_POWER_HOLD_SAVE_ANY_PLATFORM=1 \
    FAKE_RA_SYNC_SAVE_BYTES="$STATE_BYTES" -- "down +0"
expect_shutdown
expect 'power-hold save: budget-insufficient elapsed_ms='
expect 'key=held'
expect_not 'power-hold save: waiting for release'
expect_not 'power-hold save: saving'
echo "row stock-cut: 6 s cut -> budget-insufficient at once"

# From here on the policy plans against a 10 s cut: release by press + 5,100 ms.
HOOKS=(JAWAKA_TEST_POWER_HOLD_SAVE_ANY_PLATFORM=1 JAWAKA_TEST_POWER_HARD_CUT_S=10)

# --- Never released: still-held at the boundary, shutdown proceeds. ----------
run_case still-held 1 "${HOOKS[@]}" FAKE_RA_SYNC_SAVE_BYTES="$STATE_BYTES" -- "down +0"
expect_shutdown
expect 'power-hold save: waiting for release (release by +5100ms; force-off hold 10s, cut 9700ms)'
expect 'power-hold save: still-held elapsed_ms='
expect 'key=held'
expect_not 'power-hold save: saving'
expect_not 'quit grace'
grep -q 'GET_STATE_SAVE_INFO' "$RA_COMMANDS" || fail "RetroArch was not probed"
grep -q 'SAVE_STATE_SYNC' "$RA_COMMANDS" && fail "a save was requested while the key was held"
[ ! -e "$FINAL_STATE" ] || fail "a state was written while the key was held"
echo "row still-held: no save, shutdown proceeds"

# --- Released inside the boundary: saved, then QUIT and a clean exit. --------
run_case released 1 "${HOOKS[@]}" FAKE_RA_SYNC_SAVE_BYTES="$STATE_BYTES" -- "down +0" "up +2600"
expect_shutdown
expect 'power-hold save: waiting for release'
expect "power-hold save: saving bytes<=$STATE_BYTES"
expect 'power-hold save: saved elapsed_ms='
expect 'key=released'
expect 'power-hold save: quit grace: sent QUIT to RetroArch pid='
expect 'power-hold save: quit grace: RetroArch exited after'
expect_not 'power-hold save: quit grace expired'
expect 'RetroArch session ended pid='
expect_not 'RetroArch session terminated'
grep -q '^QUIT$' "$RA_COMMANDS" || fail "RetroArch never received QUIT"
[ -f "$FINAL_STATE" ] || fail "no state at $FINAL_STATE"
[ "$(wc -c <"$FINAL_STATE")" -eq "$STATE_BYTES" ] || fail "state has the wrong size"
[ -z "$(find "$STATES_DIR" -name '*.tmp-*' 2>/dev/null)" ] || fail "temporary state left behind"
# The save is published before RetroArch is asked to leave.
saved_line="$(grep -n -F 'power-hold save: saved elapsed_ms=' "$LOG" | head -1 | cut -d: -f1)"
quit_line="$(grep -n -F 'quit grace: sent QUIT' "$LOG" | head -1 | cut -d: -f1)"
[ "$saved_line" -lt "$quit_line" ] || fail "QUIT was sent before the save was published"
echo "row released: saved, QUIT grace, clean exit"

# --- Press and release both queued behind a stalled tick: recognized late ---
# --- from the edges' own stamps, release still inside the boundary. ---------
run_case queued 1 "${HOOKS[@]}" FAKE_RA_SYNC_SAVE_BYTES="$STATE_BYTES" -- "down -2600" "up -100"
expect_shutdown
expect 'power: long-press (2500ms)'
expect 'power-hold save: waiting for release'
expect 'power-hold save: saved elapsed_ms='
expect 'key=released'
expect 'power-hold save: quit grace: RetroArch exited after'
[ -f "$FINAL_STATE" ] || fail "no state at $FINAL_STATE"
echo "row queued: late-recognized hold saves"

# --- Re-press after release: feeds the policy only; shutdown still completes.
run_case repress 1 "${HOOKS[@]}" FAKE_RA_SYNC_SAVE_BYTES="$STATE_BYTES" -- \
    "down +0" "up +2600" "down +2900" "up +3300"
expect_shutdown
expect 'power-hold save: saved elapsed_ms='
expect 'power-hold save: quit grace: RetroArch exited after'
[ "$(grep -c -F 'power: long-press' "$LOG")" -eq 1 ] || fail "the re-press started a second long press"
expect_not 'power: sleep'
expect_not 'power: standby'
echo "row repress: tap after release changes nothing"

# --- RetroArch without the patch: the probe goes unanswered, no save. --------
run_case no-sync 1 "${HOOKS[@]}" -- "down +0" "up +2600"
expect_shutdown
expect 'power-hold save: RetroArch cannot sync-save (probe=timeout'
expect 'power-hold save: unsupported elapsed_ms='
expect 'key=held'
expect_not 'power-hold save: waiting for release'
expect_not 'power-hold save: saving'
expect_not 'quit grace'
[ ! -e "$FINAL_STATE" ] || fail "a state was written without the sync-save protocol"
echo "row no-sync: unsupported, shutdown proceeds"

# --- Saved, but RetroArch ignores QUIT (and SIGTERM): the grace expires and --
# --- the existing kill sequence ends it. ------------------------------------
run_case deaf 1 "${HOOKS[@]}" FAKE_RA_SYNC_SAVE_BYTES="$STATE_BYTES" FAKE_RA_MODE=deaf -- \
    "down +0" "up +2600"
expect_shutdown
expect 'power-hold save: saved elapsed_ms='
expect 'power-hold save: quit grace: sent QUIT to RetroArch pid='
expect 'power-hold save: quit grace expired after 4000 ms; killing'
expect_not 'quit grace: RetroArch exited after'
expect 'RetroArch session terminated pid='
[ -f "$FINAL_STATE" ] || fail "the published state is gone"
echo "row deaf: grace expires, kill sequence runs"

echo "PASS power-hold-save-ipc-smoke"
