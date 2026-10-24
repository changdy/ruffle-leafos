#!/usr/bin/env bash
# Runs B4a against two dedicated directories bind-mounted from real MLP1 FAT
# cards. The host runner supplies cross-built binaries and the local feed.
set -euo pipefail

PRIMARY="${B4A_PRIMARY:?B4A_PRIMARY is required}"
SECONDARY="${B4A_SECONDARY:?B4A_SECONDARY is required}"
BASE_URL="${B4A_BASE_URL:?B4A_BASE_URL is required}"
FLOOR_SHA="${B4A_FLOOR_SHA:?B4A_FLOOR_SHA is required}"
BIN_ROOT="${B4A_BIN_ROOT:?B4A_BIN_ROOT is required}"
RUNTIME_LIB_DIR="${B4A_RUNTIME_LIB_DIR:-}"
EVIDENCE="${B4A_EVIDENCE:-/tmp/jw-b4a-evidence}"
PAKRAT="$BIN_ROOT/jawaka-pakrat-smoke"
DAEMON="$BIN_ROOT/jawakad"
CTL="$BIN_ROOT/jawaka-platformctl"
PRIMARY_APPS="$PRIMARY/Apps"
SECONDARY_APPS="$SECONDARY/Apps"
PRIMARY_USERDATA="$PRIMARY/.userdata/mlp1"
SECONDARY_USERDATA="$SECONDARY/.userdata/mlp1"
STATE="$PRIMARY/.umrk/mlp1"
CONTROL_DB="$STATE/services-control.db"
PLATFORM_ROOT="$PRIMARY/.system/leaf/platforms/mlp1"
RUNTIME="/tmp/jw-b4a-runtime"
SOCKET="$RUNTIME/jawakad.sock"
LOGS="$PRIMARY_USERDATA/logs"
STORE_ID="org.umrk.test.txnservice"
INSTALL_REL="mlp1/TxnService.pak"
TARGET="$PRIMARY_APPS/$INSTALL_REL"
SECONDARY_TARGET="$SECONDARY_APPS/$INSTALL_REL"
TMP_ROOT="/tmp/jw-b4a-device"
DAEMON_PID=""
DAEMON_RUN=0
PAUSED_PID=""

export LD_LIBRARY_PATH="${RUNTIME_LIB_DIR:+$RUNTIME_LIB_DIR:}${LD_LIBRARY_PATH:-}"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

cleanup() {
    if [ -n "$PAUSED_PID" ]; then
        kill -KILL "$PAUSED_PID" >/dev/null 2>&1 || true
        wait "$PAUSED_PID" >/dev/null 2>&1 || true
    fi
    if [ -n "$DAEMON_PID" ]; then
        kill "$DAEMON_PID" >/dev/null 2>&1 || true
        wait "$DAEMON_PID" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT HUP INT TERM

case "$PRIMARY:$SECONDARY" in
    /tmp/jw-b4a-primary:/tmp/jw-b4a-secondary|/tmp/jw-b4a-secondary:/tmp/jw-b4a-primary) ;;
    *) fail "unsafe B4a fixture roots" ;;
esac
for path in "$PAKRAT" "$DAEMON" "$CTL"; do
    [ -x "$path" ] || fail "missing binary $path"
done
for root in "$PRIMARY" "$SECONDARY"; do
    line="$(awk -v target="$root" '$5 == target {print; exit}' /proc/self/mountinfo)"
    case "$line" in
        *" - vfat "*|*" - msdos "*|*" - fat "*) ;;
        *) fail "$root is not an exact FAT bind mount: $line" ;;
    esac
done

rm -rf "$TMP_ROOT" "$RUNTIME" "$EVIDENCE"
mkdir -p "$TMP_ROOT" "$RUNTIME" "$EVIDENCE"

clear_card_roots() {
    find "$PRIMARY" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} \;
    find "$SECONDARY" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} \;
}

write_release() {
    cat >"$STATE/release.json" <<JSON
{"schema":1,"product":"leaf","platform":"mlp1","version":"$1","release_id":"$1"}
JSON
}

