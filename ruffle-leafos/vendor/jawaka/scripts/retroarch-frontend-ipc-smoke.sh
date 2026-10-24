#!/usr/bin/env bash
# LEAF-RA-1, driven on the host: a real jawakad on the mock platform launches a
# content pak's own RetroArch (a "frontend") in place of Leaf's, with
# scripts/fake-retroarch.py standing in for it.
#
# The card has one content pak, Apps/mac/Frontend.pak, that adds a retroarch
# core to the mock N64 system and names scripts/retroarch.sh as its frontend.
# That script records its argv, $0 and the --config file it was given, then
# execs the fake. Leaf's own RetroArch (UMRK_RETROARCH_BIN) is a decoy that
# leaves a marker, so a launch through the wrong binary is visible.
#
# The core is chosen the way a player chooses it: the saved per-system core
# choice in library.db (system_settings.core_id). launch-game's own core_id
# field is a one-shot override that the daemon honours for standalone path
# cores only, never for a RetroArch core.
#
# FAKE_RA_FRONTEND picks how the fake answers jawakad's probe (see
# fake-retroarch.py): "answer" speaks GET_INFO and VERSION, "no-version" only
# GET_INFO, "unpatched" only VERSION (upstream RetroArch without Leaf's command
# patch). The probe is two phases: GET_INFO until it answers (the frontend may
# take seconds to open its port), then VERSION against Leaf's RetroArch build
# manifest. Rows:
#   answer          probe passes, version matches Leaf's: argv, config, the
#                   in-game menu opens (PAUSE sent), quitting promotes the
#                   config to the shared one
#   plain-manifest  the same with a build manifest that has no leading "v"
#   mismatch        probe passes, VERSION is another version: menu works, config
#                   is not promoted
#   near-miss       1.22.20 against 1.22.2: no prefix matching
#   no-version      GET_INFO answered, VERSION silent for all 8 attempts: menu
#                   works, not promoted
#   no-manifest     no build manifest, Leaf's version unknown: not promoted
#   quit-early      the session ends before the version phase finishes: not
#                   promoted
#   unpatched       GET_INFO never answered: open-menu refused with no PAUSE
#                   sent, through the whole ~20 s probe window and after it;
#                   the probe warning is logged, VERSION is never asked; the
#                   config is not promoted
#   missing-removed / missing-noexec
#                   the frontend is gone (removed, then not executable) after
#                   the catalog was built: the saved core choice is unavailable
#                   and is kept, nothing of the pak starts, and the default
#                   core launches through Leaf's own RetroArch with no probe
#                   and a promoted config
#   refused         the frontend is not executable when the daemon starts: the
#                   contract validator refuses the pak, same fallback
#
# Not covered here, because the mock platform cannot produce them: the Menu tap
# (MENU_TOGGLE for an unprobed frontend) and the recording shortcut, both of
# which come from the MLP1 input proxy, and the MLP1-only record_driver and
# audio keys (retroarch-config-test covers those).
#
# Daemon smokes run in a Linux container (gcc:14 --init), not on the Mac. The
# fixture lives under TMPDIR (default /tmp, the container's own filesystem):
# on a noowners bind mount access(X_OK) is true for every file, which would
# hide the not-executable rows.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"
FAKE_RA_PY="$ROOT_DIR/scripts/fake-retroarch.py"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-ra-frontend.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
SHARED_CFG="$STATE/retroarch/retroarch.cfg"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
APPS="$PRIMARY/Apps"
PLATFORM_ROOT="$PRIMARY/.system/leaf/platforms/mac"
DEFAULTS="$PLATFORM_ROOT/defaults"
CORES="$PLATFORM_ROOT/cores"
MANIFEST="$PLATFORM_ROOT/bin/retroarch.build-manifest.json"
BIN_DIR="$TMP_DIR/bin"
LEAF_RA="$TMP_DIR/leaf-retroarch"
PAK="$APPS/mac/Frontend.pak"
FRONTEND="$PAK/scripts/retroarch.sh"
PAK_CORE="$PAK/cores/frontend_libretro.so"
PAK_CORE_ID="frontend_ra"
ROM_REL="Roms/N64/Frontend Game.n64"
ROM_ABS="$PRIMARY/$ROM_REL"
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

fail() { echo "FAIL retroarch-frontend-ipc-smoke${CASE:+ ($CASE)}: $*" >&2; exit 1; }

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

