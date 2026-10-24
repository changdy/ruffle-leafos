"""CAT-1 canonical bytes, tree hash, and generation digest.

These three primitives are normative: `jawakad` (the producer), the Jawaka
catalog readers, and CentralScrutinizer must all agree byte for byte, or
they disagree about which generation is current. See
docs/content-paks.md#canonical-bytes-and-digests.

Kept in its own module because both gen_content_fixtures.py and
validate_fixtures.py need them, and a second copy of a hashing rule is a
second chance to get it subtly wrong.
"""
from __future__ import annotations

import hashlib
import json
import os
import struct
from typing import Any

SELECTOR_PREFIX = "gen-"


def canonical_bytes(obj: Any) -> bytes:
    """JSON with keys sorted by Unicode code point, no separator padding,
    UTF-8, no BOM, non-ASCII left unescaped, plus exactly one trailing
    newline.

    The trailing newline is part of the hashed bytes on purpose: stamp.json
    is a real file on a real card, text editors and shell tools append one,
    and leaving it out of the definition would make "the canonical stamp
    bytes" ambiguous the first time a human touched the file.
    """
    text = json.dumps(obj, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return text.encode("utf-8") + b"\n"


def sha256_hex(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical_sha256(obj: Any) -> str:
    return sha256_hex(canonical_bytes(obj))


def tree_sha256_from_entries(entries: dict[str, bytes]) -> str:
    """Hash a set of {relative path: file bytes}.

    Each entry contributes `rel_path_utf8 || 0x00 || uint64_be(len) || bytes`,
    in ascending byte order of the relative path.

    The length prefix is not decoration. Without it, {"a": b"x", "b": b"yz"}
    and {"a": b"xy", "b": b"z"} hash identically once the paths are the same
    length -- an .info directory could be rewritten across a file boundary
    and keep a valid stamp. The generations/ fixture
    `tree-hash-length-prefix` locks exactly this.
    """
    h = hashlib.sha256()
    for rel in sorted(entries, key=lambda r: r.encode("utf-8")):
        data = entries[rel]
        h.update(rel.encode("utf-8"))
        h.update(b"\x00")
        h.update(struct.pack(">Q", len(data)))
        h.update(data)
    return h.hexdigest()


def tree_sha256(directory: str) -> str:
    """tree_sha256_from_entries over every regular file under `directory`.

    A missing directory hashes as empty, which is the same value an existing
    but empty directory produces -- deliberate: neither can contribute an
    .info file, so neither should change the generation identity.
    """
    entries: dict[str, bytes] = {}
    for dirpath, _dirnames, filenames in os.walk(directory):
        for name in filenames:
            full = os.path.join(dirpath, name)
            if not os.path.isfile(full) or os.path.islink(full):
                continue
            rel = os.path.relpath(full, directory).replace(os.sep, "/")
            with open(full, "rb") as f:
                entries[rel] = f.read()
    return tree_sha256_from_entries(entries)


def generation_digest(stamp: dict) -> str:
    """SHA-256 over the canonical stamp bytes.

    Not circular: the stamp holds every input hash and every output hash,
    and is complete before the digest is taken. The digest then names the
    directory, which is what makes an identical recompile reuse the existing
    generation instead of producing a second copy of it.
    """
    return canonical_sha256(stamp)


def generation_name(stamp: dict) -> str:
    return SELECTOR_PREFIX + generation_digest(stamp)


def selector_bytes(generation_name_value: str) -> bytes:
    return (generation_name_value + "\n").encode("utf-8")


def parse_selector(raw: bytes | str) -> str | None:
    """Return the generation name, or None if `raw` is not exactly the
    selector grammar: 'gen-' + 64 lowercase hex digits + one '\\n'.

    Strict on purpose. This value names a directory a reader is about to
    open, so anything permissive here is a path-handling bug waiting to
    happen.
    """
    if isinstance(raw, bytes):
        try:
            raw = raw.decode("utf-8")
        except UnicodeDecodeError:
            return None
    if not raw.endswith("\n") or raw.count("\n") != 1:
        return None
    body = raw[:-1]
    if not body.startswith(SELECTOR_PREFIX):
        return None
    hexpart = body[len(SELECTOR_PREFIX):]
    if len(hexpart) != 64:
        return None
    if any(c not in "0123456789abcdef" for c in hexpart):
        return None
    return body
