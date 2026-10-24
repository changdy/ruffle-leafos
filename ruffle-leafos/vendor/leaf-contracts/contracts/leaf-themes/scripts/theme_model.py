"""Reference validator for THEME-1, the Leaf theme package.

This is the executable half of docs/themes.md. The store pipeline runs it on
every submission, and Jawaka's install-time check must agree with it reason
for reason; the fixture tree is what checks that agreement.

Two entry points:

    validate_manifest(obj)  -> sorted reason slugs for a parsed theme.json
    validate_archive(path)  -> (sorted reason slugs, sorted warning slugs)

Images are identified from their headers only (PNG signature and IHDR, JPEG
markers up to the frame header). Full decoding happens on the device, which
falls back to its next artwork candidate when a file will not decode.

The zip reader is written here rather than borrowed from `zipfile` on
purpose: the contract makes promises about raw entry names, Unix mode bits,
declared sizes, and the order checks run in, and `zipfile` normalizes or
hides exactly those details.
"""
from __future__ import annotations

import json
import os
import re
import struct
import zlib

# --------------------------------------------------------------------------
# Vocabulary
# --------------------------------------------------------------------------

REASONS = (
    # Stage 1: the container
    "theme-archive-too-large",
    "theme-malformed-archive",
    "theme-too-many-entries",
    # Stage 2: declared sizes, before anything is decompressed
    "theme-unsupported-compression",
    "theme-uncompressed-too-large",
    "theme-compression-ratio",
    # Stage 4: entry names and types
    "theme-entry-name-encoding",
    "theme-absolute-path",
    "theme-backslash-path",
    "theme-path-traversal",
    "theme-hidden-file",
    "theme-symlink",
    "theme-special-file",
    "theme-duplicate-entry",
    "theme-not-single-folder",
    "theme-unknown-file",
    "theme-system-id-invalid",
    "theme-reserved-system-id",
    "theme-multiple-wallpapers",
    # Stage 5: package contents
    "theme-missing-manifest",
    "theme-manifest-too-large",
    "theme-malformed-manifest",
    "theme-missing-preview",
    "theme-id-mismatch",
    "theme-reserved-name",
    "theme-unsupported-image",
    "theme-image-dimensions",
    # theme.json fields
    "theme-unknown-schema",
    "theme-unknown-field",
    "theme-id-invalid",
    "theme-name-invalid",
    "theme-author-invalid",
    "theme-version-invalid",
    "theme-min-leaf-version",
    "theme-unknown-license",
    "theme-description-invalid",
    "theme-grid-invalid",
    "theme-color-invalid",
    "theme-color-level-invalid",
    "theme-status-style-invalid",
)

WARNINGS = (
    "theme-icon-off-size",
    "theme-no-art",
)

LICENSES = ("CC-BY-4.0", "CC-BY-SA-4.0", "CC-BY-NC-SA-2.0", "CC-BY-NC-SA-4.0",
            "CC0-1.0", "redistribution-permitted")

# Folder names a Leaf release ships in Themes/ and replaces wholesale on
# every install (bundled-themes.txt). Compared case-insensitively: the card
# is FAT32.
RESERVED_INSTALL_NAMES = ("Sample",)

MAX_ARCHIVE_BYTES = 10 * 1024 * 1024        # the .zip file itself
MAX_UNCOMPRESSED_BYTES = 25 * 1024 * 1024   # sum of declared entry sizes
MAX_ENTRIES = 512                           # central directory records
MAX_RATIO = 100                             # uncompressed <= 100 x compressed
MAX_MANIFEST_BYTES = 64 * 1024              # the launcher's read limit

ART_MAX_PX = 1024
ICON_TARGET_PX = 512
WALLPAPER_MAX_PX = 2048
PREVIEW_PX = (960, 720)

MIN_LEAF_VERSION = (0, 12, 0)

# Byte limits that mirror the launcher's fixed buffers (name[96], author[64]
# including the terminator), so a valid name is never truncated mid-character.
NAME_MAX_CHARS, NAME_MAX_BYTES = 40, 95
AUTHOR_MAX_CHARS, AUTHOR_MAX_BYTES = 60, 63
DESCRIPTION_MAX_CHARS = 300

