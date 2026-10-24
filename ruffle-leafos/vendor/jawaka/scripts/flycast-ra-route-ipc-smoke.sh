#!/usr/bin/env bash
# Real-daemon child-environment fixture for UMRK_FLYCAST_RA_ROUTE (proxy plan
# P2).
#
# Boots a real jawakad against a mock SD tree and proves, through the actual
# fork/exec launch path:
#   1. the route-capable bundled Flycast child gets "native" when the
#      RAOfflineProxy pak is absent, installed but not running, or stopped,
#      and "service-live" only while the supervised service is RUNNING;
#   2. an account-only Flycast build (no ra-route-v1 record) gets its account
#      snapshot but no route, and the daemon logs the upgrade explanation;
#   3. a control standalone, a provider-bound pak core named after Flycast
#      with both capability records, and a RetroArch/libretro launch never
#      receive the route;
#   4. inherited UMRK_FLYCAST_RA_ROUTE, FLYCAST_PROBE and
#      FLYCAST_CONFIG_OVERRIDES never survive daemon startup into any child;
#   5. the route and account handoffs are independent: the route carries no
#      URL, port or credential.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-flycast-route.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PLATFORM_ROOT="$PRIMARY/.system/leaf/platforms/mac"
DEFAULTS="$PLATFORM_ROOT/defaults"
CORES="$PLATFORM_ROOT/cores"
EMU="$PLATFORM_ROOT/emulators"
APPS="$PRIMARY/Apps"
SERVICE_ID="org.umrk.raofflineproxy"
SERVICE_PAK="$APPS/mac/RAOfflineProxy.pak"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"
DB="$STATE/library.db"
FAKE_RA="$TMP_DIR/fake-retroarch.sh"

cleanup() {
    exit_status=$?
    set +e
    stop_daemon || exit_status=1
    if [ "$exit_status" -ne 0 ] && [ -f "$LOG" ]; then
        sed -n '1,240p' "$LOG" >&2
    fi
    rm -rf "$TMP_DIR"
    exit "$exit_status"
}
trap cleanup EXIT

fail() {
    echo "FAIL flycast-ra-route-ipc-smoke: $*" >&2
    exit 1
}

request() {
    "$CTL" --socket "$SOCKET" request "$1" 2>/dev/null || true
}

make -C "$ROOT_DIR" BUILD="$BUILD_REL" jawakad jawaka-platformctl >/dev/null

mkdir -p "$STATE" "$RUNTIME" "$USERDATA" "$LOGS" "$DEFAULTS" "$CORES" \
         "$EMU/flycast" "$EMU/yabasanshiro" "$PRIMARY/Roms/DC" \
         "$PRIMARY/Roms/SATURN" "$PRIMARY/Roms/N64" "$PRIMARY/Images/DC" \
         "$PRIMARY/Images/SATURN" "$PRIMARY/Images/N64" "$PRIMARY/Saves" \
         "$PRIMARY/States" "$STATE/retroarch"

# One env-dumping fixture for every standalone target in this test.
FIXTURE="$TMP_DIR/env-fixture.sh"
cat > "$FIXTURE" <<'SH'
#!/bin/sh
tag="$(basename "$(dirname "$0")")"
[ "$tag" = "scripts" ] && tag="$(basename "$(dirname "$(dirname "$0")")")"
env | sort > "$UMRK_RUNTIME_PATH/env-$tag.txt"
exit 0
SH
cp "$FIXTURE" "$EMU/flycast/launch.sh"
cp "$FIXTURE" "$EMU/yabasanshiro/launch.sh"
chmod 755 "$EMU/flycast/launch.sh" "$EMU/yabasanshiro/launch.sh"
printf '%s\n' "standalone-ra-account-v1" > "$EMU/flycast/ra-account-v1"
printf '%s\n' "umrk-flycast-ra-route-v1" > "$EMU/flycast/ra-route-v1"

