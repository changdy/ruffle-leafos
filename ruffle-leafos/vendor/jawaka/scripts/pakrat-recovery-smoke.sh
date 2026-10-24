#!/usr/bin/env bash
# Pak Rat promote-transaction recovery smoke.
#
# Exercises every fault-injection boundary of the install promote transaction
# (JW_PAKRAT_FAULT_AT) and asserts the exact resulting state after recovery --
# which tree survived, what the install record says, and which recovery branch
# ran -- not merely that some complete package survived.
set -euo pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)"
. "$JAWAKA_DIR/scripts/lib/smoke-daemon.sh"
BUILD_DIR="${BUILD:-build/pakrat-recovery-smoke}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-pakrat-recovery.XXXXXX")"
SHORT_RUNTIME_ROOT="$(mktemp -d "/tmp/jwpr.XXXXXX")"
SERVER_PID=""
DAEMON_PID=""

cleanup() {
    status=$?
    set +e
    smoke_daemon_stop || status=1
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP_ROOT"
    rm -rf "$SHORT_RUNTIME_ROOT"
    exit "$status"
}
trap cleanup EXIT

for tool in python3 zip curl sqlite3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$tool is required for pakrat-recovery-smoke" >&2
        exit 2
    }
done

sha256_file() {
    if command -v shasum >/dev/null 2>&1; then
        shasum -a 256 "$1" | awk '{print $1}'
    else
        sha256sum "$1" | awk '{print $1}'
    fi
}

FEED_ROOT="$TMP_ROOT/feed"
SD_ROOT="$TMP_ROOT/sd"
STATE_DIR="$SD_ROOT/.umrk/mlp1"
PLATFORM_ROOT="$SD_ROOT/.system/leaf/platforms/mlp1"
DB_PATH="$STATE_DIR/library.db"
STORE_ID="org.umrk.recovery-smoke"
INSTALL_PATH="$SD_ROOT/Apps/mlp1/Recovery.pak"
APPS_DIR="$SD_ROOT/Apps/mlp1"
ARTIFACT_NAME="Recovery.mlp1.pak.zip"
OLD_VERSION="0.1.0"
NEW_VERSION="0.2.0"

mkdir -p "$FEED_ROOT/artifacts/$OLD_VERSION" "$FEED_ROOT/artifacts/$NEW_VERSION"

make_artifact() {
    local version="$1" min_leaf="$2" payload="$3"
    local stage="$TMP_ROOT/stage-$version"
    local pak="$stage/Recovery.pak"
    local archive="$FEED_ROOT/artifacts/$version/$ARTIFACT_NAME"
    rm -rf "$stage"
    mkdir -p "$pak"
    if [ -n "$min_leaf" ]; then
        cat >"$pak/pak.json" <<JSON
{ "name": "Recovery Smoke", "platform": "mlp1", "pak_version": "$version", "min_leaf_version": "$min_leaf" }
JSON
    else
        cat >"$pak/pak.json" <<JSON
{ "name": "Recovery Smoke", "platform": "mlp1", "pak_version": "$version" }
JSON
    fi
    printf '#!/bin/sh\nexit 0\n' >"$pak/launch.sh"
    chmod +x "$pak/launch.sh"
    printf '%s\n' "$payload" >"$pak/payload.txt"
    (cd "$stage" && zip -qr "$archive" Recovery.pak)
}

make_artifact "$OLD_VERSION" "" "old-payload"
make_artifact "$NEW_VERSION" "0.7.0" "new-payload"
OLD_ARCHIVE="$FEED_ROOT/artifacts/$OLD_VERSION/$ARTIFACT_NAME"
NEW_ARCHIVE="$FEED_ROOT/artifacts/$NEW_VERSION/$ARTIFACT_NAME"
OLD_SHA="$(sha256_file "$OLD_ARCHIVE")"
NEW_SHA="$(sha256_file "$NEW_ARCHIVE")"
OLD_SIZE="$(wc -c <"$OLD_ARCHIVE" | tr -d ' ')"
NEW_SIZE="$(wc -c <"$NEW_ARCHIVE" | tr -d ' ')"

PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
BASE_URL="http://127.0.0.1:$PORT/"

