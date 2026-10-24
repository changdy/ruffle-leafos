#!/usr/bin/env bash
# smoke.sh — build + load-check Jawaka launcher across themes.
#
# This is the driver script that lets an agent verify Jawaka without GUI
# access. It spins up jawakad (the daemon) in the background, then
# launches the real SDL2 launcher binary with JAWAKA_AUTODEMO=1 so it
# renders one frame and exits, then greps the log for the success
# markers (Catastrophe init + layout line).
#
# Visual verification of pixels still requires a human — see SKILL.md.
#
# Usage:
#   .claude/skills/run-jawaka/smoke.sh                  # all four Jawaka themes
#   .claude/skills/run-jawaka/smoke.sh Jawaka-Vertical  # one theme
#   .claude/skills/run-jawaka/smoke.sh --no-build       # skip make
set -euo pipefail

# Resolve paths relative to the Jawaka repo root.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
cd "$REPO_ROOT"

# Catastrophe lives as a sibling repo. The Makefile auto-detects it.
CATASTROPHE_DIR="${CATASTROPHE_DIR:-$(cd "$REPO_ROOT/../Catastrophe" 2>/dev/null && pwd || true)}"
if [ -z "$CATASTROPHE_DIR" ] || [ ! -f "$CATASTROPHE_DIR/include/catastrophe.h" ]; then
  echo "FATAL: Catastrophe not found (set CATASTROPHE_DIR to its repo root)" >&2
  exit 2
fi

DO_BUILD=1
THEMES=()
for arg in "$@"; do
  case "$arg" in
    --no-build) DO_BUILD=0 ;;
    -*) echo "unknown flag: $arg" >&2; exit 2 ;;
    *) THEMES+=("$arg") ;;
  esac
done
if [ ${#THEMES[@]} -eq 0 ]; then
  THEMES=(Jawaka-Vertical Jawaka-Horizontal Jawaka-Tabs Jawaka-Coverflow)
fi

LOG_DIR="${JAWAKA_SMOKE_LOG_DIR:-/tmp/jawaka-smoke}"
mkdir -p "$LOG_DIR"

if [ "$DO_BUILD" = "1" ]; then
  echo "==> make jawakad jawaka-launcher jawaka-menu mockgen"
  make jawakad jawaka-launcher jawaka-menu mockgen > "$LOG_DIR/build.log" 2>&1 || {
    echo "FATAL: build failed — see $LOG_DIR/build.log" >&2
    tail -30 "$LOG_DIR/build.log" >&2
    exit 1
  }
fi

LAUNCHER="$REPO_ROOT/build/bin/jawaka-launcher"
DAEMON="$REPO_ROOT/build/bin/jawakad"
for f in "$LAUNCHER" "$DAEMON"; do
  [ -x "$f" ] || { echo "FATAL: $f missing — run without --no-build" >&2; exit 1; }
done

# Resolve where jawakad will put its socket (matches internal/platform/paths.c).
USER_NAME="${USER:-$(id -un)}"
SOCKET="/tmp/jawaka-${USER_NAME}/jawakad.sock"
rm -f "$SOCKET" 2>/dev/null || true

# Spin up the daemon in the background. It needs to NOT autodemo (or it'd
# try to spawn its own launcher and confuse the smoke run).
echo "==> starting jawakad (background)"
JAWAKA_SDCARD_ROOT="$REPO_ROOT/mock-sdcard" \
  JAWAKA_AUTODEMO=0 \
  "$DAEMON" --daemon-only > "$LOG_DIR/jawakad.log" 2>&1 &
DAEMON_PID=$!

cleanup() {
  if kill -0 $DAEMON_PID 2>/dev/null; then
    kill -INT $DAEMON_PID 2>/dev/null || true
    sleep 0.2
    kill -9 $DAEMON_PID 2>/dev/null || true
  fi
  wait $DAEMON_PID 2>/dev/null || true
}
trap cleanup EXIT

# Wait for the socket (up to 5s).
for _ in $(seq 1 50); do
  [ -S "$SOCKET" ] && break
  sleep 0.1
done
if [ ! -S "$SOCKET" ]; then
  echo "FATAL: jawakad socket never appeared at $SOCKET" >&2
  echo "--- jawakad.log (tail) ---" >&2
  tail -20 "$LOG_DIR/jawakad.log" >&2
  exit 1
fi

fail=0
for theme in "${THEMES[@]}"; do
  log="$LOG_DIR/${theme}.log"
  # JAWAKA_AUTODEMO=1: render one frame, autodemo fires → launcher exits.
  # JAWAKA_AUTODEMO_DELAY_MS=600: shorten the wait. Total run ~1.5s.
  CAT_WINDOW_WIDTH=1280 CAT_WINDOW_HEIGHT=720 \
    CAT_FONTS_DIR="$CATASTROPHE_DIR/res" \
    CAT_THEMES_DIR="$CATASTROPHE_DIR/res/themes" \
    JAWAKA_SDCARD_ROOT="$REPO_ROOT/mock-sdcard" \
    JAWAKA_THEME="$theme" \
    JAWAKA_AUTODEMO=1 \
    JAWAKA_AUTODEMO_DELAY_MS=600 \
    "$LAUNCHER" > "$log" 2>&1 &
  pid=$!

  # Watchdog: hard-kill if autodemo didn't fire within 5s.
  ( sleep 5; kill -9 $pid 2>/dev/null || true ) &
  watchdog=$!

  wait $pid 2>/dev/null || true
  kill -9 $watchdog 2>/dev/null || true
  wait $watchdog 2>/dev/null || true

  # Filter: the IPC "could not connect" error is the missing-daemon symptom;
  # other ERRORs/FATALs/segfaults are real failures.
  if grep -qE 'FATAL|Segmentation fault|Bus error' "$log" \
     || grep -E 'ERROR' "$log" | grep -qv 'could not connect to jawakad'; then
    echo "FAIL $theme  ($log)"
    grep -E 'ERROR|FATAL' "$log" | head -3 | sed 's/^/      /'
    fail=$((fail+1))
    continue
  fi
  if ! grep -q "Catastrophe initialized successfully" "$log"; then
    echo "FAIL $theme  (no Catastrophe init marker — $log)"
    fail=$((fail+1))
    continue
  fi
  layout=$(grep -oE "layout: [a-z]+" "$log" | head -1 || echo "layout: ?")
  echo "OK   $theme  ($layout)"
done

if [ "$fail" -gt 0 ]; then
  echo "==> $fail theme(s) failed" >&2
  exit 1
fi
echo "==> all ${#THEMES[@]} theme(s) loaded cleanly"