# Fake RetroArch: dump its environment next to the standalone dumps.
cat > "$FAKE_RA" <<'SH'
#!/bin/sh
env | sort > "$UMRK_RUNTIME_PATH/env-retroarch.txt"
exit 0
SH
chmod 755 "$FAKE_RA"
printf 'fixture-core\n' > "$CORES/fixture_libretro.so"

# A provider-bound pak core that names itself after Flycast and ships both
# capability records. The provider path decides; files alone grant nothing.
SPOOF="$APPS/mac/Flycast.pak"
mkdir -p "$SPOOF/scripts"
cp "$FIXTURE" "$SPOOF/scripts/run.sh"
chmod 755 "$SPOOF/scripts/run.sh"
printf '%s\n' "standalone-ra-account-v1" > "$SPOOF/ra-account-v1"
printf '%s\n' "umrk-flycast-ra-route-v1" > "$SPOOF/ra-route-v1"
printf '%s\n' '{"id":"org.umrk.flycast-spoof","name":"Flycast","platform":"mac","pak_version":"2.7.0","provides":{"schema":1,"systems":[],"system_extensions":[{"system_id":"DC","add_alternate_cores":["flycast"]}],"cores":[{"id":"flycast","display_name":"Flycast","type":"path","path":"scripts/run.sh","supports_menu":false,"supports_savestate":false,"supports_disk_control":false}]}}' \
    > "$SPOOF/pak.json"

printf 'rom\n' > "$PRIMARY/Roms/DC/Route Game.cdi"
printf 'rom\n' > "$PRIMARY/Roms/SATURN/Route Game.iso"
printf 'rom\n' > "$PRIMARY/Roms/N64/Route Game.n64"
printf '%s\n' '{"schema":1,"version":"0.11.0","release_id":"flycast-route-smoke-1"}' \
    > "$STATE/release.json"

python3 - "$DEFAULTS" <<'CATALOG'
import json, pathlib, sys
defaults = pathlib.Path(sys.argv[1])
def path_core(core_id, name, path):
    return {"id": core_id, "display_name": name, "type": "path",
            "libretro_name": None, "file_name": None, "config_folder": name,
            "info_name": None, "path": path, "supports_menu": False,
            "supports_savestate": False, "supports_disk_control": False,
            "needs_swap": False, "status": "packaged"}
cores = [path_core("flycast_standalone", "Flycast", "emulators/flycast/launch.sh"),
         path_core("yabasanshiro", "YabaSanshiro", "emulators/yabasanshiro/launch.sh"),
         {"id": "fixture_ra", "display_name": "Fixture RA Core",
          "type": "retroarch", "libretro_name": "fixture",
          "file_name": "fixture_libretro.so", "config_folder": "Fixture",
          "info_name": "fixture_libretro.info", "path": None,
          "supports_menu": False, "supports_savestate": True,
          "supports_disk_control": False, "needs_swap": False,
          "status": "packaged"}]
(defaults / "cores.json").write_text(json.dumps(
    {"version": 2, "platform": "mac", "cores": cores}))
def system(sid, name, exts, default, rom_root):
    return {"id": sid, "name": name, "patterns": [sid], "extensions": exts,
            "archive_extensions": [], "archive_inner_extensions": exts,
            "archive_mode": "pass_through", "file_names": [],
            "ignore_file_names": [], "playlist_extensions": [],
            "m3u_generation": "none", "default_core": default,
            "alternate_cores": [], "rom_root": rom_root,
            "image_root": f"Images/{sid}", "bios_notes": []}
(defaults / "systems.json").write_text(json.dumps(
    {"version": 2, "platform": "mac", "systems": [
        system("DC", "Dreamcast", ["cdi"], "flycast_standalone", "Roms/DC"),
        system("SATURN", "Saturn", ["iso"], "yabasanshiro", "Roms/SATURN"),
        system("N64", "Nintendo 64", ["n64"], "fixture_ra", "Roms/N64")]}))
CATALOG

