#!/usr/bin/env bash
# One launch keeps the core it selected, across the LIFE-1 pending-launch
# boundary (request -> sync prompt -> continuation -> spawn).
#
#   UMRK_CORE_PIN_TARGET=path|retroarch   kind of the system's default core;
#                                         the alternate is always a path core
#   UMRK_CORE_PIN_PHASE=vanish            the selected default disappears at the
#                                         sync prompt: Play Anyway must fail the
#                                         launch and never start the alternate
#   UMRK_CORE_PIN_PHASE=cancel-fresh      a cancelled launch drops its selection:
#                                         with the default gone, a fresh request
#                                         selects the alternate, and the blocked
#                                         launch's override starts that alternate
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
TARGET_KIND="${UMRK_CORE_PIN_TARGET:-path}"
PHASE="${UMRK_CORE_PIN_PHASE:-vanish}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-core-pin.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PLATFORM_ROOT="$PRIMARY/.system/leaf/platforms/mac"
DEFAULTS="$PLATFORM_ROOT/defaults"
CORES="$PLATFORM_ROOT/cores"
FIXTURE_DIR="$PLATFORM_ROOT/emulators/fixture"
ALTERNATE="$FIXTURE_DIR/alternate-writer"
RETROARCH_BIN="$TMP_DIR/retroarch-fixture"
PAK="$PRIMARY/Apps/mac/Life1Fixture.pak"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
SERVICE_ID="org.umrk.test.corepin"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"
ROM="Roms/N64/Pinned.n64"

case "$TARGET_KIND" in
    path)
        DEFAULT_ID="writer_fixture"
        DEFAULT_FILE="$FIXTURE_DIR/game-writer-fixture"
        DEFAULT_KIND="standalone"
        ;;
    retroarch)
        DEFAULT_ID="fixture_ra"
        DEFAULT_FILE="$CORES/fixture_ra_libretro.dylib"
        DEFAULT_KIND="retroarch"
        ;;
    *) echo "unknown UMRK_CORE_PIN_TARGET=$TARGET_KIND" >&2; exit 2 ;;
esac
case "$PHASE" in
    vanish) SCENARIO="game-check-play" ;;
    cancel-fresh) SCENARIO="game-check-cancel" ;;
    *) echo "unknown UMRK_CORE_PIN_PHASE=$PHASE" >&2; exit 2 ;;
esac

cleanup() {
    exit_status=$?
    set +e
    smoke_daemon_stop || exit_status=1
    if [ "$exit_status" -ne 0 ] && [ -f "$LOG" ]; then
        sed -n '1,240p' "$LOG" >&2
    fi
    rm -rf "$TMP_DIR"
    exit "$exit_status"
}
trap cleanup EXIT

fail() {
    echo "FAIL launch-core-pin-ipc-smoke ($TARGET_KIND/$PHASE): $*" >&2
    exit 1
}

request() {
    "$CTL" --socket "$SOCKET" request "$1" 2>/dev/null || true
}

wait_for_sync_prompt() {
    local decision=""
    for _ in $(seq 1 300); do
        decision="$(request '{"type":"game-launch-blocked-status"}')"
        printf '%s' "$decision" | grep -q '"sync_pending":true' && return 0
        sleep 0.02
    done
    fail "sync prompt never appeared: $decision"
}

resolver_count() {
    grep -c -F "launch resolver: system=N64 rom=$ROM core=$1 kind=$2 origin=$3" "$LOG" || true
}

make -C "$ROOT_DIR" BUILD="$BUILD_REL" jawakad jawaka-platformctl \
    "$BUILD_REL/bin/life1-fixture-service" \
    "$BUILD_REL/bin/game-writer-fixture" >/dev/null
mkdir -p "$PAK/bin" "$STATE" "$RUNTIME" "$USERDATA" "$LOGS" "$DEFAULTS" \
         "$CORES" "$FIXTURE_DIR" "$PRIMARY/Roms/N64" "$PRIMARY/Images/N64" \
         "$PRIMARY/Saves" "$PRIMARY/States"
cp "$ROOT_DIR/$BUILD_REL/bin/life1-fixture-service" "$PAK/bin/life1-fixture-service"
cp "$ROOT_DIR/$BUILD_REL/bin/game-writer-fixture" "$ALTERNATE"
cp "$ROOT_DIR/$BUILD_REL/bin/game-writer-fixture" "$RETROARCH_BIN"
chmod 755 "$PAK/bin/life1-fixture-service" "$ALTERNATE" "$RETROARCH_BIN"
if [ "$TARGET_KIND" = path ]; then
    cp "$ROOT_DIR/$BUILD_REL/bin/game-writer-fixture" "$DEFAULT_FILE"
    chmod 755 "$DEFAULT_FILE"