write_catalog() {
    printf '%s%s/\n' "$BASE_URL" "$1" >"$STATE/store/dev-catalog-url"
}

reset_fixture() {
    clear_card_roots
    mkdir -p "$PRIMARY_APPS/mlp1" "$PRIMARY_APPS/shared" \
        "$SECONDARY_APPS/mlp1" "$SECONDARY_APPS/shared" \
        "$PRIMARY_USERDATA" "$SECONDARY_USERDATA" "$STATE/store" \
        "$PLATFORM_ROOT" "$LOGS"
    printf '{"managed_apps":[]}\n' >"$PLATFORM_ROOT/manifest.json"
    write_release 1.0.0
    write_catalog v1
}

clean_env() {
    local mode="$1"
    shift
    if [ "$mode" = exec ]; then
        exec env -i \
            PATH=/sbin:/usr/sbin:/bin:/usr/bin \
            LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
            PLATFORM=mlp1 \
            SDCARD_PATH="$PRIMARY" \
            SDCARD_PATHS="$PRIMARY:$SECONDARY" \
            JAWAKA_SDCARD_ROOT="$PRIMARY" \
            APPS_PATH="$PRIMARY_APPS" \
            APPS_PATHS="$PRIMARY_APPS:$SECONDARY_APPS" \
            USERDATA_PATH="$PRIMARY_USERDATA" \
            USERDATA_PATHS="$PRIMARY_USERDATA:$SECONDARY_USERDATA" \
            LOGS_PATH="$LOGS" \
            UMRK_RUNTIME_PATH="$RUNTIME" \
            JAWAKA_RUNTIME_DIR="$RUNTIME" \
            UMRK_DAEMON_SOCKET="$SOCKET" \
            JAWAKA_SOCKET_PATH="$SOCKET" \
            UMRK_INTERNAL_DATA_PATH="$STATE" \
            UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
            JAWAKA_OSD=0 \
            "$@"
    fi
    env -i \
        PATH=/sbin:/usr/sbin:/bin:/usr/bin \
        LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
        PLATFORM=mlp1 \
        SDCARD_PATH="$PRIMARY" \
        SDCARD_PATHS="$PRIMARY:$SECONDARY" \
        JAWAKA_SDCARD_ROOT="$PRIMARY" \
        APPS_PATH="$PRIMARY_APPS" \
        APPS_PATHS="$PRIMARY_APPS:$SECONDARY_APPS" \
        USERDATA_PATH="$PRIMARY_USERDATA" \
        USERDATA_PATHS="$PRIMARY_USERDATA:$SECONDARY_USERDATA" \
        LOGS_PATH="$LOGS" \
        UMRK_RUNTIME_PATH="$RUNTIME" \
        JAWAKA_RUNTIME_DIR="$RUNTIME" \
        UMRK_DAEMON_SOCKET="$SOCKET" \
        JAWAKA_SOCKET_PATH="$SOCKET" \
        UMRK_INTERNAL_DATA_PATH="$STATE" \
        UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
        JAWAKA_OSD=0 \
        "$@"
}

common_env() {
    clean_env run "$@"
}

common_env_exec() {
    clean_env exec "$@"
}

start_daemon() {
    DAEMON_RUN=$((DAEMON_RUN + 1))
    rm -f "$SOCKET"
    common_env_exec "$DAEMON" --daemon-only \
        >"$TMP_ROOT/jawakad-$DAEMON_RUN.log" 2>&1 &
    DAEMON_PID=$!
    for _ in $(seq 1 500); do
        [ -S "$SOCKET" ] && return 0
        kill -0 "$DAEMON_PID" >/dev/null 2>&1 || {
            cat "$TMP_ROOT/jawakad-$DAEMON_RUN.log" >&2
            fail "daemon exited before opening socket"
        }
        sleep 0.02
    done
    fail "daemon socket missing"
}

