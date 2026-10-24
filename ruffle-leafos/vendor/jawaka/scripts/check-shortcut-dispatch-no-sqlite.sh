#!/usr/bin/env bash
# Direct-call check: none of the shortcut-dispatch functions may call jw_db_*
# or sqlite3_* in its own body.
#
# jw__on_shortcut_chord() runs from the input proxy's evdev callback, on the
# same loop that drains the gamepad. jw_db_* opens a connection, reapplies
# schema and can wait on SQLite's busy timeout, so one call there turns a
# settings lookup into dropped or delayed controller input.
#
# WHAT THIS IS NOT: it does not follow the call graph. It scans the bodies of
# the functions listed below and nothing they call. A database read moved one
# frame deeper -- into a helper these call -- would pass. Real transitive
# analysis needs a compiler or a call-graph tool, and neither is worth adding
# here for four short functions; the honest guard is a shallow one that says so.
#
# The regression it does catch is the one that happened: a lazy read added
# directly inside a hotkey handler, behind a TTL that only expired sometimes,
# where reviewing the dispatch function alone showed nothing wrong.
#
# If these functions grow helpers that touch settings, extend the list or
# replace this with something that can actually see through a call.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT_DIR/cmd/jawakad/main.c"

# The functions the callback dispatches into, and the callback itself. Kept
# explicit rather than parsed: the set is tiny by design, and adding one should
# be a deliberate edit here, which is the moment someone rechecks the
# constraint.
FUNCS=(
    jw__on_shortcut_chord
    jw__input_game_switcher
    jw__on_screenshot_hotkey
    jw__on_record_hotkey
)

status=0
for fn in "${FUNCS[@]}"; do
    body="$(awk -v f="static bool $fn(" '
        index($0, f) { depth = 0; inside = 1 }
        inside {
            print
            n = gsub(/\{/, "{"); depth += n
            m = gsub(/\}/, "}"); depth -= m
            if (depth == 0 && NR > 1 && /\}/) { inside = 0 }
        }
    ' "$SRC")"
    if [ -z "$body" ]; then
        echo "check-shortcut-dispatch: could not find $fn in main.c" >&2
        exit 2
    fi
    if hit="$(printf '%s\n' "$body" | grep -nE 'jw_db_[a-z_]+\(|sqlite3_' || true)"; [ -n "$hit" ]; then
        echo "check-shortcut-dispatch: $fn calls SQLite directly on the input path:" >&2
        printf '%s\n' "$hit" | sed 's/^/  /' >&2
        status=1
    fi
done

if [ "$status" -ne 0 ]; then
    echo "" >&2
    echo "Cache the value and refresh it from jw__tick_feature_flags() on the" >&2
    echo "main loop instead of reading it from the input callback." >&2
    exit 1
fi
echo "shortcut dispatch: no direct SQLite call in the dispatch functions"
