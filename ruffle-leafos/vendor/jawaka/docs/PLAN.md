# Jawaka

Jawaka is the planned launcher repo for UMRK.

## Purpose

Build a focused launcher/menu system on top of Catastrophe.

Jawaka should present:

- games on the SD card
- installed apps
- settings
- recents
- favorites

## Scope

Jawaka is responsible for:

- launcher navigation and menu flow
- content discovery
- box art and local metadata presentation
- search
- lightweight launcher state for recents/favorites
- launch entry points for games and apps
- launcher-facing settings
- platform abstraction points for future handheld ports

## Explicit non-goals

Jawaka should **not** become the home for:

- emulator implementations
- RetroArch build logic
- standalone utilities/apps that should live in their own repos
- the shared in-game overlay
- Catastrophe UI toolkit responsibilities
- scraper/downloader logic in the earliest phases

## Allium-guided direction

Use Allium as a guide for:

- launcher-first flow
- clear separation between games, apps, recents, favorites, and settings
- local metadata and artwork support
- search as a first-class launcher feature
- a clean path between simulator/desktop development and later device ports

Do **not** copy Allium blindly, but do adopt the part of its architecture that is likely expensive to retrofit later: a split between a long-lived daemon, the full launcher UI, and a contextual menu/overlay UI.

Jawaka should therefore be planned around:

- `jawakad` - long-lived coordinator
- `jawaka-launcher` - full launcher UI
- `jawaka-menu` - daemon-invoked contextual/overlay menu
- a shared internal core/common layer used by all three

## Agreed implementation constraints

- **Mac-first** development and preview loop
- **Multi-binary architecture from the start**
- Built on top of **Catastrophe** as a separate dependency
- **Filesystem-first** discovery model with a local SQLite index/cache
- **Allium-inspired SD-card structure**, but not exact compatibility with Allium or NextUI
- **FAT32-safe** assumptions from the start
- Early **search** and **local box art** support
- Structured **`Apps/` discovery** rather than scanning arbitrary executables
- A small **platform abstraction layer** from the beginning
- **Explicit IPC** for runtime coordination between daemon, launcher, and menu
- **Daemon-owned scanning and refresh jobs**
- `jawakad` should be the normal entrypoint even on Mac
- include a minimal working `jawaka-menu` early, not just later in theory
- Early settings focus on **launcher-owned settings**, not device-control settings

## Development workflow

- Primary local development target is **macOS**
- Jawaka should include a script that generates a **mock SD-card tree** with fake ROMs/apps/files for local testing
- The mock SD-card tooling should live inside the Jawaka repo so local development stays self-contained
- The first runnable launcher flow should scan the mock SD-card layout rather than use only hardcoded demo data
- The earliest runnable Mac prototype should still be launched through `jawakad`

## Suggested internal split

Jawaka should have a shared internal layer from the beginning so the split binaries do not duplicate core logic.

Suggested responsibilities:

- `jawakad`
  - process startup/supervision
  - IPC server
  - scan and refresh jobs
  - launch orchestration
  - long-lived platform integration hooks
- `jawaka-launcher`
  - full browsing UI
  - games/apps/recents/favorites/settings screens
  - search UX
  - query/read access to indexed content
- `jawaka-menu`
  - daemon-invoked contextual/overlay menu
  - minimal quick actions early, broader contextual actions later
- shared core/common
  - models
  - SQLite access
  - discovery rules
  - IPC protocol types
  - platform abstraction
  - metadata and artwork loading helpers

Within that shared layer, the code should still separate cleanly into modules such as:

- app shell / navigation
- content discovery
- launcher database
- metadata and artwork loading
- search
- launch actions
- settings
- platform abstraction

That mirrors the useful separation visible in Allium while adapting it to Jawaka's C-first goals.

## Phased implementation plan

### Phase 0 - Repo bootstrap and planning

Goal: establish the repo and development contract.

Target outcomes:

