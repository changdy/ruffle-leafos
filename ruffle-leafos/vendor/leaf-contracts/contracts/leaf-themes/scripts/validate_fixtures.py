#!/usr/bin/env python3
"""Validate every THEME-1 fixture against theme_model.py and the schema.

- Every fixture archive has a hand-written expectation in fixtures/expect.json,
  and the model must report exactly those reasons and warnings.
- Every valid fixture's theme.json passes theme-v1.schema.json. Every invalid
  one for a static theme.json field rule fails it; every other invalid fixture
  whose theme.json parses still passes it, so the schema and the model agree
  on which rules are about shape.
- Every reason slug the model knows has exactly one invalid fixture, and every
  warning slug appears in some valid fixture.
- Boundary and type variants of each rule are built in memory and must keep
  that rule's reason, so one named fixture per rule still covers its edges.

Exit 0 on success; on any mismatch print it and exit 1.
"""
from __future__ import annotations

import copy
import json
import os
import struct
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORKSPACE = os.path.dirname(os.path.dirname(ROOT))
sys.path.insert(0, HERE)
# minischema is shared with the other contracts rather than copied.
sys.path.insert(0, os.path.join(WORKSPACE, "contracts", "leaf-services", "scripts"))

import gen_theme_fixtures as gen  # noqa: E402
import minischema  # noqa: E402
import theme_model  # noqa: E402

FAILURES: list[str] = []
VERBOSE = "-v" in sys.argv or "--verbose" in sys.argv

# Reasons a JSON Schema can see: the shape of theme.json alone.
SCHEMA_REASONS = {
    "theme-unknown-schema", "theme-unknown-field", "theme-id-invalid",
    "theme-name-invalid", "theme-author-invalid", "theme-version-invalid",
    "theme-min-leaf-version", "theme-unknown-license", "theme-description-invalid",
    "theme-grid-invalid", "theme-color-invalid", "theme-color-level-invalid",
    "theme-status-style-invalid",
}


def fail(msg: str) -> None:
    FAILURES.append(msg)
    print(f"FAIL: {msg}")


def ok(msg: str) -> None:
    if VERBOSE:
        print(f"ok:   {msg}")


def load_json(path: str):
    with open(path, encoding="utf-8") as handle:
        return json.load(handle)


SCHEMA = load_json(os.path.join(ROOT, "theme-v1.schema.json"))


def manifest_of(path: str):
    """theme.json from a fixture, or None when it cannot be read or parsed."""
    try:
        with zipfile.ZipFile(path) as archive:
            for info in archive.infolist():
                if info.filename.endswith("/theme.json") and info.filename.count("/") == 1 \
                        and info.file_size <= theme_model.MAX_MANIFEST_BYTES \
                        and info.compress_type in (0, 8):
                    return theme_model.parse_manifest_bytes(archive.read(info))
    except Exception:  # noqa: BLE001 - any unreadable fixture just has no manifest
        return None
    return None


# ---------------------------------------------------------------------------
# Fixture archives
# ---------------------------------------------------------------------------

def findings_disagree(path: str, got) -> str | None:
    """validate_archive_findings must name exactly validate_archive's slugs."""
    reasons, warnings = theme_model.validate_archive_findings(path)
    slugs = (sorted({slug for slug, _ in reasons}), sorted({slug for slug, _ in warnings}))
    if slugs != tuple(got):
        return f"findings name {slugs}, validate_archive says {tuple(got)}"
    return None


