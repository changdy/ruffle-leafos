#!/usr/bin/env bash
# Verifies that alias-folder dedup transfers launcher metadata (favorites,
# recents, per-game settings, playtime, last-played) to the canonical survivor
# instead of dropping it. Scenario: a game is favorited/played while it only
# exists in a legacy alias folder (Roms/FC); later a canonical copy (Roms/NES)
# appears; a rescan must collapse to the canonical copy AND keep the metadata.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
JAWAKA_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
UMRK_ROOT="$(cd "$JAWAKA_DIR/.." && pwd)"
BUILD_DIR="${BUILD:-build/phase3-dedup-meta}"
TMP_ROOT="$(mktemp -d "${TMPDIR:-/tmp}/jawaka-dedup-meta.XXXXXX")"
trap 'rm -rf "$TMP_ROOT"' EXIT

SD_ROOT="$TMP_ROOT/sd"
DB_PATH="$TMP_ROOT/library.db"

mkdir -p \
    "$SD_ROOT/.system/leaf/platforms/mlp1/defaults" \
    "$SD_ROOT/Roms/FC"

cp "$UMRK_ROOT/miniloong-launcher-switcher/device/mlp1/defaults/cores.json" \
   "$SD_ROOT/.system/leaf/platforms/mlp1/defaults/cores.json"
cp "$UMRK_ROOT/miniloong-launcher-switcher/device/mlp1/defaults/systems.json" \
   "$SD_ROOT/.system/leaf/platforms/mlp1/defaults/systems.json"

# Only the legacy alias folder exists at first (canonical is Roms/NES).
printf 'rom\n' >"$SD_ROOT/Roms/FC/Mario.nes"

make -C "$JAWAKA_DIR" BUILD="$BUILD_DIR" PLATFORM=mlp1 jawaka-scan-smoke >/dev/null
SCAN="$JAWAKA_DIR/$BUILD_DIR/bin/jawaka-scan-smoke"
run_scan() { UMRK_PLATFORM_PATH="$SD_ROOT/.system/leaf/platforms/mlp1" SDCARD_PATHS="$SD_ROOT" "$SCAN" "$SD_ROOT" "$DB_PATH" >/dev/null; }
q() { sqlite3 "$DB_PATH" "$1"; }

run_scan
alias_id="$(q "SELECT id FROM games WHERE system='FC' AND name='Mario' AND rom_path LIKE '%Roms/FC/%';")"
[ -n "$alias_id" ] || { echo "FAIL: alias FC/Mario not discovered" >&2; exit 1; }

# Favorite/play the alias copy, and give it a per-game core override.
q "INSERT INTO favorites(kind,target_id,added_at) VALUES('game',$alias_id,111);"
q "INSERT INTO recents(kind,target_id,last_opened,duration_s) VALUES('game',$alias_id,222,60);"
q "INSERT INTO game_settings(game_id,key,value,updated_at) VALUES($alias_id,'core','fceumm',333);"
q "UPDATE games SET playtime_s=120, last_played=999 WHERE id=$alias_id;"

# Now the canonical folder appears with the same title.
mkdir -p "$SD_ROOT/Roms/NES"
printf 'rom\n' >"$SD_ROOT/Roms/NES/Mario.nes"
run_scan

