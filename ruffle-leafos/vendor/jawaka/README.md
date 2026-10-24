# Jawaka

Jawaka is the UMRK launcher stack built on Catastrophe. The normal entrypoint is
`jawakad`, a long-lived coordinator that owns scanning, launch requests, platform
control, process handoff, and the IPC socket. The foreground UI processes are
`jawaka-launcher` and `jawaka-menu`.

Primary target today is the Miniloong Pocket 1 (MLP1). macOS remains the fast
local preview loop with a generated mock SD-card tree.

## Translations

[![中文](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FUtility-Muffin-Research-Kitchen%2FJawaka%2Fmain%2Fi18n%2Fcoverage.json&query=%24.languages.zh_CN.percent&suffix=%25&label=%E4%B8%AD%E6%96%87&color=blue)](i18n/) [![Français](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FUtility-Muffin-Research-Kitchen%2FJawaka%2Fmain%2Fi18n%2Fcoverage.json&query=%24.languages.fr_FR.percent&suffix=%25&label=Fran%C3%A7ais&color=blue)](i18n/) [![Español](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FUtility-Muffin-Research-Kitchen%2FJawaka%2Fmain%2Fi18n%2Fcoverage.json&query=%24.languages.es_MX.percent&suffix=%25&label=Espa%C3%B1ol&color=blue)](i18n/) [![Tiếng Việt](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FUtility-Muffin-Research-Kitchen%2FJawaka%2Fmain%2Fi18n%2Fcoverage.json&query=%24.languages.vi_VN.percent&suffix=%25&label=Ti%E1%BA%BFng%20Vi%E1%BB%87t&color=blue)](i18n/) [![Русский](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FUtility-Muffin-Research-Kitchen%2FJawaka%2Fmain%2Fi18n%2Fcoverage.json&query=%24.languages.ru_RU.percent&suffix=%25&label=%D0%A0%D1%83%D1%81%D1%81%D0%BA%D0%B8%D0%B9&color=blue)](i18n/) [![日本語](https://img.shields.io/badge/dynamic/json?url=https%3A%2F%2Fraw.githubusercontent.com%2FUtility-Muffin-Research-Kitchen%2FJawaka%2Fmain%2Fi18n%2Fcoverage.json&query=%24.languages.ja_JP.percent&suffix=%25&label=%E6%97%A5%E6%9C%AC%E8%AA%9E&color=blue)](i18n/)

Live from [`i18n/coverage.json`](i18n/coverage.json), which `make i18n-check`
keeps in step with the committed translations, so a badge cannot claim coverage
the repo does not have. Percentages are of every extracted key, so they fall when
newly reachable strings are added rather than when a translation regresses.

Starting a language, or finishing one, is [documented for
translators](https://leaf.game/guide/translating/): take `i18n/leaf.pot`, fill in
the `msgstr` lines, and test it on the device before sending it in.

## What Exists

- `jawakad` daemon with Unix-domain-socket, length-prefixed JSON IPC.
- Catastrophe launcher/menu UIs with Recents, Favorites, Games, Apps, and
  Settings surfaces.
- SQLite library database with FTS search, games, apps, recents, favorites, and
  persisted settings.
- Filesystem scanner for `Roms/`, `Images/`, and platform-guarded `Apps/`
  paks.
- Local box art, system icons, multiple launcher themes, game search, and a
  game switcher (Select on the home screen, Menu + Select in a game by default).
- MLP1 platform integration for launch lifecycle, brightness, volume, audio
  output, Wi-Fi, Bluetooth, ADB pin control, boot splash, secondary SD unmount,
  LEDs, sleep, reboot, power off, and Exit to Stock.
- SVC-1 foreground-service supervision, CTL-1 status/control IPC, declarative
  storage/suspend policy stops, process-group cleanup, and a dynamic Settings
  → Services screen.
- RetroArch helpers: `jawaka-retroarch-runner`, `jawaka-retroarchctl`, metadata
  catalog support, shared config reset, command-menu integration, and in-game
  menu flow.
- Support helpers: `jawaka-osd`, `jawaka-platformctl`, `jawaka-ledd`, and
  `jawaka-scan-smoke`.

## Build

Jawaka expects Catastrophe to be available locally. The Makefile uses
`../Catastrophe` when it exists; otherwise set `CATASTROPHE_DIR`.

```sh
export CATASTROPHE_DIR=../Catastrophe
make mockgen
make
```

Run this once after clone if system icons are missing:

```sh
./scripts/fetch-system-icons.sh
```

MLP1 build:

```sh
make mlp1
```

Other device Makefile targets (`tg5040`, `tg5050`, `my355`) are placeholders.

## Run On macOS

The daemon-owned path is the main local workflow:

```sh
make run-daemon
```

`run-daemon` runs a short automated demo. For manual testing:

```sh
make run-daemon-interactive
```

To keep only `jawakad` running and attach UI processes yourself:

```sh
make run-daemon-only
make run-launcher
make run-menu
```

Useful smoke/debug targets:

```sh
make phase3-fixture-scan-smoke
make mlp1-adb-smoke
make mlp1-adb-input-capture
make mlp1-adb-ra-command-smoke
```

The RetroArch command smoke accepts an optional local content file and
`CORE_SO`/`CORE_INFO` overrides. It stages them with RetroArch's FFmpeg runtime
libraries only below a dedicated device
`/tmp` directory, verifies the command channel after content rendering, saves a
gameplay screenshot and verbose log under `build/mlp1-ra-command-smoke/`, then
removes that exact device directory:

```sh
CORE_SO=../Cores-spruce/output/mlp1/cores/mupen64plus_next_libretro.so \
CORE_INFO=../Cores-spruce/output/mlp1/info/mupen64plus_next_libretro.info \
make mlp1-adb-ra-command-smoke CONTENT=/absolute/path/to/game.z64
```

An isolated RetroArch system directory can be staged with `SYSTEM_DIR`, for
example when a core needs firmware. Its contents are removed with the rest of
the temporary device directory:

```sh
SYSTEM_DIR=/absolute/path/to/retroarch-system \
make mlp1-adb-ra-command-smoke CONTENT=/absolute/path/to/game.chd
```

## Stage To MLP1

Device staging is owned by the sibling `Leaf` repo. From `../Leaf`:

```sh
make stage-jawaka DEVICE=mlp1       # launcher payload only
make stage DEVICE=mlp1              # launcher + RetroArch/cores + apps
make stage-refresh DEVICE=mlp1      # full stage, then restart GUI stack
make refresh-jawaka DEVICE=mlp1     # restart GUI stack only
```

Leaf assembles Jawaka into:

```text
$SDCARD_PATH/.system/leaf/platforms/mlp1/launcher/
  env.sh
  bin/loong_pangu              # staged jawakad
  bin/jawaka-launcher
  bin/jawaka-menu
  bin/jawaka-osd
  bin/jawaka-platformctl
  bin/jawaka-retroarchctl
  bin/jawaka-retroarch-runner
  bin/jawaka-ledd
  res/
```

Activation is controlled by:

```text
$SDCARD_PATH/.system/leaf/platforms/mlp1/enabled
```

Jawaka's ADB restore intent marker lives at:

```text
$SDCARD_PATH/.umrk/mlp1/adb-enabled
```

## Controls

Desktop testing follows Catastrophe's default mapping:

```text
Arrows       d-pad navigation
A           face button A / select
B           face button B / back
X           search or context action
Y           refresh/rescan or secondary action
Enter       Start
Space       Select / game switcher
H           Menu
; / t       L2 / R2 tab switching
Q           desktop-only daemon shutdown
```

On MLP1, Jawaka reads the Loong Gamepad through its platform input proxy.

### In-game shortcuts (MLP1)

Leaf's in-game actions are Menu chords. **Menu is the modifier and is fixed**;
the second button is the user's, set in **Settings -> Controls & Feedback ->
In-game Shortcuts**, which also offers Disabled.

| Action | Default |
| --- | --- |
| Game Switcher | Menu + Select |
| Screenshot | Menu + L1 |
| Recording | Menu + R1 |

Two enabled actions cannot share a button; the picker dims a button another
action holds and names the owner. Bindings are stored as symbolic names
(`l1`, `select`, `disabled`) rather than numbers, because evdev codes, SDL
indices and RetroArch joypad ids disagree about which number a button is --
`internal/platform/input_shortcuts.h` owns that vocabulary and the MLP1 input
proxy alone maps it to evdev codes.

Settings shows physical silkscreen names, not logical roles: the point of the
screen is avoiding collisions with the buttons a user bound in RetroArch, and
those are physical.

RetroArch has its own hotkeys behind its own modifier, which defaults to Menu as
well (`input_enable_hotkey_btn = "5"`) and is user-owned. **Where the two
collide, Leaf wins** -- but only when its handler actually claims the chord. An
unbound, disabled, or contextually unavailable action forwards the whole chord to
RetroArch unchanged, so a declined screenshot still reaches whatever the user
bound there.

## Runtime Environment

Jawaka consumes the shared Leaf runtime contract from:

```text
$SDCARD_PATH/.system/leaf/platforms/$PLATFORM/launcher/env.sh
```

Important variables:

| Variable | Purpose |
| --- | --- |
| `CATASTROPHE_DIR` | Catastrophe checkout used by local builds |
| `PLATFORM` / `DEVICE` | platform id, usually `mac` or `mlp1` |
| `SDCARD_PATH` | mock or device SD-card root |
| `SDCARD_PATHS` | colon-separated SD roots, primary first |
| `USERDATA_PATHS`, `SHARED_USERDATA_PATHS` | PATH-2 per-source durable roots, aligned with `SDCARD_PATHS` |
| `ROMS_PATHS`, `IMAGES_PATHS`, `MUSIC_PATHS`, `VIDEO_PATHS`, `APPS_PATHS` | indexed plural content roots, aligned with `SDCARD_PATHS` |
| `UMRK_RUNTIME_PATH` | runtime socket and scratch directory |
| `UMRK_PLATFORM_PATH` / `SYSTEM_PATH` | platform payload root |
| `UMRK_INTERNAL_DATA_PATH` | launcher-owned state root |
| `UMRK_LAUNCHER_PATH` | launcher bundle root |
| `UMRK_RETROARCH_BIN`, `CORES_PATH`, `INFO_PATH` | RetroArch runtime paths |
| `UMRK_RETROARCH_SHADERS_DIR` | release-managed Leaf shader source bundle |
| `UMRK_RETROARCH_USER_SHADERS_DIR` | durable RetroArch shader browser and updater root |
| `JAWAKA_THEME` | local preview theme override |
| `JAWAKA_AUTODEMO` | `1` enables the short automated run-daemon flow |
| `JAWAKA_AUTODEMO_DELAY_MS` | auto-demo delay, default `1200` |

When the complete PATH-2 lists validate, `hello-ok.features` includes
`source-paths-v2`; malformed, incomplete, duplicate, or misaligned lists keep
that capability absent even if `UMRK_ENV_VERSION=2` was inherited.

`JAWAKA_SDCARD_ROOT`, `JAWAKA_RUNTIME_DIR`, `JAWAKA_RETROARCH_BIN`, and
`JAWAKA_RETROARCH_CORES_DIR` remain compatibility aliases. New scripts and docs
should prefer the `SDCARD_PATH` / `UMRK_*` variables from the umbrella runtime
contract.

## SD Layout

Jawaka scans content from the Leaf/UMRK SD shape:

```text
Roms/<SYSTEM_CODE>/<title>.<ext>
Images/<SYSTEM_CODE>/<title>.png
Music/<artist>/<album>/<track>
Apps/<platform>/<Name>.pak/
Apps/shared/<Name>.pak/
BIOS/
  SATURN/                       # Saturn firmware
Saves/
States/
Cheats/
.umrk/<platform>/library.db
.umrk/<platform>/services-control.db
```

`services-control.db` owns persistent `Start with Leaf` intent separately from
session Run/Stop state. Its schema-v2 migration records Release A's one-time
legacy SSH decision atomically with the enablement value: a valid existing SSH
config enables once, while a clean or invalid install stays disabled. The
marker survives later config restoration and never reads or changes host keys.

`<SYSTEM_CODE>` is matched against the platform `systems.json` catalog, where
each user system has one canonical public folder plus legacy aliases in its
`patterns[]`. Alias folders (`Roms/FC` → NES, `Roms/MGBA` → GBA, …) fold into the
one canonical system, so they show as a single library; when the same title
exists under both an alias and the canonical folder, discovery keeps one entry
and prefers the canonical-folder copy. Emulator variants are a core choice under
that one system, not separate folders.

For app paks, `pak.json.platform` must match the containing platform directory
or be `shared`. Icon paths are relative to the containing `.pak` directory
unless they are absolute. Flat `Apps/<Name>.pak/` entries are ignored.

## Settings

The current Settings tree includes:

```text
Appearance       color scheme, colors, layout (list style, fonts, font size,
                 tab switching), status bar
Display & Sound  brightness, refresh rate (60/90/120, also picks the HDMI mode),
                 black frame insertion (120Hz, incl. over HDMI), HDMI output
                 (off/4:3/stretch, auto-switch; 720p60 or 1080p120 per refresh,
                 1080p120 auto-reverts unless kept), volume, audio output, test sound
Lighting         MLP1 LED enable/mode/color/brightness/speed
Network          Wi-Fi scan/connect/forget and ADB enable/disable
Bluetooth        scan, pair/connect, disconnect/forget
Game Art         scrape artwork (all/per-system, missing or replace-all), live
                 scrape queue, artwork-type and region priority
Accounts         ScreenScraper / RetroAchievements sign-in
General          startup tab, auto-sleep, boot splash, game performance,
                 time zone, reset RetroArch config, unmount secondary SD
Services         installed/retained service status, Run/Stop and persistent
                 Start with Leaf controls (hidden when no service is present)
```

System Update and About are not in the Settings tree; they live in the **System**
menu (the Menu-button popup), which also offers a library rescan and the
session/power actions (return to launcher, sleep, exit to stock, reboot, power off).

Jawaka exports Catastrophe `CAT_*` appearance variables before launching
`jawaka-launcher`, `jawaka-menu`, and Catastrophe-based `.pak` apps. Apps should
consume the inherited environment rather than read Jawaka's SQLite DB.

### Suspend inhibitors

Long-running apps can acquire a daemon-owned `block-suspend` lease over the
length-prefixed JSON socket at `UMRK_DAEMON_SOCKET`. The request types are
`suspend-inhibit-acquire`, `suspend-inhibit-release`, and
`suspend-inhibit-status`. Jawaka derives ownership from Unix peer credentials,
reaps dead holders, keeps stage-1 screen blanking active, and defers only deep
suspend until the final lease is released. Status diagnostics expose holder
reasons and age, never opaque tokens.

Use `make suspend-inhibit-test suspend-inhibit-ipc-smoke` for native policy and
IPC coverage. `make mlp1-adb-inhibit-smoke` runs the intentionally intrusive
device suspend/reap smoke after `CONFIRM_SUSPEND_SMOKE=1` is supplied.

### Library relocation reservations

The daemon advertises `relocate-games-v1` in `hello-ok.features`. A mover first
sends the complete stable-identity batch to `library-relocate-prepare`, then
uses `status`, `commit`, `revert`, `abort`, and `finish` operations. Prepared,
committed, and reverted batches persist in Primary-owned SQLite. During that
reservation, scans suppress only the batch's exact old/new keys and launches
of the affected game ids fail with `game is relocating`.

Commit and revert preserve `games.id` and bump `library.generation` exactly
once inside the mapping transaction. `finish` releases the reservation and
returns `scan_ticket_generation`; the operation becomes `finished` only after
a successful reconciliation scan publishes a generation greater than the
current commit/revert mapping generation.

`jawaka-platformctl` provides typed commands:

```sh
jawaka-platformctl capabilities
jawaka-platformctl relocate-prepare OP GENERATION ITEMS_JSON
jawaka-platformctl relocate-status OP
jawaka-platformctl relocate-commit OP
jawaka-platformctl relocate-revert OP
jawaka-platformctl relocate-abort OP
jawaka-platformctl relocate-finish OP
```

The client uses the full 16 MiB framed transport, strictly parses daemon JSON,
and exits nonzero for daemon errors or unexpected state/generation replies.
Socket resolution is `UMRK_DAEMON_SOCKET`, then `JAWAKA_SOCKET_PATH`, then the
runtime-derived default. Use
`make relocation-test relocation-ipc-smoke` for transaction and native IPC
coverage.

The MLP1 launcher payload also installs `jawaka-inhibitctl`. PortMaster uses
its `hold` command for the full lifetime of a journaled cross-card package
move, so an automatic or explicit suspend cannot interrupt an active copy or
publication phase.

### Package replacement barrier

The daemon advertises `package-quiesce-v1` for PKG-1 callers such as Leaf's
`make stage-app` and direct update runner. A caller sends
`package-quiesce-begin` with a bounded `operation_id`; Jawaka snapshots every
service, blocks new service generations and foreground app launches, and does
not reply `ok` until every owned process group is verified absent. Stale or
unverified generations fail closed before the caller may change package bytes.

After replacement, the caller sends `package-quiesce-end` with the same id.
Jawaka rescans manifests while the start latch is still held, restores the
snapshotted persistent enablement, and starts only desired-enabled services.
Session-only Run state is intentionally not restored. Direct/generic Leaf
updates use the same barrier internally; the stock MLP1 reboot handoff remains
reboot-mediated.

Raw diagnostic calls are available through `jawaka-platformctl`:

```sh
jawaka-platformctl request \
  '{"type":"package-quiesce-begin","operation_id":"manual-check"}'
jawaka-platformctl request \
  '{"type":"package-quiesce-end","operation_id":"manual-check"}'
```

## Repo Notes

- `scripts/mockgen.sh` creates the local mock SD-card tree.
- `third_party/cjson/` is vendored and used by the IPC/config paths.
- `third_party/catastrophe/` is still a placeholder; use `CATASTROPHE_DIR` or an
  adjacent `../Catastrophe` checkout.
- Current planning documents live in the sibling workspace repo under
  `umrk-workspace/plans/Jawaka/`.
- `docs/PLAN.md` and `docs/ARCHITECTURE_DECISIONS.md` are historical planning
  docs. Current behavior is best reflected by this README, the Makefile, and
  the implementation under `cmd/` and `internal/`.

## License

Jawaka is released under the MIT License. See `LICENSE`.
