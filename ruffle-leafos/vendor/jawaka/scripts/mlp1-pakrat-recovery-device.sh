#!/usr/bin/env bash
# Runs the Pak Rat promote/recovery matrix on an already bind-mounted directory
# backed by a real MLP1 FAT card. The host runner supplies the binaries and HTTP
# fixture feed; this script owns only its dedicated fixture directory.
set -euo pipefail

SD_ROOT="${P1_SD_ROOT:-/tmp/jw-p1-fat}"
UNDERLAY="${P1_UNDERLAY:?P1_UNDERLAY is required}"
BASE_URL="${P1_BASE_URL:?P1_BASE_URL is required}"
BIN="${P1_SMOKE_BIN:?P1_SMOKE_BIN is required}"
DAEMON_BIN="${P1_DAEMON_BIN:?P1_DAEMON_BIN is required}"
RUNTIME_LIB_DIR="${P1_RUNTIME_LIB_DIR:-}"
TMP_ROOT="${P1_TMP_ROOT:-/tmp/jw-p1-device}"
MODE="${P1_MODE:-matrix}"
STATE_DIR="$SD_ROOT/.umrk/mlp1"
PLATFORM_ROOT="$SD_ROOT/.system/leaf/platforms/mlp1"
DB_PATH="$STATE_DIR/library.db"
STORE_ID="org.umrk.recovery-smoke"
INSTALL_PATH="$SD_ROOT/Apps/mlp1/Recovery.pak"
APPS_DIR="$SD_ROOT/Apps/mlp1"
OLD_VERSION="0.1.0"
NEW_VERSION="0.2.0"
SCENARIO=0
DAEMON_PID=""

export LD_LIBRARY_PATH="${RUNTIME_LIB_DIR:+$RUNTIME_LIB_DIR:}${LD_LIBRARY_PATH:-}"

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

is_exact_mount() {
    awk -v target="$1" '$5 == target { found = 1; exit }
        END { exit found ? 0 : 1 }' /proc/self/mountinfo
}

ensure_bound() {
    if ! is_exact_mount "$SD_ROOT"; then
        mkdir -p "$SD_ROOT"
        mount --bind "$UNDERLAY" "$SD_ROOT"
    fi
}

cleanup() {
    if [ -n "$DAEMON_PID" ]; then
        kill "$DAEMON_PID" >/dev/null 2>&1 || true
        wait "$DAEMON_PID" >/dev/null 2>&1 || true
    fi
    ensure_bound >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM

case "$SD_ROOT" in
    /tmp/jw-p1-fat|/tmp/jw-p1-fat.*) ;;
    *) fail "refusing unsafe fixture mount root: $SD_ROOT" ;;
esac
[ -x "$BIN" ] || fail "smoke binary missing: $BIN"
[ -x "$DAEMON_BIN" ] || fail "daemon binary missing: $DAEMON_BIN"
[ -d "$UNDERLAY" ] || fail "fixture underlay missing: $UNDERLAY"
ensure_bound

mount_line="$(awk -v target="$SD_ROOT" '$5 == target { print; exit }' \
    /proc/self/mountinfo)"
[ -n "$mount_line" ] || fail "fixture root is not an exact mountpoint"
case "$mount_line" in
    *" - vfat "*|*" - msdos "*|*" - fat "*) ;;
    *) fail "fixture mount is not FAT: $mount_line" ;;
esac
echo "physical FAT mount: $mount_line"

rm -rf "$TMP_ROOT"
mkdir -p "$TMP_ROOT"

clear_sd() {
    ensure_bound
    find "$SD_ROOT" -mindepth 1 -maxdepth 1 -exec rm -rf -- {} \;
}

write_catalog() {
    local mode="$1"
    printf '%s%s/\n' "$BASE_URL" "$mode" >"$STATE_DIR/store/dev-catalog-url"
}

reset_sd() {
    clear_sd
    mkdir -p "$STATE_DIR/store" "$PLATFORM_ROOT" "$APPS_DIR" \
        "$SD_ROOT/Apps/shared"
    printf '{ "managed_apps": [] }\n' >"$PLATFORM_ROOT/manifest.json"
    cat >"$STATE_DIR/release.json" <<'JSON'
{ "schema": 1, "product": "leaf", "platform": "mlp1", "version": "v0.7.0", "release_id": "v0.7.0" }
JSON
    write_catalog old
}