def run_fixtures() -> None:
    fixtures_dir = os.path.join(ROOT, "fixtures")
    document = load_json(os.path.join(fixtures_dir, "expect.json"))
    expected_files = {e["file"] for e in document["fixtures"] if not e.get("in_memory")}
    on_disk = {f"{group}/{name}" for group in ("valid", "invalid")
               for name in os.listdir(os.path.join(fixtures_dir, group))}
    if expected_files != on_disk:
        fail(f"fixtures without expectations {sorted(on_disk - expected_files)}, "
             f"expectations without fixtures {sorted(expected_files - on_disk)}")

    claimed: dict[str, list[str]] = {}
    warned: set[str] = set()
    for case in document["fixtures"]:
        rel, reasons, warnings = case["file"], case["reasons"], case["warnings"]
        path = os.path.join(fixtures_dir, rel)
        if rel.startswith("valid/") and reasons:
            fail(f"{rel}: a valid fixture may not expect reasons")
            continue
        if rel.startswith("invalid/") and len(reasons) != 1:
            fail(f"{rel}: an invalid fixture must fail for exactly one reason")
            continue
        if case.get("in_memory"):
            with tempfile.NamedTemporaryFile(suffix=".zip", delete=False) as handle:
                handle.write(gen.fixture_bytes(rel))
                path = handle.name
        try:
            got = theme_model.validate_archive(path)
            disagree = findings_disagree(path, got)
            obj = manifest_of(path)
        finally:
            if case.get("in_memory"):
                os.unlink(path)
        if got != (sorted(reasons), sorted(warnings)):
            fail(f"{rel}: got reasons {got[0]} warnings {got[1]}, expected "
                 f"{sorted(reasons)} {sorted(warnings)}")
            continue
        if disagree:
            fail(f"{rel}: {disagree}")
            continue
        for reason in reasons:
            claimed.setdefault(reason, []).append(rel)
        if not reasons:
            warned.update(warnings)

        if obj is not None:
            schema_ok, schema_err = minischema.is_valid(obj, SCHEMA)
            want_ok = not (set(reasons) & SCHEMA_REASONS)
            if schema_ok != want_ok:
                fail(f"{rel}: schema {'accepted' if schema_ok else 'rejected'} it "
                     f"({schema_err}) but the fixture expects {reasons or 'acceptance'}")
                continue
        elif not reasons:
            fail(f"{rel}: a valid fixture's theme.json must parse")
            continue
        ok(f"{rel} -> {reasons or 'accepted'} {warnings or ''}")

    for reason in theme_model.REASONS:
        if len(claimed.get(reason, [])) != 1:
            fail(f"reason {reason!r} has {len(claimed.get(reason, []))} invalid fixtures; "
                 f"exactly one is required")
    unknown = set(claimed) - set(theme_model.REASONS)
    if unknown:
        fail(f"fixtures expect reasons the model does not define: {sorted(unknown)}")
    for warning in theme_model.WARNINGS:
        if warning not in warned:
            fail(f"warning {warning!r} is not exercised by any valid fixture")

    valid = sum(1 for e in document["fixtures"] if e["file"].startswith("valid/"))
    print(f"THEME-1 fixtures: {valid} valid, {len(document['fixtures']) - valid} invalid, "
          f"{len(claimed)} distinct rejection reasons, {len(warned)} warnings")


# ---------------------------------------------------------------------------
# In-memory variants
# ---------------------------------------------------------------------------

def check_archive(label: str, archive: bytes, reasons, warnings=()) -> None:
    with tempfile.NamedTemporaryFile(suffix=".zip", delete=False) as handle:
        handle.write(archive)
        path = handle.name
    try:
        got = theme_model.validate_archive(path)
        disagree = findings_disagree(path, got)
    finally:
        os.unlink(path)
    want = (sorted(reasons), sorted(warnings))
    if got != want:
        fail(f"variant {label}: got {got}, expected {want}")
    elif disagree:
        fail(f"variant {label}: {disagree}")
    else:
        ok(f"variant {label} -> {reasons or 'accepted'}")


