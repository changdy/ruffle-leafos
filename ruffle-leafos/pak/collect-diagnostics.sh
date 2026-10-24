#!/bin/sh
set -eu
PAK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
export RUFFLE_PROBE_APP_MODE=1
exec "$PAK_DIR/emulators/ruffle-probe/diagnostics.sh" "$@"
