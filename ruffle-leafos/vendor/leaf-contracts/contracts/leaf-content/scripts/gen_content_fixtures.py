#!/usr/bin/env python3
"""Generate CONTENT-1 manifest fixtures under manifests/{valid,invalid}/.

Each fixture is a directory standing in for an installed `.pak`: pak.json,
expect.json, and whatever real files the rule needs. Rules like
`escaping-symlink`, `non-regular-file`, and `not-executable` are not
checkable from JSON alone, so those fixtures carry a real symlink, a real
directory, or a real 0644 file and validate_fixtures.py inspects the
filesystem.

expect.json is {"valid": true, ...} or {"valid": false, "reason": "<slug>"}.
A fixture may also carry context.json for what a manifest cannot state about
itself -- which app lane it was installed into, which card it came from,
whether it has an executable launch.sh.

Run from anywhere; paths are relative to this file's directory.
Idempotent: re-running regenerates every fixture from scratch.
"""
from __future__ import annotations

import copy
import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
VALID_DIR = os.path.join(ROOT, "manifests", "valid")
INVALID_DIR = os.path.join(ROOT, "manifests", "invalid")

# A stand-in for the real ScummVM demo pak. The bytes are irrelevant -- what
# matters is that every declared path resolves to a real regular file, so a
# fixture that is meant to fail for one reason does not also fail for a
# missing file it never meant to test.
PAK_FILES = {
    "art/SCUMMVM.png": "PNG",
    "art/SCUMMVM-photo.png": "PNG",
    "cores/scummvm_libretro.so": "ELF",
    "info/scummvm_libretro.info": "display_name = \"ScummVM\"\n",
}

BASE = {
    "name": "ScummVM",
    "platform": "mlp1",
    "pak_version": "1.0.0",
    "min_leaf_version": "0.11.0",
    "provides": {
        "schema": 1,
        "systems": [
            {
                "id": "SCUMMVM",
                "name": "ScummVM",
                "patterns": ["SCUMMVM"],
                "extensions": ["scummvm", "svm"],
                "archive_extensions": [],
                "archive_mode": "pass_through",
                "playlist_extensions": [],
                "m3u_generation": "none",
                "rom_root": "Roms/SCUMMVM",
                "image_root": "Images/SCUMMVM",
                "default_core": "scummvm",
                "icon_flat": "art/SCUMMVM.png",
                "icon_photographic": None,
                "screenscraper_platform_ids": [123],
                "group": "Computer",
                "bios_directory": None,
            }
        ],
        "system_extensions": [],
        "cores": [
            {
                "id": "scummvm",
                "display_name": "ScummVM",
                "type": "retroarch",
                "libretro_name": "scummvm",
                "file_name": "cores/scummvm_libretro.so",
                "info_name": "info/scummvm_libretro.info",
                "config_folder": "ScummVM",
                "supports_menu": True,
                "supports_savestate": False,
                "supports_disk_control": False,
            }
        ],
    },
}


def system(pak: dict) -> dict:
    return pak["provides"]["systems"][0]


def core(pak: dict) -> dict:
    return pak["provides"]["cores"][0]


def variant(**edits) -> dict:
    """Deep-copy BASE and apply edits.

    Key forms:
      system__<field>  edit the first system
      core__<field>    edit the first core
      provides__<field>
      <field>          edit the pak top level
    A value of DELETE removes the key instead of setting it.
    """
    pak = copy.deepcopy(BASE)
    for key, value in edits.items():
        if key.startswith("system__"):
            target, field = system(pak), key[len("system__"):]
        elif key.startswith("core__"):
            target, field = core(pak), key[len("core__"):]
        elif key.startswith("provides__"):
            target, field = pak["provides"], key[len("provides__"):]
        else:
            target, field = pak, key
        if value is DELETE:
            target.pop(field, None)
        else:
            target[field] = value
    return pak


class _Delete:
    def __repr__(self) -> str:
        return "DELETE"


DELETE = _Delete()