# old: only 0.1.0 selectable. new: 0.2.0 selected, 0.1.0 kept in history.
write_catalog() {
    local mode="$1"
    local versions=""
    if [ "$mode" = "new" ]; then
        versions=$(cat <<JSON
          {
            "version": "$NEW_VERSION",
            "min_leaf_version": "0.7.0",
            "artifact": {
              "url": "${BASE_URL}artifacts/$NEW_VERSION/$ARTIFACT_NAME",
              "name": "$ARTIFACT_NAME",
              "archive": "zip",
              "size": $NEW_SIZE,
              "installed_size": 64,
              "sha256": "$NEW_SHA"
            }
          },
JSON
)
    fi
    cat >"$FEED_ROOT/storefront.json" <<JSON
{
  "schema": 1,
  "product": "pak-rat",
  "apps": [{
    "id": "$STORE_ID",
    "name": "Recovery Smoke",
    "summary": "Promote-transaction recovery fixture",
    "version": "$OLD_VERSION",
    "packages": [{
      "platform": "mlp1",
      "runtime": "leaf",
      "version": "$OLD_VERSION",
      "install_name": "Recovery.pak",
      "runtime_manifest_path": "pak.json",
      "artifact": {
        "url": "${BASE_URL}artifacts/$OLD_VERSION/$ARTIFACT_NAME",
        "name": "$ARTIFACT_NAME",
        "archive": "zip",
        "size": $OLD_SIZE,
        "installed_size": 64,
        "sha256": "$OLD_SHA"
      },
      "versions": [
$versions
        {
          "version": "$OLD_VERSION",
          "artifact": {
            "url": "${BASE_URL}artifacts/$OLD_VERSION/$ARTIFACT_NAME",
            "name": "$ARTIFACT_NAME",
            "archive": "zip",
            "size": $OLD_SIZE,
            "installed_size": 64,
            "sha256": "$OLD_SHA"
          }
        }
      ]
    }]
  }]
}
JSON
}

write_catalog old
python3 -m http.server "$PORT" --bind 127.0.0.1 \
    --directory "$FEED_ROOT" >"$TMP_ROOT/http.log" 2>&1 &
SERVER_PID="$!"
for _ in $(seq 1 30); do
    if curl -fsS "${BASE_URL}storefront.json" >/dev/null 2>&1; then
        break
    fi
    sleep 0.1
done
curl -fsS "${BASE_URL}storefront.json" >/dev/null

make -C "$JAWAKA_DIR" -s BUILD="$BUILD_DIR" jawaka-pakrat-smoke jawakad
BIN="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-pakrat-smoke"
DAEMON_BIN="$JAWAKA_DIR/$BUILD_DIR/bin/jawakad"

reset_sd() {
    rm -rf "$SD_ROOT"
    mkdir -p "$STATE_DIR/store" "$PLATFORM_ROOT" "$APPS_DIR" "$SD_ROOT/Apps/shared"
    printf '{ "managed_apps": [] }\n' >"$PLATFORM_ROOT/manifest.json"
    printf '%s\n' "$BASE_URL" >"$STATE_DIR/store/dev-catalog-url"
    cat >"$STATE_DIR/release.json" <<JSON
{ "schema": 1, "product": "leaf", "platform": "mlp1", "version": "v0.7.0", "release_id": "v0.7.0" }
JSON
}

run_smoke() {
    "$BIN" --platform mlp1 --sdcard-root "$SD_ROOT" "$@"
}

run_daemon_recovery() {
    local linger="${1:-0}"
    local runtime="$SHORT_RUNTIME_ROOT/$SCENARIO"
    local socket="$runtime/jawakad.sock"
    local log="$TMP_ROOT/daemon-$SCENARIO.log"
    mkdir -p "$runtime"
    smoke_daemon_start "$PWD" "$log" \
        SDCARD_PATH="$SD_ROOT" \
        SDCARD_PATHS="$SD_ROOT" \
        JAWAKA_SDCARD_ROOT="$SD_ROOT" \
        UMRK_RUNTIME_PATH="$runtime" \
        UMRK_DAEMON_SOCKET="$socket" \
        UMRK_INTERNAL_DATA_PATH="$STATE_DIR" \
        UMRK_PLATFORM_PATH="$PLATFORM_ROOT" \
        "$DAEMON_BIN" --daemon-only
    for _ in $(seq 1 500); do
        [ -S "$socket" ] && break
        kill -0 "$DAEMON_PID" 2>/dev/null || {
            cat "$log" >&2
            fail "daemon recovery: jawakad exited before opening its socket"
        }
        sleep 0.02
    done
    [ -S "$socket" ] || {
        cat "$log" >&2
        fail "daemon recovery: jawakad socket did not appear"
    }
    if [ "$linger" != "0" ]; then
        sleep "$linger"
    fi
    smoke_daemon_stop
}

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

