#!/bin/sh
# Apps entry: immediately play the bundled probe, without a file browser.
set -eu
PAK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
export RUFFLE_PROBE_APP_MODE=1
exec "$PAK_DIR/emulators/ruffle-probe/launch-game.sh" "$@"