VIEWS = ("grid", "coverflow")
# Only what the launcher draws: Grid reads icons, labels and wordmarks and has
# its own wallpaper; Cover Flow reads icons only. Adding more later is safe,
# because min_leaf_version gates themes that use it.
VIEW_ART = {"grid": ("icons", "labels", "wordmarks"), "coverflow": ("icons",)}
WALLPAPER_VIEWS = ("grid",)
WALLPAPER_NAMES = ("wallpaper.png", "wallpaper.jpg", "wallpaper.jpeg")
# The launcher looks up the Apps tile as _apps, next to the system codes. It
# draws an icon for it in both views and a label over it in Grid; Apps has no
# wordmark.
APPS_TILE_ID = "_apps"
APPS_TILE_ART = (("grid", "icons"), ("grid", "labels"), ("coverflow", "icons"))
# icons/<ID>.png at the root: one icon set both views read. A view's own
# icons/ folder wins per system; this is where either view looks next.
SHARED_ICONS = "icons"

ID_RE = re.compile(r"[a-z0-9][a-z0-9-]{1,39}")
SYSTEM_ID_RE = re.compile(r"[A-Z0-9_]{2,32}")
VERSION_RE = re.compile(r"(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,3})\.(0|[1-9][0-9]{0,3})")
COLOR_RE = re.compile(r"#(?:[0-9A-Fa-f]{6}|[0-9A-Fa-f]{8})")
CONTROL_RE = re.compile(r"[\x00-\x1f\x7f]")
DESCRIPTION_CONTROL_RE = re.compile(r"[\x00-\x09\x0b-\x1f\x7f]")

MANIFEST_KEYS = {"schema", "id", "name", "author", "version", "min_leaf_version",
                 "license", "description", "grid", "colors", "status_style"}
GRID_KEYS = {"cols", "rows"}
COLOR_KEYS = ("text", "highlight", "highlight_text", "underlay", "tile_border",
              "focus_ring")
COLOR_LEVEL_KEYS = ("underlay_opacity", "shadow")
STATUS_STYLES = ("auto", "light", "dark")

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
# IHDR color type -> legal bit depths (PNG specification, table 11.1).
PNG_DEPTHS = {0: {1, 2, 4, 8, 16}, 2: {8, 16}, 3: {1, 2, 4, 8}, 4: {8, 16}, 6: {8, 16}}


# --------------------------------------------------------------------------
# theme.json
# --------------------------------------------------------------------------

def _is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _text_ok(value, max_chars: int, max_bytes: int | None, control_re) -> bool:
    if not isinstance(value, str) or not 1 <= len(value) <= max_chars:
        return False
    if control_re.search(value) or not value.strip():
        return False
    try:
        encoded = value.encode("utf-8")
    except UnicodeEncodeError:  # a lone surrogate from a \ud800 escape
        return False
    return max_bytes is None or len(encoded) <= max_bytes


def parse_version(value) -> tuple[int, int, int] | None:
    if not isinstance(value, str):
        return None
    match = VERSION_RE.fullmatch(value)
    return tuple(int(part) for part in match.groups()) if match else None