def run_manifest_variants() -> int:
    base = gen.manifest("variant")
    count = 0

    def check(label, edits, reason, schema_agrees=True):
        nonlocal count
        count += 1
        obj = copy.deepcopy(base)
        for key, value in edits.items():
            if value is gen.DELETE:
                obj.pop(key, None)
            else:
                obj[key] = value
        want = [reason] if reason else []
        got = theme_model.validate_manifest(obj)
        if got != want:
            fail(f"manifest variant {label}: got {got}, expected {want}")
            return
        schema_ok = minischema.is_valid(obj, SCHEMA)[0]
        if schema_agrees and schema_ok != (reason is None):
            fail(f"manifest variant {label}: schema {'accepted' if schema_ok else 'rejected'}")
            return
        ok(f"manifest variant {label} -> {want or 'accepted'}")

    for value in (1, 1.0):
        check(f"schema {value!r}", {"schema": value}, None)
    for value in (True, "1", 2, None, gen.DELETE):
        check(f"schema {value!r}", {"schema": value}, "theme-unknown-schema")
    check("base", {}, None)
    if theme_model.validate_manifest(["not", "an", "object"]) != ["theme-malformed-manifest"]:
        fail("manifest variant array: expected theme-malformed-manifest")
    for value in ("ab", "a" * 40, "0-9", "neon-nights"):
        check(f"id {value!r}", {"id": value}, None)
    for value in ("a", "a" * 41, "-ab", "Ab", "a_b", "a.b", 7, gen.DELETE):
        check(f"id {value!r}", {"id": value}, "theme-id-invalid")
    for value in ("N", "n" * 40, "Nuit étoilée", "  padded  "):
        check(f"name {value!r}", {"name": value}, None)
    for value in ("", " ", "n" * 41, "tab\there", "\u0000", 5, gen.DELETE):
        check(f"name {value!r}", {"name": value}, "theme-name-invalid")
    # 32 three-byte characters are within 40 characters but over 95 bytes.
    check("name over 95 bytes", {"name": "\u3042" * 32}, "theme-name-invalid",
          schema_agrees=False)
    check("name at 95 bytes", {"name": "\u3042" * 31 + "ab"}, None)
    check("name lone surrogate", {"name": "\ud800"}, "theme-name-invalid",
          schema_agrees=False)
    for value in ("a" * 60,):
        check("author 60 chars", {"author": value}, None)
    check("author 61 chars", {"author": "a" * 61}, "theme-author-invalid")
    check("author over 63 bytes", {"author": "\u00e9" * 32}, "theme-author-invalid",
          schema_agrees=False)
    check("author missing", {"author": gen.DELETE}, "theme-author-invalid")
    for value in ("0.0.0", "9999.9999.9999", "1.20.300"):
        check(f"version {value!r}", {"version": value}, None)
    for value in ("1.0", "01.0.0", "1.0.0-beta.1", "10000.0.0", " 1.0.0", 1, gen.DELETE):
        check(f"version {value!r}", {"version": value}, "theme-version-invalid")
    for value in ("0.12.0", "0.12.7", "0.13.0", "0.100.0", "1.0.0", "12.0.0"):
        check(f"min_leaf_version {value!r}", {"min_leaf_version": value}, None)
    for value in ("0.11.99", "0.9.0", "0.1.0", "0.12", "0.012.0", None, gen.DELETE):
        check(f"min_leaf_version {value!r}", {"min_leaf_version": value},
              "theme-min-leaf-version")
    for value in theme_model.LICENSES:
        check(f"license {value!r}", {"license": value}, None)
    for value in ("cc-by-4.0", "MIT", "", ["CC0-1.0"], gen.DELETE):
        check(f"license {value!r}", {"license": value}, "theme-unknown-license")
    for value in ("", "d" * 300, "line one\nline two"):
        check(f"description {len(value)} chars", {"description": value}, None)
    for value in ("d" * 301, "tab\there", "cr\r\n", None):
        check(f"description {value!r:.20}", {"description": value}, "theme-description-invalid")
    for grid in ({"cols": 1, "rows": 1}, {"cols": 8, "rows": 6}):
        check(f"grid {grid}", {"grid": grid}, None)
    for grid in ({"cols": 0, "rows": 2}, {"cols": 3, "rows": 7}, {"cols": 3.0, "rows": 2},
                 {"cols": True, "rows": 2}, {"cols": 3}, {"rows": 2}, [3, 2], "3x2"):
        check(f"grid {grid}", {"grid": grid}, "theme-grid-invalid")
    check("grid extra key", {"grid": {"cols": 3, "rows": 2, "gap": 4}}, "theme-unknown-field")
    for color in ("#000000", "#ffffff", "#A1b2C3d4"):
        check(f"color {color}", {"colors": {"focus_ring": color}}, None)
    for color in ("000000", "#00000", "#0000000", "#GGGGGG", "#000000000", 0xFFFFFF, None):
        check(f"color {color!r}", {"colors": {"tile_border": color}}, "theme-color-invalid")
    check("colors not object", {"colors": "#FFFFFF"}, "theme-color-invalid")
    check("colors extra key", {"colors": {"background": "#000000"}}, "theme-unknown-field")
    for level in (0, 255):
        check(f"shadow {level}", {"colors": {"shadow": level}}, None)
    for level in (-1, 256, 1.5, 128.0, "128", True):
        check(f"underlay_opacity {level!r}", {"colors": {"underlay_opacity": level}},
              "theme-color-level-invalid")
    for style in theme_model.STATUS_STYLES:
        check(f"status_style {style}", {"status_style": style}, None)
    for style in ("Auto", "", None, 1):
        check(f"status_style {style!r}", {"status_style": style}, "theme-status-style-invalid")
    return count


