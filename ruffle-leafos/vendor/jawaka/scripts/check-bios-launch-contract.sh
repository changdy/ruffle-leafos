#!/usr/bin/env bash
# Shallow wiring check for the Saturn BIOS caller/wrapper contract agreed in
# umrk-workspace/plans/bios-selection-menu.md. Behavioral coverage lives in
# bios-test, in Yabasanshiro-standalone's smoke-launch-wrapper, and on the
# device; this catches regressions at the main.c call sites those cannot reach.
#
# What it guards:
#   * YABASANSHIRO_BIOS_MODE/FILE are written ONLY in the forked child, so the
#     long-lived daemon environment never carries a game's choice and no
#     non-Saturn child inherits one.
#   * The choice is resolved and refused BEFORE the display handoff, so a stale
#     selection cannot strand the user on a black screen.
#   * The request path refuses too, while the launcher is still on screen.
#   * "auto" is never produced by the new UI path.
#   * The launcher persists a logical choice, never a transient absolute path.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DAEMON_SRC="$ROOT_DIR/cmd/jawakad/main.c"
LAUNCHER_SRC="$ROOT_DIR/cmd/jawaka-launcher/main.c"
BIOS_SRC="$ROOT_DIR/internal/launcher/bios.c"

# Prints one function DEFINITION. A forward declaration reaches its ";" before
# any "{", which restarts the search -- otherwise the first prototype in the
# file would silently stand in for the body and every check would pass on it.
body_for() {
    awk -v fn="$1" '
        !inside && $0 ~ "^static .*" fn "\\(" { inside = 1; buffer = "" }
        inside {
            buffer = buffer $0 "\n"
            opens = gsub(/\{/, "{"); depth += opens
            closes = gsub(/\}/, "}"); depth -= closes
            if (opens > 0) seen_open = 1
            if (!seen_open && /;[[:space:]]*$/) { inside = 0; depth = 0; next }
            if (seen_open && depth == 0) { printf "%s", buffer; found = 1; exit }
        }
        END { if (!found) exit 2 }
    ' "$2"
}

fail() {
    printf 'bios-launch-contract: %s\n' "$1" >&2
    exit 1
}

require() {
    [[ "$1" == *"$2"* ]] || fail "$3"
}

require_order() {
    local text="$1" first="$2" second="$3" message="$4"
    local first_line second_line
    first_line="$(grep -nF -- "$first" <<<"$text" | head -1 | cut -d: -f1 || true)"
    second_line="$(grep -nF -- "$second" <<<"$text" | head -1 | cut -d: -f1 || true)"
    [ -n "$first_line" ] && [ -n "$second_line" ] &&
        [ "$first_line" -lt "$second_line" ] || fail "$message"
}

spawn="$(body_for jw__spawn_standalone_emulator "$DAEMON_SRC")"
validate="$(body_for jw__validate_launch_request "$DAEMON_SRC")"
resolve="$(body_for jw__resolve_launch_bios "$DAEMON_SRC")"

# 1. The env contract lives in exactly one function, and inside its child branch.
env_sites="$(grep -c 'YABASANSHIRO_BIOS_MODE\|YABASANSHIRO_BIOS_FILE' \
    "$DAEMON_SRC" || true)"
[ "$env_sites" -gt 0 ] || fail "jawakad no longer writes the BIOS env contract"
spawn_env_sites="$(grep -c 'YABASANSHIRO_BIOS_MODE\|YABASANSHIRO_BIOS_FILE' \
    <<<"$spawn" || true)"
[ "$env_sites" -eq "$spawn_env_sites" ] ||
    fail "the BIOS env contract is written outside jw__spawn_standalone_emulator"

require_order "$spawn" "if (pid == 0) {" 'setenv("YABASANSHIRO_BIOS_MODE"' \
    "the BIOS env contract is applied before fork(), which would leak the choice into the daemon"
require "$spawn" 'unsetenv("YABASANSHIRO_BIOS_MODE")' \
    "a non-Saturn standalone child is no longer cleared of an ambient BIOS mode"
require "$spawn" 'unsetenv("YABASANSHIRO_BIOS_FILE")' \
    "a non-Saturn standalone child is no longer cleared of an ambient BIOS file"

# 2. Refuse before the display handoff, not after.
require_order "$spawn" "jw__bios_launch_error(&bios)" \
    "jw_platform_frontend_ready" \
    "the BIOS check moved after the display handoff"
require_order "$spawn" "jw__bios_launch_error(&bios)" "pid_t pid = fork()" \
    "the BIOS check moved after the launch fork"

# 3. Refuse at request time too, so the launcher is still up to correct it.
require "$validate" "jw__resolve_launch_bios(" \
    "the launch request path no longer resolves the Saturn BIOS choice"
require "$validate" "jw__bios_launch_error(&bios)" \
    "the launch request path no longer refuses an unavailable Saturn BIOS"
require "$validate" "*out_error = bios_error" \
    "the launch request path no longer reports why the BIOS was refused"

# 4. No silent fallback, and no "auto" from the new UI path.
#
# Scoped to the BIOS code rather than the whole launcher. Searching every line
# of main.c meant any unrelated comment containing the word tripped the guard --
# a wallpaper comment describing a status-polarity setting did exactly that --
# which teaches people to reword innocent prose instead of trusting the check.
# The invariant is unchanged: no "auto" in the BIOS grammar or its picker.
bios_ui="$(awk '
    /^static .*jw__bios/ { inside = 1 }
    inside { print; opens = gsub(/\{/, "{"); depth += opens
             closes = gsub(/\}/, "}"); depth -= closes
             if (opens > 0) seen = 1
             if (seen && depth == 0) { inside = 0; seen = 0 } }
' "$LAUNCHER_SRC")"
if grep -q '"auto"' "$BIOS_SRC" || printf '%s' "$bios_ui" | grep -q '"auto"'; then
    fail "auto reappeared in the picker or the BIOS value grammar"
fi
if grep -n 'YABASANSHIRO_BIOS_MODE", "auto"' "$DAEMON_SRC" >/dev/null; then
    fail "jawakad resolves a launch to the legacy auto mode"
fi
require "$resolve" "jw__standalone_target_is_yabasanshiro(target)" \
    "the BIOS choice is no longer scoped to the Saturn standalone"

# 5. The stored value stays logical; the absolute path is launch-local.
apply="$(body_for jw__bios_apply_choice "$LAUNCHER_SRC")"
require "$apply" "jw_bios_choice_format(choice, value, sizeof(value))" \
    "the launcher no longer persists the BIOS choice through the logical formatter"
require "$apply" "JW_CONTENT_SETTING_SATURN_BIOS" \
    "the launcher no longer persists the BIOS choice as a content setting"
if grep -q 'JW_CONTENT_SETTING_SATURN_BIOS, *abs' "$LAUNCHER_SRC"; then
    fail "the launcher persists an absolute BIOS path"
fi

# 6. The picker must not scan a tree just to draw the actions menu.
refresh="$(body_for jw__action_refresh_bios "$LAUNCHER_SRC")"
if [[ "$refresh" == *"jw_bios_list_dir"* ]]; then
    fail "refreshing the actions menu now enumerates BIOS folders"
fi
require "$refresh" "jw_bios_resolve_file(" \
    "the BIOS row no longer validates its selected file"

printf 'Verified the Saturn BIOS child-env, refusal and persistence contract\n'
