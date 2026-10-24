#!/usr/bin/env bash
# CAT-1 end-to-end smoke over a REAL release payload.
#
# The unit test in internal/catalog/effective_test.c works on a synthetic
# three-file catalog. This one answers the question that test cannot: does a
# real cores.json/systems.json/info payload load byte-for-byte identically
# through a published generation as it does straight from release defaults?
# Phase 1 promises zero behavior change, and "the catalog still parses" is
# not the same claim as "the catalog is the same catalog".
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD:-build/catalog-generation-smoke}"
PAYLOAD="${PAYLOAD:-}"
TMP_ROOT="$(mktemp -d /tmp/jw-catgen.XXXXXX)"

cleanup() {
    status=$?
    rm -rf "$TMP_ROOT"
    exit "$status"
}
trap cleanup EXIT

if [ -z "$PAYLOAD" ]; then
    # Newest built mlp1 release payload in the sibling Leaf checkout.
    PAYLOAD="$(ls -td "$ROOT_DIR"/../Leaf/build/release/sd-*/.system/leaf/releases/*/platforms/mlp1 \
        2>/dev/null | head -1 || true)"
fi
if [ -z "$PAYLOAD" ] || [ ! -f "$PAYLOAD/defaults/systems.json" ]; then
    echo "catalog-generation-smoke: no release payload found; pass PAYLOAD=<platform dir>" >&2
    exit 2
fi
echo "payload: $PAYLOAD"

