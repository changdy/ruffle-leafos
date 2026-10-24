#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
. "$ROOT_DIR/scripts/lib/smoke-daemon.sh"
BUILD_DIR="${BUILD:-build/pakrat-service-mutation-smoke}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jw-service-mutation.XXXXXX")"
PRIMARY="$TMP_ROOT/primary"
SECONDARY="$TMP_ROOT/secondary"
PRIMARY_APPS="$PRIMARY/Apps"
SECONDARY_APPS="$SECONDARY/Apps"
PRIMARY_USERDATA="$PRIMARY/.userdata/mac"
SECONDARY_USERDATA="$SECONDARY/.userdata/mac"
STATE="$PRIMARY/.umrk/mac"
CONTROL_DB="$STATE/services-control.db"
PLATFORM_ROOT="$PRIMARY/.system/leaf/platforms/mac"
RUNTIME="$TMP_ROOT/runtime"
SOCKET="$RUNTIME/jawakad.sock"
LOGS="$PRIMARY_USERDATA/logs"
DAEMON_LOG="$TMP_ROOT/jawakad.log"
HTTP_LOG="$TMP_ROOT/http.log"
FEED="$TMP_ROOT/feed"
STORE_ID="org.umrk.test.txnservice"
OTHER_ID="org.umrk.test.txnother"
INSTALL_REL="mac/TxnService.pak"
TARGET="$PRIMARY_APPS/$INSTALL_REL"
SECONDARY_TARGET="$SECONDARY_APPS/$INSTALL_REL"
DAEMON_PID=""
HTTP_PID=""

cleanup() {
    local status=$?
    set +e
    smoke_daemon_stop || status=1
    if [ -n "$HTTP_PID" ]; then
        kill "$HTTP_PID" >/dev/null 2>&1 || true
        wait "$HTTP_PID" >/dev/null 2>&1 || true
    fi
    if [ "$status" -ne 0 ]; then
        [ -f "$DAEMON_LOG" ] && tail -n 240 "$DAEMON_LOG" >&2
        [ -f "$HTTP_LOG" ] && tail -n 80 "$HTTP_LOG" >&2
        printf 'TXN-1 fixture retained at %s\n' "$TMP_ROOT" >&2
    else
        rm -rf "$TMP_ROOT"
    fi
    exit "$status"
}
trap cleanup EXIT