install_service_pak() {
    mkdir -p "$SERVICE_PAK/bin"
    printf '%s\n' \
      "{\"id\":\"$SERVICE_ID\",\"name\":\"RAOfflineProxy\",\"platform\":\"mac\",\"pak_version\":\"0.0.0\",\"service\":{\"schema\":1,\"id\":\"$SERVICE_ID\",\"run\":{\"path\":\"bin/raofflineproxy-fixture\",\"args\":[]},\"default_enabled\":false,\"stop_grace_ms\":300,\"restart\":\"no\",\"lifecycle\":{\"game\":\"ignore\"}}}" \
      > "$SERVICE_PAK/pak.json"
    cat > "$SERVICE_PAK/bin/raofflineproxy-fixture" <<'SH'
#!/bin/sh
exec tail -f /dev/null
SH
    chmod 755 "$SERVICE_PAK/bin/raofflineproxy-fixture"
}

start_daemon() {
    # Stale inherited values must not survive daemon startup or any launch.
    smoke_daemon_start "$ROOT_DIR" "$LOG" \
        PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$APPS" \
        USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
        SAVES_PATH="$PRIMARY/Saves" STATES_PATH="$PRIMARY/States" \
        UMRK_PLATFORM_PATH="$PLATFORM_ROOT" UMRK_RUNTIME_PATH="$RUNTIME" \
        UMRK_DAEMON_SOCKET="$SOCKET" UMRK_INTERNAL_DATA_PATH="$STATE" \
        UMRK_RETROARCH_BIN="$FAKE_RA" \
        JAWAKA_SDCARD_ROOT="$PRIMARY" CORES_PATH="$CORES" \
        UMRK_FLYCAST_RA_ROUTE="service-live" \
        FLYCAST_PROBE="1" \
        FLYCAST_CONFIG_OVERRIDES="achievements:HostUrl=http://inherited.invalid" \
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
            return 0
        fi
        sleep 0.02
    done
    fail "library scan never finished"
}

wait_no_active_game() {
    for _ in $(seq 1 500); do
        [ ! -e "$RUNTIME/active-game.json" ] && return 0
        sleep 0.02
    done
    fail "active launch never finished"
}

stop_daemon() {
    smoke_daemon_stop || return 1
    rm -f "$SOCKET"
}

service_op() { # op, expected effective_state
    request "{\"v\":1,\"op\":\"$1\",\"id\":\"$1\",\"service_id\":\"$SERVICE_ID\"}" |
        grep -F '"ok":true' >/dev/null || fail "service op $1 refused"
}

wait_service_state() { # effective_state
    local status=""
    for _ in $(seq 1 500); do
        status="$(request "{\"v\":1,\"op\":\"status\",\"id\":\"st\",\"service_id\":\"$SERVICE_ID\"}")"
        printf '%s' "$status" | grep -q "\"effective_state\":\"$1\"" && return 0
        sleep 0.02
    done
    fail "service never reached $1: $status"
}

