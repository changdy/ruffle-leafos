#!/usr/bin/env bash
# Pak Rat themes smoke: a local feed with a themes[] lane, driven through the
# real installer. Covers install, update in place, uninstall while selected,
# adoption consent for a hand-made folder, bundled-name refusal, a package
# THEME-1 refuses, a corrupt archive, a bad checksum, the 32-folder cap, a
# withdrawn theme, the store preview cache, and crash recovery across an update.
set -euo pipefail

SCRIPT_DIR="$(CDPATH='' cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(CDPATH='' cd -- "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="${BUILD:-build/pakrat-theme-smoke}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-pakrat-theme.XXXXXX")"
SERVER_PID=""

cleanup() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" >/dev/null 2>&1 || true
        wait "$SERVER_PID" >/dev/null 2>&1 || true
    fi
    rm -rf "$TMP_ROOT"
}
trap cleanup EXIT

for tool in python3 curl sqlite3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "$tool is required for pakrat-theme-smoke" >&2
        exit 2
    }
done

FEED_ROOT="$TMP_ROOT/feed"
SD_ROOT="$TMP_ROOT/sd"
STATE_DIR="$SD_ROOT/.umrk/mlp1"
PLATFORM_ROOT="$SD_ROOT/.system/leaf/platforms/mlp1"
RELEASE_DIR="$SD_ROOT/.system/leaf/releases/leaf-smoke"
DB_PATH="$STATE_DIR/library.db"
THEME_ID="neon-nights"
THEME_DIR="$SD_ROOT/Themes/$THEME_ID"
mkdir -p "$FEED_ROOT/artifacts"

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

PORT="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
BASE_URL="http://127.0.0.1:$PORT/"

# Theme archives and the storefront, built with the standard library only.
cat >"$TMP_ROOT/feed.py" <<'PY'
import hashlib, json, os, struct, sys, zipfile, zlib

def png(width, height, shade):
    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + \
            struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)
    row = b"\x00" + bytes([shade]) * width
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)) + \
        chunk(b"IDAT", zlib.compress(row * height)) + chunk(b"IEND", b"")

def theme_zip(path, theme_id, version, shade, extra=None):
    manifest = {"schema": 1, "id": theme_id, "name": "Neon Nights", "author": "Leaf Smoke",
                "version": version, "min_leaf_version": "0.12.0", "license": "CC0-1.0",
                "colors": {"text": "#F2F2F2"}}
    files = {"theme.json": json.dumps(manifest).encode(), "preview.png": png(960, 720, 40),
             "grid/icons/FC.png": png(512, 512, shade)}
    files.update(extra or {})
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        for name in sorted(files):
            archive.writestr(f"{theme_id}/{name}", files[name])

def entry(base, theme_id, versions, withdrawn=False, sha_override=None,
          preview_sha_override=None):
    history = []
    for version in versions:
        name = f"{theme_id}-{version}.zip"
        data = open(os.path.join(feed, "artifacts", name), "rb").read()
        history.append({"version": version, "min_leaf_version": "0.12.0", "artifact": {
            "url": f"{base}artifacts/{name}", "name": name, "archive": "zip",
            "size": len(data), "installed_size": len(data) * 4,
            "sha256": sha_override or hashlib.sha256(data).hexdigest()}})
    newest = history[0]
    preview = open(os.path.join(feed, "artifacts", "preview.png"), "rb").read()
    return {"id": theme_id, "name": theme_id.title(), "author": "Leaf Smoke",
            "owner_github_id": 1, "summary": "Smoke theme", "license": "CC0-1.0",
            "preview": {"url": f"{base}artifacts/preview.png", "size": len(preview),
                        "sha256": preview_sha_override or hashlib.sha256(preview).hexdigest()},
            "version": newest["version"], "min_leaf_version": "0.12.0",
            "install_name": theme_id, "artifact": newest["artifact"],
            "versions": history, "withdrawn": withdrawn}