def write_fixture(base_dir: str, name: str, pak: dict, expect: dict,
                  files: dict[str, str] | None = None,
                  symlinks: dict[str, str] | None = None,
                  directories: list[str] | None = None,
                  non_executable: list[str] | None = None,
                  executable: list[str] | None = None,
                  context: dict | None = None) -> None:
    fixture_dir = os.path.join(base_dir, name)
    if os.path.exists(fixture_dir) or os.path.islink(fixture_dir):
        shutil.rmtree(fixture_dir, ignore_errors=True)
    os.makedirs(fixture_dir)

    with open(os.path.join(fixture_dir, "pak.json"), "w") as f:
        json.dump(pak, f, indent=2)
        f.write("\n")
    with open(os.path.join(fixture_dir, "expect.json"), "w") as f:
        json.dump(expect, f, indent=2)
        f.write("\n")
    if context is not None:
        with open(os.path.join(fixture_dir, "context.json"), "w") as f:
            json.dump(context, f, indent=2)
            f.write("\n")

    for rel, content in (files if files is not None else PAK_FILES).items():
        path = os.path.join(fixture_dir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as f:
            f.write(content)
        os.chmod(path, 0o644)

    for rel in directories or []:
        path = os.path.join(fixture_dir, rel)
        os.makedirs(path, exist_ok=True)
        open(os.path.join(path, ".gitkeep"), "a").close()

    for rel in executable or []:
        path = os.path.join(fixture_dir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if not os.path.exists(path):
            with open(path, "w") as f:
                f.write("#!/bin/sh\nexec true\n")
        os.chmod(path, 0o755)

    for rel in non_executable or []:
        os.chmod(os.path.join(fixture_dir, rel), 0o644)

    for rel, target in (symlinks or {}).items():
        path = os.path.join(fixture_dir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        if os.path.lexists(path):
            os.remove(path)
        os.symlink(target, path)


# A pak-supplied RetroArch frontend (LEAF-RA-1). The pak adds an alternate
# core to an existing system, which is the real shape: the frontend exists to
# run a libretro core the release RetroArch cannot load.
FRONTEND_CORE = {
    "id": "mysaturn",
    "display_name": "My Saturn Core",
    "type": "retroarch",
    "libretro_name": "mysaturn",
    "file_name": "cores/mysaturn_libretro.so",
    "info_name": "info/mysaturn_libretro.info",
    "config_folder": "MySaturn",
    "frontend": {"path": "frontend/retroarch.sh", "protocol": "leaf-ra-1"},
}

FRONTEND_FILES = {
    "cores/mysaturn_libretro.so": "ELF",
    "info/mysaturn_libretro.info": "display_name = \"My Saturn Core\"\n",
}


def frontend_variant(**core_edits) -> dict:
    """A cores-plus-extension pak whose one core carries a frontend.

    A value of DELETE removes the core key instead of setting it.
    """
    pak = variant(
        name="MySaturn",
        provides__systems=[],
        provides__system_extensions=[{"system_id": "SATURN",
                                      "add_alternate_cores": ["mysaturn"]}],
    )
    core_entry = copy.deepcopy(FRONTEND_CORE)
    for key, value in core_edits.items():
        if value is DELETE:
            core_entry.pop(key, None)
        else:
            core_entry[key] = value
    pak["provides"]["cores"] = [core_entry]
    return pak


def bad(reason: str, note: str = "") -> dict:
    expect = {"valid": False, "reason": reason}
    if note:
        expect["note"] = note
    return expect


def good(note: str = "", **extra) -> dict:
    expect = {"valid": True}
    if note:
        expect["note"] = note
    expect.update(extra)
    return expect


# --------------------------------------------------------------------------

def generate_valid() -> None:
    write_fixture(
        VALID_DIR, "full", copy.deepcopy(BASE),
        good("the ScummVM demo pak: one new system, one libretro core, "
             "flat-only art, no launch.sh", apps_listed=False),
        context={"install_lane": "platform", "source_id": "primary",
                 "launch_sh_executable": False},
    )

    minimal = variant(
        system__archive_extensions=DELETE,
        system__archive_mode=DELETE,
        system__playlist_extensions=DELETE,
        system__m3u_generation=DELETE,
        system__icon_photographic=DELETE,
        system__screenscraper_platform_ids=DELETE,
        system__group=DELETE,
        system__bios_directory=DELETE,
        core__supports_menu=DELETE,
        core__supports_savestate=DELETE,
        core__supports_disk_control=DELETE,
        provides__system_extensions=DELETE,
    )
    write_fixture(VALID_DIR, "minimal", minimal,
                  good("only the required fields; every optional one takes its default"))

    photographic = variant(system__icon_photographic="art/SCUMMVM-photo.png")
    write_fixture(VALID_DIR, "both-icons", photographic,
                  good("both art slots declared, so the launcher can honor "
                       "either icon pack without falling back"))

    standalone = variant(
        system__default_core="mystandalone",
        provides__cores=[{
            "id": "mystandalone",
            "display_name": "My Standalone",
            "type": "path",
            "path": "emulators/mystandalone/launch.sh",
            "supports_menu": True,
            "supports_savestate": False,
            "supports_disk_control": False,
        }],
    )
    write_fixture(VALID_DIR, "standalone-path-core", standalone,
                  good("a type:\"path\" standalone emulator instead of a "
                       "libretro core; the target must be executable"),
                  executable=["emulators/mystandalone/launch.sh"])

    write_fixture(VALID_DIR, "retroarch-core-with-frontend", frontend_variant(),
                  good("a type:\"retroarch\" core that ships its own RetroArch "
                       "frontend, added to an existing system through "
                       "system_extensions; the frontend must be executable. "
                       "min_leaf_version is the shared fixture default: a "
                       "real pak declaring frontend sets the first Leaf "
                       "release that implements it"),
                  files=FRONTEND_FILES,
                  executable=["frontend/retroarch.sh"])

    ext_only = variant(
        provides__systems=[],
        provides__cores=[],
        provides__system_extensions=[{"system_id": "SNES",
                                      "add_alternate_cores": ["mysnescore"]}],
    )
    write_fixture(VALID_DIR, "system-extension-only", ext_only,
                  good("appends an alternate core to an existing system; the "
                       "ONLY shape allowed to touch one"))

    cores_only = variant(provides__systems=[], provides__system_extensions=[])
    write_fixture(VALID_DIR, "cores-only", cores_only,
                  good("ships a core and no system -- valid, because another "
                       "pak's system_extensions may name it"))

    hybrid = copy.deepcopy(BASE)
    write_fixture(VALID_DIR, "hybrid-app-and-content", hybrid,
                  good("PortMaster's shape: contributes content AND has an "
                       "executable launch.sh, so it is listed in Apps too",
                       apps_listed=True),
                  executable=["launch.sh"],
                  context={"install_lane": "platform", "source_id": "primary",
                           "launch_sh_executable": True})

    redundant = variant(system__patterns=["SCUMMVM", "scummvm", "ScummVM"])
    write_fixture(VALID_DIR, "redundant-case-variant-warns", redundant,
                  good("accepted, but warned: matching is already "
                       "case-insensitive, so the extra variants do nothing",
                       warnings=["redundant-case-variant"]))



def generate_invalid() -> None:
    # -- top level ---------------------------------------------------------
    write_fixture(INVALID_DIR, "unknown-schema", variant(provides__schema=2),
                  bad("unknown-schema",
                      "a newer schema is refused, never guessed at, so a v2 "
                      "pak fails loudly on a v1 device"))

    write_fixture(INVALID_DIR, "empty-provides",
                  variant(provides__systems=[], provides__cores=[],
                          provides__system_extensions=[]),
                  bad("empty-provides", "declares nothing at all"))

    write_fixture(INVALID_DIR, "unknown-field",
                  variant(provides__themes=[]),
                  bad("unknown-field",
                      "additionalProperties is false at every level of "
                      "`provides`; themes are a separate deferred contract"))

    many_systems = variant()
    many_systems["provides"]["systems"] = [
        dict(system(BASE), id=f"SYS{i:02d}", rom_root=f"Roms/SYS{i:02d}",
             image_root=f"Images/SYS{i:02d}", patterns=[f"SYS{i:02d}"])
        for i in range(33)
    ]
    write_fixture(INVALID_DIR, "too-many-systems", many_systems,
                  bad("too-many-systems", "33 systems; the cap is 32"))

    many_exts = variant()
    many_exts["provides"]["system_extensions"] = [
        {"system_id": f"SYS{i:02d}", "add_alternate_cores": ["scummvm"]}
        for i in range(33)
    ]
    write_fixture(INVALID_DIR, "too-many-system-extensions", many_exts,
                  bad("too-many-system-extensions", "33 extensions; the cap is 32"))

    many_cores = variant()
    many_cores["provides"]["cores"] = [
        dict(core(BASE), id=f"core{i:02d}", libretro_name=f"core{i:02d}",
             config_folder=f"Core{i:02d}")
        for i in range(33)
    ]
    write_fixture(INVALID_DIR, "too-many-cores", many_cores,
                  bad("too-many-cores", "33 cores; the cap is 32"))

    # -- system fields -----------------------------------------------------
    write_fixture(INVALID_DIR, "malformed-system-id", variant(system__id="scummvm"),
                  bad("malformed-system-id", "system ids are ^[A-Z0-9_]{2,32}$"))
    write_fixture(INVALID_DIR, "malformed-system-name", variant(system__name=""),
                  bad("malformed-system-name"))
    write_fixture(INVALID_DIR, "malformed-patterns", variant(system__patterns=[]),
                  bad("malformed-patterns",
                      "a system with no pattern can never match a ROM folder"))
    write_fixture(INVALID_DIR, "malformed-extensions",
                  variant(system__extensions=["SVM"]),
                  bad("malformed-extensions", "extensions are lowercase"))
    write_fixture(INVALID_DIR, "malformed-archive-extensions",
                  variant(system__archive_extensions=["ZIP"]),
                  bad("malformed-archive-extensions"))
    write_fixture(INVALID_DIR, "malformed-archive-inner-extensions",
                  variant(system__archive_inner_extensions=["ISO"]),
                  bad("malformed-archive-inner-extensions"))
    write_fixture(INVALID_DIR, "malformed-playlist-extensions",
                  variant(system__playlist_extensions=["M3U"]),
                  bad("malformed-playlist-extensions"))
    write_fixture(INVALID_DIR, "malformed-file-names",
                  variant(system__file_names=[""]),
                  bad("malformed-file-names"))
    write_fixture(INVALID_DIR, "malformed-ignore-file-names",
                  variant(system__ignore_file_names=[""]),
                  bad("malformed-ignore-file-names"))
    write_fixture(INVALID_DIR, "malformed-bios-notes",
                  variant(system__bios_notes=[""]),
                  bad("malformed-bios-notes"))
    write_fixture(INVALID_DIR, "unknown-archive-mode",
                  variant(system__archive_mode="flatten"),
                  bad("unknown-archive-mode"))
    write_fixture(INVALID_DIR, "unknown-m3u-generation",
                  variant(system__m3u_generation="sometimes"),
                  bad("unknown-m3u-generation"))
    write_fixture(INVALID_DIR, "malformed-rom-root",
                  variant(system__rom_root="Games/SCUMMVM"),
                  bad("malformed-rom-root",
                      "one public Roms/ folder per console, per the canonical "
                      "user-system-folder policy"))
    write_fixture(INVALID_DIR, "malformed-image-root",
                  variant(system__image_root="Art/SCUMMVM"),
                  bad("malformed-image-root"))
    write_fixture(INVALID_DIR, "malformed-screenscraper-platform-ids",
                  variant(system__screenscraper_platform_ids=[0]),
                  bad("malformed-screenscraper-platform-ids"))
    write_fixture(INVALID_DIR, "malformed-group", variant(system__group=""),
                  bad("malformed-group"))
    write_fixture(INVALID_DIR, "malformed-bios-directory",
                  variant(system__bios_directory="bios/scummvm"),
                  bad("malformed-bios-directory",
                      "a single safe directory component, not a path"))

    # -- core fields -------------------------------------------------------
    write_fixture(INVALID_DIR, "malformed-core-id",
                  variant(core__id="ScummVM", system__default_core="ScummVM"),
                  bad("malformed-core-id", "core ids are ^[a-z0-9_]{2,64}$"))
    write_fixture(INVALID_DIR, "malformed-core-display-name",
                  variant(core__display_name=""),
                  bad("malformed-core-display-name"))
    write_fixture(INVALID_DIR, "unknown-core-type",
                  variant(core__type="standalone"),
                  bad("unknown-core-type", "the two types are retroarch and path"))
    write_fixture(INVALID_DIR, "malformed-libretro-name",
                  variant(core__libretro_name="ScummVM"),
                  bad("malformed-libretro-name"))
    write_fixture(INVALID_DIR, "missing-core-file-name",
                  variant(core__file_name=DELETE),
                  bad("missing-core-file-name",
                      "a retroarch core must declare the .so it ships"))
    write_fixture(INVALID_DIR, "missing-core-info-name",
                  variant(core__info_name=DELETE),
                  bad("missing-core-info-name",
                      "RetroArch takes one info DIRECTORY, so the compile "
                      "materializes a merged one and needs the pak's .info"))
    write_fixture(INVALID_DIR, "missing-config-folder",
                  variant(core__config_folder=DELETE),
                  bad("missing-config-folder"))
    write_fixture(INVALID_DIR, "unsafe-config-folder",
                  variant(core__config_folder="AUX"),
                  bad("unsafe-config-folder",
                      "RetroArch uses config_folder verbatim as a FAT32 "
                      "directory component; a reserved DOS device name "
                      "corrupts the Saves/States layout device-wide"))
    write_fixture(INVALID_DIR, "missing-core-path",
                  variant(system__default_core="mystandalone",
                          provides__cores=[{"id": "mystandalone",
                                            "display_name": "My Standalone",
                                            "type": "path"}]),
                  bad("missing-core-path"))
    write_fixture(INVALID_DIR, "core-type-field-mismatch",
                  variant(system__default_core="mystandalone",
                          provides__cores=[{
                              "id": "mystandalone",
                              "display_name": "My Standalone",
                              "type": "path",
                              "path": "emulators/mystandalone/launch.sh",
                              "file_name": "cores/scummvm_libretro.so",
                          }]),
                  bad("core-type-field-mismatch",
                      "a path core carrying retroarch-only fields; the "
                      "validator never infers intent from which fields "
                      "happen to be present"),
                  executable=["emulators/mystandalone/launch.sh"])
    write_fixture(INVALID_DIR, "frontend-on-path-core",
                  variant(system__default_core="mystandalone",
                          provides__cores=[{
                              "id": "mystandalone",
                              "display_name": "My Standalone",
                              "type": "path",
                              "path": "emulators/mystandalone/launch.sh",
                              "frontend": {"path": "frontend/retroarch.sh",
                                           "protocol": "leaf-ra-1"},
                          }]),
                  bad("core-type-field-mismatch",
                      "frontend belongs to a retroarch core: a path core is "
                      "its own executable, so it has no RetroArch to replace"),
                  executable=["emulators/mystandalone/launch.sh",
                              "frontend/retroarch.sh"])
    write_fixture(INVALID_DIR, "malformed-frontend",
                  frontend_variant(frontend="frontend/retroarch.sh"),
                  bad("malformed-frontend",
                      "frontend is an object with exactly path and protocol; "
                      "a bare string is the likeliest slip. A missing or "
                      "non-string path and an unknown key are the same reason"),
                  files=FRONTEND_FILES,
                  executable=["frontend/retroarch.sh"])
    write_fixture(INVALID_DIR, "unknown-frontend-protocol",
                  frontend_variant(frontend={"path": "frontend/retroarch.sh",
                                             "protocol": "leaf-ra-2"}),
                  bad("unknown-frontend-protocol",
                      "a protocol this reader does not speak is refused, "
                      "never guessed at, so a pak written for a later "
                      "protocol cannot be run through the wrong one"),
                  files=FRONTEND_FILES,
                  executable=["frontend/retroarch.sh"])
    write_fixture(INVALID_DIR, "frontend-not-executable",
                  frontend_variant(),
                  bad("not-executable",
                      "the frontend is exec'd in place of RetroArch, so "
                      "frontend.path gets the same executable check as a "
                      "path core's path"),
                  files=dict(FRONTEND_FILES,
                             **{"frontend/retroarch.sh": "#!/bin/sh\n"}),
                  non_executable=["frontend/retroarch.sh"])
    write_fixture(INVALID_DIR, "malformed-core-flag",
                  variant(core__supports_menu="yes"),
                  bad("malformed-core-flag"))

    # -- paths -------------------------------------------------------------
    write_fixture(INVALID_DIR, "absolute-path",
                  variant(core__file_name="/usr/lib/libretro/scummvm_libretro.so"),
                  bad("absolute-path",
                      "MLP1 swaps /mnt/sdcard and /media/sdcard1 across "
                      "reboots, so an absolute path is not merely inelegant "
                      "-- it resolves to nothing on the next boot"))
    write_fixture(INVALID_DIR, "path-traversal",
                  variant(core__file_name="../elsewhere/scummvm_libretro.so"),
                  bad("path-traversal"))
    escaping = variant(core__file_name="escaped/scummvm_libretro.so")
    write_fixture(INVALID_DIR, "escaping-symlink", escaping,
                  bad("escaping-symlink",
                      "the INTERMEDIATE component escapes, not the leaf; "
                      "resolving only the last component would miss it"),
                  symlinks={"escaped": "/tmp"})
    write_fixture(INVALID_DIR, "missing-file",
                  variant(core__file_name="cores/absent_libretro.so"),
                  bad("missing-file",
                      "the stamp fingerprints every declared file, so a "
                      "declaration the compiler cannot verify is refused up front"))
    write_fixture(INVALID_DIR, "non-regular-file",
                  variant(core__file_name="cores/adirectory"),
                  bad("non-regular-file"),
                  directories=["cores/adirectory"])
    write_fixture(INVALID_DIR, "not-executable",
                  variant(system__default_core="mystandalone",
                          provides__cores=[{
                              "id": "mystandalone",
                              "display_name": "My Standalone",
                              "type": "path",
                              "path": "emulators/mystandalone/launch.sh",
                          }]),
                  bad("not-executable",
                      "a standalone target that cannot be executed produces a "
                      "tile that cannot launch"),
                  files=dict(PAK_FILES,
                             **{"emulators/mystandalone/launch.sh": "#!/bin/sh\n"}),
                  non_executable=["emulators/mystandalone/launch.sh"])

    # -- system_extensions -------------------------------------------------
    write_fixture(INVALID_DIR, "unknown-extension-field",
                  variant(provides__system_extensions=[{
                      "system_id": "SNES",
                      "add_alternate_cores": ["mysnescore"],
                      "replace_default_core": "mysnescore",
                  }]),
                  bad("unknown-extension-field",
                      "system_extensions has exactly two fields; there is no "
                      "shape in which a pak overrides a first-party default core"))
    write_fixture(INVALID_DIR, "empty-add-alternate-cores",
                  variant(provides__system_extensions=[{
                      "system_id": "SNES", "add_alternate_cores": []}]),
                  bad("empty-add-alternate-cores"))

    # -- forbidden fields (D15) -------------------------------------------
    write_fixture(INVALID_DIR, "forbidden-requires-direct-drm",
                  variant(core__requires_direct_drm=True),
                  bad("forbidden-requires-direct-drm",
                      "jw_standalone_policy_requires_direct_drm honors "
                      "metadata alone -- there is no allowlist gate on that "
                      "path, and a wrong claim can wedge the display"))
    write_fixture(INVALID_DIR, "forbidden-legacy-flat-core",
                  variant(system__legacy_flat_core="scummvm"),
                  bad("forbidden-legacy-flat-core",
                      "claims ownership of save/state files from RetroArch's "
                      "historical flat layout"))
    write_fixture(INVALID_DIR, "forbidden-name-map",
                  variant(system__name_map=True),
                  bad("forbidden-name-map", "release-owned arcade-name behavior"))
    write_fixture(INVALID_DIR, "forbidden-status",
                  variant(core__status="packaged"),
                  bad("forbidden-status",
                      "the compiler sets status from verified on-disk reality, "
                      "never from a manifest"))

    # -- location (D16) ----------------------------------------------------
    # D16's shared rule has TWO outcomes, and the plan asks for both to be
    # classified, so it is the one rule with two invalid fixtures.
    write_fixture(INVALID_DIR, "shared-content-unsupported-pure",
                  copy.deepcopy(BASE),
                  dict(bad("shared-content-unsupported",
                           "a pure content pak under Apps/shared/. Still "
                           "CLASSIFIED rather than ignored, so it cannot slip "
                           "through as an unrecognized pure app -- and with no "
                           "launch.sh it stays hidden from Apps"),
                       apps_listed=False),
                  context={"install_lane": "shared", "source_id": "primary",
                           "launch_sh_executable": False})
    write_fixture(INVALID_DIR, "shared-content-unsupported-hybrid",
                  copy.deepcopy(BASE),
                  dict(bad("shared-content-unsupported",
                           "the other half of D16: a shared-lane HYBRID "
                           "contributes nothing, but remains an ordinary "
                           "launchable app. Refusing the contribution must "
                           "never un-list a working app"),
                       apps_listed=True),
                  executable=["launch.sh"],
                  context={"install_lane": "shared", "source_id": "primary",
                           "launch_sh_executable": True})
    write_fixture(INVALID_DIR, "secondary-source-unsupported",
                  copy.deepcopy(BASE),
                  dict(bad("secondary-source-unsupported",
                           "v1 honors `provides` on the primary card only: a "
                           "system whose emulator vanishes when a card is "
                           "pulled is a bad first experience, and the stamp "
                           "would need mount-state tracking to be correct"),
                       apps_listed=False),
                  context={"install_lane": "platform", "source_id": "1",
                           "launch_sh_executable": False})


def main() -> None:
    for directory in (VALID_DIR, INVALID_DIR):
        if os.path.exists(directory):
            shutil.rmtree(directory)
        os.makedirs(directory)
    generate_valid()
    generate_invalid()
    valid = len(os.listdir(VALID_DIR))
    invalid = len(os.listdir(INVALID_DIR))
    print(f"Generated {valid} valid and {invalid} invalid CONTENT-1 manifest fixtures.")


if __name__ == "__main__":
    main()