for tool in python3 zip unzip curl sqlite3; do
    command -v "$tool" >/dev/null 2>&1 || {
        printf '%s is required for pakrat-service-mutation-smoke\n' "$tool" >&2
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

mkdir -p "$PRIMARY_APPS/mac" "$SECONDARY_APPS/mac" "$PRIMARY_USERDATA" \
    "$SECONDARY_USERDATA" "$STATE/store" "$PLATFORM_ROOT" "$RUNTIME" \
    "$LOGS" "$FEED/artifacts"
printf '{"managed_apps":[]}\n' >"$PLATFORM_ROOT/manifest.json"

make -C "$ROOT_DIR" -s BUILD="$BUILD_DIR" \
    jawakad jawaka-platformctl jawaka-pakrat-smoke
DAEMON="$ROOT_DIR/$BUILD_DIR/bin/jawakad"
CTL="$ROOT_DIR/$BUILD_DIR/bin/jawaka-platformctl"
PAKRAT="$ROOT_DIR/$BUILD_DIR/bin/jawaka-pakrat-smoke"

make_artifact() {
    local version="$1"
    local service="$2"
    local stage="$TMP_ROOT/artifact-$version"
    local pak="$stage/TxnService.pak"
    local archive="$FEED/artifacts/TxnService-$version.mac.pak.zip"
    rm -rf "$stage"
    mkdir -p "$pak/bin"
    printf '#!/bin/sh\nexit 0\n' >"$pak/launch.sh"
    chmod 755 "$pak/launch.sh"
    if [ "$service" = yes ]; then
        cat >"$pak/pak.json" <<JSON
{"id":"$STORE_ID","name":"TXN Service","platform":"mac","pak_version":"$version","min_leaf_version":"1.0.0","service":{"schema":1,"id":"$STORE_ID","run":{"path":"bin/run.sh","args":[]},"restart":"no","default_enabled":false,"stop_grace_ms":300},"state":{"root":"Syncthing","revoke_on_uninstall":["leaf/trusted.json"],"retained_roots":["Syncthing"]}}
JSON
        cat >"$pak/bin/run.sh" <<'SH'
#!/bin/sh
trap 'exit 0' TERM INT
while :; do sleep 1; done
SH
        chmod 755 "$pak/bin/run.sh"
    else
        cat >"$pak/pak.json" <<JSON
{"id":"$STORE_ID","name":"TXN Service Floor","platform":"mac","pak_version":"$version"}
JSON
    fi
    (cd "$stage" && zip -qr "$archive" TxnService.pak)
}

make_artifact 0.0.1 no
make_artifact 1.0.0 yes
make_artifact 2.0.0 yes

artifact_object() {
    local version="$1"
    local archive="$FEED/artifacts/TxnService-$version.mac.pak.zip"
    local sha size
    sha="$(sha256_file "$archive")"
    size="$(wc -c <"$archive" | tr -d ' ')"
    printf '{"url":"%sartifacts/%s","name":"%s","archive":"zip","size":%s,"installed_size":4096,"sha256":"%s"}' \
        "$BASE_URL" "$(basename "$archive")" "$(basename "$archive")" \
        "$size" "$sha"
}

artifact_json() {
    local version="$1"
    local min_leaf="$2"
    if [ -n "$min_leaf" ]; then
        printf '{"version":"%s","min_leaf_version":"%s","artifact":%s}' \
            "$version" "$min_leaf" "$(artifact_object "$version")"
    else
        printf '{"version":"%s","artifact":%s}' \
            "$version" "$(artifact_object "$version")"
    fi
}

PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')"
BASE_URL="http://127.0.0.1:$PORT/"

write_catalog() {
    local newest="$1"
    local floor v1 versions
    floor="$(artifact_json 0.0.1 '')"
    v1="$(artifact_json 1.0.0 1.0.0)"
    if [ "$newest" = 1.0.0 ]; then
        versions="$v1,$floor"
    else
        versions="$(artifact_json 2.0.0 1.0.0),$v1,$floor"
    fi
    cat >"$FEED/storefront.json" <<JSON
{"schema":1,"product":"pak-rat","apps":[{"id":"$STORE_ID","name":"TXN Service","summary":"TXN-1 fixture","version":"0.0.1","packages":[{"platform":"mac","runtime":"leaf","version":"0.0.1","install_name":"TxnService.pak","runtime_manifest_path":"pak.json","artifact":$(artifact_object 0.0.1),"versions":[$versions]}]}]}
JSON
}

write_release() {
    cat >"$STATE/release.json" <<JSON
{"schema":1,"product":"leaf","platform":"mac","version":"$1","release_id":"$1"}
JSON
}

write_catalog 1.0.0
write_release 1.0.0
python3 -m http.server "$PORT" --bind 127.0.0.1 \
    --directory "$FEED" >"$HTTP_LOG" 2>&1 &
HTTP_PID=$!
for _ in $(seq 1 100); do
    curl -fsS "${BASE_URL}storefront.json" >/dev/null 2>&1 && break
    sleep 0.03
done
curl -fsS "${BASE_URL}storefront.json" >/dev/null

mkdir -p "$PRIMARY_APPS/mac/TxnOther.pak/bin"
cat >"$PRIMARY_APPS/mac/TxnOther.pak/pak.json" <<JSON
{"id":"$OTHER_ID","name":"TXN Other","platform":"mac","pak_version":"1.0.0","service":{"schema":1,"id":"$OTHER_ID","run":{"path":"bin/run.sh","args":[]},"restart":"no","default_enabled":false,"stop_grace_ms":300}}
JSON
cat >"$PRIMARY_APPS/mac/TxnOther.pak/bin/run.sh" <<'SH'
#!/bin/sh
trap 'exit 0' TERM INT
while :; do sleep 1; done
SH
chmod 755 "$PRIMARY_APPS/mac/TxnOther.pak/bin/run.sh"

common_env=(
    PLATFORM=mac
    SDCARD_PATH="$PRIMARY"
    SDCARD_PATHS="$PRIMARY:$SECONDARY"
    APPS_PATH="$PRIMARY_APPS"
    APPS_PATHS="$PRIMARY_APPS:$SECONDARY_APPS"
    USERDATA_PATH="$PRIMARY_USERDATA"
    USERDATA_PATHS="$PRIMARY_USERDATA:$SECONDARY_USERDATA"
    LOGS_PATH="$LOGS"
    UMRK_RUNTIME_PATH="$RUNTIME"
    JAWAKA_RUNTIME_DIR="$RUNTIME"
    UMRK_DAEMON_SOCKET="$SOCKET"
    JAWAKA_SOCKET_PATH="$SOCKET"
    UMRK_INTERNAL_DATA_PATH="$STATE"
    UMRK_PLATFORM_PATH="$PLATFORM_ROOT"
    PAKRAT_CATALOG_BASE_URL="$BASE_URL"
)

start_daemon() {
    : >"$DAEMON_LOG"
    smoke_daemon_start "$PWD" "$DAEMON_LOG" \
        "${common_env[@]}" "$DAEMON" --daemon-only
    for _ in $(seq 1 250); do
        [ -S "$SOCKET" ] && return 0
        kill -0 "$DAEMON_PID" >/dev/null 2>&1 || return 1
        sleep 0.02
    done
    return 1
}

stop_daemon() {
    smoke_daemon_stop
    rm -f "$SOCKET"
}

request() {
    "$CTL" --socket "$SOCKET" request "$1"
}

ctl1() {
    local service_id="$1"
    local op="$2"
    request "{\"v\":1,\"op\":\"$op\",\"id\":\"$op-$service_id\",\"service_id\":\"$service_id\"}"
}

status_field() {
    local service_id="$1"
    local expression="$2"
    ctl1 "$service_id" status | python3 -c \
        "import json,sys; d=json.load(sys.stdin); print(d$expression)"
}

wait_state() {
    local service_id="$1"
    local wanted="$2"
    for _ in $(seq 1 300); do
        [ "$(status_field "$service_id" "['effective_state']" 2>/dev/null || true)" = "$wanted" ] && return 0
        sleep 0.02
    done
    return 1
}

run_pakrat() {
    env "${common_env[@]}" "$PAKRAT" --platform mac \
        --sdcard-root "$PRIMARY" --state-dir "$STATE" \
        --db "$STATE/library.db" --platform-root "$PLATFORM_ROOT" \
        --runtime-dir "$RUNTIME" --socket "$SOCKET" "$@"
}

run_pakrat_fault() {
    local point="$1"
    shift
    env "${common_env[@]}" JW_PAKRAT_FAULT_AT="$point" \
        "$PAKRAT" --platform mac --sdcard-root "$PRIMARY" \
        --state-dir "$STATE" --db "$STATE/library.db" \
        --platform-root "$PLATFORM_ROOT" --runtime-dir "$RUNTIME" \
        --socket "$SOCKET" "$@"
}

sql_count() {
    sqlite3 "$STATE/library.db" "$1"
}

pak_version() {
    python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["pak_version"])' "$1/pak.json"
}