command, feed = sys.argv[1], sys.argv[2]
if command == "archives":
    art = os.path.join(feed, "artifacts")
    with open(os.path.join(art, "preview.png"), "wb") as out:
        out.write(png(960, 720, 90))
    # 1.0.0 carries a label 1.1.0 dropped, so an update must remove it.
    theme_zip(os.path.join(art, "neon-nights-1.0.0.zip"), "neon-nights", "1.0.0", 64,
              {"grid/labels/FC.png": png(256, 64, 255)})
    theme_zip(os.path.join(art, "neon-nights-1.1.0.zip"), "neon-nights", "1.1.0", 200)
    theme_zip(os.path.join(art, "sample-1.0.0.zip"), "sample", "1.0.0", 64)
    theme_zip(os.path.join(art, "readme-theme-1.0.0.zip"), "readme-theme", "1.0.0", 64,
              {"README.md": b"not on the allowlist\n"})
    theme_zip(os.path.join(art, "bad-sha-1.0.0.zip"), "bad-sha", "1.0.0", 64)
    with open(os.path.join(art, "corrupt-theme-1.0.0.zip"), "wb") as out:
        out.write(b"PK\x03\x04 this is not a zip archive" * 16)
    theme_zip(os.path.join(art, "gone-theme-1.0.0.zip"), "gone-theme", "1.0.0", 64)
    theme_zip(os.path.join(art, "late-theme-1.0.0.zip"), "late-theme", "1.0.0", 64)
else:
    base, mode = sys.argv[3], sys.argv[4]
    neon = ["1.1.0", "1.0.0"] if mode == "new" else ["1.0.0"]
    themes = [entry(base, "neon-nights", neon), entry(base, "sample", ["1.0.0"]),
              entry(base, "readme-theme", ["1.0.0"]),
              entry(base, "bad-sha", ["1.0.0"], sha_override="f" * 64,
                    preview_sha_override="e" * 64),
              entry(base, "corrupt-theme", ["1.0.0"]),
              entry(base, "gone-theme", ["1.0.0"], withdrawn=True),
              entry(base, "late-theme", ["1.0.0"])]
    with open(os.path.join(feed, "storefront.json"), "w") as out:
        json.dump({"schema": 1, "product": "pak-rat", "apps": [], "themes": themes}, out, indent=1)
PY

python3 "$TMP_ROOT/feed.py" archives "$FEED_ROOT"
write_catalog() {
    python3 "$TMP_ROOT/feed.py" catalog "$FEED_ROOT" "$BASE_URL" "$1"
}
write_catalog old

python3 -m http.server "$PORT" --bind 127.0.0.1 \
    --directory "$FEED_ROOT" >"$TMP_ROOT/http.log" 2>&1 &
SERVER_PID="$!"
for _ in $(seq 1 30); do
    curl -fsS "${BASE_URL}storefront.json" >/dev/null 2>&1 && break
    sleep 0.1
done
curl -fsS "${BASE_URL}storefront.json" >/dev/null

make -C "$JAWAKA_DIR" -s BUILD="$BUILD_DIR" jawaka-pakrat-smoke
BIN="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-pakrat-smoke"

reset_sd() {
    rm -rf "$SD_ROOT"
    mkdir -p "$STATE_DIR/store" "$PLATFORM_ROOT" "$RELEASE_DIR" "$SD_ROOT/Themes/Sample"
    printf '{ "managed_apps": [] }\n' >"$PLATFORM_ROOT/manifest.json"
    printf '%s\n' "$BASE_URL" >"$STATE_DIR/store/dev-catalog-url"
    printf '{ "schema": 1, "version": "0.12.0", "release_id": "leaf-smoke" }\n' \
        >"$STATE_DIR/release.json"
    printf 'Sample\n' >"$RELEASE_DIR/bundled-themes.txt"
    printf '{ "name": "Sample" }\n' >"$SD_ROOT/Themes/Sample/theme.json"
}

run_smoke() {
    "$BIN" --platform mlp1 --sdcard-root "$SD_ROOT" "$@"
}

# run_expect <rc> <output file> <command...>
run_expect() {
    local want="$1" out="$2"
    shift 2
    set +e
    "$@" >"$out" 2>&1
    local rc=$?
    set -e
    [ "$rc" -eq "$want" ] || {
        cat "$out" >&2
        fail "expected exit $want from '$*', got $rc"
    }
}

expect_line() {
    grep -F -- "$2" "$1" >/dev/null || {
        cat "$1" >&2
        fail "missing '$2'"
    }
}

