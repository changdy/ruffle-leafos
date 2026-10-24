#!/usr/bin/env python3
"""Generate THEME-1 fixture archives under fixtures/{valid,invalid}/.

Each fixture is a theme .zip plus a hand-written expectation in
fixtures/expect.json: the exact reason slugs and warning slugs the reference
validator must report. The expectations are literals in this file, not
output of theme_model.py, so the generator and the model can disagree and
validate_fixtures.py will say so.

Every invalid fixture breaks exactly one rule and is otherwise a valid theme.

Byte-reproducible by construction, with no dependence on the local zlib:

- zip entries carry a fixed 1980-01-01 timestamp, fixed attributes, and are
  written in sorted name order by the small zip writer below;
- compressed data comes from a fixed-Huffman DEFLATE encoder below, never
  from zlib.compress, whose output differs between zlib builds;
- images are complete, decodable files generated from their dimensions:
  1-bit grayscale PNGs and baseline grayscale JPEGs.

Run from anywhere. Idempotent: re-running regenerates every fixture.
"""
from __future__ import annotations

import json
import os
import shutil
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
FIXTURES = os.path.join(ROOT, "fixtures")

MIB = 1024 * 1024


# --------------------------------------------------------------------------
# DEFLATE with the fixed Huffman code (RFC 1951, 3.2.6)
# --------------------------------------------------------------------------

_LENGTH_BASE = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43,
                51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258]
_LENGTH_EXTRA = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4,
                 4, 4, 5, 5, 5, 5, 0]


class _Bits:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.count = 0

    def put(self, value: int, width: int) -> None:
        """Append `width` bits of `value`, least significant bit first."""
        self.acc |= value << self.count
        self.count += width
        while self.count >= 8:
            self.out.append(self.acc & 0xFF)
            self.acc >>= 8
            self.count -= 8

    def huffman(self, code: int, width: int) -> None:
        """Huffman codes are packed most significant bit first."""
        self.put(int(format(code, f"0{width}b")[::-1], 2), width)

    def finish(self) -> bytes:
        if self.count:
            self.out.append(self.acc & 0xFF)
        return bytes(self.out)


def _symbol(bits: _Bits, value: int) -> None:
    if value <= 143:
        bits.huffman(0x30 + value, 8)
    elif value <= 255:
        bits.huffman(0x190 + value - 144, 9)
    elif value <= 279:
        bits.huffman(value - 256, 7)
    else:
        bits.huffman(0xC0 + value - 280, 8)


def deflate(data: bytes, *, one_match_per_literal: bool = False) -> bytes:
    """Raw DEFLATE: literals plus distance-1 matches for byte runs.

    `one_match_per_literal` caps every run at one 258-byte match before the
    next literal, which pins the ratio of a uniform input just under 100:1.
    """
    bits = _Bits()
    bits.put(1, 1)  # BFINAL
    bits.put(1, 2)  # BTYPE 01: fixed Huffman
    i, n = 0, len(data)
    while i < n:
        _symbol(bits, data[i])
        i += 1
        previous = data[i - 1:i]
        while i < n:
            window = data[i:i + 258]
            run = len(window) if window == previous * len(window) else \
                next(k for k, byte in enumerate(window) if byte != previous[0])
            if run < 3:
                break
            index = max(k for k, base in enumerate(_LENGTH_BASE) if base <= run)
            _symbol(bits, 257 + index)
            if _LENGTH_EXTRA[index]:
                bits.put(run - _LENGTH_BASE[index], _LENGTH_EXTRA[index])
            bits.huffman(0, 5)  # distance code 0: distance 1
            i += run
            if one_match_per_literal:
                break
    _symbol(bits, 256)
    out = bits.finish()
    assert zlib.decompress(out, -15) == data, "fixed-Huffman encoder bug"
    return out


# --------------------------------------------------------------------------
# Images
# --------------------------------------------------------------------------

def _chunk(kind: bytes, body: bytes) -> bytes:
    return struct.pack(">I", len(body)) + kind + body + \
        struct.pack(">I", zlib.crc32(kind + body))


