#!/bin/sh
# Compare the vendored RetroArch config parser functions with a fetched
# RetroArch tree. Skips (exit 0) when no tree is present, as on a clean CI
# clone; the vendored copy is what the round-trip test compiles either way.
#
#   RETROARCH_SRC  RetroArch checkout (default: the sibling retroarch-builds
#                  workdir, ../retroarch-builds/workdir/src/RetroArch)
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
ra=${RETROARCH_SRC:-$here/../retroarch-builds/workdir/src/RetroArch}
src=$ra/libretro-common/file/config_file.c
vendored=$here/third_party/retroarch/config_file_parser.inc
if [ ! -f "$src" ]; then
    echo "retroarch-config-parser: no RetroArch source at $ra; drift check skipped"
    exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
sh "$here/scripts/retroarch-config-parser-extract.sh" "$src" >"$tmp/fetched"
sed -n '/^\/\* BEGIN VERBATIM/,/^\/\* END VERBATIM/p' "$vendored" |
    sed '1d;$d' >"$tmp/vendored"
if ! diff -u "$tmp/vendored" "$tmp/fetched"; then
    echo "retroarch-config-parser: vendored parser differs from $src" >&2
    echo "re-vendor it (see third_party/retroarch/README.md) and re-check" >&2
    echo "jw_retroarch_cfg_value_form against the new rules" >&2
    exit 1
fi
echo "retroarch-config-parser: vendored parser matches $src"