theme_version() {
    python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["version"])' \
        "$THEME_DIR/theme.json" 2>/dev/null || echo "<none>"
}

sql() {
    sqlite3 "$DB_PATH" "$1"
}

SCENARIOS=0
scenario() {
    SCENARIOS=$((SCENARIOS + 1))
    echo "== $1"
}

scenario "catalog lists themes on any platform"
reset_sd
run_expect 0 "$TMP_ROOT/list.out" run_smoke list
expect_line "$TMP_ROOT/list.out" $'available\tneon-nights\t1.0.0'
expect_line "$TMP_ROOT/list.out" $'managed=0\tpath=Themes/neon-nights'
expect_line "$TMP_ROOT/list.out" $'kind=theme\twithdrawn=0'
grep -F $'\tgone-theme\t' "$TMP_ROOT/list.out" >/dev/null &&
    fail "a withdrawn theme that is not installed was listed"
grep -F $'\tsample\t' "$TMP_ROOT/list.out" | grep -F 'managed=1' >/dev/null ||
    fail "a bundled theme name was not reported release-managed"

scenario "install"
run_expect 0 "$TMP_ROOT/install.out" run_smoke install "$THEME_ID"
expect_line "$TMP_ROOT/install.out" "outcome: kind=theme refusal=none themes_changed=1 theme_updated=0"
[ "$(theme_version)" = "1.0.0" ] || fail "install: theme.json version $(theme_version)"
[ -f "$THEME_DIR/.pakrat-commit" ] || fail "install: commit marker missing"
[ -f "$THEME_DIR/grid/icons/FC.png" ] || fail "install: art missing"
[ -f "$THEME_DIR/grid/labels/FC.png" ] || fail "install: label missing"
[ "$(sql "SELECT kind||'|'||platform||'|'||install_path||'|'||version FROM pakrat_installs WHERE store_id='$THEME_ID';")" = \
  "theme|any|Themes/neon-nights|1.0.0" ] || fail "install: ownership row wrong"
[ -z "$(ls -A "$STATE_DIR/store/staging" 2>/dev/null)" ] || fail "install: staging left behind"
run_expect 0 "$TMP_ROOT/list-installed.out" run_smoke list
expect_line "$TMP_ROOT/list-installed.out" $'installed\tneon-nights\t1.0.0\tinstalled=1.0.0'

scenario "update in place keeps the folder"
old_icon_sum="$(cksum <"$THEME_DIR/grid/icons/FC.png")"
write_catalog new
run_expect 0 "$TMP_ROOT/list-update.out" run_smoke list
expect_line "$TMP_ROOT/list-update.out" $'update_available\tneon-nights\t1.1.0'
run_expect 0 "$TMP_ROOT/update.out" run_smoke install "$THEME_ID"
expect_line "$TMP_ROOT/update.out" "themes_changed=1 theme_updated=1"
[ "$(theme_version)" = "1.1.0" ] || fail "update: theme.json version $(theme_version)"
[ "$(cksum <"$THEME_DIR/grid/icons/FC.png")" != "$old_icon_sum" ] || fail "update: art did not change"
[ ! -e "$THEME_DIR/grid/labels/FC.png" ] || fail "update: a file the new version dropped remains"
[ ! -e "$SD_ROOT/Themes/.pakrat-rollback-$THEME_ID" ] || fail "update: rollback left behind"

scenario "uninstall while selected clears the selection"
sql "INSERT INTO settings(key,value) VALUES('user_theme','$THEME_ID') ON CONFLICT(key) DO UPDATE SET value=excluded.value;"
run_expect 0 "$TMP_ROOT/uninstall.out" run_smoke uninstall "$THEME_ID"
expect_line "$TMP_ROOT/uninstall.out" "outcome: kind=theme refusal=none themes_changed=1 theme_updated=0 selection_cleared=1 theme_dir=neon-nights"
[ ! -e "$THEME_DIR" ] || fail "uninstall: folder remains"
[ "$(sql "SELECT value FROM settings WHERE key='user_theme';")" = "" ] || fail "uninstall: selection kept"
[ "$(sql "SELECT COUNT(*) FROM pakrat_installs WHERE store_id='$THEME_ID';")" = "0" ] || fail "uninstall: row kept"
[ -d "$SD_ROOT/Themes/Sample" ] || fail "uninstall: touched a bundled theme"