else
    printf 'core\n' >"$DEFAULT_FILE"
fi
printf 'rom\n' >"$PRIMARY/$ROM"
printf '%s\n' \
  "{\"id\":\"$SERVICE_ID\",\"name\":\"Core Pin Fixture\",\"platform\":\"mac\",\"pak_version\":\"1.0.0\",\"service\":{\"schema\":1,\"id\":\"$SERVICE_ID\",\"run\":{\"path\":\"bin/life1-fixture-service\",\"args\":[]},\"default_enabled\":false,\"stop_grace_ms\":300,\"restart\":\"no\",\"lifecycle\":{\"game\":\"stop\"}}}" \
  >"$PAK/pak.json"

python3 - "$DEFAULTS" "$TARGET_KIND" <<'CATALOG'
import json, pathlib, sys
defaults = pathlib.Path(sys.argv[1])
kind = sys.argv[2]
def path_core(core_id, name, path):
    return {"id": core_id, "display_name": name, "type": "path",
            "libretro_name": None, "file_name": None, "config_folder": name,
            "info_name": None, "path": path, "supports_menu": False,
            "supports_savestate": True, "supports_disk_control": False,
            "needs_swap": False, "status": "packaged"}
if kind == "path":
    default = path_core("writer_fixture", "Writer Fixture",
                        "emulators/fixture/game-writer-fixture")
else:
    default = {"id": "fixture_ra", "display_name": "Fixture RetroArch",
               "type": "retroarch", "libretro_name": "fixture_ra",
               "file_name": "fixture_ra_libretro.dylib",
               "config_folder": "Fixture RA",
               "info_name": "fixture_ra_libretro.info", "path": None,
               "supports_menu": True, "supports_savestate": True,
               "supports_disk_control": False, "needs_swap": False,
               "status": "packaged"}
alternate = path_core("alternate_writer", "Alternate Writer",
                      "emulators/fixture/alternate-writer")
(defaults / "cores.json").write_text(json.dumps(
    {"version": 2, "platform": "mac", "cores": [default, alternate]}))
(defaults / "systems.json").write_text(json.dumps(
    {"version": 2, "platform": "mac", "systems": [{
        "id": "N64", "name": "Nintendo 64", "patterns": ["N64"],
        "extensions": ["n64"], "archive_extensions": [],
        "archive_inner_extensions": ["n64"], "archive_mode": "pass_through",
        "file_names": [], "ignore_file_names": [], "playlist_extensions": [],
        "m3u_generation": "none", "default_core": default["id"],
        "alternate_cores": ["alternate_writer"], "rom_root": "Roms/N64",
        "image_root": "Images/N64", "bios_notes": []}]}))
CATALOG

smoke_daemon_start "$ROOT_DIR" "$LOG" \
    PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$PRIMARY/Apps" \
    USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
    SAVES_PATH="$PRIMARY/Saves" STATES_PATH="$PRIMARY/States" \
    UMRK_PLATFORM_PATH="$PLATFORM_ROOT" UMRK_RUNTIME_PATH="$RUNTIME" \
    UMRK_DAEMON_SOCKET="$SOCKET" UMRK_INTERNAL_DATA_PATH="$STATE" \
    JAWAKA_SDCARD_ROOT="$PRIMARY" CORES_PATH="$CORES" \
    UMRK_RETROARCH_BIN="$RETROARCH_BIN" \
    UMRK_LIFE1_FIXTURE_SERVICE_ID="$SERVICE_ID" \
    UMRK_LIFE1_FIXTURE_SCENARIO="$SCENARIO" \
    "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only

