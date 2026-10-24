#!/usr/bin/env python3
"""Materialize A2 fixture paks into an ignored build directory.

The behavior paks are Jawaka-owned templates. Invalid manifests stay owned by
the frozen A0 contract tree and are copied only into build output so there is
no second vendored fixture set to drift.
"""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil


def copy_entry(source: Path, target: Path) -> None:
    if source.is_symlink():
        target.parent.mkdir(parents=True, exist_ok=True)
        target.symlink_to(os.readlink(source))
    elif source.is_dir():
        shutil.copytree(source, target, symlinks=True)
    else:
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target, follow_symlinks=False)


def copy_invalid_payload(source: Path, pak: Path, userdata: Path) -> None:
    pak.mkdir(parents=True, exist_ok=True)
    userdata.mkdir(parents=True, exist_ok=True)
    for entry in source.iterdir():
        if entry.name in {"expect.json", "pak-a.json", "pak-b.json"}:
            continue
        copy_entry(entry, pak / entry.name)
        userdata_target = userdata / entry.name
        if (entry.name not in {"bin", "pak.json"} and
                not os.path.lexists(userdata_target)):
            copy_entry(entry, userdata_target)


def materialize_invalid(canonical: Path, output: Path) -> int:
    count = 0
    for source in sorted(path for path in canonical.iterdir() if path.is_dir()):
        case = output / source.name
        apps = case / "Apps"
        userdata = case / "Userdata"
        case.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / "expect.json", case / "expect.json")
        if source.name == "duplicate-id":
            for suffix in ("a", "b"):
                pak = apps / f"duplicate-{suffix}.pak"
                copy_invalid_payload(source, pak, userdata)
                shutil.copy2(source / f"pak-{suffix}.json", pak / "pak.json")
        else:
            pak = apps / f"{source.name}.pak"
            copy_invalid_payload(source, pak, userdata)
        count += 1
    return count


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--templates", type=Path, required=True)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--canonical-invalid", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    for path in (args.templates, args.binary, args.canonical_invalid):
        if not path.exists():
            parser.error(f"required input does not exist: {path}")

    output = args.output.resolve()
    if output.name != "service-fixtures":
        parser.error("--output must name a dedicated service-fixtures directory")

    if output.exists():
        shutil.rmtree(output)
    valid_apps = output / "valid" / "Apps"
    valid_apps.mkdir(parents=True)

    valid_count = 0
    for template in sorted(args.templates.glob("*.pak")):
        target = valid_apps / template.name
        shutil.copytree(template, target, symlinks=True)
        binary = target / "bin" / "service-fixture"
        binary.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(args.binary, binary)
        binary.chmod(0o755)
        valid_count += 1

    invalid_count = materialize_invalid(
        args.canonical_invalid, output / "invalid"
    )
    if valid_count != 5 or invalid_count < 27:
        raise SystemExit(
            f"incomplete fixture set: {valid_count} valid, {invalid_count} invalid"
        )
    manifest = {
        "valid_behavior_paks": valid_count,
        "canonical_invalid_paks": invalid_count,
    }
    (output / "MANIFEST.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    print(f"materialized {valid_count} behavior + {invalid_count} invalid fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
