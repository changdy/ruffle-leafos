#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD:-build/pakrat-state-smoke}"
CATALOG_URL="${PAKRAT_CATALOG_BASE_URL:-}"
STORE_ID="${PAKRAT_SMOKE_STORE_ID:-org.helaas.sdlreader}"
INSTALL_PATH="${PAKRAT_SMOKE_INSTALL_PATH:-Apps/mlp1/SDLReader.pak}"
MANAGED_PATH="${INSTALL_PATH#Apps/}"

if [ -z "$CATALOG_URL" ]; then
    echo "Set PAKRAT_CATALOG_BASE_URL to a local Pak Rat feed, for example:" >&2
    echo "  PAKRAT_CATALOG_BASE_URL=http://127.0.0.1:8765/pakrat/v1/ make pakrat-state-smoke" >&2
    exit 2
fi

case "$CATALOG_URL" in
    */) ;;
    *) CATALOG_URL="$CATALOG_URL/" ;;
esac

case "$STORE_ID$INSTALL_PATH" in
    *"'"*)
        echo "PAKRAT_SMOKE_STORE_ID and PAKRAT_SMOKE_INSTALL_PATH may not contain single quotes" >&2
        exit 2
        ;;
esac

case "$INSTALL_PATH" in
    Apps/*) ;;
    *)
        echo "PAKRAT_SMOKE_INSTALL_PATH must use the Apps/<platform>/<Name>.pak form" >&2
        exit 2
        ;;
esac

if ! command -v sqlite3 >/dev/null 2>&1; then
    echo "sqlite3 is required for pakrat-state-smoke" >&2
    exit 2
fi

TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-pakrat-state.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT

SD_ROOT="$TMP_ROOT/sd"
MANAGED_SD_ROOT="$TMP_ROOT/managed-sd"
STATE_DIR="$SD_ROOT/.umrk/mlp1"
MANAGED_STATE_DIR="$MANAGED_SD_ROOT/.umrk/mlp1"
PLATFORM_ROOT="$SD_ROOT/.system/leaf/platforms/mlp1"
MANAGED_PLATFORM_ROOT="$MANAGED_SD_ROOT/.system/leaf/platforms/mlp1"
DB_PATH="$STATE_DIR/library.db"
MANAGED_DB_PATH="$MANAGED_STATE_DIR/library.db"

make -C "$JAWAKA_DIR" BUILD="$BUILD_DIR" jawaka-pakrat-smoke >/dev/null
BIN="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-pakrat-smoke"

mkdir -p "$PLATFORM_ROOT" "$STATE_DIR/store" \
    "$MANAGED_PLATFORM_ROOT" "$MANAGED_STATE_DIR/store"
printf '{ "managed_apps": [] }\n' >"$PLATFORM_ROOT/manifest.json"
printf '{ "managed_apps": [ "%s" ] }\n' "$MANAGED_PATH" \
    >"$MANAGED_PLATFORM_ROOT/manifest.json"
printf '%s\n' "$CATALOG_URL" >"$STATE_DIR/store/dev-catalog-url"
printf '%s\n' "$CATALOG_URL" >"$MANAGED_STATE_DIR/store/dev-catalog-url"

run_smoke() {
    env -u PAKRAT_CATALOG_BASE_URL "$BIN" \
        --platform mlp1 \
        --sdcard-root "$SD_ROOT" \
        "$@"
}

run_managed_smoke() {
    env -u PAKRAT_CATALOG_BASE_URL "$BIN" \
        --platform mlp1 \
        --sdcard-root "$MANAGED_SD_ROOT" \
        "$@"
}

fail_with_output() {
    local message="$1"
    local file="${2:-}"
    if [ -n "$file" ] && [ -f "$file" ]; then
        cat "$file" >&2
    fi
    echo "$message" >&2
    exit 1
}

expect_contains() {
    local file="$1"
    local needle="$2"
    local message="$3"
    grep -F "$needle" "$file" >/dev/null ||
        fail_with_output "$message" "$file"
}

expect_sql() {
    local db="$1"
    local sql="$2"
    local expected="$3"
    local got
    got="$(sqlite3 "$db" "$sql")"
    if [ "$got" != "$expected" ]; then
        echo "SQL: $sql" >&2
        echo "expected: $expected" >&2
        echo "got: $got" >&2
        exit 1
    fi
}

expect_failure() {
    local out="$1"
    shift
    set +e
    "$@" >"$out" 2>&1
    local rc=$?
    set -e
    if [ "$rc" -eq 0 ]; then
        fail_with_output "command unexpectedly succeeded" "$out"
    fi
}

LIST_AVAILABLE="$TMP_ROOT/list-available.tsv"
LIST_INSTALLED="$TMP_ROOT/list-installed.tsv"
LIST_STALE="$TMP_ROOT/list-stale.tsv"
LIST_RESTORED="$TMP_ROOT/list-restored.tsv"
LIST_MANAGED="$TMP_ROOT/list-managed.tsv"
BLOCKED_INSTALL="$TMP_ROOT/blocked-install.log"
BLOCKED_REINSTALL="$TMP_ROOT/blocked-reinstall.log"
BLOCKED_UNINSTALL="$TMP_ROOT/blocked-uninstall.log"

run_smoke list >"$LIST_AVAILABLE"
expect_contains "$LIST_AVAILABLE" \
    $'available\t'"$STORE_ID"$'\t' \
    "Pak Rat catalog did not expose the expected available app"
expect_contains "$LIST_AVAILABLE" \
    $'managed=0\tpath='"$INSTALL_PATH" \
    "available app used an unexpected managed/path state"

run_smoke install "$STORE_ID" >/dev/null
run_smoke list >"$LIST_INSTALLED"
expect_contains "$LIST_INSTALLED" \
    $'installed\t'"$STORE_ID"$'\t' \
    "install did not move the app to installed state"
expect_sql "$DB_PATH" \
    "SELECT COUNT(*) FROM pakrat_installs WHERE store_id = '$STORE_ID';" \
    "1"
expect_sql "$DB_PATH" \
    "SELECT COUNT(*) FROM apps WHERE pak_dir = '$INSTALL_PATH';" \
    "1"

rm -rf "$SD_ROOT/$INSTALL_PATH"
run_smoke rescan >/dev/null
run_smoke list >"$LIST_STALE"
expect_contains "$LIST_STALE" \
    $'stale\t'"$STORE_ID"$'\t' \
    "manual app deletion did not produce stale Pak Rat state"
expect_sql "$DB_PATH" \
    "SELECT COUNT(*) FROM pakrat_installs WHERE store_id = '$STORE_ID';" \
    "1"
expect_sql "$DB_PATH" \
    "SELECT COUNT(*) FROM apps WHERE pak_dir = '$INSTALL_PATH';" \
    "0"

run_smoke install "$STORE_ID" >/dev/null
run_smoke list >"$LIST_RESTORED"
expect_contains "$LIST_RESTORED" \
    $'installed\t'"$STORE_ID"$'\t' \
    "restore did not return the app to installed state"

run_managed_smoke list >"$LIST_MANAGED"
expect_contains "$LIST_MANAGED" \
    $'managed=1\tpath='"$INSTALL_PATH" \
    "managed app policy did not mark the target path as blocked"
expect_failure "$BLOCKED_INSTALL" run_managed_smoke install "$STORE_ID"
expect_contains "$BLOCKED_INSTALL" \
    "target path is release-managed" \
    "managed install was not blocked for the expected reason"
if [ -f "$MANAGED_DB_PATH" ]; then
    expect_sql "$MANAGED_DB_PATH" \
        "SELECT COUNT(*) FROM pakrat_installs WHERE store_id = '$STORE_ID';" \
        "0"
fi

printf '{ "managed_apps": [ "%s" ] }\n' "$MANAGED_PATH" \
    >"$PLATFORM_ROOT/manifest.json"
run_smoke list >"$LIST_MANAGED"
expect_contains "$LIST_MANAGED" \
    $'managed=1\tpath='"$INSTALL_PATH" \
    "installed app was not marked managed after manifest update"
expect_failure "$BLOCKED_REINSTALL" run_smoke install "$STORE_ID"
expect_contains "$BLOCKED_REINSTALL" \
    "target path is release-managed" \
    "managed reinstall/update was not blocked for the expected reason"
expect_failure "$BLOCKED_UNINSTALL" run_smoke uninstall "$STORE_ID"
expect_contains "$BLOCKED_UNINSTALL" \
    "target path is release-managed" \
    "managed uninstall was not blocked for the expected reason"
expect_sql "$DB_PATH" \
    "SELECT COUNT(*) FROM pakrat_installs WHERE store_id = '$STORE_ID';" \
    "1"
expect_sql "$DB_PATH" \
    "SELECT COUNT(*) FROM apps WHERE pak_dir = '$INSTALL_PATH';" \
    "1"

echo "Pak Rat state smoke passed:"
cat "$LIST_AVAILABLE"
cat "$LIST_INSTALLED"
cat "$LIST_STALE"
cat "$LIST_RESTORED"
cat "$LIST_MANAGED"