# Expect the install to die at a fault-injection crash point (exit 42).
expect_crash() {
    local point="$1"
    local out="$TMP_ROOT/crash-$point.out"
    set +e
    JW_PAKRAT_FAULT_AT="$point" run_smoke install "$STORE_ID" >"$out" 2>&1
    local rc=$?
    set -e
    [ "$rc" -eq 42 ] || {
        cat "$out" >&2
        fail "fault $point: expected crash exit 42, got $rc"
    }
    grep -F "pakrat fault injection: crash at $point" "$out" >/dev/null ||
        fail "fault $point: crash marker missing from output"
}

# Expect the install to fail (not crash) through the in-process error path.
expect_install_failure() {
    local point="$1" needle="$2"
    local out="$TMP_ROOT/fail-$point.out"
    set +e
    JW_PAKRAT_FAULT_AT="$point" run_smoke install "$STORE_ID" >"$out" 2>&1
    local rc=$?
    set -e
    [ "$rc" -eq 1 ] || {
        cat "$out" >&2
        fail "fault $point: expected install failure exit 1, got $rc"
    }
    grep -F "$needle" "$out" >/dev/null ||
        fail "fault $point: expected output '$needle'"
}

target_version() {
    python3 -c '
import json, sys
with open(sys.argv[1]) as fp:
    print(json.load(fp)["pak_version"])
' "$INSTALL_PATH/pak.json" 2>/dev/null || echo "<unreadable>"
}

