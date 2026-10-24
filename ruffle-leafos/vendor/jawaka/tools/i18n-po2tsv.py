#!/usr/bin/env python3
"""Export a .po to the device's live-override TSV.

The TSV is the review vehicle: it ignores fuzzy marks on purpose, so a seeded
translation can be seen on the panel before anyone has confirmed it. The
compiled table (i18n-compile.py) is the opposite -- fuzzy never ships. Both
read the same .po, which stays the single source of truth.

    tools/i18n-po2tsv.py i18n/zh_CN.po -o /tmp/zh_CN.tsv
"""

import argparse
import re
import sys


def unesc(s: str) -> str:
    return (s.replace(r"\n", "\n").replace(r"\t", "\t")
             .replace(r"\"", '"').replace("\\\\", "\\"))


def entries(text: str):
    """Yield (msgid, msgstr) pairs, joining continuation lines.

    A .po wraps any long string across lines -- `msgstr ""` then one quoted
    fragment per line -- purely for readability; it is still one string. The
    first version of this tool matched only single-line entries, so every
    wrapped translation arrived as an empty value and was silently skipped:
    72 of Mexican Spanish's 794, which then showed as English on the device and
    read exactly like missing work. Obsolete (#~) entries are ignored."""
    for block in re.split(r"\n\s*\n", text):
        cur = None
        parts = {"msgid": [], "msgstr": []}
        for line in block.splitlines():
            if line.startswith("#"):
                continue
            m = re.match(r'^(msgid|msgstr) "((?:[^"\\]|\\.)*)"$', line)
            if m:
                cur = m.group(1)
                parts[cur].append(m.group(2))
                continue
            m = re.match(r'^"((?:[^"\\]|\\.)*)"$', line)
            if m and cur:
                parts[cur].append(m.group(1))
        key = unesc("".join(parts["msgid"]))
        if key:
            yield key, unesc("".join(parts["msgstr"]))


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("po")
    ap.add_argument("-o", "--output", required=True)
    args = ap.parse_args()

    text = open(args.po, encoding="utf-8").read()
    lines = ["# Generated from " + args.po + " by tools/i18n-po2tsv.py.",
             "# Live-override review file: includes FUZZY entries on purpose."]
    n = untranslated = multiline = 0
    for key, val in entries(text):
        if not val:
            untranslated += 1
            continue
        # The on-device loader splits on raw newlines with no escaping, so a
        # string holding a real line break cannot travel through a TSV. Those
        # still ship in the compiled table; they just cannot be previewed here.
        if "\t" in key or "\n" in key or "\t" in val or "\n" in val:
            multiline += 1
            continue
        lines.append(f"{key}\t{val}")
        n += 1
    open(args.output, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    print(f"{args.output}: {n} entries")
    if multiline:
        print(f"  {multiline} translated string(s) contain a line break and cannot be "
              "previewed through a .tsv; they still ship in the compiled table",
              file=sys.stderr)
    if untranslated:
        print(f"  {untranslated} untranslated, left to fall back to English", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