def run_manifest_parse_variants() -> int:
    """Byte-level theme.json rules that no parsed object can express."""
    root = "parse"
    cases = [
        ("utf-8 BOM", b"\xef\xbb\xbf" + gen.manifest_bytes(gen.manifest(root))),
        ("latin-1 bytes", gen.manifest_bytes(gen.manifest(root, name="caf\u00e9"))
         .replace(b"\xc3\xa9", b"\xe9")),
        ("duplicate key", gen.manifest_bytes(gen.manifest(root))
         .replace(b'"schema": 1,', b'"schema": 1, "schema": 1,')),
        ("NaN", gen.manifest_bytes(gen.manifest(root))
         .replace(b'"schema": 1,', b'"schema": 1, "grid": {"cols": NaN, "rows": 2},')),
        ("empty", b""),
        ("top-level array", b'["not", "an", "object"]\n'),
        ("top-level string", b'"theme"\n'),
        ("top-level number", b"1\n"),
        ("top-level null", b"null\n"),
    ]
    for label, data in cases:
        files = gen.base_files(root)
        files["theme.json"] = data
        check_archive(f"manifest {label}", gen.theme_zip(root, files),
                      ["theme-malformed-manifest"])
    files = gen.base_files(root)
    exact = gen.manifest_bytes(gen.manifest(root))
    files["theme.json"] = exact[:-2] + b" " * (65536 - len(exact)) + exact[-2:]
    check_archive("manifest at 64 KiB", gen.theme_zip(root, files), [])
    return len(cases) + 1


