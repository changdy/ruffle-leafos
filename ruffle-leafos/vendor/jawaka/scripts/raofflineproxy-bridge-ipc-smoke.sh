#!/usr/bin/env bash
# RAOfflineProxy transient launch bridge smoke: real jawakad on a mock SD, a
# fake RAOfflineProxy service pak, a fake /leaf/health endpoint, and a fake
# RetroArch that captures its per-launch config. Drives the launch through
# the IPC the launcher uses and asserts the routing outcomes.
#
# The same supervisor state also decides the bundled Flycast child's
# UMRK_FLYCAST_RA_ROUTE (proxy plan P2): cases 9-13 launch a route-capable
# bundled Flycast fixture and read the environment it actually got, while
# the service is RUNNING, RUNNING without health (no gate, no wait: Flycast
# checks health itself), STARTING (for Flycast and for the RetroArch gate)
# and stopped.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_REL="${BUILD:-build}"
TMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/jw-rop-bridge.XXXXXX")"
PRIMARY="$TMP_DIR/primary"
STATE="$TMP_DIR/state"
RUNTIME="$TMP_DIR/runtime"
USERDATA="$PRIMARY/.userdata/mac"
LOGS="$USERDATA/logs"
PLATFORM_ROOT="$PRIMARY/.system/leaf/platforms/mac"
DEFAULTS="$PLATFORM_ROOT/defaults"
PAK="$PRIMARY/Apps/mac/RAOfflineProxy.pak"
FLYCAST_DIR="$PLATFORM_ROOT/emulators/flycast"
FLYCAST_ENV="$RUNTIME/env-flycast.txt"
FAKE_RA="$TMP_DIR/fake-retroarch.sh"
HEALTH_PY="$TMP_DIR/fake-health.py"
SOCKET="$RUNTIME/jawakad.sock"
LOG="$TMP_DIR/jawakad.log"
SERVICE_ID="org.umrk.raofflineproxy"
SHARED_CFG="$STATE/retroarch/retroarch.cfg"
CAPTURED_CFG="$TMP_DIR/captured.cfg"
HEALTH_PID=""
CTL="$ROOT_DIR/$BUILD_REL/bin/jawaka-platformctl"

fail() { echo "raofflineproxy-bridge-ipc-smoke: $1" >&2; exit 1; }

cleanup() {
    exit_status=$?
    set +e
    [ -n "$HEALTH_PID" ] && kill "$HEALTH_PID" 2>/dev/null
    smoke_daemon_stop || exit_status=1
    if [ "$exit_status" -ne 0 ]; then
        [ -f "$LOG" ] && tail -80 "$LOG" >&2
        [ -f "$SHARED_CFG" ] && { echo "--- shared cfg ---" >&2; cat "$SHARED_CFG" >&2; }
        [ -f "$CAPTURED_CFG" ] && { echo "--- captured runtime cfg ---" >&2; cat "$CAPTURED_CFG" >&2; }
    fi
    rm -rf "$TMP_DIR"
    exit "$exit_status"
}
trap cleanup EXIT

make -C "$ROOT_DIR" jawakad jawaka-platformctl >/dev/null
mkdir -p "$PAK/bin" "$STATE/retroarch" "$RUNTIME" "$USERDATA" "$LOGS" \
         "$DEFAULTS" "$PLATFORM_ROOT/cores" \
         "$PRIMARY/Roms/N64" "$PRIMARY/Images/N64" \
         "$PRIMARY/Roms/DC" "$PRIMARY/Images/DC" \
         "$PRIMARY/Saves" "$PRIMARY/States" "$FLYCAST_DIR"