# Exactly one FC/Mario remains, under the canonical Roms/NES folder.
count="$(q "SELECT COUNT(*) FROM games WHERE system='FC' AND name='Mario';")"
[ "$count" = "1" ] || { echo "FAIL: expected 1 FC/Mario after dedup, got $count" >&2; exit 1; }
winner_id="$(q "SELECT id FROM games WHERE system='FC' AND name='Mario';")"
winner_path="$(q "SELECT rom_path FROM games WHERE id=$winner_id;")"
case "$winner_path" in
    *Roms/NES/*) ;;
    *) echo "FAIL: survivor is not the canonical copy: $winner_path" >&2; exit 1 ;;
esac
[ "$winner_id" != "$alias_id" ] || { echo "FAIL: alias row was kept instead of canonical" >&2; exit 1; }

# Metadata followed the game to the canonical survivor.
fav="$(q "SELECT COUNT(*) FROM favorites WHERE kind='game' AND target_id=$winner_id;")"
rec="$(q "SELECT COUNT(*) FROM recents WHERE kind='game' AND target_id=$winner_id;")"
gs="$(q "SELECT value FROM game_settings WHERE game_id=$winner_id AND key='core';")"
pt="$(q "SELECT playtime_s FROM games WHERE id=$winner_id;")"
lp="$(q "SELECT last_played FROM games WHERE id=$winner_id;")"
orphans="$(q "SELECT (SELECT COUNT(*) FROM favorites WHERE target_id=$alias_id) + (SELECT COUNT(*) FROM recents WHERE target_id=$alias_id) + (SELECT COUNT(*) FROM game_settings WHERE game_id=$alias_id);")"

[ "$fav" = "1" ] || { echo "FAIL: favorite not transferred (got $fav)" >&2; exit 1; }
[ "$rec" = "1" ] || { echo "FAIL: recent not transferred (got $rec)" >&2; exit 1; }
[ "$gs" = "fceumm" ] || { echo "FAIL: game_settings core not transferred (got '$gs')" >&2; exit 1; }
[ "$pt" = "120" ] || { echo "FAIL: playtime not merged (got $pt)" >&2; exit 1; }
[ "$lp" = "999" ] || { echo "FAIL: last_played not merged (got $lp)" >&2; exit 1; }
[ "$orphans" = "0" ] || { echo "FAIL: orphaned metadata left on deleted alias row (got $orphans)" >&2; exit 1; }

# Conflict path: when both copies already have metadata, the canonical row's
# direct metadata wins, while playtime/last_played still merge from the alias.
printf 'rom\n' >"$SD_ROOT/Roms/FC/Conflict.nes"
printf 'rom\n' >"$SD_ROOT/Roms/NES/Conflict.nes"
q "INSERT INTO games(system,name,rom_path,image_path,playtime_s,last_played) VALUES('FC','Conflict','Roms/FC/Conflict.nes',NULL,7,800);"
q "INSERT INTO games(system,name,rom_path,image_path,playtime_s,last_played) VALUES('FC','Conflict','Roms/NES/Conflict.nes',NULL,10,500);"
alias_conflict_id="$(q "SELECT id FROM games WHERE system='FC' AND name='Conflict' AND rom_path LIKE '%Roms/FC/%';")"
canonical_conflict_id="$(q "SELECT id FROM games WHERE system='FC' AND name='Conflict' AND rom_path LIKE '%Roms/NES/%';")"
[ -n "$alias_conflict_id" ] || { echo "FAIL: conflict alias row was not seeded" >&2; exit 1; }
[ -n "$canonical_conflict_id" ] || { echo "FAIL: conflict canonical row was not seeded" >&2; exit 1; }

q "INSERT INTO favorites(kind,target_id,added_at) VALUES('game',$alias_conflict_id,777);"
q "INSERT INTO favorites(kind,target_id,added_at) VALUES('game',$canonical_conflict_id,444);"
q "INSERT INTO recents(kind,target_id,last_opened,duration_s) VALUES('game',$alias_conflict_id,888,80);"
q "INSERT INTO recents(kind,target_id,last_opened,duration_s) VALUES('game',$canonical_conflict_id,555,5);"
q "INSERT INTO game_settings(game_id,key,value,updated_at) VALUES($alias_conflict_id,'core','alias_core',777);"
q "INSERT INTO game_settings(game_id,key,value,updated_at) VALUES($canonical_conflict_id,'core','canonical_core',444);"

run_scan

conflict_count="$(q "SELECT COUNT(*) FROM games WHERE system='FC' AND name='Conflict';")"
[ "$conflict_count" = "1" ] || { echo "FAIL: expected 1 FC/Conflict after dedup, got $conflict_count" >&2; exit 1; }
conflict_winner_id="$(q "SELECT id FROM games WHERE system='FC' AND name='Conflict';")"
conflict_winner_path="$(q "SELECT rom_path FROM games WHERE id=$conflict_winner_id;")"
case "$conflict_winner_path" in
    *Roms/NES/*) ;;
    *) echo "FAIL: conflict survivor is not the canonical copy: $conflict_winner_path" >&2; exit 1 ;;
esac
[ "$conflict_winner_id" = "$canonical_conflict_id" ] || { echo "FAIL: conflict canonical row id was not preserved" >&2; exit 1; }

conflict_fav_added="$(q "SELECT COALESCE((SELECT added_at FROM favorites WHERE kind='game' AND target_id=$conflict_winner_id), -1);")"
conflict_recent_opened="$(q "SELECT COALESCE((SELECT last_opened FROM recents WHERE kind='game' AND target_id=$conflict_winner_id), -1);")"
conflict_recent_duration="$(q "SELECT COALESCE((SELECT duration_s FROM recents WHERE kind='game' AND target_id=$conflict_winner_id), -1);")"
conflict_core="$(q "SELECT value FROM game_settings WHERE game_id=$conflict_winner_id AND key='core';")"
conflict_pt="$(q "SELECT playtime_s FROM games WHERE id=$conflict_winner_id;")"
conflict_lp="$(q "SELECT last_played FROM games WHERE id=$conflict_winner_id;")"
conflict_orphans="$(q "SELECT (SELECT COUNT(*) FROM favorites WHERE target_id=$alias_conflict_id) + (SELECT COUNT(*) FROM recents WHERE target_id=$alias_conflict_id) + (SELECT COUNT(*) FROM game_settings WHERE game_id=$alias_conflict_id);")"

[ "$conflict_fav_added" = "444" ] || { echo "FAIL: canonical favorite metadata did not win (got $conflict_fav_added)" >&2; exit 1; }
[ "$conflict_recent_opened" = "555" ] || { echo "FAIL: canonical recent timestamp did not win (got $conflict_recent_opened)" >&2; exit 1; }
[ "$conflict_recent_duration" = "5" ] || { echo "FAIL: canonical recent duration did not win (got $conflict_recent_duration)" >&2; exit 1; }
[ "$conflict_core" = "canonical_core" ] || { echo "FAIL: canonical game_settings core did not win (got '$conflict_core')" >&2; exit 1; }
[ "$conflict_pt" = "17" ] || { echo "FAIL: conflict playtime not merged (got $conflict_pt)" >&2; exit 1; }
[ "$conflict_lp" = "800" ] || { echo "FAIL: conflict last_played did not keep newest value (got $conflict_lp)" >&2; exit 1; }
[ "$conflict_orphans" = "0" ] || { echo "FAIL: conflict orphaned metadata left on deleted alias row (got $conflict_orphans)" >&2; exit 1; }

echo "PASS phase3-dedup-metadata-smoke: metadata transferred and conflicts preserve canonical values"
