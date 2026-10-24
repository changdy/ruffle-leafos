#!/usr/bin/env bash
# Shallow wiring check for the Phase 6 in-game shader UI. Behavioral coverage
# lives in shader-picker-test and on the device; this catches regressions in the
# main.c call sites that those tests cannot link without the full SDL program.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SRC="$ROOT_DIR/cmd/jawaka-menu/main.c"
DAEMON_SRC="$ROOT_DIR/cmd/jawakad/main.c"
COMMAND_SRC="$ROOT_DIR/internal/retroarch/command.c"

body_for() {
    local source="${2:-$SRC}"
    awk -v fn="$1" '
        !inside && $0 ~ "^static .*" fn "\\(" { inside = 1; found = 1 }
        inside {
            print
            opens = gsub(/\{/, "{"); depth += opens
            closes = gsub(/\}/, "}"); depth -= closes
            if (opens > 0) seen_open = 1
            if (seen_open && depth == 0) exit
        }
        END { if (!found) exit 2 }
    ' "$source"
}

fail() {
    printf 'shader-menu-contract: %s\n' "$1" >&2
    exit 1
}

require() {
    local text="$1" needle="$2" message="$3"
    [[ "$text" == *"$needle"* ]] || fail "$message"
}

reject() {
    local text="$1" needle="$2" message="$3"
    if [[ "$text" == *"$needle"* ]]; then fail "$message"; fi
}

require_order() {
    local text="$1" first="$2" second="$3" message="$4"
    local first_line second_line
    first_line="$(grep -nF -- "$first" <<<"$text" | head -1 | cut -d: -f1 || true)"
    second_line="$(grep -nF -- "$second" <<<"$text" | head -1 | cut -d: -f1 || true)"
    [ -n "$first_line" ] && [ -n "$second_line" ] &&
        [ "$first_line" -lt "$second_line" ] || fail "$message"
}

main_render="$(body_for jw__render_ingame_menu)"
perf_render="$(body_for jw__render_ingame_performance)"
shader_render="$(body_for jw__render_ingame_shader)"
main_row="$(body_for jw__draw_ingame_menu_item)"
perf_row="$(body_for jw__draw_ingame_perf_item)"
shader_row="$(body_for jw__draw_ingame_shader_item)"
require "$shader_render" 'constraint = T(selected->constraints[i]);' \
    "preferred shader BFI caveat bypasses translation"
reject "$shader_render" 'constraint = selected->constraints[i];' \
    "preferred shader BFI caveat displays untranslated text"
for body in "$main_render" "$perf_render" "$shader_render"; do
    require "$body" "cat_draw_list_pane_layered(" \
        "an in-game list no longer uses layered focus rendering"
done
for body in "$main_row" "$perf_row" "$shader_row"; do
    require "$body" "cat_draw_color_lerp(" \
        "an in-game row no longer interpolates focus color"
done

thumb="$(body_for jw__ingame_update_thumb)"
require_order "$thumb" "if (state->list.anim_active)" \
    "jw__slot_thumb_path(state, slot" \
    "cold thumbnail lookup can run before focus settles"

picker="$(body_for jw__ingame_show_shader)"
shader_labels="$(body_for jw__shader_item_label)"
require "$shader_labels" 'T("Advanced RetroArch menu")' \
    "Advanced label is not the requested full text"
reject "$shader_labels" 'Advanced RetroArch menu…' \
    "Advanced label regained an ellipsis"
require "$picker" "int cursor = view.list.cursor;" \
    "activation no longer snapshots the logical cursor"
reject "$picker" "int cursor = view.applied_cursor;" \
    "activation uses the previously applied row instead of the logical cursor"
reject "$picker" "jw_shader_preview_coalescer_" \
    "browsing still schedules an invisible shader preview"
require_order "$picker" \
    "else if (ev.button == CAT_BTN_A || ev.button == CAT_BTN_START)" \
    "jw__shader_apply_selection(" \
    "shader application no longer follows explicit activation"
[ "$(grep -cF 'jw__shader_apply_selection(' <<<"$picker")" -eq 1 ] || \
    fail "shader application must have only the explicit activation call site"
require "$picker" "jw__shader_save_choice(socket_path, state, &view," \
    "non-Off shader activation no longer saves at the selected scope"
reject "$picker" "cursor == jw__shader_custom_index" \
    "the custom row bypasses the shared recommendation save route"