assert_no_transition_siblings() {
    [ ! -e "$PRIMARY_APPS/mac/.pakrat-stage-$STORE_ID" ]
    [ ! -e "$PRIMARY_APPS/mac/.pakrat-rollback-$STORE_ID" ]
    [ ! -e "$PRIMARY_APPS/mac/.pakrat-origin-$STORE_ID" ]
}

start_daemon
ctl1 "$OTHER_ID" enable | grep -F '"ok":true' >/dev/null
ctl1 "$OTHER_ID" run | grep -F '"ok":true' >/dev/null
wait_state "$OTHER_ID" running
OTHER_PGID="$(status_field "$OTHER_ID" "['ownership_identity']['pgid']")"

run_pakrat install-target "$STORE_ID" 1.0.0 >/dev/null
[ "$(pak_version "$TARGET")" = 1.0.0 ]
[ "$(status_field "$STORE_ID" "['desired_enabled']")" = False ]
wait_state "$STORE_ID" disabled
ctl1 "$STORE_ID" enable | grep -F '"ok":true' >/dev/null
ctl1 "$STORE_ID" run | grep -F '"ok":true' >/dev/null
wait_state "$STORE_ID" running

write_catalog 2.0.0
UPDATE_LOG="$TMP_ROOT/update-paused.log"
env "${common_env[@]}" JW_PAKRAT_PAUSE_AT=before-stage \
    "$PAKRAT" --platform mac --sdcard-root "$PRIMARY" \
    --state-dir "$STATE" --db "$STATE/library.db" \
    --platform-root "$PLATFORM_ROOT" --runtime-dir "$RUNTIME" \
    --socket "$SOCKET" install "$STORE_ID" >"$UPDATE_LOG" 2>&1 &
UPDATE_PID=$!
for _ in $(seq 1 300); do
    grep -F 'paused at before-stage' "$UPDATE_LOG" >/dev/null 2>&1 && break
    kill -0 "$UPDATE_PID" >/dev/null 2>&1 || break
    sleep 0.02
done
grep -F 'paused at before-stage' "$UPDATE_LOG" >/dev/null
[ "$(pak_version "$TARGET")" = 1.0.0 ]
wait_state "$STORE_ID" stopped
ctl1 "$STORE_ID" run | grep -F '"code":"package-in-progress"' >/dev/null
request '{"type":"launch-app","pak_dir":"Apps/mac/TxnService.pak"}' |
    grep -F 'package mutation in progress' >/dev/null