run_smoke() {
    env -i \
    PATH=/sbin:/usr/sbin:/bin:/usr/bin \
    LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
    JW_PAKRAT_FAULT_AT="${JW_PAKRAT_FAULT_AT:-}" \
    JW_PAKRAT_PAUSE_AT="${JW_PAKRAT_PAUSE_AT:-}" \
    PLATFORM=mlp1 \
    SDCARD_PATH="$SD_ROOT" \
    SDCARD_PATHS="$SD_ROOT" \
    JAWAKA_SDCARD_ROOT="$SD_ROOT" \
    UMRK_INTERNAL_DATA_PATH="$STATE_DIR" \
    UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$BIN" --platform mlp1 --sdcard-root "$SD_ROOT" \
        --state-dir "$STATE_DIR" --db "$DB_PATH" \
        --platform-root "$PLATFORM_ROOT" --socket /tmp/jw-p1-unused.sock \
        "$@"
}

target_version() {
    sed -n 's/.*"pak_version"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$INSTALL_PATH/pak.json" 2>/dev/null | head -1
}

db_version() {
    sqlite3 "$DB_PATH" \
        "SELECT version FROM pakrat_installs WHERE store_id = '$STORE_ID';" \
        2>/dev/null
}

db_token() {
    sqlite3 "$DB_PATH" \
        "SELECT COALESCE(commit_token, '') FROM pakrat_installs WHERE store_id = '$STORE_ID';" \
        2>/dev/null
}

db_app_version() {
    sqlite3 "$DB_PATH" \
        "SELECT pak_version FROM apps WHERE pak_dir = 'Apps/mlp1/Recovery.pak';" \
        2>/dev/null
}

db_row_count() {
    sqlite3 "$DB_PATH" \
        "SELECT COUNT(*) FROM pakrat_installs WHERE store_id = '$STORE_ID';" \
        2>/dev/null || echo 0
}

marker_token() {
    sed -n 's/.*"token"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' \
        "$INSTALL_PATH/.pakrat-commit" 2>/dev/null | head -1
}

sibling_count() {
    find "$SD_ROOT/Apps" -maxdepth 2 -name '.pakrat-*' 2>/dev/null |
        wc -l | tr -d ' '
}

expect_state() {
    local want_target="$1" want_db="$2" label="$3"
    local check_discovery="${4:-yes}"
    if [ "$want_target" = absent ]; then
        [ ! -e "$INSTALL_PATH" ] || fail "$label: target present"
    else
        [ -d "$INSTALL_PATH" ] || fail "$label: target missing"
        [ "$(target_version)" = "$want_target" ] ||
            fail "$label: target version $(target_version), expected $want_target"
        [ -f "$INSTALL_PATH/launch.sh" ] || fail "$label: launch.sh missing"
    fi
    if [ "$want_db" = absent ]; then
        [ "$(db_row_count)" = 0 ] || fail "$label: unexpected install row"
    else
        [ "$(db_version)" = "$want_db" ] ||
            fail "$label: record version $(db_version), expected $want_db"
        if [ "$want_target" != absent ]; then
            local record_token tree_token
            record_token="$(db_token)"
            tree_token="$(marker_token)"
            printf '%s\n' "$record_token" | grep -Eq '^[0-9a-f]{32}$' ||
                fail "$label: invalid record token"
            [ "$tree_token" = "$record_token" ] ||
                fail "$label: tree/record token mismatch"
            if [ "$check_discovery" = yes ]; then
                [ "$(db_app_version)" = "$want_target" ] ||
                    fail "$label: discovery version mismatch"
            fi
        fi
    fi
    [ "$(sibling_count)" = 0 ] || fail "$label: transition sibling remained"
}

expect_crash() {
    local point="$1" out="$TMP_ROOT/crash-$1.out" rc
    set +e
    JW_PAKRAT_FAULT_AT="$point" run_smoke install "$STORE_ID" >"$out" 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 42 ] || { cat "$out" >&2; fail "$point: crash rc=$rc"; }
    grep -F "pakrat fault injection: crash at $point" "$out" >/dev/null ||
        fail "$point: crash marker missing"
}

expect_failure() {
    local point="$1" out="$TMP_ROOT/fail-$1.out" rc
    set +e
    JW_PAKRAT_FAULT_AT="$point" run_smoke install "$STORE_ID" >"$out" 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 1 ] || { cat "$out" >&2; fail "$point: failure rc=$rc"; }
}

install_old() {
    write_catalog old
    run_smoke install "$STORE_ID" >"$TMP_ROOT/install-old-$SCENARIO.out" 2>&1 || {
        cat "$TMP_ROOT/install-old-$SCENARIO.out" >&2
        fail "seed install failed"
    }
    expect_state "$OLD_VERSION" "$OLD_VERSION" "seed-$SCENARIO"
}