def validate_manifest(obj) -> list[str]:
    """Reason slugs for a parsed theme.json object, sorted."""
    if not isinstance(obj, dict):
        return ["theme-malformed-manifest"]
    schema = obj.get("schema")
    # A newer schema is refused, never guessed at: nothing else is judged
    # against rules that may not be the ones it was written for.
    if isinstance(schema, bool) or not isinstance(schema, (int, float)) or schema != 1:
        return ["theme-unknown-schema"]

    reasons: set[str] = set()
    if set(obj) - MANIFEST_KEYS:
        reasons.add("theme-unknown-field")
    if not isinstance(obj.get("id"), str) or not ID_RE.fullmatch(obj["id"]):
        reasons.add("theme-id-invalid")
    if not _text_ok(obj.get("name"), NAME_MAX_CHARS, NAME_MAX_BYTES, CONTROL_RE):
        reasons.add("theme-name-invalid")
    if not _text_ok(obj.get("author"), AUTHOR_MAX_CHARS, AUTHOR_MAX_BYTES, CONTROL_RE):
        reasons.add("theme-author-invalid")
    if parse_version(obj.get("version")) is None:
        reasons.add("theme-version-invalid")
    minimum = parse_version(obj.get("min_leaf_version"))
    if minimum is None or minimum < MIN_LEAF_VERSION:
        reasons.add("theme-min-leaf-version")
    if obj.get("license") not in LICENSES or not isinstance(obj.get("license"), str):
        reasons.add("theme-unknown-license")

    if "description" in obj:
        value = obj["description"]
        if not isinstance(value, str) or len(value) > DESCRIPTION_MAX_CHARS \
                or DESCRIPTION_CONTROL_RE.search(value):
            reasons.add("theme-description-invalid")
        else:
            try:
                value.encode("utf-8")
            except UnicodeEncodeError:
                reasons.add("theme-description-invalid")

    if "grid" in obj:
        grid = obj["grid"]
        if not isinstance(grid, dict):
            reasons.add("theme-grid-invalid")
        else:
            if set(grid) - GRID_KEYS:
                reasons.add("theme-unknown-field")
            cols, rows = grid.get("cols"), grid.get("rows")
            if not (_is_int(cols) and 1 <= cols <= 8 and _is_int(rows) and 1 <= rows <= 6):
                reasons.add("theme-grid-invalid")

    if "colors" in obj:
        colors = obj["colors"]
        if not isinstance(colors, dict):
            reasons.add("theme-color-invalid")
        else:
            if set(colors) - set(COLOR_KEYS) - set(COLOR_LEVEL_KEYS):
                reasons.add("theme-unknown-field")
            for key in COLOR_KEYS:
                if key in colors and (not isinstance(colors[key], str)
                                      or not COLOR_RE.fullmatch(colors[key])):
                    reasons.add("theme-color-invalid")
            for key in COLOR_LEVEL_KEYS:
                if key in colors and not (_is_int(colors[key]) and 0 <= colors[key] <= 255):
                    reasons.add("theme-color-level-invalid")

    if "status_style" in obj and (not isinstance(obj["status_style"], str)
                                  or obj["status_style"] not in STATUS_STYLES):
        reasons.add("theme-status-style-invalid")
    return sorted(reasons)


class _ManifestSyntax(ValueError):
    pass


def _no_duplicate_keys(pairs):
    keys = [key for key, _ in pairs]
    if len(keys) != len(set(keys)):
        # cJSON keeps the first duplicate and Python keeps the last, so the
        # store and the device would read different themes.
        raise _ManifestSyntax("duplicate key")
    return dict(pairs)


def _reject_constant(name):
    raise _ManifestSyntax(f"{name} is not JSON")


def parse_manifest_bytes(data: bytes):
    """Strict theme.json parse: UTF-8, no BOM, no duplicate keys, no NaN."""
    if data.startswith(b"\xef\xbb\xbf"):
        raise _ManifestSyntax("byte order mark")
    text = data.decode("utf-8")
    return json.loads(text, object_pairs_hook=_no_duplicate_keys,
                      parse_constant=_reject_constant)


# --------------------------------------------------------------------------
# Image headers
# --------------------------------------------------------------------------

def png_dimensions(data: bytes) -> tuple[int, int] | None:
    """(width, height) from a well-formed PNG signature and IHDR, else None."""
    if len(data) < 33 or data[:8] != PNG_SIGNATURE:
        return None
    length, kind = struct.unpack(">I4s", data[8:16])
    if length != 13 or kind != b"IHDR":
        return None
    body = data[16:29]
    if struct.unpack(">I", data[29:33])[0] != zlib.crc32(data[12:29]):
        return None
    width, height, depth, color, compression, filtering, interlace = \
        struct.unpack(">IIBBBBB", body)
    if depth not in PNG_DEPTHS.get(color, ()) or compression or filtering \
            or interlace not in (0, 1):
        return None
    return width, height