launch_and_dump() { # tag, system, rom, [core_id]
    local tag="$1" payload
    rm -f "$RUNTIME/env-$tag.txt"
    for _ in $(seq 1 300); do
        [ ! -e "$RUNTIME/active-game.json" ] && break
        sleep 0.02
    done
    if [ $# -ge 4 ]; then
        payload="{\"type\":\"launch-game\",\"system\":\"$2\",\"rom_path\":\"$3\",\"core_id\":\"$4\"}"
    else
        payload="{\"type\":\"launch-game\",\"system\":\"$2\",\"rom_path\":\"$3\"}"
    fi
    request "$payload" | grep -F '"ok"' >/dev/null ||
        fail "launch request refused: $payload"
    for _ in $(seq 1 300); do
        [ -f "$RUNTIME/env-$tag.txt" ] && return 0
        kill -0 "$DAEMON_PID" 2>/dev/null || fail "daemon exited"
        sleep 0.02
    done
    fail "env dump for $tag never appeared"
}

expect_route() { # tag, value
    grep -Fxq "UMRK_FLYCAST_RA_ROUTE=$2" "$RUNTIME/env-$1.txt" ||
        fail "expected UMRK_FLYCAST_RA_ROUTE=$2 for $1, got: $(grep '^UMRK_FLYCAST_RA_ROUTE=' "$RUNTIME/env-$1.txt" || echo none)"
    expect_no_probe "$1"
}

expect_no_route() { # tag
    ! grep -q '^UMRK_FLYCAST_RA_ROUTE=' "$RUNTIME/env-$1.txt" ||
        fail "route intent leaked into $1"
    expect_no_probe "$1"
}

expect_no_probe() { # tag
    ! grep -qE '^FLYCAST_(PROBE|CONFIG_OVERRIDES)=' "$RUNTIME/env-$1.txt" ||
        fail "developer probe channel leaked into $1"
}

flycast() { launch_and_dump flycast DC "Roms/DC/Route Game.cdi"; }

# -- Daemon 1: no service pak installed ------------------------------------
start_daemon
python3 - "$DB" <<'PY'
import sqlite3, sys
db = sqlite3.connect(sys.argv[1])
db.executescript("""
INSERT INTO settings(key,value) VALUES ('retroachievements_user','route-user');
INSERT INTO settings(key,value) VALUES ('retroachievements_pass','route-pass');
INSERT INTO settings(key,value) VALUES ('retroachievements_revision','3');
""")
db.commit()
PY

flycast
expect_route flycast native
grep -Fxq "UMRK_RA_ACCOUNT_STATE=configured" "$RUNTIME/env-flycast.txt" ||
    fail "route-capable flycast lost its account snapshot"
echo "case1 service-absent -> native ok"

launch_and_dump yabasanshiro SATURN "Roms/SATURN/Route Game.iso"
expect_no_route yabasanshiro
echo "case2 control standalone -> no route ok"

launch_and_dump Flycast.pak DC "Roms/DC/Route Game.cdi" flycast
expect_no_route Flycast.pak
! grep -q '^UMRK_RA_ACCOUNT_' "$RUNTIME/env-Flycast.pak.txt" ||
    fail "spoof provider received the account snapshot"
echo "case3 provider-bound flycast spoof -> no route ok"

launch_and_dump retroarch N64 "Roms/N64/Route Game.n64"
expect_no_route retroarch
echo "case4 retroarch/libretro -> no route ok"
# Stopping the daemon mid-launch leaves an active-game record that the next
# daemon recovers and blocks new games on; let the last launch finish first.
wait_no_active_game
stop_daemon

# -- Daemon 2: service pak installed ---------------------------------------
install_service_pak
start_daemon

flycast
expect_route flycast native
echo "case5 installed, not running -> native ok"

service_op enable
service_op run
wait_service_state running
flycast
expect_route flycast service-live
! grep -qE '^UMRK_FLYCAST_RA_ROUTE=.*(127\.0\.0\.1|8080|http|route-pass)' "$RUNTIME/env-flycast.txt" ||
    fail "route value carries more than intent"
echo "case6 running -> service-live ok"

launch_and_dump yabasanshiro SATURN "Roms/SATURN/Route Game.iso"
expect_no_route yabasanshiro
launch_and_dump Flycast.pak DC "Roms/DC/Route Game.cdi" flycast
expect_no_route Flycast.pak
echo "case7 live service: control and spoof still get no route ok"

mv "$EMU/flycast/ra-route-v1" "$TMP_DIR/ra-route-v1.saved"
flycast
expect_no_route flycast
grep -Fxq "UMRK_RA_ACCOUNT_STATE=configured" "$RUNTIME/env-flycast.txt" ||
    fail "account-only flycast lost its account snapshot"
grep -q "this Flycast build cannot route through the offline service" "$LOG" ||
    fail "no upgrade explanation for an account-only Flycast build"
mv "$TMP_DIR/ra-route-v1.saved" "$EMU/flycast/ra-route-v1"
echo "case8 account-only flycast build -> no route, explained ok"

service_op stop
wait_service_state stopped
flycast
expect_route flycast native
echo "case9 stopped -> native ok"

! grep -q 'route-pass' "$LOG" || fail "password present in daemon log"
echo "PASS flycast-ra-route-ipc-smoke"