db_version() {
    sqlite3 "$DB_PATH" \
        "SELECT version FROM pakrat_installs WHERE store_id = '$STORE_ID';" 2>/dev/null
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

marker_token() {
    python3 -c '
import json, sys
with open(sys.argv[1]) as fp:
    marker = json.load(fp)
print(marker["token"])
' "$INSTALL_PATH/.pakrat-commit" 2>/dev/null
}

db_row_count() {
    sqlite3 "$DB_PATH" \
        "SELECT COUNT(*) FROM pakrat_installs WHERE store_id = '$STORE_ID';" 2>/dev/null || echo 0
}

sibling_count() {
    find "$SD_ROOT/Apps" -maxdepth 2 -name '.pakrat-*' 2>/dev/null | wc -l | tr -d ' '
}

# Assert the exact post-recovery state: which version is live, what the record
# says, and that no transition siblings remain.
expect_state() {
    local want_target="$1" want_db="$2" label="$3"
    local check_discovery="${4:-yes}"
    if [ "$want_target" = "absent" ]; then
        [ ! -e "$INSTALL_PATH" ] ||
            fail "$label: target present, expected absent"
    else
        [ -d "$INSTALL_PATH" ] || fail "$label: target missing"
        local got
        got="$(target_version)"
        [ "$got" = "$want_target" ] ||
            fail "$label: target pak_version=$got, expected $want_target"
        [ -f "$INSTALL_PATH/launch.sh" ] ||
            fail "$label: launch.sh missing from live tree"
    fi
    if [ "$want_db" = "absent" ]; then
        [ "$(db_row_count)" = "0" ] ||
            fail "$label: install row present, expected none"
    else
        [ "$(db_version)" = "$want_db" ] ||
            fail "$label: record version=$(db_version), expected $want_db"
        if [ "$want_target" != "absent" ]; then
            local record_token tree_token
            record_token="$(db_token)"
            tree_token="$(marker_token)"
            [[ "$record_token" =~ ^[0-9a-f]{32}$ ]] ||
                fail "$label: install record has no valid commit token"
            [ "$tree_token" = "$record_token" ] ||
                fail "$label: tree token does not match install record"
            if [ "$check_discovery" = "yes" ]; then
                [ "$(db_app_version)" = "$want_target" ] ||
                    fail "$label: discovery state version=$(db_app_version), expected $want_target"
            fi
        fi
    fi
    [ "$(sibling_count)" = "0" ] ||
        fail "$label: $(sibling_count) transition siblings left behind"
}

expect_log() {
    local needle="$1" label="$2"
    grep -F "$needle" "$STATE_DIR/store/logs/pakrat.log" >/dev/null ||
        fail "$label: expected pakrat.log line '$needle'"
}

install_old() {
    write_catalog old
    run_smoke install "$STORE_ID" >"$TMP_ROOT/install-old.out" ||
        { cat "$TMP_ROOT/install-old.out" >&2; fail "seed install $OLD_VERSION failed"; }
    expect_state "$OLD_VERSION" "$OLD_VERSION" "seed install"
}

SCENARIO=0
begin_scenario() {
    SCENARIO=$((SCENARIO + 1))
    echo "scenario $SCENARIO: $1"
    reset_sd
}

# 1. Crash before the first rename: nothing was touched.
begin_scenario "crash before-stage (before the first rename)"
install_old
write_catalog new
expect_crash "before-stage"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "before-stage"

# 2. Crash after the stage rename, before the rollback move: the staged tree
#    is discarded, the live tree was never touched.
begin_scenario "crash after-stage (staged, live tree untouched)"
install_old
write_catalog new
expect_crash "after-stage"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "after-stage"

# 3. Crash after the write-ahead origin marker but before moving the live tree:
#    the live tree stays in place, while recovery discards the stage and marker.
begin_scenario "crash after-origin-marker (live tree not moved yet)"
install_old
write_catalog new
expect_crash "after-origin-marker"
[ "$(target_version)" = "$OLD_VERSION" ] ||
    fail "after-origin-marker: live tree changed before move-aside"
[ -f "$APPS_DIR/.pakrat-origin-$STORE_ID" ] ||
    fail "after-origin-marker: write-ahead marker missing before recovery"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "after-origin-marker"

# 4. Crash between rename(target, target_rollback) and rename(target_stage, target):
#    the rollback sibling is renamed back, the staged tree discarded.
begin_scenario "crash before-promote (between the two renames)"
install_old
write_catalog new
expect_crash "before-promote"
[ ! -e "$INSTALL_PATH" ] || fail "before-promote: target should be absent before recovery"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "before-promote"
expect_log "install-recover restored store_id=$STORE_ID" "before-promote"

# 5. Crash after promote, before the install-record update: the transaction
#    never committed, so recovery rolls BACK to the previously running tree.
begin_scenario "crash after-promote (uncommitted promote rolls back)"
install_old
write_catalog new
expect_crash "after-promote"
[ "$(target_version)" = "$NEW_VERSION" ] ||
    fail "after-promote: new tree should be live before recovery"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "after-promote"
[ "$(cat "$INSTALL_PATH/payload.txt")" = "old-payload" ] ||
    fail "after-promote: restored tree is not the previously running one"
expect_log "install-recover rolled back uncommitted promote store_id=$STORE_ID target=Apps/mlp1/Recovery.pak reason=version-mismatch" \
    "after-promote"
# The user retries the update; nothing is lost but the attempt.
run_smoke install "$STORE_ID" >"$TMP_ROOT/retry.out" ||
    { cat "$TMP_ROOT/retry.out" >&2; fail "after-promote: retry install failed"; }
expect_state "$NEW_VERSION" "$NEW_VERSION" "after-promote retry"

# The production startup hook runs the same recovery synchronously before the
# first discovery scan. Plant an uncommitted promote and let jawakad—not the
# smoke helper—restore the old tree.
begin_scenario "daemon startup recovers before first scan"
install_old
write_catalog new
expect_crash "after-promote"
run_daemon_recovery
expect_state "$OLD_VERSION" "$OLD_VERSION" "daemon-startup" "no"
[ "$(cat "$INSTALL_PATH/payload.txt")" = "old-payload" ] ||
    fail "daemon-startup: startup hook left the uncommitted promoted tree live"

# A committed ownership record with no matching marker or rollback cannot heal
# with time. The daemon reports it once and disables its 500 ms startup retry.
begin_scenario "daemon stops retrying a terminal committed-tree mismatch"
install_old
rm "$INSTALL_PATH/.pakrat-commit"
run_daemon_recovery 1.2
daemon_log="$TMP_ROOT/daemon-$SCENARIO.log"
[ "$(grep -Fc 'pakrat: install-transition recovery requires manual repair; automatic retries stopped' "$daemon_log" || true)" = "1" ] ||
    fail "terminal-recovery: daemon did not report exactly one repair-required error"
[ "$(grep -Fc 'pakrat: install-transition recovery will retry' "$daemon_log" || true)" = "0" ] ||
    fail "terminal-recovery: daemon kept retrying a stable mismatch"
[ "$(grep -Fc "install-recover inconsistent committed tree store_id=$STORE_ID" "$STATE_DIR/store/logs/pakrat.log" || true)" = "1" ] ||
    fail "terminal-recovery: recovery ran more than once"

# The device-only removal test needs an observable boundary rather than a race.
# Pause after promotion, prove the old record is still the commit authority,
# then kill the stopped process and recover exactly as after a power loss.
begin_scenario "pause after-promote exposes deterministic removal boundary"
install_old
write_catalog new
old_token="$(db_token)"
JW_PAKRAT_PAUSE_AT="after-promote" \
    "$BIN" --platform mlp1 --sdcard-root "$SD_ROOT" install "$STORE_ID" \
    >"$TMP_ROOT/pause-after-promote.out" 2>&1 &
paused_pid=$!
paused_state=""
for _ in $(seq 1 500); do
    if ! kill -0 "$paused_pid" 2>/dev/null; then
        cat "$TMP_ROOT/pause-after-promote.out" >&2
        fail "pause-after-promote: install exited before SIGSTOP"
    fi
    if [ -r "/proc/$paused_pid/status" ]; then
        paused_state="$(awk '/^State:/ {print $2}' "/proc/$paused_pid/status")"
    else
        paused_state="$(ps -o state= -p "$paused_pid" 2>/dev/null | tr -d '[:space:]')"
    fi
    case "$paused_state" in T*) break ;; esac
    sleep 0.01