# Frame headers the device decoder handles: baseline, extended sequential,
# progressive. Every other SOFn is lossless, hierarchical, or arithmetic coded.
JPEG_SUPPORTED_SOF = {0xC0, 0xC1, 0xC2}
JPEG_OTHER_SOF = {0xC3, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF}


def jpeg_dimensions(data: bytes) -> tuple[int, int] | None:
    """(width, height) from the first frame header of a JPEG, else None.

    Walks marker segments from SOI to the first SOFn. Only an 8-bit frame
    with 1 (grayscale) or 3 (YCbCr) components is supported; a zero height
    (defined later by a DNL marker) is not.
    """
    if data[:2] != b"\xff\xd8":
        return None
    i, n = 2, len(data)
    while i < n:
        if data[i] != 0xFF:
            return None
        while i < n and data[i] == 0xFF:
            i += 1
        if i >= n:
            return None
        marker = data[i]
        i += 1
        if marker == 0x01 or 0xD0 <= marker <= 0xD7:
            continue
        if marker in (0x00, 0xD8, 0xD9, 0xDA) or marker in JPEG_OTHER_SOF:
            return None
        if i + 2 > n:
            return None
        length = struct.unpack(">H", data[i:i + 2])[0]
        if length < 2 or i + length > n:
            return None
        if marker in JPEG_SUPPORTED_SOF:
            if length < 8:
                return None
            precision, height, width, components = struct.unpack(">BHHB", data[i + 2:i + 8])
            if precision != 8 or components not in (1, 3) or length != 8 + 3 * components \
                    or height == 0:
                return None
            return width, height
        i += length
    return None


# --------------------------------------------------------------------------
# Zip container
# --------------------------------------------------------------------------

class _Entry:
    __slots__ = ("raw_name", "name", "flags", "method", "crc", "csize", "usize",
                 "made_by", "external", "offset", "data_start", "data")

    @property
    def is_dir(self) -> bool:
        return self.raw_name.endswith(b"/")


class _Malformed(Exception):
    pass


def _read_central_directory(blob: bytes) -> list[_Entry] | str:
    """Entries, or a stage-1 reason slug."""
    eocd = -1
    for pos in range(len(blob) - 22, max(-1, len(blob) - 22 - 65535) - 1, -1):
        if blob[pos:pos + 4] == b"PK\x05\x06" and \
                pos + 22 + struct.unpack("<H", blob[pos + 20:pos + 22])[0] == len(blob):
            eocd = pos
            break
    if eocd < 0:
        return "theme-malformed-archive"
    disk, cd_disk, on_disk, total, cd_size, cd_offset = \
        struct.unpack("<HHHHII", blob[eocd + 4:eocd + 20])
    # ZIP64 and multi-volume archives are never needed under these limits.
    if disk or cd_disk or on_disk != total or total == 0xFFFF \
            or cd_offset == 0xFFFFFFFF or cd_offset + cd_size != eocd:
        return "theme-malformed-archive"
    if total > MAX_ENTRIES:
        return "theme-too-many-entries"

    entries: list[_Entry] = []
    pos = cd_offset
    for _ in range(total):
        if pos + 46 > eocd or blob[pos:pos + 4] != b"PK\x01\x02":
            return "theme-malformed-archive"
        (made_by, _needed, flags, method, _time, _date, crc, csize, usize, name_len,
         extra_len, comment_len, disk_start, _internal, external, offset) = \
            struct.unpack("<HHHHHHIIIHHHHHII", blob[pos + 4:pos + 46])
        end = pos + 46 + name_len + extra_len + comment_len
        if end > eocd or disk_start or 0xFFFFFFFF in (csize, usize, offset):
            return "theme-malformed-archive"
        entry = _Entry()
        entry.raw_name = blob[pos + 46:pos + 46 + name_len]
        entry.flags, entry.method, entry.crc = flags, method, crc
        entry.csize, entry.usize, entry.made_by = csize, usize, made_by
        entry.external, entry.offset = external, offset
        entry.data = None
        if entry.is_dir and usize:
            return "theme-malformed-archive"
        entries.append(entry)
        pos = end
    if pos != eocd:
        return "theme-malformed-archive"

    # Local headers: each must repeat its central name and method, and the
    # entries must tile the file from offset 0 without overlapping. Overlap
    # is how one stored payload is counted many times by a zip bomb.
    ordered = sorted(entries, key=lambda e: e.offset)
    if ordered and ordered[0].offset != 0:  # nothing may precede the first entry
        return "theme-malformed-archive"
    expected_start = 0
    for entry in ordered:
        at = entry.offset
        if at < expected_start:
            return "theme-malformed-archive"
        if at + 30 > cd_offset or blob[at:at + 4] != b"PK\x03\x04":
            return "theme-malformed-archive"
        _needed, _flags, method, _t, _d, _crc, _cs, _us, name_len, extra_len = \
            struct.unpack("<HHHHHIIIHH", blob[at + 4:at + 30])
        if method != entry.method or blob[at + 30:at + 30 + name_len] != entry.raw_name:
            return "theme-malformed-archive"
        entry.data_start = at + 30 + name_len + extra_len
        expected_start = entry.data_start + entry.csize
        if expected_start > cd_offset:
            return "theme-malformed-archive"
    return entries