def run_image_variants() -> int:
    root = "images"
    cases = [
        # (rel, data, reasons, warnings)
        ("grid/icons/GBA.png", gen.png(1024, 1024), [], ["theme-icon-off-size"]),
        ("grid/icons/GBA.png", gen.png(1024, 1025), ["theme-image-dimensions"], []),
        ("grid/icons/GBA.png", gen.png(0, 512), ["theme-image-dimensions"], []),
        ("coverflow/icons/GBA.png", gen.png(512, 256), [], ["theme-icon-off-size"]),
        ("grid/labels/GBA.png", gen.png(1024, 1), [], []),
        ("grid/wordmarks/GBA.png", gen.png(1025, 100), ["theme-image-dimensions"], []),
        ("grid/wordmarks/GBA.color.png", gen.png(100, 1025),
         ["theme-image-dimensions"], []),
        ("grid/icons/_apps.png", gen.png(512, 512), [], []),
        ("coverflow/icons/_apps.png", gen.png(512, 512), [], []),
        ("grid/labels/_apps.png", gen.png(512, 512), [], []),
        ("grid/labels/_apps.png", gen.png(1025, 512), ["theme-image-dimensions"], []),
        ("wallpaper.png", gen.png(2048, 2048), [], []),
        ("wallpaper.png", gen.png(2049, 720), ["theme-image-dimensions"], []),
        ("grid/wallpaper.jpg", gen.jpeg(2048, 2048), [], []),
        ("grid/wallpaper.jpg", gen.jpeg(960, 2049), ["theme-image-dimensions"], []),
        ("grid/wallpaper.jpeg", gen.jpeg(960, 720, sof=0xC1), [], []),
        ("grid/wallpaper.jpeg", gen.jpeg(960, 720, sof=0xC2), [], []),
        ("wallpaper.jpeg", gen.jpeg(960, 720, sof=0xC0, components=3), [], []),
        ("wallpaper.jpeg", gen.jpeg(960, 720, sof=0xC3), ["theme-unsupported-image"], []),
        ("wallpaper.jpeg", gen.jpeg(960, 720, sof=0xC9), ["theme-unsupported-image"], []),
        ("wallpaper.jpeg", gen.jpeg(960, 720, components=4), ["theme-unsupported-image"], []),
        ("wallpaper.jpeg", gen.jpeg(960, 720, precision=12), ["theme-unsupported-image"], []),
        ("wallpaper.jpeg", gen.jpeg(960, 0), ["theme-unsupported-image"], []),
        ("wallpaper.jpeg", gen.jpeg(960, 720)[:30], ["theme-unsupported-image"], []),
        ("wallpaper.jpg", gen.png(960, 720), ["theme-unsupported-image"], []),
        ("grid/labels/GBA.png", gen.png(64, 64)[:24], ["theme-unsupported-image"], []),
        ("grid/labels/GBA.png", gen.png(64, 64)[:29] + b"\0\0\0\0" + gen.png(64, 64)[33:],
         ["theme-unsupported-image"], []),
        ("grid/labels/GBA.png", b"not a png at all", ["theme-unsupported-image"], []),
        ("preview.png", gen.png(961, 720), ["theme-image-dimensions"], []),
        ("preview.png", gen.png(720, 960), ["theme-image-dimensions"], []),
        ("preview.png", gen.jpeg(960, 720), ["theme-unsupported-image"], []),
    ]
    # A 16-bit PNG depth in a palette image is not a PNG any decoder accepts.
    bad_depth = bytearray(gen.png(64, 64))
    bad_depth[24:26] = bytes((16, 3))
    bad_depth[29:33] = struct.pack(">I", __import__("zlib").crc32(bytes(bad_depth[12:29])))
    cases.append(("grid/labels/GBA.png", bytes(bad_depth), ["theme-unsupported-image"], []))
    for rel, data, reasons, warnings in cases:
        files = gen.base_files(root)
        files[rel] = data
        check_archive(f"image {rel} {len(data)}B", gen.theme_zip(root, files), reasons, warnings)
    # Wallpaper-only themes are art; a lone preview is not.
    files = gen.base_files(root)
    del files["grid/icons/FC.png"]
    files["grid/wallpaper.png"] = gen.png(960, 720)
    check_archive("wallpaper-only theme", gen.theme_zip(root, files), [])
    return len(cases) + 1