# Fake RetroArch: capture the per-launch config, then sit in the foreground
# briefly so the daemon registers a real game session before it exits.
cat >"$FAKE_RA" <<'EOF'
#!/bin/sh
config=""
while [ $# -gt 0 ]; do
    if [ "$1" = "--config" ]; then config="$2"; shift 2; continue; fi
    shift
done
[ -n "$config" ] && cp "$config" "__CAPTURED__"
sleep 0.2
exit 0
EOF
sed "s|__CAPTURED__|$CAPTURED_CFG|" "$FAKE_RA" >"$FAKE_RA.tmp"
mv "$FAKE_RA.tmp" "$FAKE_RA"
chmod 755 "$FAKE_RA"

# Fake /leaf/health endpoint with the fixed ready document.
cat >"$HEALTH_PY" <<'EOF'
import http.server

class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = b'{"service":"org.umrk.raofflineproxy","protocol":"leaf-health-1","ready":true}'
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass

http.server.HTTPServer(("127.0.0.1", 8080), Handler).serve_forever()
EOF

# Fixture service pak (game: ignore lifecycle, enabled below via IPC).
printf '%s\n' \
  "{\"id\":\"$SERVICE_ID\",\"name\":\"RAOfflineProxy\",\"platform\":\"mac\",\"pak_version\":\"0.0.0\",\"service\":{\"schema\":1,\"id\":\"$SERVICE_ID\",\"run\":{\"path\":\"bin/raofflineproxy-fixture\",\"args\":[]},\"default_enabled\":false,\"stop_grace_ms\":300,\"restart\":\"no\",\"lifecycle\":{\"game\":\"ignore\"}}}" \
  >"$PAK/pak.json"
cat >"$PAK/bin/raofflineproxy-fixture" <<'EOF'
#!/bin/sh
exec tail -f /dev/null
EOF
chmod 755 "$PAK/bin/raofflineproxy-fixture"

# Fake libretro core.
printf 'fixture-core\n' >"$PLATFORM_ROOT/cores/fixture_libretro.so"

# The bundled Flycast child: the release-owned launcher plus both capability
# records a route-capable build ships. It records its environment and exits.
cat >"$FLYCAST_DIR/launch.sh" <<'EOF'
#!/bin/sh
env | sort >"$UMRK_RUNTIME_PATH/env-flycast.tmp"
mv "$UMRK_RUNTIME_PATH/env-flycast.tmp" "$UMRK_RUNTIME_PATH/env-flycast.txt"
exit 0
EOF
chmod 755 "$FLYCAST_DIR/launch.sh"
printf '%s\n' "standalone-ra-account-v1" >"$FLYCAST_DIR/ra-account-v1"
printf '%s\n' "umrk-flycast-ra-route-v1" >"$FLYCAST_DIR/ra-route-v1"

printf '%s\n' \
  '{"version":2,"platform":"mac","cores":[{"id":"fixture_ra","display_name":"Fixture RA Core","type":"retroarch","libretro_name":"fixture","file_name":"fixture_libretro.so","config_folder":"Fixture","info_name":"fixture_libretro.info","path":null,"supports_menu":false,"supports_savestate":true,"supports_disk_control":false,"needs_swap":false,"status":"packaged"},{"id":"flycast_standalone","display_name":"Flycast","type":"path","libretro_name":null,"file_name":null,"config_folder":"Flycast","info_name":null,"path":"emulators/flycast/launch.sh","supports_menu":false,"supports_savestate":false,"supports_disk_control":false,"needs_swap":false,"status":"packaged"}]}' \
  >"$DEFAULTS/cores.json"
printf '%s\n' \
  '{"version":2,"platform":"mac","systems":[{"id":"N64","name":"Nintendo 64","patterns":["N64"],"extensions":["n64"],"archive_extensions":[],"archive_inner_extensions":["n64"],"archive_mode":"pass_through","file_names":[],"ignore_file_names":[],"playlist_extensions":[],"m3u_generation":"none","default_core":"fixture_ra","alternate_cores":[],"rom_root":"Roms/N64","image_root":"Images/N64","bios_notes":[]},{"id":"DC","name":"Dreamcast","patterns":["DC"],"extensions":["cdi"],"archive_extensions":[],"archive_inner_extensions":["cdi"],"archive_mode":"pass_through","file_names":[],"ignore_file_names":[],"playlist_extensions":[],"m3u_generation":"none","default_core":"flycast_standalone","alternate_cores":[],"rom_root":"Roms/DC","image_root":"Images/DC","bios_notes":[]}]}' \
  >"$DEFAULTS/systems.json"
printf 'rom\n' >"$PRIMARY/Roms/N64/Bridge.n64"
printf 'rom\n' >"$PRIMARY/Roms/DC/Bridge.cdi"

smoke_daemon_start "$ROOT_DIR" "$LOG" \
    PLATFORM=mac SDCARD_PATH="$PRIMARY" APPS_PATH="$PRIMARY/Apps" \
    USERDATA_PATH="$USERDATA" LOGS_PATH="$LOGS" \
    SAVES_PATH="$PRIMARY/Saves" STATES_PATH="$PRIMARY/States" \
    UMRK_PLATFORM_PATH="$PLATFORM_ROOT" UMRK_RUNTIME_PATH="$RUNTIME" \
    UMRK_DAEMON_SOCKET="$SOCKET" UMRK_INTERNAL_DATA_PATH="$STATE" \
    UMRK_RETROARCH_BIN="$FAKE_RA" JAWAKA_SDCARD_ROOT="$PRIMARY" \
    "$ROOT_DIR/$BUILD_REL/bin/jawakad" --daemon-only

for _ in $(seq 1 300); do
    [ -S "$SOCKET" ] && break
    kill -0 "$DAEMON_PID" 2>/dev/null || exit 1
    sleep 0.02
done
[ -S "$SOCKET" ]
for _ in $(seq 1 500); do
    status="$($CTL --socket "$SOCKET" request '{"type":"library-status"}' 2>/dev/null || true)"
    if python3 -c 'import json,sys; d=json.load(sys.stdin); raise SystemExit(d.get("scan_running", True) or d.get("generation", 0) <= 0)' <<<"$status" 2>/dev/null; then
        break
    fi
    sleep 0.02
done

enable_service() {
    reply="$("$CTL" --socket "$SOCKET" request \
        "{\"v\":1,\"op\":\"enable\",\"id\":\"en\",\"service_id\":\"$SERVICE_ID\"}")"
    printf '%s' "$reply" | grep -F '"ok":true' >/dev/null
    "$CTL" --socket "$SOCKET" request \
        "{\"v\":1,\"op\":\"run\",\"id\":\"run\",\"service_id\":\"$SERVICE_ID\"}" |
        grep -F '"ok":true' >/dev/null
    for _ in $(seq 1 500); do
        status="$($CTL --socket "$SOCKET" request \
            "{\"v\":1,\"op\":\"status\",\"id\":\"st\",\"service_id\":\"$SERVICE_ID\"}" 2>/dev/null || true)"
        printf '%s' "$status" | grep -q '"effective_state":"running"' && break
        sleep 0.02
    done
    printf '%s' "$status" | grep -q '"effective_state":"running"'
}

wait_game_finished() {
    for _ in $(seq 1 500); do
        [ -e "$CAPTURED_CFG" ] && \
            ! grep -q "RetroArch child" "$LOG" 2>/dev/null && sleep 0.05
        grep -q "shared config backed up" "$LOG" && break
        sleep 0.05
    done
    grep -q "shared config backed up" "$LOG"
}

launch() {
    rm -f "$CAPTURED_CFG"
    : >"$LOG.marks"
    "$CTL" --socket "$SOCKET" request \
        '{"type":"launch-game","system":"N64","rom_path":"Roms/N64/Bridge.n64"}'
}

now_ms() { python3 -c 'import time; print(int(time.monotonic()*1000))'; }

# Time the synchronous launch request itself: jawakad's IPC handler runs the
# gate and the spawn before replying, so this measures the added routing wait
# (and nothing of the fixture child's lifetime).
launch_timed() {
    local start reply elapsed
    start=$(now_ms)
    reply=$(launch)
    elapsed=$(( $(now_ms) - start ))
    printf '%s' "$reply" | grep -F '"type":"ok"' >/dev/null
    echo "$elapsed"
}

wait_backup_fresh() {
    # Wait for the "shared config backed up" line emitted after the marker.
    for _ in $(seq 1 500); do
        if [ -f "$LOG" ] && grep -c "shared config backed up" "$LOG" | grep -q "^${1}$"; then
            break
        fi
        sleep 0.05
    done
    [ "$(grep -c 'shared config backed up' "$LOG")" = "$1" ]
}

# -- Case 1: service installed but DISABLED -> direct launch, zero wait --
printf '%s' \
  "menu_driver = \"rgui\"
cheevos_custom_host = \"foreign.example:9999\"
cheevos_hardcore_mode_enable = \"false\"
" >"$SHARED_CFG"

elapsed_ms=$(launch_timed)
wait_backup_fresh 1
[ -e "$CAPTURED_CFG" ]
! grep -q "127.0.0.1:8080" "$CAPTURED_CFG" ||
    fail "case1: a disabled service still routed through the proxy"
grep -q "cheevos_custom_host = \"foreign.example:9999\"" "$SHARED_CFG"
[ "$elapsed_ms" -lt 450 ]
echo "case1 disabled-direct ok (${elapsed_ms}ms)"

# -- Case 2: enabled + healthy -> proxied launch, transient restore --
enable_service
printf '%s' \
  "menu_driver = \"rgui\"
cheevos_custom_host = \"foreign.example:9999\"
cheevos_hardcore_mode_enable = \"false\"
" >"$SHARED_CFG"
python3 "$HEALTH_PY" & HEALTH_PID=$!
sleep 0.3

launch | grep -F '"type":"ok"' >/dev/null
wait_backup_fresh 2
[ -e "$CAPTURED_CFG" ]
grep -q "cheevos_custom_host = \"127.0.0.1:8080\"" "$CAPTURED_CFG"
grep -q "cheevos_hardcore_mode_enable = \"false\"" "$CAPTURED_CFG"
# Snapshot restore: foreign shared values byte-identical, no injected content.
grep -qxF 'cheevos_custom_host = "foreign.example:9999"' "$SHARED_CFG"
[ "$(grep -c 'cheevos_custom_host' "$SHARED_CFG")" = 1 ]
! grep -q "127.0.0.1:8080" "$SHARED_CFG" ||
    fail "case2: the proxy host leaked into the shared config"
! grep -q "cheevos_token" "$SHARED_CFG" ||
    fail "case2: a token leaked into the shared config"
echo "case2 ready-proxied ok"

# -- Case 3: durable Hardcore=true -> direct, no injection, no wait --
printf '%s' \
  "menu_driver = \"rgui\"
cheevos_hardcore_mode_enable = \"true\"
" >"$SHARED_CFG"
elapsed_ms=$(launch_timed)
wait_backup_fresh 3
[ -e "$CAPTURED_CFG" ]
! grep -q "127.0.0.1:8080" "$CAPTURED_CFG" ||
    fail "case3: a Hardcore launch still routed through the proxy"
grep -qxF 'cheevos_hardcore_mode_enable = "true"' "$SHARED_CFG"
[ "$elapsed_ms" -lt 450 ]
echo "case3 hardcore-direct ok (${elapsed_ms}ms)"

# -- Case 4: session-stopped -> direct even when Start with Leaf is true --
"$CTL" --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"stop\",\"id\":\"stop\",\"service_id\":\"$SERVICE_ID\"}" |
    grep -F '"ok":true' >/dev/null
for _ in $(seq 1 500); do
    status="$($CTL --socket "$SOCKET" request \
        "{\"v\":1,\"op\":\"status\",\"id\":\"stopped\",\"service_id\":\"$SERVICE_ID\"}" 2>/dev/null || true)"
    printf '%s' "$status" | grep -q '"effective_state":"stopped"' && break
    sleep 0.02
done
printf '%s' "$status" | grep -q '"effective_state":"stopped"'
printf '%s' \
  "menu_driver = \"rgui\"
cheevos_hardcore_mode_enable = \"false\"
" >"$SHARED_CFG"
elapsed_ms=$(launch_timed)
wait_backup_fresh 4
[ -e "$CAPTURED_CFG" ]
! grep -q "127.0.0.1:8080" "$CAPTURED_CFG" ||
    fail "case4: a session-stopped service still routed through the proxy"
[ "$elapsed_ms" -lt 450 ]
echo "case4 session-stopped-direct ok (${elapsed_ms}ms)"

# Start it again for the intended-but-unhealthy cases below.
enable_service

# -- Case 5: intended but NOT healthy now -> blocked prompt; dismiss cancels --
kill "$HEALTH_PID" 2>/dev/null; wait "$HEALTH_PID" 2>/dev/null || true
HEALTH_PID=""
printf '%s' \
  "menu_driver = \"rgui\"
cheevos_hardcore_mode_enable = \"false\"
" >"$SHARED_CFG"
rm -f "$CAPTURED_CFG"

# A blocked launch replies error, matching the LIFE-1 fail-closed convention.
"$CTL" --socket "$SOCKET" request \
    '{"type":"launch-game","system":"N64","rom_path":"Roms/N64/Bridge.n64"}' |
    grep -F '"type":"error"' >/dev/null
blocked=""
for _ in $(seq 1 500); do
    blocked="$($CTL --socket "$SOCKET" request '{"type":"game-launch-blocked-status"}' 2>/dev/null || true)"
    printf '%s' "$blocked" | grep -q '"blocked":true' && break
    sleep 0.02
done
printf '%s' "$blocked" | grep -q '"reason":"raofflineproxy-not-ready"'
printf '%s' "$blocked" | grep -q '"override_allowed":true'
printf '%s' "$blocked" | grep -q "\"service_id\":\"$SERVICE_ID\""
[ ! -e "$CAPTURED_CFG" ]

# Cancel path: dismiss leaves the game unlaunched.
"$CTL" --socket "$SOCKET" request '{"type":"game-launch-blocked-dismiss"}' |
    grep -F '"type":"ok"' >/dev/null
sleep 0.2
[ ! -e "$CAPTURED_CFG" ]
blocked="$($CTL --socket "$SOCKET" request '{"type":"game-launch-blocked-status"}' 2>/dev/null || true)"
printf '%s' "$blocked" | grep -q '"blocked":false'
echo "case5 blocked-cancel ok"

# Retry is refused when nothing is blocked: it can never start a launch of
# its own.
"$CTL" --socket "$SOCKET" request '{"type":"game-launch-retry"}' |
    grep -F '"type":"error"' >/dev/null

block_launch() {
    "$CTL" --socket "$SOCKET" request \
        '{"type":"launch-game","system":"N64","rom_path":"Roms/N64/Bridge.n64"}' |
        grep -F '"type":"error"' >/dev/null
    for _ in $(seq 1 500); do
        blocked="$($CTL --socket "$SOCKET" request '{"type":"game-launch-blocked-status"}' 2>/dev/null || true)"
        printf '%s' "$blocked" | grep -q '"blocked":true' && break
        sleep 0.02
    done
    printf '%s' "$blocked" | grep -q '"reason":"raofflineproxy-not-ready"'
    printf '%s' "$blocked" | grep -q '"override_allowed":true'
    printf '%s' "$blocked" | grep -q '"retry_allowed":true'
}

# Retry once, timed: the daemon re-runs the bounded check synchronously, so a
# still-down service costs the full 500 ms budget before it re-blocks.
retry_timed() {
    local start reply elapsed
    start=$(now_ms)
    reply="$("$CTL" --socket "$SOCKET" request '{"type":"game-launch-retry"}' || true)"
    elapsed=$(( $(now_ms) - start ))
    printf '%s' "$reply" >"$TMP_DIR/retry.reply"
    echo "$elapsed"
}

# -- Case 5b: Retry while still down re-blocks; Cancel after it still cancels --
block_launch
elapsed_ms=$(retry_timed)
grep -F '"type":"error"' "$TMP_DIR/retry.reply" >/dev/null
grep -F 'still blocked' "$TMP_DIR/retry.reply" >/dev/null
[ "$elapsed_ms" -ge 400 ]
blocked="$($CTL --socket "$SOCKET" request '{"type":"game-launch-blocked-status"}')"
printf '%s' "$blocked" | grep -q '"blocked":true'
printf '%s' "$blocked" | grep -q '"reason":"raofflineproxy-not-ready"'
printf '%s' "$blocked" | grep -q '"retry_allowed":true'
[ ! -e "$CAPTURED_CFG" ]
# Each Retry is one explicit attempt; a second one behaves the same.
elapsed_ms=$(retry_timed)
grep -F '"type":"error"' "$TMP_DIR/retry.reply" >/dev/null
[ "$elapsed_ms" -ge 400 ]
[ "$(grep -c 'user chose Retry for blocked launch' "$LOG")" = 2 ]
"$CTL" --socket "$SOCKET" request '{"type":"game-launch-blocked-dismiss"}' |
    grep -F '"type":"ok"' >/dev/null
sleep 0.2
[ ! -e "$CAPTURED_CFG" ]
blocked="$($CTL --socket "$SOCKET" request '{"type":"game-launch-blocked-status"}')"
printf '%s' "$blocked" | grep -q '"blocked":false'
printf '%s' "$blocked" | grep -q '"retry_allowed":false'
echo "case5b retry-still-down-reblocks ok (${elapsed_ms}ms)"

# -- Case 5c: Retry after the service came back -> normal proxied launch --
printf '%s' \
  "menu_driver = \"rgui\"
cheevos_custom_host = \"foreign.example:9999\"
cheevos_hardcore_mode_enable = \"false\"
" >"$SHARED_CFG"
block_launch
python3 "$HEALTH_PY" & HEALTH_PID=$!
sleep 0.3
"$CTL" --socket "$SOCKET" request '{"type":"game-launch-retry"}' |
    grep -F '"type":"ok"' >/dev/null
wait_backup_fresh 5
[ -e "$CAPTURED_CFG" ]
grep -q "cheevos_custom_host = \"127.0.0.1:8080\"" "$CAPTURED_CFG"
grep -q "cheevos_hardcore_mode_enable = \"false\"" "$CAPTURED_CFG"
# Snapshot restore intact, exactly as case 2.
grep -qxF 'cheevos_custom_host = "foreign.example:9999"' "$SHARED_CFG"
[ "$(grep -c 'cheevos_custom_host' "$SHARED_CFG")" = 1 ]
! grep -q "127.0.0.1:8080" "$SHARED_CFG" ||
    fail "case5c: the proxy host leaked into the shared config"
! grep -q "cheevos_token" "$SHARED_CFG" ||
    fail "case5c: a token leaked into the shared config"
blocked="$($CTL --socket "$SOCKET" request '{"type":"game-launch-blocked-status"}')"
printf '%s' "$blocked" | grep -q '"blocked":false'
echo "case5c retry-healthy-proxied ok"

kill "$HEALTH_PID" 2>/dev/null; wait "$HEALTH_PID" 2>/dev/null || true
HEALTH_PID=""
printf '%s' \
  "menu_driver = \"rgui\"
cheevos_hardcore_mode_enable = \"false\"
" >"$SHARED_CFG"
rm -f "$CAPTURED_CFG"

# -- Case 6: blocked prompt again; a failed Retry first, then override ->
# direct play, single bypass (Play Anyway unchanged by Retry) --
block_launch
elapsed_ms=$(retry_timed)
grep -F '"type":"error"' "$TMP_DIR/retry.reply" >/dev/null
"$CTL" --socket "$SOCKET" request '{"type":"game-launch-override"}' |
    grep -F '"type":"ok"' >/dev/null
wait_backup_fresh 6
[ -e "$CAPTURED_CFG" ]
! grep -q "127.0.0.1:8080" "$CAPTURED_CFG" ||
    fail "case6: the override launch still routed through the proxy"
! grep -q "cheevos_custom_host" "$SHARED_CFG" ||
    fail "case6: a custom host leaked into the shared config"
! grep -q "cheevos_token" "$SHARED_CFG" ||
    fail "case6: a token leaked into the shared config"
echo "case6 override-direct ok"

# The bypass is consumed: with the service back up and healthy, the very next
# launch routes through the proxy again (no stale direct memories).
python3 "$HEALTH_PY" & HEALTH_PID=$!
sleep 0.3
launch | grep -F '"type":"ok"' >/dev/null
wait_backup_fresh 7
grep -q "cheevos_custom_host = \"127.0.0.1:8080\"" "$CAPTURED_CFG"
echo "case7 bypass-consumed ok"

# -- Case 8: Run pressed WITHOUT Start with Leaf -> still routes --
# The device found this one. desired_enabled ("Start with Leaf") and
# session_run ("Run") are independent, and every case above happened to set
# both, so a gate that required both looked correct here while ignoring a
# healthy hand-started service on real hardware.
# Disable (which also stops), then Run: a session-only run with autostart off.
# That is what a user gets by pressing Run without enabling Start with Leaf,
# and it is the state the old gate silently refused to route to.
"$CTL" --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"disable\",\"id\":\"d8\",\"service_id\":\"$SERVICE_ID\"}" |
    grep -F '"ok":true' >/dev/null
"$CTL" --socket "$SOCKET" request \
    "{\"v\":1,\"op\":\"run\",\"id\":\"r8\",\"service_id\":\"$SERVICE_ID\"}" |
    grep -F '"ok":true' >/dev/null
for _ in $(seq 1 750); do
    status="$($CTL --socket "$SOCKET" request \
        "{\"v\":1,\"op\":\"status\",\"id\":\"st8\",\"service_id\":\"$SERVICE_ID\"}" 2>/dev/null || true)"
    printf '%s' "$status" | grep -q '"effective_state":"running"' && break
    sleep 0.02
done
# Both halves matter: without the first this proves nothing, without the
# second it is just case 2 again.
printf '%s' "$status" | grep -q '"effective_state":"running"'
printf '%s' "$status" | grep -q '"desired_enabled":false'
printf '%s' \
  "menu_driver = \"rgui\"
cheevos_hardcore_mode_enable = \"false\"
" >"$SHARED_CFG"
rm -f "$CAPTURED_CFG"
launch | grep -F '"type":"ok"' >/dev/null
wait_backup_fresh 8
grep -q "cheevos_custom_host = \"127.0.0.1:8080\"" "$CAPTURED_CFG"
grep -q "cheevos_hardcore_mode_enable = \"false\"" "$CAPTURED_CFG"
! grep -q "cheevos_custom_host" "$SHARED_CFG" ||
    fail "case8: a custom host leaked into the shared config"
echo "case8 run-without-start-with-leaf ok"

# -- Bundled Flycast child and the STARTING supervisor state ---------------

service_status() {
    "$CTL" --socket "$SOCKET" request \
        "{\"v\":1,\"op\":\"status\",\"id\":\"fs\",\"service_id\":\"$SERVICE_ID\"}" 2>/dev/null || true
}

# Not running: "stopped", or "disabled" once Start with Leaf is off (case 8
# left it off).
wait_service_idle() {
    for _ in $(seq 1 500); do
        service_status | grep -qE '"effective_state":"(stopped|disabled)"' && return 0
        sleep 0.02
    done
    service_status | grep -qE '"effective_state":"(stopped|disabled)"'
}

service_op() { # op
    "$CTL" --socket "$SOCKET" request \
        "{\"v\":1,\"op\":\"$1\",\"id\":\"$1\",\"service_id\":\"$SERVICE_ID\"}" |
        grep -F '"ok":true' >/dev/null
}

wait_no_active_game() {
    for _ in $(seq 1 500); do
        [ ! -e "$RUNTIME/active-game.json" ] && return 0
        sleep 0.02
    done
    [ ! -e "$RUNTIME/active-game.json" ]
}

launch_flycast() {
    wait_no_active_game
    rm -f "$FLYCAST_ENV"
    "$CTL" --socket "$SOCKET" request \
        '{"type":"launch-game","system":"DC","rom_path":"Roms/DC/Bridge.cdi"}' |
        grep -F '"type":"ok"' >/dev/null
    for _ in $(seq 1 300); do
        [ -f "$FLYCAST_ENV" ] && break
        sleep 0.02
    done
    [ -f "$FLYCAST_ENV" ]
}

expect_flycast_route() { # value
    grep -Fxq "UMRK_FLYCAST_RA_ROUTE=$1" "$FLYCAST_ENV" || {
        echo "expected UMRK_FLYCAST_RA_ROUTE=$1, got: $(grep '^UMRK_FLYCAST_RA_ROUTE=' "$FLYCAST_ENV" || echo none)" >&2
        return 1
    }
    # Intent only: never a host, port, URL or credential.
    ! grep -qE '^UMRK_FLYCAST_RA_ROUTE=.*(127\.0\.0\.1|8080|http)' "$FLYCAST_ENV" ||
        fail "route value carries more than intent"
}

# Run a launch inside the service's STARTING window. The window is the
# supervisor's settle time after a fresh generation, so start one, confirm
# "starting", launch, and confirm "starting" again after the (synchronous)
# launch reply: the route was then decided during STARTING. A slow runner
# that misses the window retries with a fresh generation.
in_starting_window() { # command...
    for _ in 1 2 3 4 5; do
        service_op stop
        wait_service_idle
        service_op run
        service_status | grep -q '"effective_state":"starting"' || continue
        "$@"
        service_status | grep -q '"effective_state":"starting"' && return 0
        # Missed the window: clear a pending bypass prompt before retrying.
        "$CTL" --socket "$SOCKET" request '{"type":"game-launch-blocked-dismiss"}' \
            >/dev/null 2>&1 || true
        wait_no_active_game
    done
    echo "never landed a launch inside the STARTING window" >&2
    return 1
}

# -- Case 9: service RUNNING -> the bundled Flycast child gets service-live --
wait_backup_fresh 8
service_status | grep -q '"effective_state":"running"'
launch_flycast
expect_flycast_route service-live
! grep -q '^JAWAKA_CHEEVOS_' "$FLYCAST_ENV" ||
    fail "case9: RetroArch credentials reached the Flycast child"
echo "case9 flycast running -> service-live ok"

# -- Case 10: RUNNING but health down -> still service-live, never gated --
kill "$HEALTH_PID" 2>/dev/null; wait "$HEALTH_PID" 2>/dev/null || true
HEALTH_PID=""
gate_blocks=$(grep -c 'blocking launch' "$LOG" || true)
start=$(now_ms)
launch_flycast
elapsed_ms=$(( $(now_ms) - start ))
expect_flycast_route service-live
"$CTL" --socket "$SOCKET" request '{"type":"game-launch-blocked-status"}' |
    grep -q '"blocked":false'
[ "$(grep -c 'blocking launch' "$LOG" || true)" = "$gate_blocks" ]
echo "case10 flycast running, unhealthy -> service-live, not gated ok (${elapsed_ms}ms)"
python3 "$HEALTH_PY" & HEALTH_PID=$!
sleep 0.3

# -- Case 11: STARTING -> the bundled Flycast child gets service-live --
in_starting_window launch_flycast
expect_flycast_route service-live
echo "case11 flycast starting -> service-live ok"

# -- Case 12: STARTING -> the RetroArch gate never treats it as absent --
# A STARTING service is live, so RetroArch must not take the zero-wait
# direct path. The gate probes health only once the supervisor reports
# RUNNING, and it does not advance the supervisor inside its 500 ms window,
# so today a launch inside the STARTING window ends in the bypass prompt even
# with a healthy endpoint. Either outcome of the gate (proxied, or the
# prompt) is accepted here; a silent direct launch is not.
printf '%s' \
  "menu_driver = \"rgui\"
cheevos_hardcore_mode_enable = \"false\"
" >"$SHARED_CFG"
rm -f "$CAPTURED_CFG"
wait_no_active_game
starting_backups=$(grep -c 'shared config backed up' "$LOG")
STARTING_REPLY="$TMP_DIR/starting-reply.json"
starting_retroarch() { launch >"$STARTING_REPLY"; }
in_starting_window starting_retroarch
if grep -qF '"type":"ok"' "$STARTING_REPLY"; then
    wait_backup_fresh $((starting_backups + 1))
    grep -q "cheevos_custom_host = \"127.0.0.1:8080\"" "$CAPTURED_CFG"
    ! grep -q "cheevos_custom_host" "$SHARED_CFG" ||
        fail "case12: a custom host leaked into the shared config"
    echo "case12 retroarch starting -> proxied ok"
else
    blocked="$($CTL --socket "$SOCKET" request '{"type":"game-launch-blocked-status"}' 2>/dev/null || true)"
    printf '%s' "$blocked" | grep -q '"blocked":true'
    printf '%s' "$blocked" | grep -q '"reason":"raofflineproxy-not-ready"'
    [ ! -e "$CAPTURED_CFG" ]
    "$CTL" --socket "$SOCKET" request '{"type":"game-launch-blocked-dismiss"}' |
        grep -F '"type":"ok"' >/dev/null
    echo "case12 retroarch starting -> bypass prompt, not direct ok"
fi

# -- Case 13: stopped -> native --
wait_no_active_game
service_op stop
wait_service_idle
launch_flycast
expect_flycast_route native
echo "case13 flycast stopped -> native ok"

wait_no_active_game
echo "PASS raofflineproxy-bridge-ipc-smoke"