done
case "$paused_state" in
    T*) ;;
    *)
    fail "pause-after-promote: process did not enter stopped state"
        ;;
esac
grep -F "pakrat fault injection: paused at after-promote" \
    "$TMP_ROOT/pause-after-promote.out" >/dev/null ||
    fail "pause-after-promote: pause marker missing"
[ "$(target_version)" = "$NEW_VERSION" ] ||
    fail "pause-after-promote: promoted tree is not live"
[ "$(db_version)" = "$OLD_VERSION" ] ||
    fail "pause-after-promote: install record committed before pause"
[ "$(marker_token)" != "$old_token" ] ||
    fail "pause-after-promote: promoted tree reused the old token"
kill -9 "$paused_pid" 2>/dev/null || true
wait "$paused_pid" 2>/dev/null || true
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "pause-after-promote"

# Every crash after promotion but before the durable record remains
# uncommitted, including both sides of the filesystem flush barrier.
for point in before-syncfs after-syncfs before-record; do
    begin_scenario "crash $point (pre-record promote rolls back)"
    install_old
    old_token="$(db_token)"
    write_catalog new
    expect_crash "$point"
    [ "$(target_version)" = "$NEW_VERSION" ] ||
        fail "$point: new tree should be live before recovery"
    [ "$(marker_token)" != "$old_token" ] ||
        fail "$point: promoted tree unexpectedly reused the committed token"
    run_smoke recover
    expect_state "$OLD_VERSION" "$OLD_VERSION" "$point"
done

# A syncfs failure takes the normal in-process rollback path and cannot publish
# either discovery state or the token-bearing install record.
begin_scenario "during-syncfs (flush fails, in-process rollback)"
install_old
write_catalog new
expect_install_failure "during-syncfs" "Pak Rat install filesystem sync failed"
expect_state "$OLD_VERSION" "$OLD_VERSION" "during-syncfs"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "during-syncfs post-recover"

# The record update itself fails (in-process error path, no crash):
#    promote rolls back immediately and the row is left untouched.
begin_scenario "during-record (record update fails, in-process rollback)"
install_old
write_catalog new
expect_install_failure "during-record" \
    "pakrat fault injection: failing record update"
expect_state "$OLD_VERSION" "$OLD_VERSION" "during-record"
expect_log "install-rollback-restored store_id=$STORE_ID" "during-record"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "during-record post-recover"

# Crash after the record update committed, before rollback cleanup:
#    recovery keeps the new tree and deletes the rollback sibling.
begin_scenario "crash after-record (committed, cleanup pending)"
install_old
write_catalog new
expect_crash "after-record"
run_smoke recover
expect_state "$NEW_VERSION" "$NEW_VERSION" "after-record"
[ "$(cat "$INSTALL_PATH/payload.txt")" = "new-payload" ] ||
    fail "after-record: live tree is not the committed new one"
expect_log "install-recover cleaned rollback store_id=$STORE_ID" "after-record"
run_smoke rescan >"$TMP_ROOT/rescan-after-record.out"
[ "$(sqlite3 "$DB_PATH" "SELECT COUNT(*) FROM apps WHERE pak_dir = 'Apps/mlp1/Recovery.pak';")" = "1" ] ||
    fail "after-record: library scan lost the committed app"

# Crash during rollback cleanup (same committed state as after-record).
begin_scenario "crash before-cleanup (committed, cleanup pending)"
install_old
write_catalog new
expect_crash "before-cleanup"
run_smoke recover
expect_state "$NEW_VERSION" "$NEW_VERSION" "before-cleanup"
expect_log "install-recover cleaned rollback store_id=$STORE_ID" "before-cleanup"