def run_archive_variants() -> int:
    root = "archive"
    files = gen.base_files(root)
    plain = gen.theme_zip(root, files)
    count = 0

    def check(label, archive, reasons, warnings=()):
        nonlocal count
        count += 1
        check_archive(label, archive, reasons, warnings)

    check("not a zip", b"PK\x03\x04 but nothing else", ["theme-malformed-archive"])
    check("empty file", b"", ["theme-malformed-archive"])
    check("truncated", plain[:-10], ["theme-malformed-archive"])
    check("leading bytes", b"MZ" + plain, ["theme-malformed-archive"])
    check("zero entries", gen.build_zip([]), ["theme-not-single-folder"])
    mismatch = plain.replace(b"archive/grid/icons/FC.png", b"archive/grid/icons/MD.png", 1)
    check("local name differs from central", mismatch, ["theme-malformed-archive"])
    stream = gen.deflate(b"x" * 100)
    check("deflate stream corrupt", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/LICENSE.txt", b"x" * 100, method=8)])
        .replace(stream, b"\xff" * len(stream)), ["theme-malformed-archive"])
    check("encrypted flag", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/LICENSE.txt", b"CC0\n", flags=0x0001)]),
        ["theme-unsupported-compression"])
    check("lzma method", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/LICENSE.txt", b"CC0\n", method=14)]),
        ["theme-unsupported-compression"])
    check("ratio just under 100", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/LICENSE.txt", b"\x01" * 25900, method=8,
                      one_match_per_literal=True)]), [])

    labels = {f"grid/labels/L{k:03d}.png": gen.png(1, 1) for k in range(509)}
    check("512 entries", gen.theme_zip(root, dict(files, **labels)), [])
    check("deflated with directories", gen.theme_zip(root, files, method=8,
                                                     directories=True), [])
    # FAT32 folds case, so fc.png lands on FC.png. The writer sorts names and
    # uppercase sorts first, so the lowercase spelling is the later entry.
    check("case-only duplicate", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/grid/icons/fc.png", gen.png(512, 512))]),
        ["theme-duplicate-entry"])
    check("duplicate directory entry", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/grid/"), gen.zip_entry(f"{root}/grid/")]),
        ["theme-duplicate-entry"])
    check("two top-level folders", gen.theme_zip(root, files, extra=[
        gen.zip_entry("other/LICENSE.txt", b"CC0\n")]), ["theme-not-single-folder"])
    check("drive letter", gen.theme_zip(root, files, extra=[
        gen.zip_entry("C:/archive/LICENSE.txt", b"CC0\n")]), ["theme-absolute-path"])
    check("dot component", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/./LICENSE.txt", b"CC0\n")]), ["theme-path-traversal"])
    check("control character", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/LICENSE\x01.txt", b"CC0\n")]), ["theme-entry-name-encoding"])
    check("symlinked directory", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/coverflow", b"grid", mode=0o120777)]), ["theme-symlink"])
    check("directory bits on a file name", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/LICENSE.txt", b"", mode=0o040755)]), ["theme-special-file"])
    check("block device", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/LICENSE.txt", b"", mode=0o060644)]), ["theme-special-file"])
    check("mode bits zero", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/LICENSE.txt", b"CC0\n", mode=0)]), [])
    check("unknown directory", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/fonts/")]), ["theme-unknown-file"])
    check("uppercase extension", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/grid/icons/MD.PNG", gen.png(512, 512))]),
        ["theme-unknown-file"])
    check("icon in a wordmark-only name", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/grid/icons/MD.color.png", gen.png(512, 512))]),
        ["theme-system-id-invalid"])
    check("system id 33 chars", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/grid/icons/{'A' * 33}.png", gen.png(512, 512))]),
        ["theme-system-id-invalid"])
    check("uppercase _DEFAULT", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/grid/labels/_DEFAULT.png", gen.png(512, 512))]),
        ["theme-reserved-system-id"])
    for rel in ("coverflow/wallpaper.png", "coverflow/labels/FC.png",
                "coverflow/wordmarks/FC.png", "coverflow/wordmarks/FC.color.png"):
        data = gen.jpeg(960, 720) if rel.endswith("wallpaper.png") else gen.png(512, 512)
        check(f"launcher never draws {rel}", gen.theme_zip(root, files, extra=[
            gen.zip_entry(f"{root}/{rel}", data)]), ["theme-unknown-file"])
    check("_apps has no wordmark", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/grid/wordmarks/_apps.png", gen.png(400, 100))]),
        ["theme-system-id-invalid"])
    check("_apps has no color wordmark", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/grid/wordmarks/_apps.color.png", gen.png(400, 100))]),
        ["theme-system-id-invalid"])
    check("_APPS is not the Apps tile label", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/grid/labels/_APPS.png", gen.png(512, 512))]), [])
    check("_APPS is not the Apps tile", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/grid/icons/_APPS.png", gen.png(512, 512))]), [])
    check("coverflow labels folder", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/coverflow/labels/")]), ["theme-unknown-file"])
    check("two root wallpapers", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/wallpaper.jpg", gen.jpeg(960, 720)),
        gen.zip_entry(f"{root}/wallpaper.jpeg", gen.jpeg(960, 720))]),
        ["theme-multiple-wallpapers"])
    check("root and grid wallpaper", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/wallpaper.jpg", gen.jpeg(960, 720)),
        gen.zip_entry(f"{root}/grid/wallpaper.jpg", gen.jpeg(960, 720))]), [])
    # icons/ at the root is the set both views share: flat, icons only, and
    # the same system id rules as a view's icons/ folder.
    check("shared icon", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/icons/SNES.png", gen.png(512, 512))]), [])
    check("shared Apps icon", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/icons/_apps.png", gen.png(512, 512))]), [])
    check("shared icons folder entry", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/icons/")]), [])
    check("shared icon off-size", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/icons/SNES.png", gen.png(256, 256))]),
        [], ["theme-icon-off-size"])
    check("shared _default", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/icons/_default.png", gen.png(512, 512))]),
        ["theme-reserved-system-id"])
    check("shared lowercase id", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/icons/fc.png", gen.png(512, 512))]),
        ["theme-system-id-invalid"])
    check("shared folder is flat", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/icons/grid/FC.png", gen.png(512, 512))]),
        ["theme-unknown-file"])
    check("shared icons are PNG only", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/icons/FC.jpg", gen.jpeg(512, 512))]),
        ["theme-unknown-file"])
    check("no shared color wordmark", gen.theme_zip(root, files, extra=[
        gen.zip_entry(f"{root}/icons/FC.color.png", gen.png(512, 512))]),
        ["theme-system-id-invalid"])
    check("unknown schema skips id checks",
          gen.theme_zip("other", gen.base_files(root, schema=2)), ["theme-unknown-schema"])
    return count


def run_schema_selfcheck() -> None:
    """minischema silently ignores keywords it does not implement."""
    supported = {
        "$schema", "$id", "$ref", "title", "description", "definitions",
        "type", "const", "enum", "not", "allOf", "anyOf", "pattern",
        "minLength", "maxLength", "minimum", "maximum", "minItems",
        "maxItems", "items", "properties", "required", "additionalProperties",
    }
    unsupported: list[str] = []

    def walk(node, path: str, names_only: bool) -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                if not names_only and key not in supported:
                    unsupported.append(f"{path}.{key}")
                walk(value, f"{path}.{key}", key in ("properties", "definitions")
                     and not names_only)
        elif isinstance(node, list):
            for i, item in enumerate(node):
                walk(item, f"{path}[{i}]", False)

    walk(SCHEMA, "theme-v1.schema.json", False)
    if unsupported:
        fail(f"theme-v1.schema.json uses keywords minischema does not implement: {unsupported}")
    else:
        ok("theme-v1.schema.json: every keyword is enforced")


def main() -> None:
    run_schema_selfcheck()
    run_fixtures()
    counts = (run_manifest_variants(), run_manifest_parse_variants(),
              run_image_variants(), run_archive_variants())
    print(f"THEME-1 variants: {counts[0]} manifest, {counts[1]} manifest bytes, "
          f"{counts[2]} image, {counts[3]} archive")
    if FAILURES:
        print(f"\n{len(FAILURES)} failure(s).")
        sys.exit(1)
    print("\nAll leaf-themes fixtures pass.")


if __name__ == "__main__":
    main()