stop_daemon() {
    if [ -n "$DAEMON_PID" ]; then
        kill "$DAEMON_PID" >/dev/null 2>&1 || true
        wait "$DAEMON_PID" >/dev/null 2>&1 || true
        DAEMON_PID=""
    fi
    rm -f "$SOCKET"
}

run_pakrat() {
    common_env \
        JW_PAKRAT_FAULT_AT="${JW_PAKRAT_FAULT_AT:-}" \
        JW_PAKRAT_PAUSE_AT="${JW_PAKRAT_PAUSE_AT:-}" \
        "$PAKRAT" --platform mlp1 --sdcard-root "$PRIMARY" \
        --state-dir "$STATE" --db "$STATE/library.db" \
        --platform-root "$PLATFORM_ROOT" \
        --socket "$SOCKET" "$@"
}

run_pakrat_exec() {
    common_env_exec \
        JW_PAKRAT_FAULT_AT="${JW_PAKRAT_FAULT_AT:-}" \
        JW_PAKRAT_PAUSE_AT="${JW_PAKRAT_PAUSE_AT:-}" \
        "$PAKRAT" --platform mlp1 --sdcard-root "$PRIMARY" \
        --state-dir "$STATE" --db "$STATE/library.db" \
        --platform-root "$PLATFORM_ROOT" \
        --socket "$SOCKET" "$@"
}

request() {
    common_env "$CTL" --socket "$SOCKET" request "$1"
}

ctl1() {
    local op="$1"
    request "{\"v\":1,\"op\":\"$op\",\"id\":\"$op\",\"service_id\":\"$STORE_ID\"}"
}

status_json() {
    ctl1 status
}

status_state() {
    status_json 2>/dev/null |
        sed -n 's/.*"effective_state":"\([^"]*\)".*/\1/p'
}

status_pgid() {
    status_json 2>/dev/null |
        sed -n 's/.*"ownership_identity":{"pgid":\([0-9][0-9]*\).*/\1/p'
}

wait_state() {
    local wanted="$1"
    for _ in $(seq 1 600); do
        [ "$(status_state || true)" = "$wanted" ] && return 0
        sleep 0.02
    done
    return 1
}

