#!/bin/sh
# Apps opens the data manager; the bundled SWF is never ticked in this mode.
set -eu
PAK_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)"
export RUFFLE_PROBE_APP_MODE=1
exec "$PAK_DIR/emulators/ruffle-probe/launch-game.sh" "$@"