[ "$(status_field "$OTHER_ID" "['ownership_identity']['pgid']")" = "$OTHER_PGID" ]
kill -CONT "$UPDATE_PID"
wait "$UPDATE_PID"
[ "$(pak_version "$TARGET")" = 2.0.0 ]
wait_state "$STORE_ID" running
[ "$(status_field "$STORE_ID" "['installed_package']['version']")" = 2.0.0 ]
[ "$(status_field "$OTHER_ID" "['ownership_identity']['pgid']")" = "$OTHER_PGID" ]

# Exercise the complete P1 promote state table while a real service generation
# is enabled and running. Every crash drops the client flock; jawakad must adopt
# the still-active mutation gate, reconcile exactly one complete tree, and only
# then restore the service. Same-version repair keeps the content version fixed
# while P1's commit token still distinguishes pre/post-record outcomes.
install_crash_points=(
    before-stage after-stage after-origin-marker before-promote after-promote
    before-syncfs after-syncfs before-record after-record before-cleanup
)
for point in "${install_crash_points[@]}"; do
    set +e
    run_pakrat_fault "$point" repair "$STORE_ID" 2.0.0 \
        >"$TMP_ROOT/install-$point.log" 2>&1
    point_rc=$?
    set -e
    [ "$point_rc" -eq 42 ]
    wait_state "$STORE_ID" running
    [ "$(pak_version "$TARGET")" = 2.0.0 ]
    [ -x "$TARGET/bin/run.sh" ]
    [ -f "$TARGET/.pakrat-commit" ]
    assert_no_transition_siblings
    [ "$(status_field "$OTHER_ID" "['ownership_identity']['pgid']")" = "$OTHER_PGID" ]
done

for point in during-syncfs during-record; do
    set +e
    run_pakrat_fault "$point" repair "$STORE_ID" 2.0.0 \
        >"$TMP_ROOT/install-$point.log" 2>&1
    point_rc=$?
    set -e
    [ "$point_rc" -ne 0 ]
    wait_state "$STORE_ID" running
    [ "$(pak_version "$TARGET")" = 2.0.0 ]
    [ -x "$TARGET/bin/run.sh" ]
    assert_no_transition_siblings
done

# A rolled-back Leaf selects the inert floor. This is the sole permitted
# automatic downgrade and must clear durable/session service state atomically.
write_release 0.9.0
run_pakrat install "$STORE_ID" >/dev/null
[ "$(pak_version "$TARGET")" = 0.0.1 ]
request '{"v":1,"op":"list","id":"list-after-floor"}' |
    python3 -c 'import json,sys; d=json.load(sys.stdin); assert all(s["service_id"] != sys.argv[1] for s in d["services"])' "$STORE_ID"
[ "$(sqlite3 "$CONTROL_DB" "SELECT COUNT(*) FROM service_control_state WHERE service_id='$STORE_ID';")" = 0 ]

# Raising the client back to Release P selects the real pak. A floor-to-real
# replacement is installed on Primary and begins disabled/stopped.
write_release 1.0.0
run_pakrat install "$STORE_ID" >/dev/null
[ "$(pak_version "$TARGET")" = 2.0.0 ]
[ "$(status_field "$STORE_ID" "['desired_enabled']")" = False ]
wait_state "$STORE_ID" disabled

# Stopped uninstall uses only Jawaka's cached declaration: trust is revoked,
# history is retained, and no controller response/socket is required.
mkdir -p "$PRIMARY_USERDATA/Syncthing/leaf" "$PRIMARY_USERDATA/Syncthing/history"
printf secret >"$PRIMARY_USERDATA/Syncthing/leaf/trusted.json"
printf history >"$PRIMARY_USERDATA/Syncthing/history/index.db"
rm -f "$PRIMARY_USERDATA/Syncthing/controller.sock"
run_pakrat uninstall "$STORE_ID" >/dev/null
[ ! -e "$TARGET" ]
[ ! -e "$PRIMARY_USERDATA/Syncthing/leaf/trusted.json" ]
[ -e "$PRIMARY_USERDATA/Syncthing/history/index.db" ]
[ "$(sql_count "SELECT COUNT(*) FROM pakrat_installs WHERE store_id='$STORE_ID';")" = 0 ]

