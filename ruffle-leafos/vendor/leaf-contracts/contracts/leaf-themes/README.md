# `contracts/leaf-themes/`

Canonical schema, fixtures, and reference validator for `THEME-1`, the Leaf
theme package. The normative text is
[`../../docs/themes.md`](../../docs/themes.md).

This directory is the machine-checkable half: a schema for `theme.json`, a
fixture archive for every rule, and a validator that checks every fixture
against the *specific* rejection reason the contract names.

```text
leaf-themes/
├── theme-v1.schema.json     THEME-1 the theme.json shape
├── fixtures/valid/          accepted theme archives
├── fixtures/invalid/        one archive per rejection rule, named after its reason
├── fixtures/expect.json     the reasons and warnings each archive must produce
└── scripts/
    ├── theme_model.py           the reference validator
    ├── gen_theme_fixtures.py    builds fixtures/ and expect.json
    └── validate_fixtures.py     checks everything
```

## The reference validator

`scripts/theme_model.py` is the executable statement of the contract. The
store's submission pipeline runs it, and Jawaka's install-time check has to
agree with it reason for reason.

```python
import theme_model

theme_model.validate_manifest(obj)    # -> ["theme-grid-invalid", ...]
theme_model.validate_archive(path)    # -> (reasons, warnings), both sorted
theme_model.validate_archive_findings(path)
                                       # -> the same checks as (slug, entry name) pairs
```

A package is accepted when `reasons` is empty. Warnings never refuse anything.

`validate_archive_findings` says where each problem is, so a submitter can be
told which file to fix: the zip entry a rule is about, such as
`neon-nights/grid/icons/GB.png`, or `None` for a rule about the whole archive.
Its slugs always equal `validate_archive`'s, and the fixture checks enforce
that. Entry names come from the archive and are untrusted; escape them before
display.

The validator reads image **headers** only: the PNG signature and IHDR chunk,
or JPEG markers up to the frame header. That is enough to identify the format
and bound the decode size. Full decoding happens on the device, which falls
back to its next artwork candidate when a file will not decode.

It reads the zip itself instead of using `zipfile`, because the contract makes
promises about raw entry names, Unix mode bits, declared sizes, and the order
checks run in, and `zipfile` normalizes or hides exactly those.

## What keeps the fixtures honest

- **Hand-written expectations.** Every reason and warning in
  `fixtures/expect.json` is a literal in `gen_theme_fixtures.py`. The generator
  never imports the model, so the two can disagree and the validator says so.
- **One archive per rule, failing for that rule only.** Each invalid fixture is
  an otherwise valid theme with one thing wrong. The validator fails if a
  reason slug has no fixture or more than one, or if a warning is never
  exercised.
- **Edges in memory.** Boundary and type variants (a 1024 px icon and a 1025 px
  one, `3.0` columns, an encrypted entry, a SOF3 JPEG, a case-only duplicate)
  are built at validation time and must keep their rule's reason, so one named
  fixture per rule still covers the rule's edges.
- **Schema agreement.** Every valid fixture's `theme.json` passes
  `theme-v1.schema.json`; an invalid fixture fails the schema exactly when its
  reason is about the shape of `theme.json`. The validator also checks that the
  schema only uses keywords `minischema.py` implements.

## Reproducible archives

The fixtures are generator output and byte-for-byte reproducible on any
machine: fixed 1980-01-01 timestamps, fixed attributes, entries in sorted name
order, and compressed data from a small fixed-Huffman DEFLATE encoder in the
generator rather than from the local zlib, whose output varies between builds.

The images inside are real, decodable files built from their dimensions:
1-bit grayscale PNGs and baseline grayscale JPEGs, a few hundred bytes to a few
kilobytes each.

`invalid/archive-too-large.zip` is built in memory each time the checks run
instead of committed: it has to be just over 10 MiB to test its rule, which is
not worth keeping in the repository. `expect.json` marks it `"in_memory": true`.
`invalid/uncompressed-too-large.zip` declares 25 MiB but is a run of one byte
value, so it stays small.

## Running it

```bash
cd contracts/leaf-themes/scripts
python3 gen_theme_fixtures.py    # regenerate fixtures/ and expect.json
python3 validate_fixtures.py     # check everything (-v for per-fixture lines)
```

Stdlib-only Python. `minischema.py` is imported from
`../leaf-services/scripts/` rather than copied. Edit the generator and let it
overwrite `fixtures/`; never hand-edit an archive or `expect.json`.

## How another repo references this directory

Identical to the SVC-1 rule, for the same reasons; see
[`../leaf-services/README.md`](../leaf-services/README.md):

- **Local development**: a local `leaf-contracts` checkout.
- **CI**: an explicit `leaf-contracts` commit SHA pinned in the consuming
  repo's own workflow.

There is no vendored copy of this directory.

## Contract text vs. this directory

If the schema, a fixture, or the validator here disagrees with
[`docs/themes.md`](../../docs/themes.md), the contract text wins. Fix this
directory, unless the disagreement reveals that the text itself needs a
change, in which case follow its
[change procedure](../../docs/themes.md#change-procedure).
