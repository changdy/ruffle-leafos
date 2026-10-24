# `contracts/leaf-services/`

Public schemas and fixtures for `SVC-1`, `CTL-1`, `LIFE-1`, `PATH-2`,
`PKG-1`, `TXN-1`, and `standalone-ra-account-v1`. This directory is the
distributable contract: schemas,
fixtures, and a validator that checks every fixture against its schema and,
for invalid fixtures, against the specific rejection reason it names. No
private workspace checkout is required to consume it.

```text
leaf-services/
├── app-services-v1.schema.json        SVC-1 manifest shape
├── control-ipc-v1.schema.json         CTL-1 request/response shapes
├── game-coordination-v1.schema.json   LIFE-1 message shapes
├── manifests/{valid,invalid}/         SVC-1 fixtures, one per field-table rule
├── supervisor-state/                  generation-lease scenario fixtures
├── game-coordination/                 LIFE-1 subscription accept/reject fixtures
├── source-paths-v2/                   PATH-2 env-var fixtures
├── standalone-ra-account-v1/          standalone-ra-account-v1 env-var fixtures
│                                      (normative text: docs/standalone-ra-account.md)
├── transactions/                      P1/TXN-1/PKG-1 ordering + floor/real pairs
├── wire-fixtures/                     byte-level CTL-1/LIFE-1 frame fixtures
└── scripts/                           generators + validate_fixtures.py
```

## How another repo references this directory

Product repos (`Jawaka`, `ssh-server`, `Leaf-Syncthing-Pak`, …) are
independent GitHub repos. "The sibling directory" is not a usable reference
for their CI, so there are exactly two ways to point at this contract, and
nothing in between:

- **Local development**: a local `leaf-contracts` checkout.
- **CI**: an **explicit `leaf-contracts` commit SHA**, pinned in that repo's
  own workflow file, checked out alongside the existing Catastrophe SHA pin
  pattern already used by dependent repos' CI.

A local developer's sibling checkout and CI's pinned SHA can disagree — that
mismatch is *supposed* to be visible, because the fixture run then fails. That
failure is the signal to bump the pin, not a bug in the pin.

There is no vendored copy of this directory. Consumers use a local checkout
or a pinned commit; a second copy would only add another source that can drift.

## Running the validator

```bash
cd contracts/leaf-services/scripts
python3 gen_manifest_fixtures.py   # regenerate manifests/{valid,invalid}/
python3 gen_wire_fixtures.py       # regenerate wire-fixtures/
python3 validate_fixtures.py       # check everything
```

`validate_fixtures.py` is stdlib-only Python (no `jsonschema`/`ajv` install
required) — see `scripts/minischema.py` for the small JSON-Schema-draft-07
subset it implements, which is exactly the set of keywords the three schema
files in this directory use. If a schema file starts using a keyword
`minischema.py` doesn't support, that keyword is silently ignored rather than
enforced — extend `minischema.py` before relying on one.

The generator scripts (`gen_manifest_fixtures.py`, `gen_wire_fixtures.py`) are
the source of truth for `manifests/` and `wire-fixtures/`; edit the generator,
re-run it, and let it overwrite its output rather than hand-editing generated
fixtures. `supervisor-state/`, `game-coordination/`, `source-paths-v2/`,
`standalone-ra-account-v1/`, and `transactions/` are hand-authored JSON and
are edited directly.

## Contract text vs. this directory

If a schema and fixture disagree, treat that as a contract bug: stop, correct
the public schema and fixture together, and pin the resulting reviewed commit
in each consumer.
