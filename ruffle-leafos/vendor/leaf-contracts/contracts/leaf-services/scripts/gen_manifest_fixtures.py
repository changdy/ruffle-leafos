#!/usr/bin/env python3
"""Generate SVC-1 manifest fixtures under manifests/{valid,invalid}/.

Each fixture is a directory containing pak.json and expect.json.
expect.json is {"valid": true} or {"valid": false, "reason": "<slug>"}.
Some fixtures also contain a bin/ tree because their rule
(escaping symlink, non-regular executable, symlink path component) is
not checkable from JSON alone -- validate_fixtures.py inspects the
filesystem for those.

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

BASE = {
    "id": "org.umrk.syncthing",
    "service": {
        "schema": 1,
        "id": "org.umrk.syncthing",
        "run": {"path": "bin/leaf-syncthing", "args": ["service", "run"]},
        "restart": "on-failure",
        "default_enabled": False,
        "stop_grace_ms": 10000,
        "lifecycle": {
            "game": "notify",
            "stop_on_storage_change": True,
            "stop_on_suspend": True,
        },
    },
    "state": {
        "root": "Syncthing",
        "revoke_on_uninstall": ["leaf/trusted-clients.json"],
        "retained_roots": ["Syncthing"],
    },
}


def write_fixture(base_dir: str, name: str, manifest: dict | None, expect: dict,
                   files: dict[str, str] | None = None,
                   symlinks: dict[str, str] | None = None,
                   dirs_as_files: list[str] | None = None,
                   non_executable: list[str] | None = None) -> None:
    """files: relative-path -> content (regular file). Every path under
    bin/ is chmod +x by default -- SVC-1 requires run.path to resolve to a
    regular EXECUTABLE file, and a fixture meant to be otherwise-valid must
    actually satisfy that or it is testing the wrong thing.
    symlinks: relative-path -> target (may be relative, may escape).
    dirs_as_files: relative-path that should be a directory standing in
    for what the manifest calls an executable file (non-regular check).
    non_executable: relative-path(s) under files= that must be chmod 0644
    despite living under bin/ -- for a fixture that specifically wants a
    present, regular, non-executable file.
    """
    fixture_dir = os.path.join(base_dir, name)
    if os.path.exists(fixture_dir):
        shutil.rmtree(fixture_dir)
    os.makedirs(fixture_dir)

    if manifest is not None:
        with open(os.path.join(fixture_dir, "pak.json"), "w") as f:
            json.dump(manifest, f, indent=2, sort_keys=False)
            f.write("\n")

    with open(os.path.join(fixture_dir, "expect.json"), "w") as f:
        json.dump(expect, f, indent=2)
        f.write("\n")

    for rel, content in (files or {}).items():
        p = os.path.join(fixture_dir, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w") as f:
            f.write(content)
        if rel.startswith("bin/"):
            os.chmod(p, 0o755)

    for rel in non_executable or []:
        os.chmod(os.path.join(fixture_dir, rel), 0o644)

    for rel, target in (symlinks or {}).items():
        p = os.path.join(fixture_dir, rel)
        parent = os.path.dirname(p)
        if parent:
            os.makedirs(parent, exist_ok=True)
        os.symlink(target, p)

    for rel in dirs_as_files or []:
        p = os.path.join(fixture_dir, rel)
        os.makedirs(p, exist_ok=True)


def mutate(**overrides):
    """Deep-copy BASE and apply dotted-path overrides, e.g. service__id='x'."""
    m = copy.deepcopy(BASE)
    for dotted, value in overrides.items():
        parts = dotted.split("__")
        node = m
        for part in parts[:-1]:
            node = node[part]
        if value is _DELETE:
            del node[parts[-1]]
        else:
            node[parts[-1]] = value
    return m


_DELETE = object()


def main() -> None:
    if os.path.isdir(VALID_DIR):
        shutil.rmtree(VALID_DIR)
    if os.path.isdir(INVALID_DIR):
        shutil.rmtree(INVALID_DIR)
    os.makedirs(VALID_DIR)
    os.makedirs(INVALID_DIR)

    # ---- valid/ --------------------------------------------------------

    write_fixture(VALID_DIR, "full", BASE, {"valid": True},
                  files={"bin/leaf-syncthing": "#!/bin/sh\nexit 0\n"})

    minimal = {
        "id": "org.umrk.thing",
        "service": {
            "schema": 1,
            "id": "org.umrk.thing",
            "run": {"path": "bin/thing"},
            "restart": "no",
            "default_enabled": False,
        },
    }
    write_fixture(VALID_DIR, "minimal", minimal, {"valid": True},
                  files={"bin/thing": "#!/bin/sh\nexit 0\n"})

    stop_grace_absent = mutate(service__stop_grace_ms=_DELETE)
    write_fixture(VALID_DIR, "stop-grace-ms-absent", stop_grace_absent,
                  {"valid": True, "resolved_stop_grace_ms": 5000,
                   "note": "absent stop_grace_ms resolves to the SVC-1 default"},
                  files={"bin/leaf-syncthing": "x"})

    stop_grace_clamped = mutate(service__stop_grace_ms=20000)
    write_fixture(VALID_DIR, "stop-grace-ms-over-ceiling-clamped", stop_grace_clamped,
                  {"valid": True, "resolved_stop_grace_ms": 15000,
                   "note": "declared value above the 15000ms ceiling is valid at "
                           "schema level and clamped (and logged) at load time, "
                           "never rejected"},
                  files={"bin/leaf-syncthing": "x"})

    lifecycle_stop = mutate(service__lifecycle={"game": "stop"})
    write_fixture(VALID_DIR, "lifecycle-game-stop", lifecycle_stop, {"valid": True},
                  files={"bin/leaf-syncthing": "x"})

    lifecycle_ignore_implicit = mutate(service__lifecycle=_DELETE)
    write_fixture(VALID_DIR, "lifecycle-absent-defaults-ignore", lifecycle_ignore_implicit,
                  {"valid": True, "note": "absent lifecycle means game: ignore, "
                                          "stop_on_storage_change: false, stop_on_suspend: false"},
                  files={"bin/leaf-syncthing": "x"})

    restart_no = mutate(service__restart="no")
    write_fixture(VALID_DIR, "restart-no", restart_no, {"valid": True},
                  files={"bin/leaf-syncthing": "x"})

    args_at_max = mutate(service__run={
        "path": "bin/leaf-syncthing",
        "args": ["x" * 256 for _ in range(16)],
    })
    write_fixture(VALID_DIR, "args-at-max", args_at_max, {"valid": True},
                  files={"bin/leaf-syncthing": "x"})

    revoke_and_retained_at_max = mutate(
        state__revoke_on_uninstall=[f"leaf/token-{i}.json" for i in range(16)],
        state__retained_roots=[f"Syncthing/root-{i}" for i in range(16)],
    )
    write_fixture(VALID_DIR, "revoke-and-retained-at-max", revoke_and_retained_at_max,
                  {"valid": True}, files={"bin/leaf-syncthing": "x"})

    no_state = mutate(state=_DELETE)
    write_fixture(VALID_DIR, "no-state-object", no_state,
                  {"valid": True, "note": "state is optional; a service with no "
                                          "durable userdata may omit it entirely"},
                  files={"bin/leaf-syncthing": "x"})

    # ---- invalid/ -------------------------------------------------------

    unknown_schema = mutate(service__schema=2)
    write_fixture(INVALID_DIR, "unknown-schema", unknown_schema,
                  {"valid": False, "reason": "unknown-schema"},
                  files={"bin/leaf-syncthing": "x"})

    malformed_id = mutate(id="Org_Umrk_Syncthing", service__id="Org_Umrk_Syncthing")
    write_fixture(INVALID_DIR, "malformed-id", malformed_id,
                  {"valid": False, "reason": "malformed-id"},
                  files={"bin/leaf-syncthing": "x"})

    id_mismatch = mutate(id="org.umrk.syncthing.other")
    write_fixture(INVALID_DIR, "id-mismatch", id_mismatch,
                  {"valid": False, "reason": "id-mismatch"},
                  files={"bin/leaf-syncthing": "x"})

    absolute_run_path = mutate(service__run={"path": "/bin/leaf-syncthing"})
    write_fixture(INVALID_DIR, "absolute-run-path", absolute_run_path,
                  {"valid": False, "reason": "absolute-run-path"})

    traversal_run_path = mutate(service__run={"path": "../escape/bin/leaf-syncthing"})
    write_fixture(INVALID_DIR, "run-path-traversal", traversal_run_path,
                  {"valid": False, "reason": "path-traversal"})

    escaping_symlink = mutate(service__run={"path": "bin/leaf-syncthing"})
    write_fixture(INVALID_DIR, "run-path-escaping-symlink", escaping_symlink,
                  {"valid": False, "reason": "escaping-symlink",
                   "note": "bin/leaf-syncthing is a symlink resolving outside the pak root"},
                  symlinks={"bin/leaf-syncthing": "../../../../outside/binary"})

    non_regular = mutate(service__run={"path": "bin/leaf-syncthing"})
    write_fixture(INVALID_DIR, "run-path-non-regular", non_regular,
                  {"valid": False, "reason": "non-regular-executable",
                   "note": "bin/leaf-syncthing is a directory, not a regular file"},
                  dirs_as_files=["bin/leaf-syncthing"])

    run_path_missing = mutate(service__run={"path": "bin/does-not-exist"})
    write_fixture(INVALID_DIR, "run-path-missing", run_path_missing,
                  {"valid": False, "reason": "run-path-missing",
                   "note": "bin/does-not-exist is never created by this fixture"})

    run_path_not_executable = mutate(service__run={"path": "bin/leaf-syncthing"})
    write_fixture(INVALID_DIR, "run-path-not-executable", run_path_not_executable,
                  {"valid": False, "reason": "run-path-not-executable",
                   "note": "bin/leaf-syncthing exists and is a regular file, but has no execute bit"},
                  files={"bin/leaf-syncthing": "#!/bin/sh\nexit 0\n"},
                  non_executable=["bin/leaf-syncthing"])

    args_count_over = mutate(service__run={
        "path": "bin/leaf-syncthing", "args": ["a"] * 17,
    })
    write_fixture(INVALID_DIR, "args-count-over-limit", args_count_over,
                  {"valid": False, "reason": "args-count-over-limit"},
                  files={"bin/leaf-syncthing": "x"})

    args_item_too_long = mutate(service__run={
        "path": "bin/leaf-syncthing", "args": ["a" * 257],
    })
    write_fixture(INVALID_DIR, "args-item-too-long", args_item_too_long,
                  {"valid": False, "reason": "args-item-too-long"},
                  files={"bin/leaf-syncthing": "x"})

    # 256 'e-acute' characters: 256 Unicode code points (would pass a
    # code-point-counting check) but 512 UTF-8 bytes (over SVC-1's real
    # 256-BYTE limit). Catches an implementation that measures characters.
    args_item_too_long_multibyte = mutate(service__run={
        "path": "bin/leaf-syncthing", "args": ["é" * 256],
    })
    write_fixture(INVALID_DIR, "args-item-too-long-multibyte", args_item_too_long_multibyte,
                  {"valid": False, "reason": "args-item-too-long",
                   "note": "the arg is exactly 256 Unicode code points (u+00e9, "
                           "2 bytes each in UTF-8) but 512 UTF-8 bytes, over the "
                           "256-byte limit; a code-point-counting check would "
                           "wrongly accept this"},
                  files={"bin/leaf-syncthing": "x"})

    unknown_restart = mutate(service__restart="always")
    write_fixture(INVALID_DIR, "unknown-restart-policy", unknown_restart,
                  {"valid": False, "reason": "unknown-restart-policy"},
                  files={"bin/leaf-syncthing": "x"})

    unknown_lifecycle_key = mutate(service__lifecycle={
        "game": "notify", "stop_on_storage_change": True,
        "stop_on_suspend": True, "restart_on_wifi_change": True,
    })
    write_fixture(INVALID_DIR, "unknown-lifecycle-key", unknown_lifecycle_key,
                  {"valid": False, "reason": "unknown-lifecycle-key"},
                  files={"bin/leaf-syncthing": "x"})

    default_enabled_true = mutate(service__default_enabled=True)
    write_fixture(INVALID_DIR, "default-enabled-true", default_enabled_true,
                  {"valid": False, "reason": "default-enabled-true"},
                  files={"bin/leaf-syncthing": "x"})

    stop_grace_negative = mutate(service__stop_grace_ms=-1)
    write_fixture(INVALID_DIR, "stop-grace-ms-negative", stop_grace_negative,
                  {"valid": False, "reason": "stop-grace-ms-negative"},
                  files={"bin/leaf-syncthing": "x"})

    stop_grace_non_integer = mutate(service__stop_grace_ms=10.5)
    write_fixture(INVALID_DIR, "stop-grace-ms-non-integer", stop_grace_non_integer,
                  {"valid": False, "reason": "stop-grace-ms-non-integer"},
                  files={"bin/leaf-syncthing": "x"})

    revoke_traversal = mutate(state__revoke_on_uninstall=["../outside.json"])
    write_fixture(INVALID_DIR, "revoke-on-uninstall-traversal", revoke_traversal,
                  {"valid": False, "reason": "revoke-on-uninstall-traversal"},
                  files={"bin/leaf-syncthing": "x"})

    revoke_absolute = mutate(state__revoke_on_uninstall=["/etc/passwd"])
    write_fixture(INVALID_DIR, "revoke-on-uninstall-absolute", revoke_absolute,
                  {"valid": False, "reason": "revoke-on-uninstall-absolute"},
                  files={"bin/leaf-syncthing": "x"})

    revoke_symlink_component = mutate(state__revoke_on_uninstall=["leaf/trusted-clients.json"])
    write_fixture(INVALID_DIR, "revoke-on-uninstall-symlink-component", revoke_symlink_component,
                  {"valid": False, "reason": "revoke-on-uninstall-symlink-component",
                   "note": "state.root/leaf is a symlink; no existing component of a "
                           "revoke_on_uninstall path may be a symlink"},
                  files={"bin/leaf-syncthing": "x"},
                  symlinks={"Syncthing/leaf": "/tmp"})

    revoke_count_over = mutate(state__revoke_on_uninstall=[f"leaf/t-{i}.json" for i in range(17)])
    write_fixture(INVALID_DIR, "revoke-on-uninstall-count-over-limit", revoke_count_over,
                  {"valid": False, "reason": "revoke-on-uninstall-count-over-limit"},
                  files={"bin/leaf-syncthing": "x"})

    # state.root itself is a symlink -- every revoke_on_uninstall entry is
    # beneath it, so a symlinked root is exactly as dangerous as a
    # symlinked intermediate/leaf component and must be rejected the same
    # way. Previously the walk started INSIDE state.root and never checked
    # state.root itself.
    revoke_root_symlink = mutate(
        state__root="Syncthing",
        state__revoke_on_uninstall=["trusted-clients.json"],
    )
    write_fixture(INVALID_DIR, "revoke-on-uninstall-root-symlink", revoke_root_symlink,
                  {"valid": False, "reason": "revoke-on-uninstall-symlink-component",
                   "note": "state.root (Syncthing) itself is a symlink, not a directory"},
                  files={"bin/leaf-syncthing": "x"},
                  symlinks={"Syncthing": "/tmp"})

    # The FINAL path component (the leaf file itself) is a symlink. Only
    # checking components up to but excluding the leaf (rp.split("/")[:-1])
    # missed exactly this case.
    revoke_leaf_symlink = mutate(
        state__root="Syncthing",
        state__revoke_on_uninstall=["trusted-clients.json"],
    )
    write_fixture(INVALID_DIR, "revoke-on-uninstall-leaf-symlink", revoke_leaf_symlink,
                  {"valid": False, "reason": "revoke-on-uninstall-symlink-component",
                   "note": "Syncthing/trusted-clients.json (the declared leaf entry itself) "
                           "is an existing symlink, not the real file"},
                  files={"bin/leaf-syncthing": "x"},
                  symlinks={"Syncthing/trusted-clients.json": "/tmp/elsewhere"})

    retained_traversal = mutate(state__retained_roots=["../outside"])
    write_fixture(INVALID_DIR, "retained-roots-traversal", retained_traversal,
                  {"valid": False, "reason": "retained-roots-traversal"},
                  files={"bin/leaf-syncthing": "x"})

    retained_absolute = mutate(state__retained_roots=["/mnt/sdcard"])
    write_fixture(INVALID_DIR, "retained-roots-absolute", retained_absolute,
                  {"valid": False, "reason": "retained-roots-absolute"},
                  files={"bin/leaf-syncthing": "x"})

    retained_count_over = mutate(state__retained_roots=[f"root-{i}" for i in range(17)])
    write_fixture(INVALID_DIR, "retained-roots-count-over-limit", retained_count_over,
                  {"valid": False, "reason": "retained-roots-count-over-limit"},
                  files={"bin/leaf-syncthing": "x"})

    # Pairwise duplicate-id case: two otherwise-valid manifests sharing the
    # same service.id. Both must become unavailable -- neither wins.
    dup_dir = os.path.join(INVALID_DIR, "duplicate-id")
    if os.path.exists(dup_dir):
        shutil.rmtree(dup_dir)
    os.makedirs(dup_dir)
    dup_a = mutate(id="org.umrk.syncthing", service__id="org.umrk.syncthing")
    dup_b = mutate(id="org.umrk.syncthing", service__id="org.umrk.syncthing",
                    service__run={"path": "bin/leaf-syncthing", "args": ["--alternate-build"]})
    with open(os.path.join(dup_dir, "pak-a.json"), "w") as f:
        json.dump(dup_a, f, indent=2)
        f.write("\n")
    with open(os.path.join(dup_dir, "pak-b.json"), "w") as f:
        json.dump(dup_b, f, indent=2)
        f.write("\n")
    with open(os.path.join(dup_dir, "expect.json"), "w") as f:
        json.dump({
            "valid": False,
            "reason": "duplicate-service-id",
            "note": "pak-a.json and pak-b.json declare the same service.id "
                    "(org.umrk.syncthing); both services become unavailable, "
                    "neither wins",
            "applies_to": ["pak-a.json", "pak-b.json"],
        }, f, indent=2)
        f.write("\n")
    os.makedirs(os.path.join(dup_dir, "bin"), exist_ok=True)
    dup_bin = os.path.join(dup_dir, "bin", "leaf-syncthing")
    with open(dup_bin, "w") as f:
        f.write("x")
    os.chmod(dup_bin, 0o755)

    print("Generated manifest fixtures.")


if __name__ == "__main__":
    main()