scenario "a hand-made folder of the same name needs adoption consent"
mkdir -p "$THEME_DIR"
printf '{ "name": "Mine" }\n' >"$THEME_DIR/theme.json"
run_expect 1 "$TMP_ROOT/adopt-refused.out" run_smoke install "$THEME_ID"
expect_line "$TMP_ROOT/adopt-refused.out" "refusal=needs-adoption"
grep -F '"Mine"' "$THEME_DIR/theme.json" >/dev/null || fail "adoption: folder changed without consent"
run_expect 0 "$TMP_ROOT/adopt.out" run_smoke adopt "$THEME_ID"
[ "$(theme_version)" = "1.1.0" ] || fail "adoption: theme not installed"
expect_line "$TMP_ROOT/adopt.out" "theme_updated=1"

scenario "bundled theme names are refused"
run_expect 1 "$TMP_ROOT/reserved.out" run_smoke install sample
expect_line "$TMP_ROOT/reserved.out" "refusal=reserved-name"
[ "$(cat "$SD_ROOT/Themes/Sample/theme.json")" = '{ "name": "Sample" }' ] || fail "reserved: Sample changed"

scenario "a package THEME-1 refuses never reaches Themes/"
run_expect 1 "$TMP_ROOT/invalid.out" run_smoke install readme-theme
expect_line "$TMP_ROOT/invalid.out" "refusal=invalid-theme"
expect_line "$TMP_ROOT/invalid.out" "reasons=theme-unknown-file"
[ ! -e "$SD_ROOT/Themes/readme-theme" ] || fail "invalid: folder created"
[ -z "$(ls -A "$STATE_DIR/store/staging" 2>/dev/null)" ] || fail "invalid: staging left behind"

scenario "a checksum mismatch is refused"
run_expect 1 "$TMP_ROOT/sha.out" run_smoke install bad-sha
expect_line "$TMP_ROOT/sha.out" "artifact SHA-256 mismatch"
expect_line "$TMP_ROOT/sha.out" "refusal=checksum"
[ ! -e "$SD_ROOT/Themes/bad-sha" ] || fail "sha: folder created"

scenario "a corrupt archive is refused as an invalid theme"
run_expect 1 "$TMP_ROOT/corrupt.out" run_smoke install corrupt-theme
expect_line "$TMP_ROOT/corrupt.out" "refusal=invalid-theme"
expect_line "$TMP_ROOT/corrupt.out" "reasons=theme-malformed-archive"
[ ! -e "$SD_ROOT/Themes/corrupt-theme" ] || fail "corrupt: folder created"

scenario "store previews are verified and cached by checksum"
PREVIEW_SHA="$(shasum -a 256 "$FEED_ROOT/artifacts/preview.png" | cut -d' ' -f1)"
PREVIEW_PATH="$STATE_DIR/store/previews/$PREVIEW_SHA.png"
preview_requests() {
    grep -c 'GET /artifacts/preview.png' "$TMP_ROOT/http.log" || true
}
run_expect 0 "$TMP_ROOT/preview.out" run_smoke preview "$THEME_ID"
expect_line "$TMP_ROOT/preview.out" "preview: $PREVIEW_PATH"
cmp -s "$PREVIEW_PATH" "$FEED_ROOT/artifacts/preview.png" || fail "preview: cached bytes differ"
before="$(preview_requests)"
[ "$before" -ge 1 ] || fail "preview: the server never saw the download"
run_expect 0 "$TMP_ROOT/preview-cached.out" run_smoke preview "$THEME_ID"
[ "$(preview_requests)" = "$before" ] || fail "preview: a cached preview was downloaded again"
printf 'damaged' >"$PREVIEW_PATH"
run_expect 0 "$TMP_ROOT/preview-repaired.out" run_smoke preview "$THEME_ID"
cmp -s "$PREVIEW_PATH" "$FEED_ROOT/artifacts/preview.png" || fail "preview: damaged cache was trusted"
run_expect 1 "$TMP_ROOT/preview-bad.out" run_smoke preview bad-sha
expect_line "$TMP_ROOT/preview-bad.out" "preview: refused"
[ ! -e "$STATE_DIR/store/previews/$(printf 'e%.0s' $(seq 64)).png" ] || fail "preview: bad checksum cached"
[ -z "$(find "$STATE_DIR/store/previews" -name '*.download*')" ] || fail "preview: download left behind"