begin_scenario() {
    SCENARIO=$((SCENARIO + 1))
    echo "scenario $SCENARIO: $*"
    reset_sd
}

run_daemon_recovery() {
    local runtime="$TMP_ROOT/runtime-$SCENARIO"
    local socket="$runtime/jawakad.sock"
    local log="$TMP_ROOT/daemon-$SCENARIO.log"
    mkdir -p "$runtime"
    env -i \
    PATH=/sbin:/usr/sbin:/bin:/usr/bin \
    LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
    PLATFORM=mlp1 \
    SDCARD_PATH="$SD_ROOT" \
    SDCARD_PATHS="$SD_ROOT" \
    JAWAKA_SDCARD_ROOT="$SD_ROOT" \
    UMRK_RUNTIME_PATH="$runtime" \
    UMRK_DAEMON_SOCKET="$socket" \
    UMRK_INTERNAL_DATA_PATH="$STATE_DIR" \
    UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
        "$DAEMON_BIN" --daemon-only >"$log" 2>&1 &
    DAEMON_PID=$!
    for _ in $(seq 1 500); do
        [ -S "$socket" ] && break
        kill -0 "$DAEMON_PID" 2>/dev/null || {
            cat "$log" >&2
            fail "daemon exited before opening socket"
        }
        sleep 0.02
    done
    [ -S "$socket" ] || { cat "$log" >&2; fail "daemon socket missing"; }
    kill "$DAEMON_PID" >/dev/null 2>&1 || true
    wait "$DAEMON_PID" >/dev/null 2>&1 || true
    DAEMON_PID=""
}

if [ "$MODE" = removal-prepare ]; then
    begin_scenario "large package paused after promote for physical removal"
    install_old
    write_catalog new
    old_token="$(db_token)"
    env -i \
        PATH=/sbin:/usr/sbin:/bin:/usr/bin \
        LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
        JW_PAKRAT_PAUSE_AT=after-promote \
        PLATFORM=mlp1 \
        SDCARD_PATH="$SD_ROOT" \
        SDCARD_PATHS="$SD_ROOT" \
        JAWAKA_SDCARD_ROOT="$SD_ROOT" \
        UMRK_INTERNAL_DATA_PATH="$STATE_DIR" \
        UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
        "$BIN" --platform mlp1 --sdcard-root "$SD_ROOT" \
            --state-dir "$STATE_DIR" --db "$DB_PATH" \
            --platform-root "$PLATFORM_ROOT" --socket /tmp/jw-p1-unused.sock \
            install "$STORE_ID" \
        >"$TMP_ROOT/removal-pause.out" 2>&1 &
    paused_pid=$!
    paused_state=""
    for _ in $(seq 1 3000); do
        kill -0 "$paused_pid" 2>/dev/null || {
            cat "$TMP_ROOT/removal-pause.out" >&2
            fail "removal prepare exited before pause"
        }
        paused_state="$(awk '/^State:/ {print $2}' "/proc/$paused_pid/status")"
        [ "$paused_state" = T ] && break
        sleep 0.01
    done
    [ "$paused_state" = T ] || fail "removal prepare did not stop"
    [ "$(target_version)" = "$NEW_VERSION" ] || fail "removal: new tree not live"
    [ "$(db_version)" = "$OLD_VERSION" ] || fail "removal: record committed early"
    [ "$(marker_token)" != "$old_token" ] || fail "removal: token reused"
    [ -d "$APPS_DIR/.pakrat-rollback-$STORE_ID" ] ||
        fail "removal: rollback tree missing"
    printf '%s\n' "$paused_pid" >"$TMP_ROOT/removal-pid"
    printf '%s\n' "$old_token" >"$TMP_ROOT/removal-old-token"
    printf '%s\n' "$(marker_token)" >"$TMP_ROOT/removal-new-token"
    disown "$paused_pid"
    echo "P1_REMOVAL_READY pid=$paused_pid target=$INSTALL_PATH rollback=$APPS_DIR/.pakrat-rollback-$STORE_ID"
    echo "P1_REMOVAL_STATE target=$NEW_VERSION record=$OLD_VERSION syncfs=not-started"
    exit 0