- create the `Jawaka` workspace folder
- keep Jawaka as a separate repo from Catastrophe
- lock the broad architecture decisions in this plan
- define the initial Mac-first and FAT32-safe constraints

Notes:

- This phase is planning-first and intentionally avoids premature code structure decisions beyond the agreed repo shape.

### Phase 1 - Desktop foundation and mock SD card

Goal: prove the basic runtime and data model on Mac.

Target outcomes:

- simple Makefile-based Mac build for `jawakad`, `jawaka-launcher`, and `jawaka-menu`
- Catastrophe wired in as the UI dependency for launcher/menu surfaces
- shared internal core/common layer created from the beginning
- a small platform abstraction layer for paths, process launching, and future device hooks
- initial IPC contract between daemon, launcher, and menu
- mock SD-card generator script inside the repo
- initial Allium-inspired folder contract for games, apps, artwork, and metadata
- SQLite schema for launcher index, recents, favorites, and search-facing discovery data

This phase should prove that `jawakad` can boot on Mac, own the startup path, and read a realistic fake SD-card layout.

### Phase 2 - Daemon flow, library discovery, search, and local artwork

Goal: make the split architecture real while also making the launcher useful as a content browser.

Target outcomes:

- `jawakad` launches and supervises `jawaka-launcher`
- `jawakad` owns library scans and refresh requests
- minimal working `jawaka-menu` can be invoked through daemon-controlled flow
- scan games from the mock SD-card structure
- scan apps from a structured `Apps/` directory
- load local metadata where present
- load local box art where present
- populate the SQLite index
- support search over the indexed library

Important scope cut:

- local artwork and app `pak.json` metadata only
- per-game metadata stays deferred, matching the current architecture decision
- no scraper/downloader yet

### Phase 3 - Core launcher UI

Goal: deliver the main user-facing Jawaka shell.

Target outcomes:

- top-level sections for Games, Apps, Recents, Favorites, and Settings
- list/detail navigation built on Catastrophe
- artwork-aware browsing
- search entry/results flow
- empty-state and error-state handling
- basic launch action plumbing for games/apps via daemon requests

This phase is where Jawaka starts to feel like a real launcher rather than just a content indexer.

### Phase 4 - Stateful launcher behavior

Goal: make daily use feel persistent and personal.

Target outcomes:

- favorites management
- recents tracking
- last-opened context restoration where appropriate
- index refresh/update behavior
- robust persistence rules in SQLite
- clearer launcher/menu/daemon state handoff rules

This phase should make the launcher behave more like Allium's day-to-day flow without depending on device-specific features yet.

### Phase 5 - Launcher settings and polish

Goal: round out the Mac-first launcher experience.

Target outcomes:

- launcher-owned settings pages
- content root configuration
- theme/display preferences owned by the launcher
- search/display behavior preferences
- library refresh controls
- visual polish, better messaging, and UX cleanup

Out of scope for this phase:

- Wi-Fi
- brightness
- power management
- other device-control surfaces

### Phase 6 - Real launch integration and platform prep

Goal: prepare Jawaka for real handheld use without bloating the launcher boundary.

Target outcomes:

- replace or extend Mac launch stubs with real platform launch behavior
- validate the filesystem model against FAT32 constraints
- harden the platform abstraction layer for tg5040/my355-style targets
- test SD-card portability assumptions
- expand daemon responsibilities where real-device lifecycle requires them

This phase should still preserve Jawaka's repo boundary: launcher only, not emulator build ownership.

### Phase 7 - Device-port phases

Goal: move from Mac-first launcher to handheld-ready launcher.

Target outcomes:

- tg5040-class port
- my355-class port
- input and layout tuning per device class
- launcher settings expansion only where platform support requires it

These phases should happen after the Mac-first launcher flow is already strong.

## Deferred items

These are intentionally not part of the first implementation push:

- metadata scraping/downloading
- OTA/update systems
- device Wi-Fi management
- in-game overlay behavior
- emulator-specific settings ownership
- standalone apps that belong in sibling repos
