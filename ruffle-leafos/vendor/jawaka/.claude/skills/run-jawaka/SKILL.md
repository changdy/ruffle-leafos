---
name: run-jawaka
description: Build, launch, and load-check the Jawaka launcher (handheld game launcher built on SDL2 + the sibling Catastrophe library). Use when asked to run, build, start, screenshot, smoke-test, or verify Jawaka — including verifying a theme stylesheet change, a Catastrophe library change, or a launcher-side feature.
---

# Run Jawaka

Jawaka is a C/SDL2 game launcher with three binaries (`jawakad` daemon, `jawaka-launcher`, `jawaka-menu`) that share a Unix socket. It depends on the sibling **Catastrophe** library (`../Catastrophe/include/catastrophe.h` — auto-detected by the Makefile, override with `CATASTROPHE_DIR=`).

**The launcher won't even reach SDL init without jawakad's socket** (see Gotchas), so any "run the launcher" recipe must bring up the daemon first. The driver below handles that.

All paths in this doc are relative to the Jawaka repo root (`<unit>/`).

## Prerequisites

macOS, with Homebrew. The Makefile's `check-sdl` target verifies these:

```
brew install sdl2 sdl2_ttf sdl2_image sqlite pkg-config
```

A sibling checkout of the Catastrophe repo at `../Catastrophe` (or `CATASTROPHE_DIR=` env override).

## Run (agent path) — primary

The smoke driver builds the binaries, spins up `jawakad` in the background, then launches the SDL2 launcher with `JAWAKA_AUTODEMO=1` (renders one frame, then exits) for each theme. It greps the launcher's log for `Catastrophe initialized successfully` + the layout line, and treats anything else as a failure.

```bash
.claude/skills/run-jawaka/smoke.sh                                 # all 4 Jawaka themes
.claude/skills/run-jawaka/smoke.sh Jawaka-Vertical                 # one theme
.claude/skills/run-jawaka/smoke.sh Jawaka-Vertical Catastrophe     # mix (non-regression on Catastrophe themes)
.claude/skills/run-jawaka/smoke.sh --no-build                      # skip make if you've already built
```

Successful output:

```
==> starting jawakad (background)
OK   Jawaka-Vertical  (layout: vertical)
OK   Jawaka-Horizontal  (layout: horizontal)
OK   Jawaka-Tabs  (layout: tabbed)
OK   Jawaka-Coverflow  (layout: coverflow)
==> all 4 theme(s) loaded cleanly
```

Logs land in `/tmp/jawaka-smoke/` — one per theme, plus `build.log` and `jawakad.log`. Override with `JAWAKA_SMOKE_LOG_DIR=`.

**What this covers:** Catastrophe init, stylesheet JSON parsing (including new optional fields and unknown-key tolerance), font loading, layout selection, theme `cat_stylesheet_apply` path. Catches: parser regressions, missing font files, theme not found, segfaults in init/render path.

**What this does NOT cover:** actual pixel rendering. The launcher renders one frame to an offscreen surface and exits; the driver never sees those pixels. Visual changes (palette, glyph shape, contrast, layout positioning) need the human-eye path below.

## Run (human path) — visual verification

To actually see the launcher rendering:

```bash
JAWAKA_THEME=Jawaka-Vertical make run-launcher
# (and again with Jawaka-Horizontal / Jawaka-Tabs / Jawaka-Coverflow)
```

`make run-launcher` does mockgen, sets `CAT_FONTS_DIR` / `CAT_THEMES_DIR` / `JAWAKA_SDCARD_ROOT`, and launches the launcher. **It still needs jawakad** — either run `make run-daemon-only` in another terminal first, or use `make run-daemon` (which launches both with `JAWAKA_AUTODEMO=1` driving a launcher→menu→shutdown cycle).

## Gotchas

- **The launcher exits with `return 1` if jawakad's socket isn't there** (`cmd/jawaka-launcher/main.c` ~line 1976, `jw_ipc_hello` before `cat_init`). The single log line you get is `ERROR could not connect to jawakad ... is the daemon running?` and no Catastrophe init output. This is why the driver spins up `jawakad --daemon-only` first and waits for the socket at `/tmp/jawaka-$USER/jawakad.sock` before testing any theme.
- **In `JAWAKA_AUTODEMO=1` mode**, after the autodemo delay the launcher calls `jw_ipc_open_menu(socket_path)`. When the daemon is up but the menu binary isn't reachable, this can log `ERROR could not connect to jawakad …` during shutdown. That specific message is **whitelisted** in the smoke filter; any other `ERROR`/`FATAL`/segfault still fails the run.
- **No headless screenshot path exists.** SDL2 needs a real display. On macOS, `screencapture` against the SDL window fails with `could not create image from display` unless your terminal has Screen Recording permission. No agent-driven visual diff today — visual changes need a human eyeball or someone to add an `SDL_RenderReadPixels` dump to Catastrophe.
- **Themes live in two places.** `res/themes/Jawaka-*` (Jawaka's own four themes) and `../Catastrophe/res/themes/{Catastrophe,Catastrophe-Demo}` (the library's defaults). The launcher resolves via `CAT_THEMES_DIR` which the smoke script and Makefile both point at the Catastrophe res dir. Jawaka's themes work because the launcher checks its own `res/themes/` first.
- **macOS-only verified.** The Makefile has Linux-target shims (`tg5040`, `tg5050`, `mlp1` cross-compile targets) but this driver only exercises the host build. The launcher itself runs on the handheld via the mlp1 toolchain.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `FATAL: Catastrophe not found` | Clone Catastrophe alongside Jawaka, or `export CATASTROPHE_DIR=/path/to/Catastrophe`. |
| `FAIL <theme> (no Catastrophe init marker)` and log shows only IPC error | Daemon didn't start. Check `/tmp/jawaka-smoke/jawakad.log` — usually a stale lockfile or a previous `jawakad` still running (`pgrep -fl jawakad`). |
| `FAIL <theme>` and log shows `theme 'X' not found, using defaults` | Theme directory missing from `res/themes/` or `../Catastrophe/res/themes/`. Check spelling — names are case-sensitive. |
| Build fails with `SDL2/SDL.h: No such file or directory` | `brew install sdl2 sdl2_ttf sdl2_image` and re-run. |
| `screencapture: could not create image from display` (human path) | Grant your terminal Screen Recording permission in System Settings → Privacy & Security → Screen Recording. |

## Test suite

Separate from the smoke driver. The repo's own checks:

```bash
make jawaka-scan-smoke                    # builds the smoke binary
bash scripts/phase3-fixture-scan-smoke.sh # runs it against fixtures
```

Not used by this skill; mention only if a PR touches `internal/discovery/`.