for _ in $(seq 1 300); do
    [ -S "$SOCKET" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon exited during startup"
    sleep 0.02
done
[ -S "$SOCKET" ] || fail "daemon socket never appeared"
for _ in $(seq 1 500); do
    status="$(request '{"type":"library-status"}')"
    if python3 -c 'import json,sys; d=json.load(sys.stdin); raise SystemExit(d.get("scan_running", True) or d.get("generation", 0) <= 0)' <<<"$status" 2>/dev/null; then
        break
    fi
    sleep 0.02
done

request "{\"v\":1,\"op\":\"run\",\"id\":\"run\",\"service_id\":\"$SERVICE_ID\"}" |
    grep -F '"ok":true' >/dev/null || fail "fixture service did not start"
for _ in $(seq 1 300); do
    grep -Fx 'ready=1' "$RUNTIME/services/$SERVICE_ID/life1-fixture-result" >/dev/null 2>&1 && break
    sleep 0.02
done
grep -Fx 'ready=1' "$RUNTIME/services/$SERVICE_ID/life1-fixture-result" >/dev/null ||
    fail "fixture service never subscribed"

LAUNCH="{\"type\":\"launch-game\",\"system\":\"N64\",\"rom_path\":\"$ROM\"}"

if [ "$PHASE" = vanish ]; then
    request "$LAUNCH" | grep -F '"type":"ok"' >/dev/null || fail "launch request refused"
    wait_for_sync_prompt
    [ "$(resolver_count "$DEFAULT_ID" "$DEFAULT_KIND" default)" = 1 ] ||
        fail "request did not select the default exactly once"

    rm "$DEFAULT_FILE"
    request '{"type":"game-check-play-anyway"}' >/dev/null

    for _ in $(seq 1 150); do
        grep -F "launch failed: selected core $DEFAULT_ID is missing" "$LOG" >/dev/null && break
        sleep 0.02
    done
    grep -F "launch failed: selected core $DEFAULT_ID is missing" "$LOG" >/dev/null ||
        fail "missing selected core was not reported"
    sleep 1
    [ ! -e "$RUNTIME/game-writer-live" ] || fail "a game writer started"
    [ ! -e "$RUNTIME/active-game.json" ] || fail "the failed launch stayed active"
    [ "$(resolver_count "$DEFAULT_ID" "$DEFAULT_KIND" default)" = 1 ] ||
        fail "the pending launch selected a core again"
    ! grep -F 'origin=alternate' "$LOG" >/dev/null || fail "an alternate was selected"
    ! grep -F 'core_id=alternate_writer' "$LOG" >/dev/null || fail "the alternate was spawned"
    ! grep -F 'spawned RetroArch' "$LOG" >/dev/null || fail "RetroArch was spawned"
    grep -F 'reason=writer-spawn-failed' "$LOG" >/dev/null ||
        fail "launch was not aborted as a spawn failure"
    echo "PASS launch-core-pin-ipc-smoke ($TARGET_KIND/$PHASE)"
    exit 0
fi

# cancel-fresh
request "$LAUNCH" | grep -F '"type":"ok"' >/dev/null || fail "first launch request refused"
wait_for_sync_prompt
[ "$(resolver_count "$DEFAULT_ID" "$DEFAULT_KIND" default)" = 1 ] ||
    fail "first request did not select the default"
request '{"type":"game-check-cancel"}' | grep -F '"type":"ok"' >/dev/null ||
    fail "cancel refused"
for _ in $(seq 1 150); do
    [ ! -e "$RUNTIME/active-game.json" ] && break
    sleep 0.02
done
[ ! -e "$RUNTIME/active-game.json" ] || fail "cancelled launch stayed active"

rm "$DEFAULT_FILE"
request "$LAUNCH" | grep -F '"type":"ok"' >/dev/null ||
    fail "fresh request refused with the default gone"
[ "$(resolver_count alternate_writer standalone alternate)" = 1 ] ||
    fail "fresh request did not select the alternate"

# The fixture answers only its first check, so this launch blocks; the
# override continues the same launch with its stored selection.
blocked=""
for _ in $(seq 1 300); do
    blocked="$(request '{"type":"game-launch-blocked-status"}')"
    printf '%s' "$blocked" | grep -q '"override_allowed":true' && break
    sleep 0.02
done
printf '%s' "$blocked" | grep -q '"override_allowed":true' ||
    fail "fresh launch never offered an override: $blocked"
request '{"type":"game-launch-override"}' | grep -F '"type":"ok"' >/dev/null ||
    fail "override refused"

for _ in $(seq 1 500); do
    [ -e "$RUNTIME/game-writer-live" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon exited"
    sleep 0.01
done
[ -e "$RUNTIME/game-writer-live" ] || fail "the alternate never started"
grep -F 'spawned standalone emulator' "$LOG" | grep -F 'core_id=alternate_writer' >/dev/null ||
    fail "the started writer was not the alternate"
[ "$(resolver_count alternate_writer standalone alternate)" = 1 ] ||
    fail "the blocked launch selected a core again"
! grep -F 'launch failed: selected core' "$LOG" >/dev/null ||
    fail "a stale selection was used"
echo "PASS launch-core-pin-ipc-smoke ($TARGET_KIND/$PHASE)"