# A copy of the daemon with a stand-in in-game menu beside it, so open-menu can
# succeed on a host that has not built the UI binaries. The stub ignores the
# show/hide signals the daemon sends it.
mkdir -p "$BIN_DIR"
cp "$ROOT_DIR/$BUILD_REL/bin/jawakad" "$BIN_DIR/jawakad"
cat >"$BIN_DIR/jawaka-menu" <<'EOF'
#!/bin/sh
trap ':' USR1 USR2
while :; do sleep 0.2; done
EOF
chmod 755 "$BIN_DIR/jawakad" "$BIN_DIR/jawaka-menu"

mkdir -p "$USERDATA" "$LOGS" "$DEFAULTS" "$CORES" "$(dirname "$MANIFEST")" \
         "$PRIMARY/Roms/N64" "$PRIMARY/Images/N64" "$PRIMARY/Saves" \
         "$PRIMARY/States" "$APPS/mac"
printf 'core\n' >"$CORES/fixture_libretro.so"
printf 'rom\n' >"$ROM_ABS"

# Leaf's own RetroArch: marks that it ran, then is the fake.
cat >"$LEAF_RA" <<EOF
#!/bin/sh
printf '%s\n' "\$0" >"\${FRONTEND_SMOKE_DIR:?}/leaf-ra-argv0.txt"
exec python3 "$FAKE_RA_PY" "\$@"
EOF
chmod 755 "$LEAF_RA"

