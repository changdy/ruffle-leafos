# leaf-contracts

The **public** machine-checkable contracts for Leaf: schemas, fixtures, and
reference validators that anything talking to a Leaf device is written
against.

This repository exists because a contract nobody can read is not a contract.
Third-party developers build content paks and service paks against these
interfaces, so the definitive artifacts have to be fetchable without
credentials.

```bash
git clone https://github.com/Utility-Muffin-Research-Kitchen/leaf-contracts
cd leaf-contracts
python3 contracts/leaf-content/scripts/validate_fixtures.py
python3 contracts/leaf-services/scripts/validate_fixtures.py
python3 contracts/leaf-themes/scripts/validate_fixtures.py
```

Stdlib-only Python. No install step.

## What is here

| Contract | Covers | Normative text |
| --- | --- | --- |
| `CONTENT-1` | the `provides` block a content pak declares | [`docs/content-paks.md`](docs/content-paks.md) |
| `CONTENT-ART-2` | optional wordmarks, untinted color wordmarks, and Grid icons, retaining v1 reader support | [`docs/content-paks.md`](docs/content-paks.md) |
| `CONTENT-ART-1` | optional system wordmarks in a `content_art` companion | [`docs/content-paks.md`](docs/content-paks.md) |
| `CAT-1` | the effective catalog: generations, selector, stamp | same |
| `STORE-CONTENT-1` | the storefront `content[]` lane | same |
| `LEAF-RA-1` | the protocol a pak-supplied RetroArch frontend speaks: argv, configuration ownership, the UDP command set and its replies, exit | [`docs/leaf-retroarch-frontend.md`](docs/leaf-retroarch-frontend.md) |
| `THEME-1` | a theme package: its layout, `theme.json`, images, and archive limits | [`docs/themes.md`](docs/themes.md) |
| `SVC-1` | the `service` block a service pak declares | `contracts/leaf-services/README.md` |
| `CTL-1`, `LIFE-1`, `PATH-2`, `PKG-1`, `TXN-1` | control IPC, game coordination, source paths, packaging, transactions | as above |
| `standalone-ra-account-v1` | the RetroAchievements account snapshot the launcher hands to an authorized standalone emulator | [`docs/standalone-ra-account.md`](docs/standalone-ra-account.md) |

```text
contracts/
  leaf-content/     CONTENT-1 / CAT-1 / STORE-CONTENT-1 schemas, fixtures, reference models
  leaf-services/    SVC-1 and friends; also hosts minischema.py, shared by all three
  leaf-themes/      THEME-1 schema, fixture archives, reference validator
docs/
  content-paks.md   normative field-by-field text for the content-pak contracts
  leaf-retroarch-frontend.md  normative text for LEAF-RA-1, pak-supplied RetroArch frontends
  themes.md         normative field-by-field text for the theme package
  standalone-ra-account.md  normative text for the standalone RA account handoff
```

`contracts/leaf-services/scripts/minischema.py` is the small JSON-Schema
subset every validator uses. It lives in one place and is imported, never
copied — a second copy of a schema validator is a second thing to keep in
step.

## Consuming this from your own repository

Two ways, and deliberately nothing in between:

- **Local development**: clone this repository and point your tooling at it.
- **CI**: check it out at an **explicit commit SHA** pinned in your workflow.

A local clone and CI's pinned SHA can disagree. That is supposed to be
visible: the fixture run fails, and that failure is the signal to bump the
pin, not a bug in the pin.

There is no vendored copy of these contracts anywhere, and adding one is the
wrong fix for a version skew you can see.

[`ScummVM-pak`](https://github.com/Utility-Muffin-Research-Kitchen/ScummVM-pak)
is a worked example: its `scripts/validate-pak.py` loads the reference
validator out of a checkout of this repository rather than reimplementing any
rule, so a rejection a developer sees locally is exactly what the store will
say.

## What is deliberately NOT here

The **design records** — why a contract is shaped the way it is, what was
rejected, the phase plans — stay in UMRK's private workspace. They are
working notes, they change constantly, and reading them is not required to
build against these interfaces.

What you need to build is here. If something is missing that you need, that
is a bug in this repository.

## Changing a contract

A published contract is frozen once something ships against it. Before that:

1. Change the normative text first.
2. Update the schema and fixtures to match, and re-run the validators.
3. Every new rejection rule gets **exactly one** invalid fixture whose
   `expect.json` names its reason slug. That one-fixture-per-rule discipline
   is what makes "your pak was rejected for X" a checkable claim.
4. A breaking change after freeze is a **new** version identifier
   (`content-paks-v2`), never a mutation of v1.