# pak.json truncated after promote: the tree cannot be identified, so
#    recovery restores the tree that was already running.
begin_scenario "truncated pak.json after promote (restores)"
install_old
write_catalog new
expect_crash "after-promote"
printf '{ "name": "Recov' >"$INSTALL_PATH/pak.json"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "truncated-manifest"
expect_log "reason=manifest-unreadable" "truncated-manifest"

# Invalid or incomplete commit metadata is never enough to discard the known-
# good rollback tree.
begin_scenario "truncated commit marker after promote (restores)"
install_old
write_catalog new
expect_crash "after-promote"
printf '{"schema":1,"store_id":"%s"' "$STORE_ID" \
    >"$INSTALL_PATH/.pakrat-commit"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "truncated-commit-marker"
expect_log "reason=commit-marker-unreadable" "truncated-commit-marker"

# A healthy same-version repair promoted before its new token-bearing record is
# not committed. Version equality must never authorize the promoted tree.
begin_scenario "same-version repair, crash after-promote (token rolls back)"
install_old
write_catalog new
old_token="$(db_token)"
set +e
JW_PAKRAT_FAULT_AT="after-promote" run_smoke repair "$STORE_ID" "$OLD_VERSION" \
    >"$TMP_ROOT/crash-same-version.out" 2>&1
same_version_rc=$?
set -e
[ "$same_version_rc" -eq 42 ] || {
    cat "$TMP_ROOT/crash-same-version.out" >&2
    fail "same-version: repair did not crash at after-promote"
}
[ "$(marker_token)" != "$old_token" ] ||
    fail "same-version: promoted repair reused the committed token"
printf 'uncommitted-same-version-payload\n' >"$INSTALL_PATH/payload.txt"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "same-version-token"
[ "$(cat "$INSTALL_PATH/payload.txt")" = "old-payload" ] ||
    fail "same-version: uncommitted promoted tree survived recovery"
expect_log "reason=token-mismatch" "same-version-token"

# Declared entry point deleted after promote (same-version repair, so the
#    manifest identity matches and only the entry point can decide): restore.
begin_scenario "launch.sh deleted after promote (restores)"
install_old
write_catalog new
set +e
JW_PAKRAT_FAULT_AT="after-promote" run_smoke repair "$STORE_ID" "$OLD_VERSION" \
    >"$TMP_ROOT/crash-entry.out" 2>&1
rc=$?
set -e
[ "$rc" -eq 42 ] || { cat "$TMP_ROOT/crash-entry.out" >&2; fail "repair did not crash at after-promote"; }
rm -f "$INSTALL_PATH/launch.sh"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "missing-entry-point"
expect_log "reason=entry-point-missing" "missing-entry-point"

# A same-version promoted tree that declares the wrong platform is not the
#     recorded app identity, so recovery restores the prior tree.
begin_scenario "wrong platform after promote (restores)"
install_old
write_catalog new
set +e
JW_PAKRAT_FAULT_AT="after-promote" run_smoke repair "$STORE_ID" "$OLD_VERSION" \
    >"$TMP_ROOT/crash-platform.out" 2>&1
platform_rc=$?
set -e
[ "$platform_rc" -eq 42 ] || {
    cat "$TMP_ROOT/crash-platform.out" >&2
    fail "platform-mismatch: repair did not crash at after-promote"
}
printf '{"name":"Recovery Smoke","platform":"mac","pak_version":"%s"}\n' \
    "$OLD_VERSION" >"$INSTALL_PATH/pak.json"
printf 'promoted-but-wrong-platform\n' >"$INSTALL_PATH/payload.txt"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "platform-mismatch"
[ "$(cat "$INSTALL_PATH/payload.txt")" = "old-payload" ] ||
    fail "platform-mismatch: prior tree was not restored"
expect_log "reason=platform-mismatch" "platform-mismatch"

# Same-version repair committed but cleanup pending: the exact marker token
# matches the record, so recovery keeps the repaired tree and cleans up.
begin_scenario "same-version repair, crash after-record (cleanup wins)"
install_old
write_catalog new
set +e
JW_PAKRAT_FAULT_AT="after-record" run_smoke repair "$STORE_ID" "$OLD_VERSION" \
    >"$TMP_ROOT/crash-repair.out" 2>&1
rc=$?
set -e
[ "$rc" -eq 42 ] || { cat "$TMP_ROOT/crash-repair.out" >&2; fail "repair did not crash at after-record"; }
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "repair-cleanup"
expect_log "install-recover cleaned rollback store_id=$STORE_ID" "repair-cleanup"

