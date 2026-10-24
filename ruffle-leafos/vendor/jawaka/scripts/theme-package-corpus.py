#!/usr/bin/env python3
"""Write the THEME-1 parity corpus for `make theme-package-test`.

leaf-contracts checks its reference validator against three kinds of input:
the committed fixtures, one fixture too large to commit (built in memory), and
a few hundred boundary variants built in memory. The C validator has to agree
with all of them, so this script materializes the in-memory ones as files and
records what the reference validator says about every file it writes.

    theme-package-corpus.py <leaf-contracts>/contracts/leaf-themes <out-dir>

<out-dir>/index.json lists {file, label, reasons, warnings}; every file path is
relative to <out-dir>. Nothing from the contracts checkout is modified.
"""
from __future__ import annotations

import json
import os
import shutil
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    themes_root = os.path.abspath(sys.argv[1])
    out_dir = os.path.abspath(sys.argv[2])
    scripts = os.path.join(themes_root, "scripts")
    workspace = os.path.dirname(os.path.dirname(themes_root))
    # leaf-services/scripts has a validate_fixtures.py of its own: search it
    # after this contract's scripts, not before.
    sys.path.insert(0, os.path.join(workspace, "contracts", "leaf-services", "scripts"))
    sys.path.insert(0, scripts)
    sys.dont_write_bytecode = True

    import gen_theme_fixtures as gen  # noqa: E402
    import theme_model  # noqa: E402
    import validate_fixtures as vf  # noqa: E402

    shutil.rmtree(out_dir, ignore_errors=True)
    os.makedirs(os.path.join(out_dir, "fixtures"))
    os.makedirs(os.path.join(out_dir, "variants"))
    index: list[dict] = []
    skipped: list[str] = []

    def record(rel: str, label: str, archive: bytes) -> None:
        path = os.path.join(out_dir, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(archive)
        reasons, warnings = theme_model.validate_archive(path)
        index.append({"file": rel, "label": label, "reasons": reasons, "warnings": warnings})

    with open(os.path.join(themes_root, "fixtures", "expect.json"), encoding="utf-8") as handle:
        expect = json.load(handle)
    for case in expect["fixtures"]:
        if case.get("in_memory"):
            record(os.path.join("fixtures", case["file"]), case["file"],
                   gen.fixture_bytes(case["file"]))

    def variant(label: str, archive: bytes) -> None:
        record(os.path.join("variants", f"{len(index):04d}.zip"), label, archive)

    # Archive, image and byte-level manifest variants all go through
    # check_archive; capture the archive instead of judging it there.
    vf.check_archive = lambda label, archive, _reasons, _warnings=(): variant(label, archive)
    vf.run_manifest_parse_variants()
    vf.run_image_variants()
    vf.run_archive_variants()

    # Field variants are checked on a parsed object. Wrap each in an otherwise
    # valid archive whose folder matches its id, so the C side sees the same
    # object through the same stages a device would.
    manifests: list[object] = []
    original = theme_model.validate_manifest

    def capture(obj):
        manifests.append(obj)
        return original(obj)

    theme_model.validate_manifest = capture
    vf.run_manifest_variants()
    theme_model.validate_manifest = original
    for obj in manifests:
        if not isinstance(obj, dict):
            # theme_model.validate_archive raises on a theme.json that is not
            # an object (it calls obj.get on the parsed value), so there is no
            # reference answer to compare against.
            skipped.append(f"manifest {json.dumps(obj)[:40]}: not an object")
            continue
        root = obj["id"] if isinstance(obj.get("id"), str) and \
            theme_model.ID_RE.fullmatch(obj["id"]) else "variant"
        files = gen.base_files(root)
        files["theme.json"] = (json.dumps(obj, indent=2) + "\n").encode("utf-8")
        variant(f"manifest {json.dumps(obj, sort_keys=True)[:120]}", gen.theme_zip(root, files))

    with open(os.path.join(out_dir, "index.json"), "w", encoding="utf-8") as handle:
        json.dump({"cases": index, "skipped": skipped}, handle, indent=1)
    print(f"theme-package corpus: {len(index)} archives, {len(skipped)} skipped")
    for note in skipped:
        print(f"  skipped {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
