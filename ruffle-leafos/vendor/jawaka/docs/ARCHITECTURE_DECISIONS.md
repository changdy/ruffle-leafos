# Jawaka Architecture Decisions

## 1. IPC transport

Use a real socket-based IPC layer immediately so Mac and device behave identically from the start.

Use:

- Unix domain socket
- Length-prefixed JSON

## 2. Daemon ↔ UI process model

Use separate OS processes from day one.

`jawakad` should fork/exec `jawaka-launcher` and `jawaka-menu` on Mac, matching the Linux/device process model as closely as possible.

Do not initially run launcher and menu as threads behind a shared SDL window. That would make early development easier, but it would create a fake architecture that hides the problems the project needs to solve early:

- process lifetime
- IPC ownership
- focus
- input routing
- UI crash handling
- what happens when one UI exits

Because Catastrophe owns the event loop and SDL framebuffer, only one UI process should own the screen/input at a time.

Recommended model:

- `jawakad` is the long-lived coordinator.
- `jawaka-launcher` is a separate UI process.
- `jawaka-menu` is a separate UI process.
- The daemon decides which UI is active.
- Before showing the menu, the launcher is suspended, hidden, asked to release the display, or exited depending on the phase.
- The menu gets exclusive screen/input ownership.
- When the menu exits, the daemon restores or relaunches the launcher.

On Mac, this can still use a desktop SDL window, but the ownership model should mimic the handheld: one active foreground UI at a time.

## 3. `jawaka-menu` invocation on Mac

Exercise the menu via:

- keyboard chord
- plugged-in Xbox controller

This keeps the Mac path close to real input behavior rather than relying only on a separate CLI command.

## 4. Launch action on Mac

Stub this for now.

The next phase will involve compiling RetroArch on Mac, but for the initial phase the launch path can use a stub child so `jawakad` can already exercise supervision and lifecycle behavior.

## 5. Recents granularity

Include playtime tracking, not just last-opened timestamps.

## 6. SD-card folder names

Try to be as compatible as possible with the Done Set Three layout at:

```text
/Volumes/downloads/done-set-three_202501/done-set-three_202501/done-set-three_202501
```

Do not store images inline inside ROM folders.

Use a separate `/Images` folder for artwork.

## 7. System detection

Detect systems by folder name, using a built-in mapping/alias table where needed.

## 8. Per-game metadata

Skip file-based per-game metadata for now, at least until a scraper or stronger metadata requirement exists.

## 9. `Apps/` pak convention

Use the platform-guarded `Apps/<platform>/<Name>.pak/` convention, plus
`Apps/shared/<Name>.pak/` for wrappers that delegate to platform payloads:

```text
Apps/
├── mlp1/<Name>.pak/
│   ├── launch.sh
│   └── pak.json
└── shared/<Name>.pak/
    ├── launch.sh
    └── pak.json
```

`jawakad` should discover only apps under the compiled platform directory and
`shared`; flat `Apps/<Name>.pak/` entries are ignored.

`pak.json` should include at least:

- `name`
- `icon` (relative to the containing `.pak` directory unless absolute)
- `platform`, such as `mac`, `tg5040`, etc.
- `pak_version`
- `min_jawaka_version`

`pak.json.platform` must match the containing platform directory, or be
`shared` for apps under `Apps/shared/`.

## 10. Mock SD-card root

Use:

```sh
$JAWAKA_SDCARD_ROOT
```

falling back to:

```text
./mock-sdcard/
```

in the repo.

The generator script should create:

- launchable stub apps
- a few hundred fake ROMs

## 11. SQLite scope

Use SQLite with tables for:

- `games`
- `apps`
- `recents`
- `favorites`

Use FTS5 for search.

## 12. DB & cache location

On Mac, all files should live inside the mock SD-card root.

Some default mock SD-card content can be committed to git, but local/generated changes should be excluded.

## 13. FAT32-safe scope

Use `/tmp` for sockets on devices.

FAT32-safe rules mainly apply to the SD-card content tree. Runtime IPC sockets should live on tmpfs/non-FAT32 storage.

## 14. Language

C++ is acceptable where it makes sense, especially in `jawakad` for:

- supervision
- IPC framing
- process management

The project does not need to be pure C11 if C++ materially improves the implementation.

## 15. Catastrophe vendoring

Use a git submodule under:

```text
third_party/catastrophe/
```

It is OK for each of the three binaries to compile its own translation unit with `CAT_IMPLEMENTATION`, resulting in three compiled copies of the implementation.

## 16. Build system

Use a single root `Makefile` that branches out to other Makefiles as needed.

There will also be an even more general Makefile at the UMRK level above Jawaka.

## 17. Repo top-level layout

Use the proposed layout:

```text
Jawaka/
  Makefile
  README.md
  cmd/{jawakad,jawaka-launcher,jawaka-menu}/main.c
  internal/{core,db,discovery,ipc,platform,metadata,search}/...
  scripts/mockgen.sh
  mock-sdcard/        # gitignored output
  third_party/catastrophe/
  ports/{tg5040,tg5050,my355}/Makefile   # stubs for Phase 6/7
```

## 18. Repo bootstrap

This is currently an empty repo. The project is starting from scratch.

No origin exists yet; origin will be added later.

The suggested Phase 0+1 first commit set is approved:

- `README.md`
- plan copy or pointer back to `umrk-workspace/plans/Jawaka/jawaka.md`
- `Makefile` skeleton
- three `main.c` stubs that boot and log
- `scripts/mockgen.sh`
- empty SQLite schema