require "$picker" '"shader-settings"' \
    "Advanced no longer requests the direct RetroArch shader handoff"
reject "$picker" "In RetroArch, open Quick Menu -> Shaders." \
    "the obsolete manual-navigation hint returned"

reject "$(<"$SRC")" "jw__ingame_prompt_shader_scope" \
    "the removed automatic scope prompt returned"
reject "$(<"$SRC")" "jw__shader_scope_list" \
    "the separate scope chooser returned"

ui_mode="$(body_for jw__ingame_ui_mode_read)"
reject "$ui_mode" '"shader-scope"' \
    "resident menu still recognizes the removed automatic scope surface"

daemon_action="$(body_for jw__handle_retroarch_action "$DAEMON_SRC")"
require "$daemon_action" 'strcmp(action, "shader-settings") == 0' \
    "daemon no longer accepts the direct shader handoff action"
require "$daemon_action" "jw_ra_open_menu(&ra)" \
    "daemon no longer starts RetroArch's input-flushing menu handoff"

daemon_menu_tap="$(body_for jw__input_menu_tap "$DAEMON_SRC")"
require "$daemon_menu_tap" "if (state->advanced_shader_pending)" \
    "Menu can stack Leaf over the Advanced RetroArch flow"
require "$daemon_menu_tap" "jw_ra_menu_toggle(&client)" \
    "Menu no longer closes the foreground RetroArch shader menu"
require_order "$daemon_menu_tap" \
    "if (state->advanced_shader_pending)" \
    "status.state == JW_RA_STATE_MENU" \
    "ordinary RetroArch-menu passthrough can bypass Advanced shader close handling"
require "$daemon_menu_tap" \
    'jw_log_info("menu tap: forwarding to foreground RetroArch menu")' \
    "Menu cannot reach RetroArch's native binding listener"

daemon_tick="$(body_for jw__tick_advanced_shader "$DAEMON_SRC")"
require_order "$daemon_tick" \
    "if (status.state == JW_RA_STATE_MENU)" \
    "now + JW_ADVANCED_SHADER_SETTLE_MS" \
    "shader destination can be sent before RetroArch's menu is initialized"
require_order "$daemon_tick" \
    "if (!state->advanced_shader_destination_sent)" \
    "jw_ra_open_shader_menu(&client)" \
    "shader destination no longer waits for the menu settle gate"
require "$daemon_tick" $'if (!state->advanced_shader_menu_seen) {\n        if (now - state->advanced_shader_started_ms >=' \
    "shader handoff no longer times out when RetroArch's menu fails to open"
require "$daemon_tick" $'if (!state->advanced_shader_destination_sent) {\n        jw__advanced_shader_clear(state);\n        return;' \
    "game can resume before the Shaders destination was sent"
require "$daemon_tick" "jw_ra_resume_direct(&client)" \
    "closing Advanced no longer resumes the game"
reject "$daemon_tick" "jw__request_open_in_game_ui(" \
    "closing Advanced reopens Leaf instead of resuming the game"

command_src="$(<"$COMMAND_SRC")"
require "$command_src" 'jw_ra_send_raw(client, "OPEN_MENU SHADERS")' \
    "typed client no longer requests the shader screen"
require "$command_src" "status->state = JW_RA_STATE_MENU" \
    "typed client no longer recognizes RetroArch's menu state"

item_path="$(body_for jw__shader_item_path)"
require "$item_path" "view->picker.original_path" \
    "the custom row no longer preserves the active Advanced preset"

# Fugazi 0.2.0 assembles the ownership-safe resolver, so All RetroArch ships
# enabled. The interlock is now the assembled Fugazi version rather than this
# constant: an older Fugazi cannot resolve a conflict the picker can create.
require "$(<"$SRC")" "#define JW_FUGAZI_RESOLVER_ASSEMBLED true" \
    "the assembly gate no longer reflects the shipped Fugazi resolver"
require "$(body_for jw__shader_scope_cycle)" \
    "jw_shader_picker_scope_enabled(" \
    "scope navigation no longer enforces the assembly gate"
require "$(body_for jw__shader_off_choice)" \
    "jw_shader_picker_scope_enabled(scope, JW_FUGAZI_RESOLVER_ASSEMBLED)" \
    "shader removal no longer enforces the assembly gate"

echo "PASS shader-menu-contract-test"
