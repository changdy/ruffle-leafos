#!/usr/bin/env bash
set -euo pipefail

# Fetches libretro Systematic console icons at a pinned commit and renames
# them to Jawaka system short codes. Safe to re-run: existing files are left
# in place unless FORCE=1 is set. Requires curl.
#
# To keep load on the libretro server (and the rate limiter) low, each unique
# artwork is downloaded ONCE under a primary code; alias codes that share the
# same console art are produced by a local copy, never a second download.

COMMIT="e11d6708b49a893f392b238effc713c6c7cfadef"
# GitHub mirror of the same repo at the same pinned commit. The upstream
# git.libretro.com host rate-limits bulk fetches aggressively (connection
# resets after a handful of requests); the GitHub raw mirror serves the
# identical files reliably. Override with REPO_BASE= to use upstream directly.
REPO_BASE="${REPO_BASE:-https://raw.githubusercontent.com/libretro/retroarch-assets/$COMMIT/xmb/systematic/png}"
BASE="$REPO_BASE"

DEST="$(cd "$(dirname "$0")/.." && pwd)/res/system_icons"
mkdir -p "$DEST"

FORCE="${FORCE:-0}"
RETRIES="${RETRIES:-5}"
DELAY="${DELAY:-2}"          # polite pause between downloads (seconds)

# Distinct downloads: jawaka_code|libretro filename (no .png suffix).
# One entry per unique console artwork.
# PICO8.png is intentionally not listed: it is checked-in artwork rasterized
# from CentralScrutinizer/web/public/platforms/PICO8.svg, not an upstream
# Systematic PNG. Keep it when refreshing this fetched icon set.
declare -a DOWNLOADS=(
  # Nintendo
  "GB|Nintendo - Game Boy"
  "GBC|Nintendo - Game Boy Color"
  "GBA|Nintendo - Game Boy Advance"
  "FC|Nintendo - Nintendo Entertainment System"
  "FDS|Nintendo - Family Computer Disk System"
  "SFC|Nintendo - Super Nintendo Entertainment System"
  "N64|Nintendo - Nintendo 64"
  "NDS|Nintendo - Nintendo DS"
  "VB|Nintendo - Virtual Boy"
  # Sega
  "MD|Sega - Mega Drive - Genesis"
  "32X|Sega - 32X"
  "MS|Sega - Master System - Mark III"
  "GG|Sega - Game Gear"
  "SEGACD|Sega - Mega-CD - Sega CD"
  "SATURN|Sega - Saturn"
  "DC|Sega - Dreamcast"
  # Sony
  "PS|Sony - PlayStation"
  "PSP|Sony - PlayStation Portable"
  # NEC
  "PCE|NEC - PC Engine - TurboGrafx 16"
  "PCECD|NEC - PC Engine CD - TurboGrafx-CD"
  # SNK
  "NEOGEO|SNK - Neo Geo"
  "NGP|SNK - Neo Geo Pocket"
  "NGPC|SNK - Neo Geo Pocket Color"
  # Atari
  "LYNX|Atari - Lynx"
  "ATARI2600|Atari - 2600"
  "PROSYSTEM|Atari - 7800"
  # Other
  "COLECO|Coleco - ColecoVision"
  "VECTREX|GCE - Vectrex"
  "WS|Bandai - WonderSwan"
  "WSC|Bandai - WonderSwan Color"
  "ARCADE|MAME"
  "AMIGA|Commodore - Amiga"
)

# Aliases: alias_code|primary_code. The alias shares the primary's artwork via
# a local copy (the consoles are the same hardware or the same emulator family).
declare -a ALIASES=(
  "NES|FC"                  # NES == FC (Famicom)
  "SNES|SFC"                # SNES == SFC
  "SFC_JP|SFC"              # Super Famicom (JP) == SFC
  "GEN|MD"                  # Genesis == Mega Drive
  "GENESIS|MD"
  "MD32X|32X"               # Mega Drive 32X == 32X
  "PSX|PS"                  # PSX == PlayStation
  "TG16|PCE"                # TurboGrafx-16 == PC Engine
  "MAME|ARCADE"             # MAME family shares the arcade icon
  "MAME2003|ARCADE"
  "MAME2010|ARCADE"
  "SEVENTYEIGHTHUNDRED|PROSYSTEM"   # Atari 7800 (alt id) == PROSYSTEM
)
# Note: PORTS has no console artwork; it falls back to _default.png at runtime.

is_png() {
  # PNG magic: 89 50 4E 47. Guards against saved error/HTML pages.
  [ -s "$1" ] && [ "$(head -c4 "$1" | od -An -tx1 | tr -d ' \n')" = "89504e47" ]
}

failures=()
for entry in "${DOWNLOADS[@]}"; do
  code="${entry%%|*}"
  source_name="${entry#*|}"
  out="$DEST/$code.png"
  if [ "$FORCE" != "1" ] && is_png "$out"; then
    echo "= $code (have it, skipping)"
    continue
  fi
  url="$BASE/$(printf '%s' "$source_name" | sed 's/ /%20/g').png"
  ok=0
  for attempt in $(seq 1 "$RETRIES"); do
    echo "-> $code <- $source_name (try $attempt)"
    if curl --fail --silent --show-error -L "$url" -o "$out.tmp" 2>/dev/null && is_png "$out.tmp"; then
      mv "$out.tmp" "$out"
      ok=1
      break
    fi
    rm -f "$out.tmp"
    sleep $((DELAY * attempt))   # linear backoff (handles 429 rate limiting)
  done
  if [ "$ok" != "1" ]; then
    echo "!! FAILED: $code <- $source_name"
    failures+=("$code|$source_name")
  fi
  sleep "$DELAY"
done

echo ""
echo "Resolving aliases (local copies, no downloads):"
for entry in "${ALIASES[@]}"; do
  alias_code="${entry%%|*}"
  primary="${entry#*|}"
  src="$DEST/$primary.png"
  dst="$DEST/$alias_code.png"
  if is_png "$src"; then
    cp "$src" "$dst"
    echo "  $alias_code <- $primary"
  else
    echo "  !! cannot alias $alias_code: primary $primary.png missing"
    failures+=("$alias_code|alias of $primary")
  fi
done

echo ""
if [ "${#failures[@]}" -gt 0 ]; then
  echo "Completed WITH ${#failures[@]} failure(s):"
  for f in "${failures[@]}"; do echo "  - $f"; done
  exit 1
fi
echo "Done. $(( ${#DOWNLOADS[@]} + ${#ALIASES[@]} )) system icons in $DEST"
echo "Note: _tools.png and _default.png are hand-authored; PICO8.png is a checked-in"
echo "      rasterization of Central Scrutinizer artwork and is not fetched here."
