#!/bin/sh
# Mirror Leaf's theme resolution; all SQLite access is read-only.
ruffle_load_theme() {
    command -v sqlite3 >/dev/null 2>&1 || return 0
    RUFFLE_THEME_DB="${SDCARD_PATH:-/mnt/sdcard}/.umrk/mlp1/library.db"
    RUFFLE_THEME_NAME="${JAWAKA_THEME:-}"
    if [ -z "$RUFFLE_THEME_NAME" ] && [ -r "$RUFFLE_THEME_DB" ]; then
        RUFFLE_THEME_NAME="$(sqlite3 -readonly "$RUFFLE_THEME_DB" "SELECT value FROM settings WHERE key='theme_name';" 2>/dev/null || :)"
    fi
    RUFFLE_THEME_NAME="${RUFFLE_THEME_NAME:-Jawaka-Tabs}"
    case "$RUFFLE_THEME_NAME" in *[!a-zA-Z0-9_\ -]*) RUFFLE_THEME_NAME=Jawaka-Tabs ;; esac
    RUFFLE_THEME_FILE="/mnt/sdcard/.system/leaf/platforms/mlp1/launcher/res/themes/$RUFFLE_THEME_NAME/stylesheet.json"
    if [ -r "$RUFFLE_THEME_FILE" ]; then
        RUFFLE_THEME_BG="$(sqlite3 :memory: "SELECT json_extract(CAST(readfile('$RUFFLE_THEME_FILE') AS TEXT),'$.ui.background_color');" 2>/dev/null || :)"
        RUFFLE_THEME_TEXT="$(sqlite3 :memory: "SELECT json_extract(CAST(readfile('$RUFFLE_THEME_FILE') AS TEXT),'$.ui.text_color');" 2>/dev/null || :)"
        RUFFLE_THEME_HIGHLIGHT="$(sqlite3 :memory: "SELECT json_extract(CAST(readfile('$RUFFLE_THEME_FILE') AS TEXT),'$.ui.highlight_color');" 2>/dev/null || :)"
        RUFFLE_THEME_SELECTED="$(sqlite3 :memory: "SELECT json_extract(CAST(readfile('$RUFFLE_THEME_FILE') AS TEXT),'$.ui.highlight_text_color');" 2>/dev/null || :)"
        [ -z "$RUFFLE_THEME_BG" ] || export RUFFLE_THEME_BG
        [ -z "$RUFFLE_THEME_TEXT" ] || export RUFFLE_THEME_TEXT
        [ -z "$RUFFLE_THEME_HIGHLIGHT" ] || export RUFFLE_THEME_HIGHLIGHT
        [ -z "$RUFFLE_THEME_SELECTED" ] || export RUFFLE_THEME_SELECTED
    fi
    # Leaf user-theme overrides (colors only).
    if [ -r "$RUFFLE_THEME_DB" ]; then
        RUFFLE_USER_THEME="$(sqlite3 -readonly "$RUFFLE_THEME_DB" "SELECT value FROM settings WHERE key='user_theme';" 2>/dev/null || :)"
        case "$RUFFLE_USER_THEME" in *[!a-zA-Z0-9_\ -]*) RUFFLE_USER_THEME= ;; esac
        RUFFLE_THEME_FILE="/mnt/sdcard/Themes/$RUFFLE_USER_THEME/theme.json"
        if [ -r "$RUFFLE_THEME_FILE" ]; then
            for RUFFLE_THEME_KEY in underlay text highlight highlight_text; do
                RUFFLE_THEME_VALUE="$(sqlite3 :memory: "SELECT json_extract(CAST(readfile('$RUFFLE_THEME_FILE') AS TEXT),'$.colors.$RUFFLE_THEME_KEY');" 2>/dev/null || :)"
                case "$RUFFLE_THEME_VALUE" in \#??????)
                    case "$RUFFLE_THEME_KEY" in
                        underlay) export RUFFLE_THEME_BG="$RUFFLE_THEME_VALUE" ;;
                        text) export RUFFLE_THEME_TEXT="$RUFFLE_THEME_VALUE" ;;
                        highlight) export RUFFLE_THEME_HIGHLIGHT="$RUFFLE_THEME_VALUE" ;;
                        highlight_text) export RUFFLE_THEME_SELECTED="$RUFFLE_THEME_VALUE" ;;
                    esac ;;
                esac
            done
        fi
        # Leaf applies these persisted appearance roles AFTER the stylesheet
        # and user theme. Omitting this step produced the wrong red/gray menu.
        for RUFFLE_THEME_KEY in bg_color text_color hint_color highlight_color accent_color; do
            RUFFLE_THEME_VALUE="$(sqlite3 -readonly "$RUFFLE_THEME_DB" "SELECT value FROM settings WHERE key='$RUFFLE_THEME_KEY';" 2>/dev/null || :)"
            case "$RUFFLE_THEME_VALUE" in \#??????)
                case "$RUFFLE_THEME_KEY" in
                    bg_color) export RUFFLE_THEME_BG="$RUFFLE_THEME_VALUE" ;;
                    text_color) export RUFFLE_THEME_TEXT="$RUFFLE_THEME_VALUE" ;;
                    hint_color) export RUFFLE_THEME_HINT="$RUFFLE_THEME_VALUE" ;;
                    highlight_color) export RUFFLE_THEME_HIGHLIGHT="$RUFFLE_THEME_VALUE" ;;
                    accent_color) export RUFFLE_THEME_ACCENT="$RUFFLE_THEME_VALUE" ;;
                esac ;;
            esac
        done
        RUFFLE_MENU_PILL="$(sqlite3 -readonly "$RUFFLE_THEME_DB" "SELECT value FROM settings WHERE key='pill_shape_index';" 2>/dev/null || :)"
        case "$RUFFLE_MENU_PILL" in 0|1|2|3) export RUFFLE_MENU_PILL ;; esac
        RUFFLE_THEME_LUMA="${RUFFLE_THEME_HIGHLIGHT:-#993B41}"
        case "$RUFFLE_THEME_LUMA" in \#??????)
            RUFFLE_THEME_RGB="${RUFFLE_THEME_LUMA#\#}"
            case "$RUFFLE_THEME_RGB" in *[!a-fA-F0-9]*) RUFFLE_THEME_RGB=993B41 ;; esac
            RUFFLE_THEME_NUMBER="$((0x$RUFFLE_THEME_RGB))"
            RUFFLE_THEME_LIGHT="$(( ((RUFFLE_THEME_NUMBER >> 16) & 255)*299 + ((RUFFLE_THEME_NUMBER >> 8) & 255)*587 + (RUFFLE_THEME_NUMBER & 255)*114 ))"
            if [ "$RUFFLE_THEME_LIGHT" -gt 140000 ]; then export RUFFLE_THEME_SELECTED='#0E0E0E'; else export RUFFLE_THEME_SELECTED='#ECECEC'; fi ;;
        esac
        # Use the same language-aware font path supplied by Leaf's daemon.
        case "${CAT_FONT_PATH:-}" in /*) RUFFLE_THEME_FONT="$CAT_FONT_PATH" ;;
            '') RUFFLE_THEME_FONT= ;;
            *) RUFFLE_THEME_FONT="/mnt/sdcard/.system/leaf/platforms/mlp1/launcher/res/$CAT_FONT_PATH" ;;
        esac
        if [ -n "$RUFFLE_THEME_FONT" ] && [ -r "$RUFFLE_THEME_FONT" ]; then export RUFFLE_MENU_FONT="$RUFFLE_THEME_FONT"; fi
    fi
    return 0
}