def png(width: int, height: int) -> bytes:
    """A black 1-bit grayscale PNG. Decodable, a few hundred bytes."""
    row = b"\x00" * (1 + (width + 7) // 8)
    raw = row * height
    stream = b"\x78\x01" + deflate(raw) + struct.pack(">I", zlib.adler32(raw))
    ihdr = struct.pack(">IIBBBBB", width, height, 1, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + _chunk(b"IHDR", ihdr) + _chunk(b"IDAT", stream) + \
        _chunk(b"IEND", b"")


def jpeg(width: int, height: int, *, sof: int = 0xC0, components: int = 1,
         precision: int = 8) -> bytes:
    """A mid-gray baseline grayscale JPEG.

    One Huffman code per table (DC difference 0, AC end-of-block) makes every
    8x8 block two zero bits, so the file stays tiny at any size. `sof`,
    `components` and `precision` exist so the validator's variants can build
    frame headers the device cannot decode; those are not decodable files.
    """
    def segment(marker: int, body: bytes) -> bytes:
        return bytes((0xFF, marker)) + struct.pack(">H", len(body) + 2) + body

    table = bytes([1] + [0] * 15)
    frame = struct.pack(">BHHB", precision, height, width, components) + \
        b"".join(bytes((c + 1, 0x11, 0)) for c in range(components))
    blocks = ((width + 7) // 8) * ((height + 7) // 8)
    scan_bits = 2 * blocks
    scan = bytearray(b"\x00" * (scan_bits // 8))
    if scan_bits % 8:
        scan.append(0xFF >> (scan_bits % 8))  # pad the final byte with 1 bits
    return (b"\xff\xd8"
            + segment(0xDB, b"\x00" + b"\x01" * 64)
            + segment(sof, frame)
            + segment(0xC4, b"\x00" + table + b"\x00")
            + segment(0xC4, b"\x10" + table + b"\x00")
            + segment(0xDA, b"\x01\x01\x00\x00\x3f\x00")
            + bytes(scan) + b"\xff\xd9")


# --------------------------------------------------------------------------
# Zip writer
# --------------------------------------------------------------------------

DOS_DATE_1980_01_01 = (0 << 9) | (1 << 5) | 1
FILE_MODE = 0o100644
DIR_MODE = 0o040755


def zip_entry(name, data: bytes = b"", *, method: int = 0, mode: int | None = None,
              crc: int | None = None, flags: int = 0,
              one_match_per_literal: bool = False) -> dict:
    raw = name if isinstance(name, bytes) else name.encode("utf-8")
    if mode is None:
        mode = DIR_MODE if raw.endswith(b"/") else FILE_MODE
    return dict(name=raw, data=data, method=method, mode=mode, crc=crc, flags=flags,
                one_match_per_literal=one_match_per_literal)


def build_zip(entries: list[dict]) -> bytes:
    """Entries in sorted raw-name order (stable, so duplicates keep theirs)."""
    local = bytearray()
    central = bytearray()
    ordered = sorted(entries, key=lambda e: e["name"])
    for entry in ordered:
        data, name = entry["data"], entry["name"]
        payload = deflate(data, one_match_per_literal=entry["one_match_per_literal"]) \
            if entry["method"] == 8 and data else data
        method = entry["method"] if data or entry["method"] != 8 else 0
        crc = zlib.crc32(data) if entry["crc"] is None else entry["crc"]
        flags = entry["flags"] | (0x0800 if any(b > 0x7F for b in name) else 0)
        external = entry["mode"] << 16 | (0x10 if name.endswith(b"/") else 0)
        offset = len(local)
        local += struct.pack("<4sHHHHHIIIHH", b"PK\x03\x04", 20, flags, method, 0,
                             DOS_DATE_1980_01_01, crc, len(payload), len(data),
                             len(name), 0) + name + payload
        central += struct.pack("<4sHHHHHHIIIHHHHHII", b"PK\x01\x02", 0x0314, 20, flags,
                               method, 0, DOS_DATE_1980_01_01, crc, len(payload),
                               len(data), len(name), 0, 0, 0, 0, external,
                               offset) + name
    end = struct.pack("<4sHHHHIIH", b"PK\x05\x06", 0, 0, len(ordered), len(ordered),
                      len(central), len(local), 0)
    return bytes(local + central + end)


# --------------------------------------------------------------------------
# Themes
# --------------------------------------------------------------------------

def manifest(theme_id: str, **fields) -> dict:
    obj = {
        "schema": 1,
        "id": theme_id,
        "name": "Fixture Theme",
        "author": "Leaf Contracts",
        "version": "1.0.0",
        "min_leaf_version": "0.12.0",
        "license": "CC-BY-4.0",
    }
    for key, value in fields.items():
        if value is DELETE:
            obj.pop(key, None)
        else:
            obj[key] = value
    return obj


class _Delete:
    def __repr__(self) -> str:
        return "DELETE"


DELETE = _Delete()


def manifest_bytes(obj: dict) -> bytes:
    return (json.dumps(obj, indent=2, ensure_ascii=False) + "\n").encode("utf-8")


def base_files(theme_id: str, **fields) -> dict:
    """The smallest theme that validates with no warnings: one 512px icon."""
    return {
        "theme.json": manifest_bytes(manifest(theme_id, **fields)),
        "preview.png": png(960, 720),
        "grid/icons/FC.png": png(512, 512),
    }


def theme_zip(root: str, files: dict, *, extra: list[dict] | None = None,
              method: int = 0, directories: bool = False) -> bytes:
    entries = [zip_entry(f"{root}/{rel}", data, method=method)
               for rel, data in files.items() if data is not None]
    if directories:
        dirs = {""}
        for rel in files:
            parts = rel.split("/")[:-1]
            dirs.update("/".join(parts[:k]) for k in range(1, len(parts) + 1))
        entries += [zip_entry(f"{root}/{d}/" if d else f"{root}/") for d in dirs]
    return build_zip(entries + (extra or []))


# --------------------------------------------------------------------------
# Fixtures: (name, archive bytes, reasons, warnings, note)
# --------------------------------------------------------------------------

def valid_fixtures():
    files = base_files("cc-by-nc-sa", license="CC-BY-NC-SA-2.0")
    files["LICENSE.txt"] = (
        b"Fixture artwork by Leaf Contracts. CC BY-NC-SA 2.0.\n"
        b"https://creativecommons.org/licenses/by-nc-sa/2.0/\n")
    yield ("cc-by-nc-sa", theme_zip("cc-by-nc-sa", files), [], [],
           "a theme declaring CC BY-NC-SA 2.0 with its license notice")

    files = base_files("cc-by-nc-sa-4", license="CC-BY-NC-SA-4.0")
    files["LICENSE.txt"] = (
        b"Fixture artwork by Leaf Contracts. CC BY-NC-SA 4.0.\n"
        b"https://creativecommons.org/licenses/by-nc-sa/4.0/\n")
    yield ("cc-by-nc-sa-4", theme_zip("cc-by-nc-sa-4", files), [], [],
           "a theme declaring CC BY-NC-SA 4.0 with its license notice")

    yield ("colors-only", theme_zip("colors-only", {
        "theme.json": manifest_bytes(manifest(
            "colors-only", name="Colors Only",
            colors={"text": "#F2F2F2", "highlight": "#68C7C3",
                    "highlight_text": "#12292B", "underlay": "#000000CC",
                    "underlay_opacity": 178, "shadow": 0},
            status_style="light")),
        "preview.png": png(960, 720),
    }), [], ["theme-no-art"],
        "the smallest store theme: theme.json and a preview, colors only. "
        "Accepted, with a warning that it ships no art")

    full = {
        "theme.json": manifest_bytes(manifest(
            "full", name="Full Example", author="Leaf Contracts", version="2.10.3",
            min_leaf_version="0.12.0", license="redistribution-permitted",
            description="Every optional file and key THEME-1 allows.\nTwo lines.",
            grid={"cols": 4, "rows": 3},
            colors={"text": "#5C6367", "highlight": "#68c7c3", "highlight_text": "#12292B",
                    "underlay": "#FFFFFF", "tile_border": "#FFFFFF40",
                    "focus_ring": "#68C7C3FF", "underlay_opacity": 255, "shadow": 96},
            status_style="auto")),
        "preview.png": png(960, 720),
        "LICENSE.txt": b"Redistribution of these images is permitted.\n",
        "wallpaper.jpg": jpeg(960, 720),
        "grid/wallpaper.png": png(2048, 1536),
        "grid/icons/FC.png": png(512, 512),
        "grid/icons/GBA.png": png(512, 512),
        "grid/icons/_apps.png": png(512, 512),
        "grid/labels/FC.png": png(512, 512),
        "grid/wordmarks/FC.png": png(400, 100),
        "grid/wordmarks/FC.color.png": png(400, 100),
        "coverflow/icons/FC.png": png(512, 512),
        "coverflow/icons/MD.png": png(512, 512),
    }
    yield ("full", theme_zip("full", full, method=8, directories=True), [], [],
           "every allowlisted file kind, both views, the Apps tile, deflated, with "
           "directory entries; wallpaper at the 2048 px edge")

    files = base_files("shared-icons")
    files.update({
        "icons/FC.png": png(512, 512),
        "icons/MD.png": png(512, 512),
        "icons/_apps.png": png(512, 512),
        "coverflow/icons/FC.png": png(512, 512),
    })
    yield ("shared-icons", theme_zip("shared-icons", files, directories=True), [], [],
           "one icon set at the root that both views read, plus a Cover Flow override "
           "for FC, which wins over the shared FC in that view")

    files = base_files("off-size-icon")
    files["grid/icons/GBA.png"] = png(256, 256)
    yield ("off-size-icon", theme_zip("off-size-icon", files), [], ["theme-icon-off-size"],
           "a 256x256 icon is accepted and drawn contain-fit, with a warning")


def invalid_fixtures():
    def fixture(slug, archive, note):
        return (slug[len("theme-"):], archive, [slug], [], note)

    def simple(slug, note, *, extra=None, drop=(), replace=None, root=None, **fields):
        name = slug[len("theme-"):]
        theme_id = fields.pop("id", name)
        files = base_files(theme_id, **fields)
        for rel in drop:
            files.pop(rel)
        files.update(replace or {})
        return fixture(slug, theme_zip(root or theme_id, files, extra=extra), note)

    # -- stage 1: container ------------------------------------------------
    files = base_files("archive-too-large")
    files["LICENSE.txt"] = b" " * (10 * MIB)
    yield fixture("theme-archive-too-large", theme_zip("archive-too-large", files),
                  "just over 10 MiB on disk: a stored 10 MiB LICENSE.txt, which would "
                  "otherwise pass every other limit. Rejected from the file size alone")

    files = base_files("malformed-archive")
    yield fixture("theme-malformed-archive", theme_zip("malformed-archive", files, extra=[
        zip_entry("malformed-archive/LICENSE.txt", b"CC BY 4.0\n", crc=0)]),
        "LICENSE.txt's CRC-32 does not match its bytes; found only after the "
        "size stages pass and the entries are read back")

    files = base_files("too-many-entries")
    for k in range(510):
        files[f"grid/labels/L{k:03d}.png"] = png(1, 1)
    yield fixture("theme-too-many-entries", theme_zip("too-many-entries", files),
                  "513 entries, each a valid 1x1 label; the cap is 512 including "
                  "directory entries")

    # -- stage 2: declared sizes -------------------------------------------
    yield simple("theme-unsupported-compression",
                 "LICENSE.txt uses method 12 (bzip2); only stored (0) and deflated (8) "
                 "are accepted, so the device never needs another decompressor",
                 extra=[zip_entry("unsupported-compression/LICENSE.txt", b"CC BY 4.0\n",
                                  method=12)])

    yield fixture("theme-uncompressed-too-large", theme_zip(
        "uncompressed-too-large", base_files("uncompressed-too-large"), extra=[
            zip_entry("uncompressed-too-large/LICENSE.txt", b"\x01" * (25 * MIB),
                      method=8, one_match_per_literal=True)]),
                  "25 MiB of LICENSE.txt plus the rest of the theme; the entry "
                  "compresses at about 98.7:1, under the ratio limit, so only the "
                  "total is over")

    yield simple("theme-compression-ratio",
                 "a 64 KiB LICENSE.txt of spaces deflates to about 1/158 of its size; "
                 "no entry may expand more than 100 times",
                 extra=[zip_entry("compression-ratio/LICENSE.txt", b" " * 65536, method=8)])

    # -- stage 4: names, types, layout -------------------------------------
    yield simple("theme-entry-name-encoding",
                 "an entry name that is not UTF-8 (a raw 0xFF byte)",
                 extra=[zip_entry(b"entry-name-encoding/grid/icons/\xff.png", png(512, 512))])
    yield simple("theme-absolute-path", "an entry name starting with /",
                 extra=[zip_entry("/absolute-path/LICENSE.txt", b"CC BY 4.0\n")])
    yield simple("theme-backslash-path",
                 "a backslash is a separator on Windows and a filename byte "
                 "everywhere else, so it means two different paths",
                 extra=[zip_entry("backslash-path\\LICENSE.txt", b"CC BY 4.0\n")])
    yield simple("theme-path-traversal", "a .. component",
                 extra=[zip_entry("path-traversal/../LICENSE.txt", b"CC BY 4.0\n")])
    yield simple("theme-hidden-file",
                 "what a macOS Finder zip adds: .DS_Store and a __MACOSX tree",
                 extra=[zip_entry("hidden-file/.DS_Store", b"\x00\x00\x00\x01Bud1"),
                        zip_entry("__MACOSX/hidden-file/._theme.json", b"\x00\x05\x16\x07")])
    yield simple("theme-symlink",
                 "grid/icons/GBA.png is a symbolic link (Unix mode 0120777) to FC.png",
                 extra=[zip_entry("symlink/grid/icons/GBA.png", b"FC.png", mode=0o120777)])
    yield simple("theme-special-file",
                 "LICENSE.txt is a FIFO (Unix mode 0010644)",
                 extra=[zip_entry("special-file/LICENSE.txt", b"", mode=0o010644)])
    yield simple("theme-duplicate-entry",
                 "grid/icons/FC.png appears twice; FAT32 can hold only one",
                 extra=[zip_entry("duplicate-entry/grid/icons/FC.png", png(512, 512))])

    files = base_files("not-single-folder")
    yield fixture("theme-not-single-folder",
                  build_zip([zip_entry(rel, data) for rel, data in files.items()]),
                  "the theme's files zipped without their folder, the most common "
                  "packaging mistake")

    yield simple("theme-unknown-file", "a README.md is not on the path allowlist",
                 replace={"README.md": b"# Unknown file\n"})
    yield simple("theme-system-id-invalid",
                 "grid/labels/snes.png: system ids are ^[A-Z0-9_]{2,32}$",
                 replace={"grid/labels/snes.png": png(512, 512)})
    yield simple("theme-reserved-system-id",
                 "_default is Leaf's fallback, not a tile, and is never themed",
                 replace={"grid/icons/_default.png": png(512, 512)})
    yield simple("theme-multiple-wallpapers",
                 "grid/wallpaper.png and grid/wallpaper.jpg; the launcher would only "
                 "ever draw one",
                 replace={"grid/wallpaper.png": png(960, 720),
                          "grid/wallpaper.jpg": jpeg(960, 720)})

    # -- stage 5: contents ---------------------------------------------------
    yield simple("theme-missing-manifest", "no theme.json", drop=["theme.json"])

    padded = manifest_bytes(manifest("manifest-too-large"))
    padded = padded[:-2] + b" " * (65537 - len(padded)) + padded[-2:]
    yield simple("theme-manifest-too-large",
                 "valid JSON padded with spaces to 65,537 bytes; the launcher reads at "
                 "most 64 KiB",
                 replace={"theme.json": padded})
    yield simple("theme-malformed-manifest", "theme.json is cut off mid-object",
                 replace={"theme.json": b'{\n  "schema": 1,\n  "id": "malformed-manifest",\n'})
    yield simple("theme-missing-preview", "no preview.png", drop=["preview.png"])
    yield simple("theme-id-mismatch",
                 "the folder is other-folder but theme.json says id-mismatch",
                 root="other-folder")
    yield simple("theme-reserved-name",
                 "sample would collide with the bundled Sample theme on a FAT32 card",
                 id="sample")
    yield simple("theme-unsupported-image",
                 "grid/icons/FC.png holds JPEG bytes; the extension is not trusted",
                 replace={"grid/icons/FC.png": jpeg(512, 512)})
    yield simple("theme-image-dimensions",
                 "a 1025 px wide label; art is capped at 1024 px per edge",
                 replace={"grid/labels/FC.png": png(1025, 64)})

    # -- theme.json fields ---------------------------------------------------
    yield simple("theme-unknown-schema",
                 "schema 99, a number no reader will ever accept", schema=99)
    yield simple("theme-unknown-field",
                 "a top-level key THEME-1 does not define", wallpaper="wallpaper.png")
    yield simple("theme-id-invalid",
                 "ids are lowercase: ^[a-z0-9][a-z0-9-]{1,39}$", id="Bad_Id")
    yield simple("theme-name-invalid", "an empty name", name="")
    yield simple("theme-author-invalid", "an author of only spaces", author="   ")
    yield simple("theme-version-invalid",
                 "1.0 is not MAJOR.MINOR.PATCH (it is the bundled Sample's version)",
                 version="1.0")
    yield simple("theme-min-leaf-version",
                 "0.11.0 predates Grid View and user themes", min_leaf_version="0.11.0")
    yield simple("theme-unknown-license", "MIT is not on the license list", license="MIT")
    yield simple("theme-description-invalid", "301 characters; the cap is 300",
                 description="x" * 301)
    yield simple("theme-grid-invalid", "9 columns; the launcher's range is 1-8",
                 grid={"cols": 9, "rows": 2})
    yield simple("theme-color-invalid", "#12345 is five hex digits",
                 colors={"text": "#12345"})
    yield simple("theme-color-level-invalid", "underlay_opacity 256; the range is 0-255",
                 colors={"underlay_opacity": 256})
    yield simple("theme-status-style-invalid", "sepia is not light, dark or auto",
                 status_style="sepia")


# Built in memory when the checks run instead of committed: a 10 MiB file in the
# repository is not worth keeping for one size rule.
IN_MEMORY = {"invalid/archive-too-large.zip"}


def fixture_bytes(rel: str) -> bytes:
    group, name = rel.split("/", 1)
    source = valid_fixtures() if group == "valid" else invalid_fixtures()
    for fixture_name, archive, _reasons, _warnings, _note in source:
        if f"{fixture_name}.zip" == name:
            return archive
    raise KeyError(rel)


def main() -> None:
    if os.path.exists(FIXTURES):
        shutil.rmtree(FIXTURES)
    expectations = []
    for group, fixtures in (("valid", valid_fixtures()), ("invalid", invalid_fixtures())):
        os.makedirs(os.path.join(FIXTURES, group))
        for name, archive, reasons, warnings, note in fixtures:
            rel = f"{group}/{name}.zip"
            entry = dict(file=rel, reasons=reasons, warnings=warnings, note=note)
            if rel in IN_MEMORY:
                entry["in_memory"] = True
            else:
                with open(os.path.join(FIXTURES, rel), "wb") as handle:
                    handle.write(archive)
            expectations.append(entry)
    expectations.sort(key=lambda e: e["file"])
    with open(os.path.join(FIXTURES, "expect.json"), "w") as handle:
        json.dump({"fixtures": expectations}, handle, indent=2, ensure_ascii=False)
        handle.write("\n")
    valid = sum(1 for e in expectations if not e["reasons"])
    print(f"Generated {valid} valid and {len(expectations) - valid} invalid THEME-1 fixtures.")


if __name__ == "__main__":
    main()