# The mock N64 system: Leaf's own core is the default, the pak adds an alternate.
python3 - "$DEFAULTS" <<'CATALOG'
import json, pathlib, sys
defaults = pathlib.Path(sys.argv[1])
core = {"id": "fixture_ra", "display_name": "Fixture RetroArch",
        "type": "retroarch", "libretro_name": "fixture",
        "file_name": "fixture_libretro.so", "config_folder": "Fixture RA",
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

# The content pak, rebuilt at the start of every row (rows damage it).
install_pak() {
    rm -rf "$PAK"
    mkdir -p "$PAK/cores" "$PAK/info" "$PAK/scripts"
    printf 'core\n' >"$PAK_CORE"
    printf 'display_name = "Frontend Fixture"\n' >"$PAK/info/frontend_libretro.info"
    cat >"$FRONTEND" <<EOF
#!/bin/sh
# LEAF-RA-1 fixture frontend: record what jawakad gave it, then be the fake.
out="\${FRONTEND_SMOKE_DIR:?}"
printf '%s\n' "\$0" >"\$out/argv0.txt"
printf '%s\n' "\$@" >"\$out/argv.txt"
prev=
for arg in "\$@"; do
    [ "\$prev" = "--config" ] && cp "\$arg" "\$out/config.cfg"
    prev="\$arg"
done
exec python3 "$FAKE_RA_PY" "\$@"
EOF
    chmod 755 "$FRONTEND"
    cat >"$PAK/pak.json" <<EOF
{"id": "org.umrk.test.frontend", "name": "Frontend Fixture", "platform": "mac",
 "pak_version": "1.0.0",
 "provides": {"schema": 1, "systems": [],
  "system_extensions": [{"system_id": "N64", "add_alternate_cores": ["$PAK_CORE_ID"]}],
  "cores": [{"id": "$PAK_CORE_ID", "display_name": "Frontend RetroArch",
   "type": "retroarch", "libretro_name": "frontend",
   "file_name": "cores/frontend_libretro.so",
   "info_name": "info/frontend_libretro.info", "config_folder": "Frontend",
   "supports_menu": true, "supports_savestate": true,
   "supports_disk_control": false,
   "frontend": {"path": "scripts/retroarch.sh", "protocol": "leaf-ra-1"}}]}}
EOF
}

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
expect_re() { grep -E -q -- "$1" "$LOG" || fail "missing log line matching: $1"; }
commands_have() { grep -q -x -- "$1" "$RA_COMMANDS"; }

# write_manifest VERSION|none: Leaf's RetroArch build manifest.
write_manifest() {
    if [ "$1" = none ]; then
        rm -f "$MANIFEST"
    else
        printf '{"retroarch_version":"%s","commit":"69a4f0ea1e8aaf442ae4858f2e7f2b31a1776576"}\n' \
            "$1" >"$MANIFEST"
    fi
}

# start_daemon NAME MANIFEST_VERSION [NAME=VALUE ...]: fresh state, runtime dir
# and log; the pak is rebuilt unless NO_PAK_INSTALL is set.
start_daemon() {
    CASE="$1"
    local manifest="$2"
    shift 2
    RUNTIME="$TMP_DIR/runtime-$CASE"
    SOCKET="$RUNTIME/jawakad.sock"
    LOG="$TMP_DIR/$CASE.log"
    OUT="$TMP_DIR/out-$CASE"
    RA_COMMANDS="$OUT/ra-commands.txt"
    RA_READY="$OUT/ra-ready"
    rm -rf "$RUNTIME" "$STATE" "$OUT"
    mkdir -p "$RUNTIME" "$STATE" "$OUT"
    : >"$RA_COMMANDS"
    printf '%s\n' '{"schema":1,"version":"0.11.0","release_id":"ra-frontend-smoke-1"}' \
        >"$STATE/release.json"
    write_manifest "$manifest"
    [ -n "${NO_PAK_INSTALL:-}" ] || install_pak

    smoke_daemon_start "$ROOT_DIR" "$LOG" \
        PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$APPS" \
        USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
        SAVES_PATH="$PRIMARY/Saves" STATES_PATH="$PRIMARY/States" \
        UMRK_PLATFORM_PATH="$PLATFORM_ROOT" UMRK_RUNTIME_PATH="$RUNTIME" \
        UMRK_DAEMON_SOCKET="$SOCKET" UMRK_INTERNAL_DATA_PATH="$STATE" \
        JAWAKA_SDCARD_ROOT="$PRIMARY" CORES_PATH="$CORES" \
        UMRK_RETROARCH_BIN="$LEAF_RA" \
        FRONTEND_SMOKE_DIR="$OUT" \
        FAKE_RA_QUIT_LOG="$RA_COMMANDS" FAKE_RA_READY="$RA_READY" \
        "$@" \
        "$BIN_DIR/jawakad" --daemon-only

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
            return 0
        fi
        sleep 0.02
    done
    fail "library scan never finished"
}

stop_daemon() {
    smoke_daemon_stop || fail "daemon would not stop"
    pkill -f "$TMP_DIR" 2>/dev/null || true
    rm -f "$SOCKET"
}

# select_pak_core: the player's saved choice for N64 is the pak's core.
select_pak_core() {
    python3 - "$STATE/library.db" "$PAK_CORE_ID" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.execute("INSERT OR REPLACE INTO system_settings(system,key,value,updated_at) "
           "VALUES ('N64','core_id',?,strftime('%s','now'))", (sys.argv[2],))
db.commit()
PY
}

# launch_game: launch the fixture ROM with the saved core choice.
launch_game() {
    LAUNCH_REPLY="$(request "{\"type\":\"launch-game\",\"system\":\"N64\",\"rom_path\":\"$ROM_REL\"}")"
}

# launch_ok: the launch was accepted and the session is up.
launch_ok() {
    printf '%s' "$LAUNCH_REPLY" | grep -F '"type":"ok"' >/dev/null || fail "launch-game refused: $LAUNCH_REPLY"
    wait_session
}

# wait_probe: GET_INFO answered. wait_version: the VERSION phase has concluded
# (up to 8 attempts, 250 ms timeout each and 250 ms apart).
wait_probe() {
    wait_log 'RetroArch frontend probe: Leaf commands available' 100 || fail "the probe never passed"
}
wait_version() {
    wait_log 'RetroArch frontend version=' 100 || fail "the version phase never concluded"
}

wait_session() {
    wait_log 'RetroArch session started pid=' 100 || fail "RetroArch session never started"
    local i
    for i in $(seq 1 200); do
        [ -s "$RA_READY" ] && return 0
        sleep 0.05
    done
    fail "the fake RetroArch never bound its port"
}

# quit_game: QUIT through the IPC action the in-game menu uses, then wait for
# the session to end and the daemon to be idle again.
quit_game() {
    request '{"type":"retroarch-action","action":"quit"}' |
        grep -F '"type":"ok"' >/dev/null || fail "retroarch-action quit refused"
    wait_log 'RetroArch session ended pid=' 100 || fail "the session never ended after QUIT"
    grep -q -x QUIT "$RA_COMMANDS" || fail "the frontend never received QUIT"
    # The config decision is logged just after the session-ended line.
    local i
    for i in $(seq 1 50); do
        grep -E -q 'RetroArch shared config (backed up|not kept|backup)' "$LOG" && return 0
        sleep 0.1
    done
    fail "no config decision was logged after the session ended"
}

# shared_config_promoted: the fake's save-on-exit adds rewind_enable, which is
# not a protected key, so it reaches the shared config only by promotion.
shared_config_has_rewind() {
    [ -f "$SHARED_CFG" ] && grep -q '^rewind_enable = "true"$' "$SHARED_CFG"
}

expect_promoted() {
    expect 'RetroArch shared config backed up'
    expect_not 'not kept from frontend session'
    shared_config_has_rewind || fail "the shared config was not updated"
}

expect_not_promoted() {
    expect 'RetroArch shared config not kept from frontend session'
    expect_not 'RetroArch shared config backed up'
    shared_config_has_rewind && fail "the shared config was updated by a frontend that does not match Leaf's"
    return 0
}

# frontend_launched: the pak's script, not Leaf's RetroArch, was executed with
# the pak's core and the generated config, and the ROM last.
frontend_launched() {
    [ -s "$OUT/argv0.txt" ] || fail "the frontend script never ran"
    [ ! -e "$OUT/leaf-ra-argv0.txt" ] || fail "Leaf's own RetroArch ran too"
    [ "$(cat "$OUT/argv0.txt")" = "$FRONTEND" ] ||
        fail "exec'd $(cat "$OUT/argv0.txt"), want $FRONTEND"
    python3 - "$OUT/argv.txt" "$PAK_CORE" "$RUNTIME" "$ROM_ABS" <<'PY' ||
import re, sys
argv_file, core, runtime, rom = sys.argv[1:]
argv = open(argv_file, encoding="utf-8").read().split("\n")[:-1]
want_cfg = re.escape(runtime) + r"/retroarch-current-[0-9]+\.cfg"
ok = (len(argv) >= 5 and argv[0] == "-L" and argv[1] == core and
      argv[2] == "--config" and re.fullmatch(want_cfg, argv[3]) and
      argv[-1] == rom)
if not ok:
    print("unexpected argv: %r" % argv, file=sys.stderr)
    sys.exit(1)
PY
        fail "the frontend's argv is wrong (see above)"
    expect "RetroArch frontend for core $PAK_CORE_ID: $FRONTEND"
    expect "spawned RetroArch pid="
    expect "retroarch=$FRONTEND"
    # The session is the pak core's.
    local session
    session="$(request '{"type":"retroarch-session"}')"
    python3 -c '
import json, sys
d = json.loads(sys.argv[1])
assert d["active"] and d["core_id"] == sys.argv[2], d
assert d["core_path"] == sys.argv[3], d
' "$session" "$PAK_CORE_ID" "$PAK_CORE" || fail "retroarch-session does not describe the pak core: $session"
}

# frontend_config: the --config file the frontend was given (copied by its
# script before it started) keeps Leaf's command interface.
frontend_config() {
    [ -s "$OUT/config.cfg" ] || fail "no config copied from the frontend's --config"
    grep -q '^network_cmd_enable = "true"$' "$OUT/config.cfg" || fail "network_cmd_enable is not true"
    grep -q '^network_cmd_port = "55355"$' "$OUT/config.cfg" || fail "network_cmd_port is not 55355"
    grep -q '^config_save_on_exit = "true"$' "$OUT/config.cfg" || fail "config_save_on_exit is not true"
}

# probe_row NAME MANIFEST_VERSION [NAME=VALUE ...]: launch the pak core against
# a frontend that answers the probe, wait for the version phase, return with
# the session still running.
probe_row() {
    start_daemon "$@"
    select_pak_core
    launch_game
    launch_ok
    wait_probe
    wait_version
}

# --- answer: the whole happy path. ----------------------------------------------
probe_row answer v1.22.2 FAKE_RA_FRONTEND=answer
frontend_launched
frontend_config
expect "launch resolver: system=N64 rom=$ROM_REL core=$PAK_CORE_ID kind=retroarch origin=saved"
expect_re 'RetroArch frontend probe: Leaf commands available after [0-9]+ attempts?$'
expect 'RetroArch frontend version=1.22.2 leaf=1.22.2 config_promotion=on'
commands_have GET_INFO || fail "GET_INFO was never sent"
[ "$(grep -c -x VERSION "$RA_COMMANDS")" -eq 1 ] || fail "VERSION was not asked exactly once"
commands_have PAUSE && fail "PAUSE was sent before the menu was opened"
request '{"type":"open-menu"}' | grep -F '"type":"ok"' >/dev/null || fail "open-menu refused by an answering frontend"
expect 'open-menu requested mode=in-game'
expect_not 'has not answered the probe'
commands_have PAUSE || fail "the in-game menu did not pause the frontend"
quit_game
expect_promoted
[ -z "$(find "$RUNTIME" -name 'retroarch-current-*.cfg' 2>/dev/null)" ] ||
    fail "the session's runtime config was left behind"
stop_daemon
echo "row answer: frontend exec'd with the pak core and Leaf's config, probe passed, menu opens, config promoted"

# --- plain-manifest: "1.22.2" without the leading v. ----------------------------
probe_row plain-manifest 1.22.2 FAKE_RA_FRONTEND=answer
expect 'RetroArch frontend version=1.22.2 leaf=1.22.2 config_promotion=on'
quit_game
expect_promoted
stop_daemon
echo "row plain-manifest: a manifest version without the v matches"

# --- mismatch: another version. -------------------------------------------------
probe_row mismatch v1.22.2 FAKE_RA_FRONTEND=answer FAKE_RA_VERSION=1.21.0
expect 'RetroArch frontend version=1.21.0 leaf=1.22.2 config_promotion=off'
# A different version is still a Leaf-speaking frontend: the menu works.
request '{"type":"open-menu"}' | grep -F '"type":"ok"' >/dev/null || fail "open-menu refused after the probe passed"
commands_have PAUSE || fail "the in-game menu did not pause the frontend"
quit_game
expect_not_promoted
stop_daemon
echo "row mismatch: menu works, config not promoted"

# --- near-miss: no prefix matching. ---------------------------------------------
probe_row near-miss v1.22.2 FAKE_RA_FRONTEND=answer FAKE_RA_VERSION=1.22.20
expect 'RetroArch frontend version=1.22.20 leaf=1.22.2 config_promotion=off'
quit_game
expect_not_promoted
stop_daemon
echo "row near-miss: 1.22.20 is not 1.22.2"

# --- no-version: GET_INFO answered, VERSION never. ------------------------------
probe_row no-version v1.22.2 FAKE_RA_FRONTEND=no-version
expect 'RetroArch frontend version=(no reply) leaf=1.22.2 config_promotion=off'
[ "$(grep -c -x VERSION "$RA_COMMANDS")" -eq 8 ] || fail "VERSION was not tried 8 times"
request '{"type":"open-menu"}' | grep -F '"type":"ok"' >/dev/null || fail "open-menu refused after the probe passed"
quit_game
expect_not_promoted
stop_daemon
echo "row no-version: 8 VERSION attempts, menu works, config not promoted"

# --- no-manifest: Leaf's own version unknown. -----------------------------------
probe_row no-manifest none FAKE_RA_FRONTEND=answer
expect 'RetroArch frontend version=1.22.2 leaf=(unknown) config_promotion=off'
quit_game
expect_not_promoted
stop_daemon
echo "row no-manifest: an unknown Leaf version promotes nothing"

# --- quit-early: the session ends before the version phase concludes. -----------
# With VERSION silent the phase takes ~4 s, so a quit right after the probe is
# deterministic. A config whose frontend was never shown to match is not kept.
start_daemon quit-early v1.22.2 FAKE_RA_FRONTEND=no-version
select_pak_core
launch_game
launch_ok
wait_probe
quit_game
expect_not 'RetroArch frontend version='
expect_not_promoted
stop_daemon
echo "row quit-early: no version verdict, config not promoted"

# --- unpatched: GET_INFO is never answered. -------------------------------------
# Upstream RetroArch answers VERSION but not GET_INFO. Until a frontend answers
# GET_INFO, Leaf assumes nothing about its command set: the in-game menu would
# send PAUSE, which such a RetroArch ignores, and leave the menu over a running
# game.
start_daemon unpatched v1.22.2 FAKE_RA_FRONTEND=unpatched
select_pak_core
launch_game
launch_ok
frontend_launched
frontend_config
for i in $(seq 1 100); do
    commands_have GET_INFO && break
    sleep 0.1
done
commands_have GET_INFO || fail "the probe never sent GET_INFO"
reply="$(request '{"type":"open-menu"}')"
printf '%s' "$reply" | grep -F 'open-menu failed' >/dev/null || fail "open-menu was not refused during the probe window: $reply"
expect 'in-game menu: RetroArch frontend has not answered the probe'
commands_have PAUSE && fail "PAUSE was sent to a frontend that has not answered GET_INFO"
expect_not 'RetroArch frontend probe: Leaf commands available'
# The ~20 s window runs out: 76 GET_INFOs, then the warning.
wait_log 'RetroArch frontend probe: no GET_INFO reply after 76 attempts' 600 ||
    fail "the probe warning never came"
expect "Menu opens the frontend's own menu"
[ "$(grep -c -x GET_INFO "$RA_COMMANDS")" -ge 76 ] || fail "fewer than 76 GET_INFO probes were sent"
commands_have VERSION && fail "VERSION was asked of a frontend that never passed the probe"
reply="$(request '{"type":"open-menu"}')"
printf '%s' "$reply" | grep -F 'open-menu failed' >/dev/null || fail "open-menu was not refused after the probe gave up: $reply"
commands_have PAUSE && fail "PAUSE was sent after the probe gave up"
# The game was never disturbed: the session is still the same one.
request '{"type":"retroarch-session"}' | grep -F '"active":true' >/dev/null || fail "the session ended"
quit_game
expect_not 'RetroArch frontend version='
expect_not_promoted
stop_daemon
echo "row unpatched: open-menu refused with no PAUSE, probe warning after 76 attempts, config not promoted"

# --- missing: the saved core's frontend is unusable. ----------------------------
# damage: remove / noexec after the catalog was built, or refuse (not
# executable from the start, so the contract validator keeps the pak out of the
# catalog altogether). Either way the saved choice is unavailable and kept, and
# the system's default core (Leaf's own RetroArch, no frontend) launches. The
# chmod rows need the container's own /tmp: a bind mount calls every file
# executable.
missing_row() { # NAME remove|noexec|refuse
    local name="$1" damage="$2"
    if [ "$damage" = refuse ]; then
        NO_PAK_INSTALL=1
        install_pak
        chmod 644 "$FRONTEND"
    fi
    start_daemon "$name" v1.22.2 FAKE_RA_FRONTEND=answer
    unset NO_PAK_INSTALL
    select_pak_core
    case "$damage" in
        remove) rm -f "$FRONTEND" ;;
        noexec) chmod 644 "$FRONTEND" ;;
    esac
    launch_game
    launch_ok
    if [ "$damage" = refuse ]; then
        grep -q 'not-executable' "$STATE"/catalog/diagnostics.json ||
            fail "the catalog diagnostics do not name not-executable"
    else
        expect "core $PAK_CORE_ID: frontend scripts/retroarch.sh missing or not executable; core unavailable"
    fi
    expect "launch resolver: saved core $PAK_CORE_ID is unavailable or not allowed for N64; saved choice kept"
    expect "launch resolver: system=N64 rom=$ROM_REL core=fixture_ra kind=retroarch origin=default"
    [ -s "$OUT/leaf-ra-argv0.txt" ] || fail "Leaf's own RetroArch did not run for the default core"
    [ ! -e "$OUT/argv0.txt" ] || fail "the pak's frontend ran"
    expect "retroarch=$LEAF_RA"
    expect_not "RetroArch frontend for core"
    sleep 2.5
    expect_not 'RetroArch frontend probe'
    commands_have GET_INFO && fail "a regular session was probed"
    commands_have VERSION && fail "a regular session was asked for its VERSION"
    python3 - "$STATE/library.db" "$PAK_CORE_ID" <<'PY' || fail "the saved core choice was not kept"
import sqlite3, sys
row = sqlite3.connect(sys.argv[1]).execute(
    "SELECT value FROM system_settings WHERE system='N64' AND key='core_id'").fetchone()
sys.exit(0 if row and row[0] == sys.argv[2] else 1)
PY
    quit_game
    expect 'RetroArch shared config backed up'
    expect_not 'not kept from frontend session'
    shared_config_has_rewind || fail "a regular session's config was not promoted"
    stop_daemon
}

missing_row missing-removed remove
echo "row missing-removed: a removed frontend makes the saved core unavailable; the default core is unaffected"
missing_row missing-noexec noexec
echo "row missing-noexec: a non-executable frontend likewise"
missing_row refused refuse
echo "row refused: a pak whose frontend is not executable never enters the catalog; same fallback"

echo "PASS retroarch-frontend-ipc-smoke"