# Reinstall is disabled even when the prior service was enabled. Kill the live
# generation and crash the client after durable intent; jawakad must adopt the
# abandoned lock and finish forward without an app controller.
run_pakrat install "$STORE_ID" >/dev/null
[ "$(status_field "$STORE_ID" "['desired_enabled']")" = False ]
ctl1 "$STORE_ID" enable | grep -F '"ok":true' >/dev/null
ctl1 "$STORE_ID" run | grep -F '"ok":true' >/dev/null
wait_state "$STORE_ID" running
TARGET_PGID="$(status_field "$STORE_ID" "['ownership_identity']['pgid']")"
kill -KILL -- "-$TARGET_PGID"
for _ in $(seq 1 300); do
    kill -0 "$TARGET_PGID" >/dev/null 2>&1 || break
    sleep 0.02
done
mkdir -p "$PRIMARY_USERDATA/Syncthing/leaf"
printf secret >"$PRIMARY_USERDATA/Syncthing/leaf/trusted.json"
set +e
env "${common_env[@]}" JW_PAKRAT_FAULT_AT=uninstall-after-intent \
    "$PAKRAT" --platform mac --sdcard-root "$PRIMARY" \
    --state-dir "$STATE" --db "$STATE/library.db" \
    --platform-root "$PLATFORM_ROOT" --runtime-dir "$RUNTIME" \
    --socket "$SOCKET" uninstall "$STORE_ID" \
    >"$TMP_ROOT/uninstall-crash.log" 2>&1
CRASH_RC=$?
set -e
[ "$CRASH_RC" -eq 42 ]
for _ in $(seq 1 400); do
    [ ! -e "$TARGET" ] && \
        [ "$(sql_count "SELECT COUNT(*) FROM pakrat_pending_uninstalls WHERE store_id='$STORE_ID';")" = 0 ] && break
    sleep 0.03
done
[ ! -e "$TARGET" ]
[ ! -e "$PRIMARY_USERDATA/Syncthing/leaf/trusted.json" ]
[ "$(sql_count "SELECT COUNT(*) FROM pakrat_installs WHERE store_id='$STORE_ID';")" = 0 ]
[ "$(sql_count "SELECT COUNT(*) FROM pakrat_pending_uninstalls WHERE store_id='$STORE_ID';")" = 0 ]

# Reproduce a legacy floor owned by Secondary. The real service may not update
# there or appear beside it on Primary; committed uninstall must happen first.
stop_daemon
mkdir -p "$(dirname "$SECONDARY_TARGET")"
unzip -q "$FEED/artifacts/TxnService-0.0.1.mac.pak.zip" -d "$TMP_ROOT/secondary-unzip"
mv "$TMP_ROOT/secondary-unzip/TxnService.pak" "$SECONDARY_TARGET"
FLOOR_SHA="$(sha256_file "$FEED/artifacts/TxnService-0.0.1.mac.pak.zip")"
sqlite3 "$STATE/library.db" <<SQL
INSERT INTO pakrat_installs(store_id,version,platform,source_id,install_path,artifact_sha256,installed_at,commit_token)
VALUES('$STORE_ID','0.0.1','mac','secondary_sd','$INSTALL_REL','$FLOOR_SHA',strftime('%Y-%m-%dT%H:%M:%SZ','now'),NULL);
INSERT OR REPLACE INTO pakrat_service_metadata(store_id,install_path,package_id,display_name,has_service,service_id,state_root,revoke_json,retained_json,validated_at)
VALUES('$STORE_ID','$INSTALL_REL','$STORE_ID','TXN Service Floor',0,'','','[]','[]',strftime('%Y-%m-%dT%H:%M:%SZ','now'));
SQL
start_daemon
set +e
run_pakrat install "$STORE_ID" >"$TMP_ROOT/secondary-refusal.log" 2>&1
SECONDARY_RC=$?
set -e
[ "$SECONDARY_RC" -ne 0 ]
grep -F 'installed on Secondary. Uninstall it there first, then install the service pak on Primary.' \
    "$TMP_ROOT/secondary-refusal.log" >/dev/null
[ -d "$SECONDARY_TARGET" ]
[ ! -e "$TARGET" ]
run_pakrat uninstall "$STORE_ID" >/dev/null
[ ! -e "$SECONDARY_TARGET" ]
run_pakrat install "$STORE_ID" >/dev/null
[ -d "$TARGET" ]
[ ! -e "$SECONDARY_TARGET" ]
[ "$(sql_count "SELECT source_id FROM pakrat_installs WHERE store_id='$STORE_ID';")" = primary ]
[ "$(status_field "$STORE_ID" "['desired_enabled']")" = False ]

printf 'PASS pakrat-service-mutation-smoke\n'