fi
if [ "$MODE" = removal-recover ]; then
    echo "scenario 1: recover physically interrupted pre-syncfs promote"
    run_smoke recover | tee "$TMP_ROOT/removal-recover.out"
    expect_state "$OLD_VERSION" "$OLD_VERSION" removal-recover
    printf '%s\n' "$(db_token)" >"$TMP_ROOT/removal-recovered-token"
    printf '%s\n' "$(stat -c %s "$INSTALL_PATH/bulk.bin")" \
        >"$TMP_ROOT/removal-recovered-bulk-bytes"
    sync
    echo "P1 physical removal recovery passed: target=$OLD_VERSION siblings=0"
    exit 0
fi
[ "$MODE" = matrix ] || fail "unsupported P1_MODE: $MODE"

for point in before-stage after-stage after-origin-marker before-promote \
    after-promote before-syncfs after-syncfs before-record; do
    begin_scenario "crash $point rolls back"
    install_old
    write_catalog new
    expect_crash "$point"
    run_smoke recover
    expect_state "$OLD_VERSION" "$OLD_VERSION" "$point"
done

for point in during-syncfs during-record; do
    begin_scenario "$point failure rolls back in process"
    install_old
    write_catalog new
    expect_failure "$point"
    expect_state "$OLD_VERSION" "$OLD_VERSION" "$point"
    run_smoke recover
    expect_state "$OLD_VERSION" "$OLD_VERSION" "$point-recover"
done

for point in after-record before-cleanup; do
    begin_scenario "crash $point keeps committed tree"
    install_old
    write_catalog new
    expect_crash "$point"
    run_smoke recover
    expect_state "$NEW_VERSION" "$NEW_VERSION" "$point"
done

begin_scenario "daemon startup recovers before scan"
install_old
write_catalog new
expect_crash after-promote
run_daemon_recovery
expect_state "$OLD_VERSION" "$OLD_VERSION" daemon-startup no

begin_scenario "deterministic pause exposes uncommitted promote"
install_old
write_catalog new
old_token="$(db_token)"
env -i \
    PATH=/sbin:/usr/sbin:/bin:/usr/bin \
    LD_LIBRARY_PATH="$LD_LIBRARY_PATH" \
    JW_PAKRAT_PAUSE_AT=after-promote \
    PLATFORM=mlp1 \
    SDCARD_PATH="$SD_ROOT" \
    SDCARD_PATHS="$SD_ROOT" \
    JAWAKA_SDCARD_ROOT="$SD_ROOT" \
    UMRK_INTERNAL_DATA_PATH="$STATE_DIR" \
    UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
    "$BIN" --platform mlp1 --sdcard-root "$SD_ROOT" \
        --state-dir "$STATE_DIR" --db "$DB_PATH" \
        --platform-root "$PLATFORM_ROOT" --socket /tmp/jw-p1-unused.sock \
        install "$STORE_ID" \
    >"$TMP_ROOT/pause.out" 2>&1 &
paused_pid=$!
paused_state=""
for _ in $(seq 1 500); do
    kill -0 "$paused_pid" 2>/dev/null || {
        cat "$TMP_ROOT/pause.out" >&2
        fail "paused install exited"
    }
    paused_state="$(awk '/^State:/ {print $2}' "/proc/$paused_pid/status")"
    [ "$paused_state" = T ] && break
    sleep 0.01
done
[ "$paused_state" = T ] || fail "install did not enter SIGSTOP state"
[ "$(target_version)" = "$NEW_VERSION" ] || fail "pause: new tree not live"
[ "$(db_version)" = "$OLD_VERSION" ] || fail "pause: record committed early"
[ "$(marker_token)" != "$old_token" ] || fail "pause: token reused"
kill -9 "$paused_pid" 2>/dev/null || true
wait "$paused_pid" 2>/dev/null || true
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" pause-after-promote

begin_scenario "truncated manifest restores known-good tree"
install_old
write_catalog new
expect_crash after-promote
printf '{ "name": "Recov' >"$INSTALL_PATH/pak.json"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" truncated-manifest

begin_scenario "missing declared entry point restores"
install_old
write_catalog new
set +e
JW_PAKRAT_FAULT_AT=after-promote run_smoke repair "$STORE_ID" "$OLD_VERSION" \
    >"$TMP_ROOT/missing-entry.out" 2>&1
rc=$?
set -e
[ "$rc" -eq 42 ] || fail "missing-entry: repair crash rc=$rc"
rm -f "$INSTALL_PATH/launch.sh"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" missing-entry