scenario "a withdrawn theme cannot be installed"
run_expect 1 "$TMP_ROOT/withdrawn.out" run_smoke install gone-theme
expect_line "$TMP_ROOT/withdrawn.out" "refusal=withdrawn"

scenario "Themes/ past the launcher's 32-folder cap"
for i in $(seq -w 1 30); do mkdir -p "$SD_ROOT/Themes/filler-$i"; done
# Sample + neon-nights + 30 fillers = 32.
run_expect 0 "$TMP_ROOT/list-full.out" run_smoke list
expect_line "$TMP_ROOT/list-full.out" $'kind=theme\twithdrawn=0\tslots_full=1'
run_expect 1 "$TMP_ROOT/cap.out" run_smoke install late-theme
expect_line "$TMP_ROOT/cap.out" "refusal=theme-limit"
run_expect 0 "$TMP_ROOT/cap-update.out" run_smoke install "$THEME_ID"
rm -rf "$SD_ROOT/Themes/filler-"*

scenario "a crash mid-update rolls back to the running theme"
reset_sd
write_catalog old
run_expect 0 "$TMP_ROOT/crash-install.out" run_smoke install "$THEME_ID"
write_catalog new
set +e
JW_PAKRAT_FAULT_AT=after-promote run_smoke install "$THEME_ID" >"$TMP_ROOT/crash.out" 2>&1
crash_rc=$?
set -e
[ "$crash_rc" -eq 42 ] || { cat "$TMP_ROOT/crash.out" >&2; fail "crash: expected exit 42, got $crash_rc"; }
[ "$(theme_version)" = "1.1.0" ] || fail "crash: promote did not happen before the crash"
[ -d "$SD_ROOT/Themes/.pakrat-rollback-$THEME_ID" ] || fail "crash: rollback missing"
run_expect 0 "$TMP_ROOT/recover.out" run_smoke recover
[ "$(theme_version)" = "1.0.0" ] || fail "crash: recovery kept the uncommitted update"
[ ! -e "$SD_ROOT/Themes/.pakrat-rollback-$THEME_ID" ] || fail "crash: rollback left behind"
[ "$(sql "SELECT version FROM pakrat_installs WHERE store_id='$THEME_ID';")" = "1.0.0" ] ||
    fail "crash: record changed"

scenario "a crash after the record keeps the update"
set +e
JW_PAKRAT_FAULT_AT=after-record run_smoke install "$THEME_ID" >"$TMP_ROOT/crash-record.out" 2>&1
crash_rc=$?
set -e
[ "$crash_rc" -eq 42 ] || { cat "$TMP_ROOT/crash-record.out" >&2; fail "record crash: exit $crash_rc"; }
run_expect 0 "$TMP_ROOT/recover-record.out" run_smoke recover
[ "$(theme_version)" = "1.1.0" ] || fail "record crash: committed update lost"
[ ! -e "$SD_ROOT/Themes/.pakrat-rollback-$THEME_ID" ] || fail "record crash: rollback left behind"

scenario "an interrupted first install is swept"
run_expect 0 "$TMP_ROOT/uninstall-2.out" run_smoke uninstall "$THEME_ID"
set +e
JW_PAKRAT_FAULT_AT=after-promote run_smoke install "$THEME_ID" >"$TMP_ROOT/crash-first.out" 2>&1
crash_rc=$?
set -e
[ "$crash_rc" -eq 42 ] || { cat "$TMP_ROOT/crash-first.out" >&2; fail "first install crash: exit $crash_rc"; }
[ -d "$THEME_DIR" ] || fail "first install crash: nothing promoted"
run_expect 0 "$TMP_ROOT/recover-first.out" run_smoke recover
[ ! -e "$THEME_DIR" ] || fail "first install crash: uncommitted folder kept"
[ -d "$SD_ROOT/Themes/Sample" ] || fail "first install crash: sweep touched Sample"

echo "Pak Rat theme smoke passed ($SCENARIOS scenarios)"
