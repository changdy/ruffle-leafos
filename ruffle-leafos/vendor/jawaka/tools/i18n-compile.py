#!/usr/bin/env python3
"""Compile a .po (or .tsv) translation into the flat table the launcher loads.

The device never parses .po. Translators and gettext tooling see .po; the build
emits a blob that the runtime validates and binary-searches. Keeping the parser
here rather than in C is deliberate: a malformed translation should fail a build,
not a handheld.

    tools/i18n-compile.py zh_CN.po -o build/i18n/zh_CN.jwi

Entries that are fuzzy, empty, or untranslated are dropped rather than emitted.
A dropped key falls back to its English literal at runtime, which is the whole
point of using the source string as the key: a half-finished translation is
shippable, and every string it does not cover simply stays English.

Layout (all integers little-endian) is documented in internal/i18n/i18n.c.
"""

import argparse
import re
import struct
import sys

MAGIC = b"JWI18N\0\0"
VERSION = 1
HEADER = 24


def fnv1a(data: bytes) -> int:
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


_ESCAPES = {"n": "\n", "t": "\t", "r": "\r", '"': '"', "\\": "\\"}


def unquote(literal: str) -> str:
    """Decode one "..." chunk from a .po file."""
    body = literal.strip()
    if len(body) < 2 or body[0] != '"' or body[-1] != '"':
        raise ValueError(f"not a quoted string: {literal!r}")
    out, i, body = [], 0, body[1:-1]
    while i < len(body):
        c = body[i]
        if c == "\\" and i + 1 < len(body):
            nxt = body[i + 1]
            out.append(_ESCAPES.get(nxt, nxt))
            i += 2
        else:
            out.append(c)
            i += 1
    return "".join(out)


def parse_po(text: str):
    """Yield (key, value) for every translated, non-fuzzy entry.

    Handles the subset real translation tools emit: msgctxt/msgid/msgstr with
    multi-line continuation. Plural forms are rejected loudly rather than
    silently half-imported -- no UI string in Leaf needs them today, and a
    silent drop would be a translation that mysteriously does not apply.
    """
    ctx = msgid = msgstr = None
    fuzzy = False
    field = None
    pending_flags = False

    def flush():
        nonlocal ctx, msgid, msgstr, fuzzy, field
        if msgid is not None and msgstr and not fuzzy and msgid != "":
            key = f"{ctx}|{msgid}" if ctx else msgid
            yielded.append((key, msgstr))
        ctx = msgid = msgstr = None
        fuzzy = False
        field = None

    yielded = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line:
            flush()
            continue
        if line.startswith("#"):
            if line.startswith("#,") and "fuzzy" in line:
                fuzzy = True
                pending_flags = True
            continue
        if line.startswith("msgid_plural") or line.startswith("msgstr["):
            raise SystemExit(
                f"{lineno}: plural forms are not supported; "
                "no Leaf string needs them, so this is probably a mistake"
            )
        for name, target in (("msgctxt", "ctx"), ("msgid", "msgid"), ("msgstr", "msgstr")):
            if line.startswith(name + " "):
                value = unquote(line[len(name):])
                if target == "ctx":
                    ctx = value
                elif target == "msgid":
                    msgid = value
                else:
                    msgstr = value
                field = target
                break
        else:
            if line.startswith('"') and field:
                chunk = unquote(line)
                if field == "ctx":
                    ctx += chunk
                elif field == "msgid":
                    msgid += chunk
                else:
                    msgstr += chunk
            else:
                raise SystemExit(f"{lineno}: cannot parse {raw!r}")
        _ = pending_flags
    flush()
    return yielded


def parse_tsv(text: str):
    out = []
    for lineno, raw in enumerate(text.splitlines(), 1):
        line = raw.rstrip("\r")
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        if "\t" not in line:
            raise SystemExit(f"{lineno}: no tab separator in {raw!r}")
        key, _, val = line.partition("\t")
        if key and val:
            out.append((key, val))
    return out


_SPEC = re.compile(r"%(?:%|[-+ #0'*.\d]*([hlLqjzt]*[diouxXeEfFgGaAcspn]))")


def fmt_sig(s: str):
    return [m for m in _SPEC.findall(s) if m]


def build(pairs, strict_collisions=True) -> bytes:
    seen = {}
    for key, val in pairs:
        if key in seen and seen[key] != val:
            raise SystemExit(f"duplicate key with conflicting values: {key!r}")
        if fmt_sig(key) != fmt_sig(val):
            raise SystemExit(
                f"format specifiers differ between key and translation: {key!r} -> {val!r}"
            )
        seen[key] = val

    pool = bytearray()
    offsets = {}

    def intern(s: str) -> int:
        if s in offsets:
            return offsets[s]
        off = len(pool)
        pool.extend(s.encode("utf-8"))
        pool.append(0)
        offsets[s] = off
        return off

    entries = []
    for key, val in seen.items():
        kb = key.encode("utf-8")
        entries.append((fnv1a(kb), intern(key), intern(val)))

    # Sorted by hash so the runtime can binary-search; ties keep a stable order
    # and are resolved there by strcmp, so a collision returns the right string
    # rather than a plausible wrong one.
    entries.sort(key=lambda e: (e[0], e[1]))
    if strict_collisions:
        hashes = [e[0] for e in entries]
        dupes = {h for h, nxt in zip(hashes, hashes[1:]) if h == nxt}
        if dupes:
            print(
                f"note: {len(dupes)} hash collision(s); resolved by strcmp at runtime",
                file=sys.stderr,
            )

    if not pool:
        pool.append(0)

    out = bytearray()
    out += MAGIC
    out += struct.pack("<IIII", VERSION, len(entries), len(pool), 0)
    for h, k, v in entries:
        out += struct.pack("<III", h, k, v)
    out += pool
    assert len(out) == HEADER + len(entries) * 12 + len(pool)
    return bytes(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help=".po or .tsv translation")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--min-coverage", type=float, default=0.0,
                    help="fail if translated share of --total falls below this (0..1)")
    ap.add_argument("--total", type=int, default=0,
                    help="extracted key count, for --min-coverage")
    args = ap.parse_args()

    text = open(args.source, encoding="utf-8").read()
    pairs = parse_po(text) if args.source.endswith(".po") else parse_tsv(text)

    if args.min_coverage > 0:
        if args.total <= 0:
            raise SystemExit("--min-coverage needs --total")
        share = len(pairs) / args.total
        if share < args.min_coverage:
            raise SystemExit(
                f"coverage {share:.1%} is below the {args.min_coverage:.0%} gate "
                f"({len(pairs)}/{args.total} translated)"
            )

    blob = build(pairs)
    with open(args.output, "wb") as fh:
        fh.write(blob)
    print(f"{args.output}: {len(pairs)} entries, {len(blob)} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