wait_mutation_unlocked() {
    local lock_path
    for _ in $(seq 1 600); do
        for lock_path in "$RUNTIME"/pakrat-mutations/*; do
            [ -e "$lock_path" ] || return 0
            flock -n "$lock_path" true >/dev/null 2>&1 || break
        done
        [ -e "${lock_path:-}" ] &&
            flock -n "$lock_path" true >/dev/null 2>&1 && return 0
        sleep 0.02
    done
    {
        echo "mutation lock did not release"
        ls -l "$RUNTIME"/pakrat-mutations 2>&1 || true
        for lock_path in "$RUNTIME"/pakrat-mutations/*; do
            [ -e "$lock_path" ] || continue
            fuser "$lock_path" 2>&1 || true
        done
        ps 2>&1 || true
    } >"$TMP_ROOT/mutation-lock-timeout.out"
    return 1
}

target_version() {
    sed -n 's/.*"pak_version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$TARGET/pak.json" 2>/dev/null | head -1
}

db_value() {
    sqlite3 -cmd '.timeout 5000' "$STATE/library.db" "$1" 2>/dev/null
}

db_version() {
    db_value "SELECT version FROM pakrat_installs WHERE store_id='$STORE_ID';"
}

db_token() {
    db_value "SELECT COALESCE(commit_token,'') FROM pakrat_installs WHERE store_id='$STORE_ID';"
}

marker_token() {
    sed -n 's/.*"token"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$TARGET/.pakrat-commit" 2>/dev/null | head -1
}

pending_count() {
    db_value "SELECT COUNT(*) FROM pakrat_pending_uninstalls WHERE store_id='$STORE_ID';" || echo 0
}

assert_no_transition_siblings() {
    [ ! -e "$PRIMARY_APPS/mlp1/.pakrat-stage-$STORE_ID" ] || return 1
    [ ! -e "$PRIMARY_APPS/mlp1/.pakrat-rollback-$STORE_ID" ] || return 1
    [ ! -e "$PRIMARY_APPS/mlp1/.pakrat-origin-$STORE_ID" ] || return 1
}

assert_complete_real() {
    [ -d "$TARGET" ] || fail "real target missing"
    [ "$(target_version)" = 2.0.0 ] || fail "target version is $(target_version)"
    [ "$(db_version)" = 2.0.0 ] || fail "record version is $(db_version)"
    [ -x "$TARGET/bin/run.sh" ] || fail "service entry point missing"
    [ -f "$TARGET/launch.sh" ] || fail "launch entry point missing"
    [ -n "$(db_token)" ] && [ "$(db_token)" = "$(marker_token)" ] ||
        fail "tree/record commit token mismatch"
    assert_no_transition_siblings || fail "transition sibling remained"
}

install_real_retry() {
    write_release 1.0.0
    write_catalog v2
    wait_mutation_unlocked || fail "prior mutation did not release before install"
    for _ in $(seq 1 120); do
        if run_pakrat install "$STORE_ID" >"$TMP_ROOT/install-retry.log" 2>&1; then
            return 0
        fi
        sleep 0.05
    done
    cat "$TMP_ROOT/install-retry.log" >&2
    fail "real install did not become available after recovery"
}

ensure_real_running() {
    if [ ! -d "$TARGET" ] || [ "$(target_version)" != 2.0.0 ]; then
        install_real_retry
    fi
    ctl1 enable >/dev/null || fail "enable failed"
    ctl1 run >/dev/null || fail "run failed"
    wait_state running || fail "service did not reach running"
}

expect_fault() {
    local point="$1"
    shift
    local out="$TMP_ROOT/fault-$point.out" rc
    set +e
    JW_PAKRAT_FAULT_AT="$point" run_pakrat "$@" >"$out" 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 42 ] || { cat "$out" >&2; fail "$point exited $rc"; }
}

reset_fixture
start_daemon

echo "phase 1: running update quiesces before the first byte"
run_pakrat install-target "$STORE_ID" 1.0.0 >"$TMP_ROOT/install-v1.out"
ctl1 enable >/dev/null
ctl1 run >/dev/null
wait_state running || fail "v1 did not start"
write_catalog v2
JW_PAKRAT_PAUSE_AT=before-stage run_pakrat_exec install "$STORE_ID" \
    >"$TMP_ROOT/update-pause.out" 2>&1 &
PAUSED_PID=$!
paused_child=""
for _ in $(seq 1 500); do
    kill -0 "$PAUSED_PID" >/dev/null 2>&1 || {
        cat "$TMP_ROOT/update-pause.out" >&2
        fail "update exited before pause"
    }
    paused_child="$(sed -n 's/.*pid=\([0-9][0-9]*\).*/\1/p' \
        "$TMP_ROOT/update-pause.out" | tail -1)"
    [ -n "$paused_child" ] && break
    sleep 0.01
done
[ -n "$paused_child" ] || fail "update did not pause"
[ "$(awk '/^State:/ {print $2}' "/proc/$paused_child/status")" = T ] ||
    fail "Pak Rat child was not stopped at the pause boundary"
[ "$(target_version)" = 1.0.0 ] || fail "live package changed before stage"
paused_status="$(status_json 2>&1 || true)"
printf '%s\n' "$paused_status" >"$TMP_ROOT/update-pause-status.out"
[ "$(printf '%s\n' "$paused_status" |
    sed -n 's/.*"effective_state":"\([^"]*\)".*/\1/p')" = stopped ] ||
    fail "service was not proven stopped: $paused_status"
kill -CONT "$paused_child"
wait "$PAUSED_PID"
PAUSED_PID=""
wait_state running || fail "v2 was not restored running"
assert_complete_real

echo "phase 2: P1 promote boundaries under TXN-1"
install_crash_points="before-stage after-stage after-origin-marker before-promote after-promote before-syncfs after-syncfs before-record after-record before-cleanup"
for point in $install_crash_points; do
    expect_fault "$point" repair "$STORE_ID" 2.0.0
    wait_state running || fail "$point did not restore service"
    assert_complete_real
done
for point in during-syncfs during-record; do
    set +e
    JW_PAKRAT_FAULT_AT="$point" run_pakrat repair "$STORE_ID" 2.0.0 \
        >"$TMP_ROOT/fault-$point.out" 2>&1
    rc=$?
    set -e
    [ "$rc" -ne 0 ] || fail "$point unexpectedly succeeded"
    wait_state running || fail "$point did not restore service"
    assert_complete_real
done

echo "phase 3: durable uninstall boundaries recover forward"
uninstall_cases="
uninstall-before-intent:preserve
uninstall-during-intent:preserve
uninstall-after-intent:remove
uninstall-before-revoke:remove
uninstall-after-revoke:remove
uninstall-before-package-remove:remove
uninstall-after-package-remove:remove
uninstall-before-syncfs:remove
uninstall-after-syncfs:remove
uninstall-before-final-db:remove
uninstall-during-final-db:remove
uninstall-after-final-db:remove"
for item in $uninstall_cases; do
    point="${item%%:*}"
    outcome="${item#*:}"
    ensure_real_running
    mkdir -p "$PRIMARY_USERDATA/Syncthing/leaf" \
        "$PRIMARY_USERDATA/Syncthing/history"
    printf secret >"$PRIMARY_USERDATA/Syncthing/leaf/trusted.json"
    printf history >"$PRIMARY_USERDATA/Syncthing/history/index.db"
    rm -f "$PRIMARY_USERDATA/Syncthing/controller.sock"
    wait_mutation_unlocked || fail "$point inherited an active mutation lock"
    expect_fault "$point" uninstall "$STORE_ID"
    if [ "$outcome" = preserve ]; then
        wait_state running || fail "$point did not restore the authorized service"
        [ -d "$TARGET" ] || fail "$point removed package without intent"
        [ -e "$PRIMARY_USERDATA/Syncthing/leaf/trusted.json" ] ||
            fail "$point revoked trust without intent"
        [ "$(pending_count)" = 0 ] || fail "$point left pending intent"
    else
        for _ in $(seq 1 800); do
            [ ! -e "$TARGET" ] && [ "$(pending_count)" = 0 ] && break
            sleep 0.02
        done
        [ ! -e "$TARGET" ] || fail "$point did not remove package"
        [ ! -e "$PRIMARY_USERDATA/Syncthing/leaf/trusted.json" ] ||
            fail "$point restored revoked trust"
        [ -e "$PRIMARY_USERDATA/Syncthing/history/index.db" ] ||
            fail "$point removed retained history"
        [ "$(pending_count)" = 0 ] || fail "$point left pending intent"
        wait_mutation_unlocked || fail "$point did not release mutation ownership"
        [ "$(db_value "SELECT COUNT(*) FROM pakrat_installs WHERE store_id='$STORE_ID';")" = 0 ] ||
            fail "$point left install record"
    fi
done

echo "phase 4: floor transitions and Secondary ownership"
install_real_retry
[ "$(status_state)" = disabled ] || fail "reinstall was not disabled"
write_release 0.9.0
run_pakrat install "$STORE_ID" >"$TMP_ROOT/real-to-floor.out"
[ "$(target_version)" = 0.0.1 ] || fail "real-to-floor did not select floor"
[ "$(sqlite3 -cmd '.timeout 5000' "$CONTROL_DB" "SELECT COUNT(*) FROM service_control_state WHERE service_id='$STORE_ID';")" = 0 ] ||
    fail "real-to-floor retained service control"
write_release 1.0.0
run_pakrat install "$STORE_ID" >"$TMP_ROOT/floor-to-real.out"
assert_complete_real
[ "$(status_state)" = disabled ] || fail "floor-to-real did not start disabled"
run_pakrat uninstall "$STORE_ID" >"$TMP_ROOT/uninstall-before-secondary.out"

stop_daemon
rm -rf "$TMP_ROOT/floor-unzip"
mkdir -p "$TMP_ROOT/floor-unzip" "$SECONDARY_APPS/mlp1"
wget -q -O "$TMP_ROOT/floor.zip" \
    "${BASE_URL}artifacts/0.0.1/TxnService.mlp1.pak.zip"
unzip -q "$TMP_ROOT/floor.zip" -d "$TMP_ROOT/floor-unzip"
mv "$TMP_ROOT/floor-unzip/TxnService.pak" "$SECONDARY_TARGET"
sqlite3 "$STATE/library.db" <<SQL
INSERT INTO pakrat_installs(store_id,version,platform,source_id,install_path,artifact_sha256,installed_at,commit_token)
VALUES('$STORE_ID','0.0.1','mlp1','secondary_sd','$INSTALL_REL','$FLOOR_SHA',strftime('%Y-%m-%dT%H:%M:%SZ','now'),NULL);
INSERT OR REPLACE INTO pakrat_service_metadata(store_id,install_path,package_id,display_name,has_service,service_id,state_root,revoke_json,retained_json,validated_at)
VALUES('$STORE_ID','$INSTALL_REL','$STORE_ID','TXN Service Floor',0,'','','[]','[]',strftime('%Y-%m-%dT%H:%M:%SZ','now'));
SQL
start_daemon
set +e
run_pakrat install "$STORE_ID" >"$TMP_ROOT/secondary-refusal.out" 2>&1
rc=$?
set -e
[ "$rc" -ne 0 ] || fail "Secondary service update unexpectedly succeeded"
grep -F "installed on Secondary. Uninstall it there first, then install the service pak on Primary." \
    "$TMP_ROOT/secondary-refusal.out" >/dev/null || fail "Secondary refusal reason missing"
[ -d "$SECONDARY_TARGET" ] || fail "Secondary floor was changed"
[ ! -e "$TARGET" ] || fail "Primary duplicate was created"
run_pakrat uninstall "$STORE_ID" >"$TMP_ROOT/secondary-uninstall.out"
[ ! -e "$SECONDARY_TARGET" ] || fail "Secondary floor uninstall did not commit"
run_pakrat install "$STORE_ID" >"$TMP_ROOT/primary-after-secondary.out"
assert_complete_real
[ ! -e "$SECONDARY_TARGET" ] || fail "duplicate package id exists"
[ "$(db_value "SELECT source_id FROM pakrat_installs WHERE store_id='$STORE_ID';")" = primary ] ||
    fail "real package is not Primary-owned"
[ "$(status_state)" = disabled ] || fail "Primary real install was not disabled"

sync
cp "$TMP_ROOT"/*.out "$EVIDENCE/" 2>/dev/null || true
cp "$TMP_ROOT"/jawakad-*.log "$EVIDENCE/" 2>/dev/null || true
awk -v a="$PRIMARY" -v b="$SECONDARY" '$5 == a || $5 == b {print}' \
    /proc/self/mountinfo >"$EVIDENCE/bind-mountinfo.txt"
sqlite3 "$STATE/library.db" '.dump pakrat_installs' \
    >"$EVIDENCE/final-install-record.sql"
sqlite3 "$CONTROL_DB" '.dump service_control_state' \
    >"$EVIDENCE/final-service-control.sql"
sha256sum "$TARGET/pak.json" "$TARGET/launch.sh" "$TARGET/bin/run.sh" \
    >"$EVIDENCE/final-package-sha256.txt"
cat >"$EVIDENCE/summary.txt" <<'EOF'
B4a real-FAT result: PASS
P1 service-mutation crash points: 10 crash + 2 in-process failures
Uninstall intent/completion crash points: 12
Transitions: running update, real-to-floor, floor-to-real, Secondary-floor refusal/uninstall/Primary install
EOF
echo "PASS mlp1-service-mutation-device"