# Interrupted first-time install: no install row exists, so only the Apps
#     dir sweep can see the staged tree. It must not remain.
begin_scenario "interrupted first install (orphaned stage swept)"
write_catalog new
expect_crash "before-promote"
[ -d "$APPS_DIR/.pakrat-stage-$STORE_ID" ] ||
    fail "first-install: stage sibling should exist before recovery"
run_smoke recover
expect_state "absent" "absent" "first-install"
expect_log "install-recover swept orphan store_id=$STORE_ID" "first-install"

# A first install can also lose power after the package reached its final name
# but before the record commit. Its marker identifies it as an uncommitted Pak
# Rat target, so recovery removes it rather than adopting it by accident.
begin_scenario "interrupted first install after promote (target removed)"
write_catalog new
expect_crash "after-promote"
[ -d "$INSTALL_PATH" ] ||
    fail "first-install-promoted: target should exist before recovery"
[ -f "$INSTALL_PATH/.pakrat-commit" ] ||
    fail "first-install-promoted: commit marker missing before recovery"
run_smoke recover
expect_state "absent" "absent" "first-install-promoted"
expect_log "install-recover removed uncommitted first install store_id=$STORE_ID" \
    "first-install-promoted"

# Missing media must make recovery defer without recreating rootfs stubs or
# interpreting an absent tree as deletion. Once the same source returns, the
# pending rollback is recovered normally.
begin_scenario "owning source absent (defer with zero mutation)"
install_old
write_catalog new
expect_crash "before-promote"
DETACHED_SD="$TMP_ROOT/detached-sd"
mv "$SD_ROOT" "$DETACHED_SD"
run_smoke recover >"$TMP_ROOT/recover-source-absent.out" 2>&1
[ ! -e "$SD_ROOT" ] ||
    fail "source-absent: recovery recreated the missing SD root"
[ -d "$DETACHED_SD/Apps/mlp1/.pakrat-stage-$STORE_ID" ] ||
    fail "source-absent: staged tree was mutated while source was absent"
[ -d "$DETACHED_SD/Apps/mlp1/.pakrat-rollback-$STORE_ID" ] ||
    fail "source-absent: rollback tree was mutated while source was absent"
grep -F "recovery deferred: owning Apps source is not mounted" \
    "$TMP_ROOT/recover-source-absent.out" >/dev/null ||
    fail "source-absent: deferred recovery message missing"
set +e
run_smoke install "$STORE_ID" >"$TMP_ROOT/install-source-absent.out" 2>&1
source_install_rc=$?
set -e
[ "$source_install_rc" -eq 1 ] ||
    fail "source-absent: install did not reject the missing source"
[ ! -e "$SD_ROOT" ] ||
    fail "source-absent: install recreated the missing SD root"
mv "$DETACHED_SD" "$SD_ROOT"
run_smoke recover
expect_state "$OLD_VERSION" "$OLD_VERSION" "source-returned"

# An adopted install that crashes after its write-ahead marker but before
#     the move-aside keeps the original target in place. Recovery must discard
#     both the staged tree and the now-unneeded marker even though no install
#     row exists.
begin_scenario "adopted install crash after origin marker (target stays live)"
install_old
sqlite3 "$DB_PATH" "DELETE FROM pakrat_installs;" >/dev/null
write_catalog new
set +e
JW_PAKRAT_FAULT_AT=after-origin-marker run_smoke adopt "$STORE_ID" \
    >"$TMP_ROOT/adopt-origin-crash.out" 2>&1
adopt_origin_rc=$?
set -e
[ "$adopt_origin_rc" -eq 42 ] || {
    cat "$TMP_ROOT/adopt-origin-crash.out" >&2
    fail "adopted-origin: expected crash exit 42, got $adopt_origin_rc"
}
[ -d "$INSTALL_PATH" ] ||
    fail "adopted-origin: target was moved before the write-ahead crash point"
[ -f "$APPS_DIR/.pakrat-origin-$STORE_ID" ] ||
    fail "adopted-origin: write-ahead marker missing before recovery"
run_smoke recover
expect_state "$OLD_VERSION" "absent" "adopted-origin"

# An adopted install that crashes between the move-aside and the promote
#     has no install row yet, so it reaches the orphan sweep rather than
#     reconcile. Its rollback sibling is the only surviving copy of the app the
#     user already had: it must be restored from its origin marker, never
#     swept. Regression guard for a sweep that deleted it.
begin_scenario "adopted install crash before promote (unrecorded rollback restored)"
install_old
# Drop the ownership record but keep the tree, reproducing an adopted install.
sqlite3 "$DB_PATH" "DELETE FROM pakrat_installs;" >/dev/null
write_catalog new
set +e
JW_PAKRAT_FAULT_AT=before-promote run_smoke adopt "$STORE_ID" \
    >"$TMP_ROOT/adopt-crash.out" 2>&1