for point in after-promote after-record; do
    begin_scenario "same-version repair $point"
    install_old
    old_token="$(db_token)"
    write_catalog new
    set +e
    JW_PAKRAT_FAULT_AT="$point" run_smoke repair "$STORE_ID" "$OLD_VERSION" \
        >"$TMP_ROOT/same-version-$point.out" 2>&1
    rc=$?
    set -e
    [ "$rc" -eq 42 ] || fail "same-version $point: crash rc=$rc"
    run_smoke recover
    expect_state "$OLD_VERSION" "$OLD_VERSION" "same-version-$point"
    if [ "$point" = after-promote ]; then
        [ "$(db_token)" = "$old_token" ] || fail "uncommitted repair changed record"
    else
        [ "$(db_token)" != "$old_token" ] || fail "committed repair kept old token"
    fi
done

begin_scenario "interrupted first install after promote is removed"
write_catalog new
expect_crash after-promote
run_smoke recover
expect_state absent absent first-install

begin_scenario "adopted rollback without row is restored"
install_old
sqlite3 "$DB_PATH" "DELETE FROM pakrat_installs;" >/dev/null
write_catalog new
set +e
JW_PAKRAT_FAULT_AT=before-promote run_smoke adopt "$STORE_ID" \
    >"$TMP_ROOT/adopt.out" 2>&1
rc=$?
set -e
[ "$rc" -eq 42 ] || fail "adopt: crash rc=$rc"
run_smoke recover
[ "$(target_version)" = "$OLD_VERSION" ] || fail "adopt: original not restored"
[ "$(db_row_count)" = 0 ] || fail "adopt: record manufactured"
[ "$(sibling_count)" = 0 ] || fail "adopt: sibling remained"

begin_scenario "unidentifiable rollback retained while stage is swept"
mkdir -p "$APPS_DIR/.pakrat-rollback-$STORE_ID" \
    "$APPS_DIR/.pakrat-stage-$STORE_ID"
printf '{"pak_version":"%s","platform":"mlp1"}\n' "$OLD_VERSION" \
    >"$APPS_DIR/.pakrat-rollback-$STORE_ID/pak.json"
run_smoke recover
[ -d "$APPS_DIR/.pakrat-rollback-$STORE_ID" ] || fail "rollback deleted"
[ ! -e "$APPS_DIR/.pakrat-stage-$STORE_ID" ] || fail "stage not swept"
rm -rf "$APPS_DIR/.pakrat-rollback-$STORE_ID"

begin_scenario "owning FAT source absent defers without mutation"
install_old
write_catalog new
expect_crash before-promote
[ -d "$APPS_DIR/.pakrat-stage-$STORE_ID" ] || fail "absent: stage missing"
[ -d "$APPS_DIR/.pakrat-rollback-$STORE_ID" ] || fail "absent: rollback missing"
umount "$SD_ROOT"
set +e
run_smoke recover >"$TMP_ROOT/absent.out" 2>&1
rc=$?
set -e
[ "$rc" -eq 0 ] || { cat "$TMP_ROOT/absent.out" >&2; fail "absent recovery rc=$rc"; }
[ ! -e "$SD_ROOT/Apps" ] || fail "absent: rootfs path created"
[ -d "$UNDERLAY/Apps/mlp1/.pakrat-stage-$STORE_ID" ] || fail "absent: stage mutated"
[ -d "$UNDERLAY/Apps/mlp1/.pakrat-rollback-$STORE_ID" ] || fail "absent: rollback mutated"
grep -F "recovery deferred: owning Apps source is not mounted" \
    "$TMP_ROOT/absent.out" >/dev/null || fail "absent: deferred message missing"
mount --bind "$UNDERLAY" "$SD_ROOT"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" source-returned

begin_scenario "update while old package file remains open"
install_old
exec 9<"$INSTALL_PATH/payload.txt"
write_catalog new
run_smoke install "$STORE_ID" >"$TMP_ROOT/open-file-update.out" 2>&1
IFS= read -r old_open_payload <&9
exec 9<&-
[ "$old_open_payload" = old-payload ] || fail "open fd no longer reads old bytes"
[ "$(cat "$INSTALL_PATH/payload.txt")" = new-payload ] ||
    fail "open-file update did not promote new bytes"
expect_state "$NEW_VERSION" "$NEW_VERSION" open-file-update

begin_scenario "normal update"
install_old
write_catalog new
run_smoke install "$STORE_ID" >"$TMP_ROOT/normal-update.out" 2>&1
expect_state "$NEW_VERSION" "$NEW_VERSION" normal-update

sync
echo "P1 MLP1 physical FAT recovery smoke passed ($SCENARIO scenarios)"