def _inflate(entry: _Entry, blob: bytes) -> bytes | None:
    """Decompress one entry, never producing more than its declared size."""
    payload = blob[entry.data_start:entry.data_start + entry.csize]
    if entry.method == 0:
        data = payload
    else:
        inflater = zlib.decompressobj(-15)
        try:
            data = inflater.decompress(payload, entry.usize + 1)
        except zlib.error:
            return None
        if not inflater.eof or inflater.unconsumed_tail:
            return None
    if len(data) != entry.usize or zlib.crc32(data) != entry.crc:
        return None
    return data


# --------------------------------------------------------------------------
# Entry names
# --------------------------------------------------------------------------

S_IFMT, S_IFREG, S_IFDIR, S_IFLNK = 0o170000, 0o100000, 0o040000, 0o120000


def _name_reason(entry: _Entry) -> str | None:
    """The first per-entry rule this name breaks, in contract order."""
    try:
        name = entry.raw_name.decode("utf-8")
    except UnicodeDecodeError:
        return "theme-entry-name-encoding"
    if not name or CONTROL_RE.search(name):
        return "theme-entry-name-encoding"
    entry.name = name
    if name.startswith("/") or re.match(r"[A-Za-z]:", name):
        return "theme-absolute-path"
    if "\\" in name:
        return "theme-backslash-path"
    parts = name.rstrip("/").split("/")
    if any(part in (".", "..") for part in parts):
        return "theme-path-traversal"
    if any(part.startswith(".") or part == "__MACOSX" for part in parts):
        return "theme-hidden-file"
    if entry.made_by >> 8 == 3:  # Unix: the high 16 bits are st_mode
        kind = (entry.external >> 16) & S_IFMT
        if kind == S_IFLNK:
            return "theme-symlink"
        if kind not in (0, S_IFREG, S_IFDIR) or \
                (kind == S_IFDIR and not entry.is_dir) or \
                (kind == S_IFREG and entry.is_dir):
            return "theme-special-file"
    return None


def _classify(rel: str, is_dir: bool):
    """Map a root-relative path to (slot, view, kind, system id) or a reason.

    slot is one of: dir, manifest, preview, license, wallpaper, art.
    """
    if is_dir:
        rel = rel.rstrip("/")
        allowed = {"", SHARED_ICONS} | set(VIEWS) | \
            {f"{v}/{k}" for v in VIEWS for k in VIEW_ART[v]}
        return ("dir", None, None, None) if rel in allowed else "theme-unknown-file"
    if rel == "theme.json":
        return ("manifest", None, None, None)
    if rel == "preview.png":
        return ("preview", None, None, None)
    if rel == "LICENSE.txt":
        return ("license", None, None, None)
    parts = rel.split("/")
    if len(parts) == 1 and rel in WALLPAPER_NAMES:
        return ("wallpaper", None, None, None)
    if len(parts) == 2 and parts[0] in WALLPAPER_VIEWS and parts[1] in WALLPAPER_NAMES:
        return ("wallpaper", parts[0], None, None)
    if len(parts) == 2 and parts[0] == SHARED_ICONS:
        return _classify_art(None, SHARED_ICONS, parts[1])
    if len(parts) == 3 and parts[0] in VIEWS and parts[1] in VIEW_ART[parts[0]]:
        return _classify_art(parts[0], parts[1], parts[2])
    return "theme-unknown-file"


