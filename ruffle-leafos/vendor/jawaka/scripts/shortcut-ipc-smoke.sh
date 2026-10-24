#!/usr/bin/env bash
# In-game shortcut state: startup load, and live update over IPC.
#
# Covers the two halves the input path depends on and that no unit test can
# reach, because both live in jawakad rather than in the shared model:
#
#   1. startup reads the three bindings AND the two capture opt-ins in one
#      batched database open, resolving fail-closed and saying what it dropped;
#   2. set-input-shortcuts replaces that state in the running daemon, and
#      rejects a malformed message without disturbing what is already there.
#
# Runs a local daemon against a temp SD tree -- no device. Same shape as the
# life1 IPC smokes next to it.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-shortcut-ipc.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PLATFORM_ROOT="$PRIMARY/.system/leaf/platforms/mac"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"
DAEMON_PID=""

cleanup() {
    status=$?
    set +e
    smoke_daemon_stop || status=1
    rm -rf "$TMP_DIR"
    exit "$status"
}
trap cleanup EXIT

fail() { echo "shortcut-ipc-smoke: $1" >&2; [ -f "$LOG" ] && tail -20 "$LOG" >&2; exit 1; }

make -C "$ROOT_DIR" jawakad jawaka-platformctl >/dev/null

mkdir -p "$PRIMARY/Roms" "$PRIMARY/Apps" "$USERDATA" "$LOGS" "$RUNTIME" \
         "$STATE" "$PLATFORM_ROOT/defaults"

# Seed a deliberately broken configuration: Recording wants the button the Game
# Switcher already has, and Screenshot's row is unreadable. Startup must fail
# closed on both and name them, which is also how we observe that the load ran.
# jw_state_dir() honours UMRK_INTERNAL_DATA_PATH, so the daemon's library.db
# is $STATE/library.db -- not the .umrk/<platform>/ path a real card uses.
DB="$STATE/library.db"
sqlite3 "$DB" "CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT);
INSERT OR REPLACE INTO settings(key,value) VALUES
  ('input_shortcut_game_switcher','select'),
  ('input_shortcut_screenshot','nonsense'),
  ('input_shortcut_recording','select'),
  ('screenshots_enabled','1'),
  ('recording_enabled','0');"

smoke_daemon_start "$ROOT_DIR" "$LOG" \
    PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$PRIMARY/Apps" \
    USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
    UMRK_PLATFORM_PATH="$PLATFORM_ROOT" UMRK_RUNTIME_PATH="$RUNTIME" \
    UMRK_DAEMON_SOCKET="$SOCKET" UMRK_INTERNAL_DATA_PATH="$STATE" \
    JAWAKA_SDCARD_ROOT="$PRIMARY" \
    "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only
for _ in $(seq 1 300); do
    [ -S "$SOCKET" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon exited before opening its socket"
    sleep 0.02
done
[ -S "$SOCKET" ] || fail "daemon socket never appeared"

# --- 1. startup load ---------------------------------------------------------
# The two warnings prove the batched read happened AND that resolution was
# fail-closed: an unreadable value disabled its action rather than quietly
# using the default, and the duplicate named both sides.
for _ in $(seq 1 200); do
    grep -q 'input shortcuts:' "$LOG" && break
    sleep 0.02
done
grep -q 'input_shortcut_screenshot has an unreadable value' "$LOG" \
    || fail "startup did not report the unreadable screenshot value"
grep -qE 'Recording and Game Switcher both want select' "$LOG" \
    || fail "startup did not name both sides of the duplicate"
# The resolved state, including the two capture opt-ins. The warnings above
# only prove the bindings were read; this proves the same batched open carried
# the flags, and carried them the right way round.
grep -q 'switcher=select screenshot=disabled recording=disabled screenshots_enabled=1 recording_enabled=0' "$LOG" \
    || fail "startup did not load the resolved state and both capture opt-ins"

req() { "$CTL" --socket "$SOCKET" request "$1" 2>&1; }

# --- 2. live IPC update ------------------------------------------------------
ok='{"type":"set-input-shortcuts","buttons":["start","l2","disabled"],"screenshots_enabled":false,"recording_enabled":true}'
case "$(req "$ok")" in
    *'"type":"ok"'*) ;;
    *) fail "a valid snapshot was rejected" ;;
esac

# Each malformed shape must be refused. The daemon keeps what it has; a
# partly-applied snapshot is the failure mode being guarded against.
while read -r label body; do
    [ -z "$label" ] && continue
    case "$(req "$body")" in
        *'"type":"error"'*) ;;
        *) fail "$label was accepted" ;;
    esac
done <<'CASES'
duplicate {"type":"set-input-shortcuts","buttons":["l1","l1","r1"],"screenshots_enabled":true,"recording_enabled":true}
unknown-button {"type":"set-input-shortcuts","buttons":["nope","l1","r1"],"screenshots_enabled":true,"recording_enabled":true}
short-array {"type":"set-input-shortcuts","buttons":["l1","r1"],"screenshots_enabled":true,"recording_enabled":true}
not-an-array {"type":"set-input-shortcuts","buttons":"l1","screenshots_enabled":true,"recording_enabled":true}
missing-flags {"type":"set-input-shortcuts","buttons":["l1","r1","select"]}
flag-not-bool {"type":"set-input-shortcuts","buttons":["l1","r1","select"],"screenshots_enabled":"yes","recording_enabled":true}
CASES

# Still serving after the rejections, and still accepting a good one.
case "$(req "$ok")" in
    *'"type":"ok"'*) ;;
    *) fail "daemon stopped accepting valid snapshots after rejections" ;;
esac

echo "PASS shortcut-ipc-smoke"