adopt_rc=$?
set -e
[ "$adopt_rc" -eq 42 ] || {
    cat "$TMP_ROOT/adopt-crash.out" >&2
    fail "adopted: expected crash exit 42, got $adopt_rc"
}
[ -d "$APPS_DIR/.pakrat-rollback-$STORE_ID" ] ||
    fail "adopted: rollback sibling should exist before recovery"
[ -f "$APPS_DIR/.pakrat-origin-$STORE_ID" ] ||
    fail "adopted: origin marker should exist before recovery"
[ ! -e "$INSTALL_PATH" ] ||
    fail "adopted: target should be absent before recovery"
run_smoke recover
[ -d "$INSTALL_PATH" ] ||
    fail "adopted: rollback was destroyed instead of restored"
[ "$(target_version)" = "$OLD_VERSION" ] ||
    fail "adopted: restored tree is not the app that was moved aside"
[ -f "$INSTALL_PATH/launch.sh" ] ||
    fail "adopted: restored tree is incomplete"
[ "$(sibling_count)" = "0" ] ||
    fail "adopted: transition siblings left behind"
[ ! -f "$APPS_DIR/.pakrat-origin-$STORE_ID" ] ||
    fail "adopted: origin marker left behind"
expect_log "install-recover restored unrecorded rollback store_id=$STORE_ID" "adopted"

# A rollback sibling with no origin marker cannot be mapped back to a
#     target, so it is retained and surfaced rather than deleted.
begin_scenario "unidentifiable rollback retained, stage orphan still swept"
rm -rf "$APPS_DIR"; mkdir -p "$APPS_DIR"
mkdir -p "$APPS_DIR/.pakrat-rollback-$STORE_ID" "$APPS_DIR/.pakrat-stage-$STORE_ID"
printf '{"pak_version":"%s","platform":"mlp1"}\n' "$OLD_VERSION" \
    > "$APPS_DIR/.pakrat-rollback-$STORE_ID/pak.json"
run_smoke recover
[ -d "$APPS_DIR/.pakrat-rollback-$STORE_ID" ] ||
    fail "unidentifiable: rollback must be retained, not deleted"
[ ! -d "$APPS_DIR/.pakrat-stage-$STORE_ID" ] ||
    fail "unidentifiable: stage orphan should still be swept"
expect_log "install-recover retained unidentifiable rollback store_id=$STORE_ID" \
    "unidentifiable"
rm -rf "$APPS_DIR/.pakrat-rollback-$STORE_ID"

# An archive cannot supply the reserved commit marker. The installer owns this
# file and must reject the artifact rather than overwrite attacker-controlled
# commit metadata.
begin_scenario "archive-supplied commit marker rejected"
printf '{"schema":1}\n' >"$TMP_ROOT/stage-$NEW_VERSION/Recovery.pak/.pakrat-commit"
(cd "$TMP_ROOT/stage-$NEW_VERSION" &&
    zip -q "$NEW_ARCHIVE" Recovery.pak/.pakrat-commit)
NEW_SHA="$(sha256_file "$NEW_ARCHIVE")"
NEW_SIZE="$(wc -c <"$NEW_ARCHIVE" | tr -d ' ')"
write_catalog new
expect_install_failure "reserved-marker" \
    "artifact contains or cannot write reserved commit marker"
expect_state "absent" "absent" "reserved-marker"
# Restore the normal fixture for the final happy-path regression.
rm -f "$NEW_ARCHIVE"
make_artifact "$NEW_VERSION" "0.7.0" "new-payload"
NEW_SHA="$(sha256_file "$NEW_ARCHIVE")"
NEW_SIZE="$(wc -c <"$NEW_ARCHIVE" | tr -d ' ')"

# A normal, uninterrupted update leaves exactly one tree and a matching
#     record (regression guard: recovery changes nothing on the happy path).
begin_scenario "normal update (one tree, matching record)"
install_old
write_catalog new
run_smoke install "$STORE_ID" >"$TMP_ROOT/update.out" ||
    { cat "$TMP_ROOT/update.out" >&2; fail "normal update failed"; }
expect_state "$NEW_VERSION" "$NEW_VERSION" "normal-update"
[ "$(cat "$INSTALL_PATH/payload.txt")" = "new-payload" ] ||
    fail "normal-update: live tree is not the new one"

echo "Pak Rat recovery smoke passed ($SCENARIO scenarios)"