def _classify_art(view, kind: str, name: str):
    """(slot, view, kind, system id) for one art file, or a reason. view is
    None for the shared icons/ folder, which both views read."""
    stem = name
    if kind == "wordmarks" and stem.endswith(".color.png"):
        stem = stem[:-len(".color.png")]
    elif stem.endswith(".png"):
        stem = stem[:-len(".png")]
    else:
        return "theme-unknown-file"
    if stem.casefold() == "_default":
        return "theme-reserved-system-id"
    # Every view draws an Apps icon, so the shared set may carry one too.
    if stem == APPS_TILE_ID and (view is None or (view, kind) in APPS_TILE_ART):
        return ("art", view, kind, stem)
    if not SYSTEM_ID_RE.fullmatch(stem):
        return "theme-system-id-invalid"
    return ("art", view, kind, stem)


# --------------------------------------------------------------------------
# The archive
# --------------------------------------------------------------------------

Finding = tuple[str, "str | None"]


def validate_archive(path: str) -> tuple[list[str], list[str]]:
    """(sorted reason slugs, sorted warning slugs) for a theme .zip.

    Checks run in the contract's stages. A failing stage 1, 2 or 3 stops
    validation, so nothing is ever decompressed from an archive whose
    declared sizes are over a limit. Stages 4 and 5 report every violation.
    """
    reasons, warnings = validate_archive_findings(path)
    return sorted({slug for slug, _ in reasons}), sorted({slug for slug, _ in warnings})


