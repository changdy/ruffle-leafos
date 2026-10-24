# Mac Packaging Contract

This is the concrete Track B contract for the first Mac payload. It is a
plain directory tree under `UMRK/mac/` on the SD root.

## Inputs

The assembler is expected to consume these sibling repos:

- `Jawaka/` for `jawakad`, `jawaka-launcher`, and `jawaka-menu`
- `retroarch-builds/` for `RetroArch.app`
- `Cores-spruce/` for libretro cores and `.info` files

`retroarch-overlays/` can be added as an input once overlay packaging exists.

## Tree

```text
UMRK/
  mac/
    manifest.json
    defaults/
      cores.json
      retroarch.cfg
    retroarch/
      RetroArch.app/
    cores/
      *_libretro.dylib
    info/
      *_libretro.info
    themes/
```

## Manifest

`manifest.json` is the per-platform descriptor. It is both the build-time
payload description and the runtime descriptor.

Required initial fields:

- `platform`
- `version`
- `artifact_format`
- `retroarch_bin_relpath`
- `cores_dir_relpath`
- `info_dir_relpath`
- `defaults_dir_relpath`
- `theme_dir_relpath`
- `source_repos`

Optional runtime policy fields:

- `managed_apps` — array of Apps-namespace paths owned by the active Leaf
  release, such as `mlp1/SSHServer.pak` or `shared/RetroArch.pak`. Store
  install/update/uninstall code must treat these paths as blocked.

The mock SD generator emits a concrete example at
`mock-sdcard/UMRK/mac/manifest.json`.

## Defaults

`defaults/cores.json` owns the system-to-core map. Jawaka loads this file at
runtime from `UMRK/mac/defaults/cores.json`; if it is missing, malformed, or
does not contain the requested system, Jawaka falls back to the compiled demo
map. It is deliberately outside the manifest so core selection can evolve
without changing the platform descriptor schema.

`defaults/retroarch.cfg` contains portable defaults only. The shared editable
RetroArch config is stored under the primary SD state root, while absolute
`BIOS`, `Saves`, and `States` paths are written per launch based on the active
ROM source root.

## State And Upgrades

The launcher state directory is `.umrk/`. Jawaka does not automatically select
legacy `.jawaka/` state; old data remains untouched unless an explicit migration
is run.

Reinstalling a platform payload may fully replace `UMRK/<platform>/`.
Assemblers must not delete `.umrk/`, `Roms/`, `Images/`, `Apps/`, `BIOS/`,
`Saves/`, or `States/`.