PLATFORM_DIR="$TMP_ROOT/platform"
STATE_DIR="$TMP_ROOT/state"
mkdir -p "$PLATFORM_DIR/defaults" "$STATE_DIR"
cp -R "$PAYLOAD/info" "$PLATFORM_DIR/info"
for f in "$PAYLOAD"/defaults/*; do
    cp "$f" "$PLATFORM_DIR/defaults/"
done

# The native build reports platform "mac", and catalog load refuses a
# platform mismatch. Retarget the copies so the smoke exercises the real
# payload's SHAPE and SIZE without needing a cross build.
python3 - "$PLATFORM_DIR" <<'PY'
import json, sys, pathlib
base = pathlib.Path(sys.argv[1], "defaults")
for name in ("systems.json", "cores.json", "cores.v2.json"):
    path = base / name
    if not path.exists():
        continue
    doc = json.loads(path.read_text())
    doc["platform"] = "mac"
    for core in doc.get("cores", []):
        core["platforms"] = ["mac"]
    path.write_text(json.dumps(doc, indent=2))
PY

cat > "$STATE_DIR/release.json" <<'EOF'
{"schema": 1, "version": "0.11.0", "release_id": "smoke-0001"}
EOF

make -C "$ROOT_DIR" -s BUILD="$BUILD_DIR" jawaka-catalog-smoke jawaka-scan-smoke
SMOKE="$ROOT_DIR/$BUILD_DIR/bin/jawaka-catalog-smoke"

export UMRK_PLATFORM_PATH="$PLATFORM_DIR"
export UMRK_INTERNAL_DATA_PATH="$STATE_DIR"
CATALOG_DIR="$STATE_DIR/catalog"

# Every system in the payload, not a hand-picked one: a regression that only
# touches an unusual system is exactly the kind this smoke exists to catch.
SYSTEMS="$(python3 -c "
import json,sys
doc=json.load(open('$PLATFORM_DIR/defaults/systems.json'))
print(' '.join(s['id'] for s in doc['systems']))")"
echo "systems: $(echo "$SYSTEMS" | wc -w | tr -d ' ')"

dump_all() {
    for system in $SYSTEMS; do
        echo "== $system"
        "$SMOKE" "$TMP_ROOT" "$system" "$PLATFORM_DIR/cores" "$PLATFORM_DIR" || true
    done
}

echo "--- before publication (release defaults) ---"
dump_all > "$TMP_ROOT/before.txt"
test ! -e "$CATALOG_DIR/current" || { echo "FAIL: selector exists before publish" >&2; exit 1; }

# jawakad owns publication; drive the same entry point it calls.
cat > "$TMP_ROOT/publish.c" <<'EOF'
#include "internal/retroarch/catalog.h"
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    char generation[128] = "", reason[64] = "";
    int rc = jw_ra_catalog_refresh(argv[1], generation, sizeof(generation),
                                   reason, sizeof(reason));
    printf("rc=%d reason=%s generation=%s\n", rc, reason, generation);
    return rc == 0 ? 0 : 1;
}
EOF
cc -std=c11 -Wall -Wextra -Wno-unused-parameter -I"$ROOT_DIR" \
   -I"$ROOT_DIR/internal" -I"$ROOT_DIR/third_party/cjson" \
   -o "$TMP_ROOT/publish" "$TMP_ROOT/publish.c" \
   "$ROOT_DIR/internal/retroarch/catalog.c" \
   "$ROOT_DIR/internal/catalog/effective.c" \
   "$ROOT_DIR/internal/catalog/json.c" \
   "$ROOT_DIR/internal/catalog/merge.c" \
   "$ROOT_DIR/internal/update/sha256.c" \
   "$ROOT_DIR/internal/platform/leaf_version.c" \
   "$ROOT_DIR/internal/platform/platform_id_mock.c" \
   "$ROOT_DIR/third_party/cjson/cJSON.c"

echo "--- publish ---"
FIRST="$("$TMP_ROOT/publish" "$TMP_ROOT")"
echo "$FIRST"
echo "$FIRST" | grep -q 'reason=published' || { echo "FAIL: expected a first publish" >&2; exit 1; }

GEN="$(cat "$CATALOG_DIR/current")"
echo "selector: $GEN"
GEN="${GEN%$'\n'}"
test -f "$CATALOG_DIR/$GEN/systems.json" || { echo "FAIL: no systems.json" >&2; exit 1; }
test -d "$CATALOG_DIR/$GEN/info" || { echo "FAIL: no info/" >&2; exit 1; }
# Release-owned ancillary data stays put. Repointing one shared defaults
# accessor would have dragged arcade_names.txt along -- or, worse, left the
# arcade lookup pointing at a directory that does not contain it.
test ! -e "$CATALOG_DIR/$GEN/arcade_names.txt" \
    || { echo "FAIL: arcade_names.txt was copied into the generation" >&2; exit 1; }
test -f "$PLATFORM_DIR/defaults/arcade_names.txt" \
    || echo "note: payload has no arcade_names.txt"

echo "--- after publication (effective generation) ---"
dump_all > "$TMP_ROOT/after.txt"

if ! diff -u "$TMP_ROOT/before.txt" "$TMP_ROOT/after.txt" > "$TMP_ROOT/diff.txt"; then
    echo "FAIL: catalog changed after publication" >&2
    head -40 "$TMP_ROOT/diff.txt" >&2
    exit 1
fi
echo "ok: identical core resolution for all $(echo "$SYSTEMS" | wc -w | tr -d ' ') systems"

echo "--- idempotence ---"
SECOND="$("$TMP_ROOT/publish" "$TMP_ROOT")"
echo "$SECOND"
echo "$SECOND" | grep -q 'reason=reused' || { echo "FAIL: expected reuse" >&2; exit 1; }
COUNT="$(find "$CATALOG_DIR" -maxdepth 1 -name 'gen-*' | wc -l | tr -d ' ')"
test "$COUNT" = "1" || { echo "FAIL: $COUNT generations after a no-op recompile" >&2; exit 1; }

echo "--- payload change republishes and the reader follows ---"
# Add a whole SYSTEM rather than editing a field. Core resolution filters out
# cores whose status is not "packaged", so an edit that swapped in an
# unpackaged core would produce an empty result on BOTH sides and assert
# nothing. A system that exists or does not exist is unambiguous -- and it is
# exactly the shape Phase 2 will produce for real.
python3 - "$PLATFORM_DIR" <<'PY'
import json, sys, pathlib
base = pathlib.Path(sys.argv[1], "defaults")
systems = json.loads((base / "systems.json").read_text())
cores = json.loads((base / "cores.json").read_text())
packaged = next(c["id"] for c in cores["cores"]
                if c.get("status") == "packaged" and c.get("type") == "retroarch")
template = dict(systems["systems"][0])
template.update({
    "id": "SMOKETEST",
    "name": "Smoke Test",
    "patterns": ["SMOKETEST"],
    "rom_root": "Roms/SMOKETEST",
    "image_root": "Images/SMOKETEST",
    "default_core": packaged,
    "alternate_cores": [],
})
systems["systems"].append(template)
(base / "systems.json").write_text(json.dumps(systems, indent=2))
print("added SMOKETEST with default core", packaged)
PY

resolves() {
    "$SMOKE" "$TMP_ROOT" "$1" "$PLATFORM_DIR/cores" "$PLATFORM_DIR" >/dev/null 2>&1
}

resolves SMOKETEST && { echo "FAIL: SMOKETEST resolved before republish" >&2; exit 1; }

THIRD="$("$TMP_ROOT/publish" "$TMP_ROOT")"
echo "$THIRD"
echo "$THIRD" | grep -q 'reason=published' || { echo "FAIL: edit did not republish" >&2; exit 1; }
NEWGEN="$(cat "$CATALOG_DIR/current")"
NEWGEN="${NEWGEN%$'\n'}"
test "$NEWGEN" != "$GEN" || { echo "FAIL: selector did not move" >&2; exit 1; }
test -d "$CATALOG_DIR/$GEN" || { echo "FAIL: v1 must not prune the old generation" >&2; exit 1; }
resolves SMOKETEST || { echo "FAIL: reader did not follow the selector swap" >&2; exit 1; }
echo "ok: reader follows the selector swap"

echo "--- fallback goes to RELEASE DEFAULTS, not to the generation ---"
# Revert the release payload WITHOUT republishing, so the generation and the
# release defaults genuinely disagree. Now every fallback below is observable
# rather than assumed.
python3 - "$PLATFORM_DIR" <<'PY'
import json, sys, pathlib
path = pathlib.Path(sys.argv[1], "defaults", "systems.json")
doc = json.loads(path.read_text())
doc["systems"] = [s for s in doc["systems"] if s["id"] != "SMOKETEST"]
path.write_text(json.dumps(doc, indent=2))
PY
resolves SMOKETEST || { echo "FAIL: generation should still win while its stamp is valid" >&2; exit 1; }

echo 'not json' > "$CATALOG_DIR/$NEWGEN/stamp.json"
resolves SMOKETEST && { echo "FAIL: corrupt stamp did not fall back" >&2; exit 1; }
echo "ok: corrupt stamp falls back to release defaults"

python3 - "$CATALOG_DIR/$NEWGEN/stamp.json" <<'PY'
import json, sys, pathlib
path = pathlib.Path(sys.argv[1])
path.write_text(json.dumps({"schema": 1, "platform": "mac",
                            "release_id": "some-other-release"}))
PY
resolves SMOKETEST && { echo "FAIL: post-OTA release_id did not fall back" >&2; exit 1; }
echo "ok: release_id mismatch falls back (the post-OTA case)"

echo 'gen-nonsense' > "$CATALOG_DIR/current"
resolves SMOKETEST && { echo "FAIL: malformed selector did not fall back" >&2; exit 1; }
rm -f "$CATALOG_DIR/current"
resolves SMOKETEST && { echo "FAIL: absent selector did not fall back" >&2; exit 1; }
echo "ok: malformed and absent selectors fall back"

echo "--- release defaults still load after every fallback ---"
dump_all > "$TMP_ROOT/fallback.txt"
diff -q "$TMP_ROOT/before.txt" "$TMP_ROOT/fallback.txt" >/dev/null \
    || { echo "FAIL: release-defaults catalog differs from the pre-publication baseline" >&2;
         diff -u "$TMP_ROOT/before.txt" "$TMP_ROOT/fallback.txt" | head -30 >&2; exit 1; }
echo "ok: fallback catalog is byte-identical to the pre-publication baseline"

echo "catalog-generation-smoke: ok"