def validate_archive_findings(path: str) -> tuple[list[Finding], list[Finding]]:
    """The same checks as validate_archive, with where each one applies.

    Each finding is (slug, entry name) for a rule about one zip entry, such as
    "neon-nights/grid/icons/GB.png", or (slug, None) for a rule about the
    whole archive. Entry names come from the archive and are untrusted: escape
    them before showing them anywhere. The set of slugs always equals
    validate_archive's.
    """
    try:
        size = os.stat(path).st_size
    except OSError:
        return [("theme-malformed-archive", None)], []
    if size > MAX_ARCHIVE_BYTES:
        return [("theme-archive-too-large", None)], []
    with open(path, "rb") as handle:
        blob = handle.read(MAX_ARCHIVE_BYTES + 1)

    # Stage 1: container structure and entry count.
    entries = _read_central_directory(blob)
    if isinstance(entries, str):
        return [(entries, None)], []

    # Stage 2: declared sizes and encodings.
    reasons: set[Finding] = set()
    for entry in entries:
        if entry.method not in (0, 8) or entry.flags & 0x0001:
            reasons.add(("theme-unsupported-compression", _where(entry)))
        if entry.usize > MAX_RATIO * entry.csize:
            reasons.add(("theme-compression-ratio", _where(entry)))
    if sum(entry.usize for entry in entries) > MAX_UNCOMPRESSED_BYTES:
        reasons.add(("theme-uncompressed-too-large", None))
    if reasons:
        return _sorted_findings(reasons), []

    # Stage 3: integrity. Every byte is bounded by stage 2 now.
    for entry in entries:
        if entry.method == 0 and entry.csize != entry.usize:
            return [("theme-malformed-archive", None)], []
        entry.data = _inflate(entry, blob)
        if entry.data is None:
            return [("theme-malformed-archive", None)], []

    # Stage 4: names, types, layout.
    clean: list[_Entry] = []
    seen: set[str] = set()
    for entry in entries:
        reason = _name_reason(entry)
        if reason:
            reasons.add((reason, _where(entry)))
            continue
        key = entry.name.rstrip("/").casefold()
        if key in seen:
            reasons.add(("theme-duplicate-entry", _where(entry)))
            continue
        seen.add(key)
        clean.append(entry)

    roots = {entry.name.split("/", 1)[0] for entry in clean}
    if len(roots) != 1 or any("/" not in entry.name for entry in clean):
        reasons.add(("theme-not-single-folder", None))
        return _sorted_findings(reasons), []
    root = roots.pop()

    slots: dict[str, tuple] = {}
    wallpapers: dict[str | None, int] = {}
    for entry in clean:
        rel = entry.name[len(root) + 1:]
        result = _classify(rel, entry.is_dir)
        if isinstance(result, str):
            reasons.add((result, _where(entry)))
            continue
        if result[0] == "dir":
            continue
        slots[rel] = result + (entry.data, _where(entry))
        if result[0] == "wallpaper":
            wallpapers[result[1]] = wallpapers.get(result[1], 0) + 1
    for view, count in wallpapers.items():
        if count > 1:
            reasons.add(("theme-multiple-wallpapers",
                         f"{root}/{view}/" if view else f"{root}/"))

    # Stage 5: contents.
    warnings: set[Finding] = set()
    manifest = slots.get("theme.json")
    manifest_name = f"{root}/theme.json"
    if manifest is None:
        reasons.add(("theme-missing-manifest", None))
    elif len(manifest[4]) > MAX_MANIFEST_BYTES:
        reasons.add(("theme-manifest-too-large", manifest_name))
    else:
        try:
            obj = parse_manifest_bytes(manifest[4])
        except (ValueError, RecursionError):
            obj = None
        # Valid JSON that is not an object (an array, string, number or null)
        # is as malformed as a syntax error.
        if not isinstance(obj, dict):
            reasons.add(("theme-malformed-manifest", manifest_name))
        else:
            field_reasons = validate_manifest(obj)
            reasons.update((slug, manifest_name) for slug in field_reasons)
            theme_id = obj.get("id")
            if "theme-unknown-schema" not in field_reasons and \
                    "theme-id-invalid" not in field_reasons:
                if theme_id != root:
                    reasons.add(("theme-id-mismatch", manifest_name))
                if theme_id.casefold() in {n.casefold() for n in RESERVED_INSTALL_NAMES}:
                    reasons.add(("theme-reserved-name", manifest_name))

    if "preview.png" not in slots:
        reasons.add(("theme-missing-preview", None))

    art_files = 0
    for rel, (slot, _view, kind, _system, data, name) in sorted(slots.items()):
        if slot not in ("preview", "wallpaper", "art"):
            continue
        if slot != "preview":
            art_files += 1
        if rel.endswith(".png"):
            dims = png_dimensions(data)
        else:
            dims = jpeg_dimensions(data)
        if dims is None:
            reasons.add(("theme-unsupported-image", name))
            continue
        width, height = dims
        if slot == "preview":
            ok = (width, height) == PREVIEW_PX
        elif slot == "wallpaper":
            ok = 1 <= width <= WALLPAPER_MAX_PX and 1 <= height <= WALLPAPER_MAX_PX
        else:
            ok = 1 <= width <= ART_MAX_PX and 1 <= height <= ART_MAX_PX
        if not ok:
            reasons.add(("theme-image-dimensions", name))
        elif kind == "icons" and (width, height) != (ICON_TARGET_PX, ICON_TARGET_PX):
            warnings.add(("theme-icon-off-size", name))
    if art_files == 0:
        warnings.add(("theme-no-art", None))
    return _sorted_findings(reasons), _sorted_findings(warnings)


def _where(entry: _Entry) -> str:
    """An entry's name for a finding, even when it is not valid UTF-8."""
    return entry.raw_name.decode("utf-8", "replace")


def _sorted_findings(findings) -> list[Finding]:
    return sorted(findings, key=lambda finding: (finding[0], finding[1] or ""))
