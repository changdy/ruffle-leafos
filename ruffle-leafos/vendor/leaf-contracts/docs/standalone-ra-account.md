# standalone-ra-account-v1: Normative Contract

Date: 2026-09-24

This is the **normative text** for `standalone-ra-account-v1`: the
RetroAchievements account snapshot a Leaf launcher hands to one authorized
standalone emulator process (an emulator that is not RetroArch) at launch.

| Party | Who | Obligation in one line |
| --- | --- | --- |
| **Producer** | the launcher daemon (Jawaka's `jawakad`) | resolve the saved account once, export the snapshot only in the forked child of an authorized target |
| **Consumer** | an emulator's account adapter (the bundled Flycast, the DSperate pak) | copy, validate and unset the snapshot before anything else runs, then apply one decision |

The machine-checkable half is
[`../contracts/leaf-services/standalone-ra-account-v1/fixtures.json`](../contracts/leaf-services/standalone-ra-account-v1/fixtures.json),
checked by the single reference classifier in
[`../contracts/leaf-services/scripts/validate_fixtures.py`](../contracts/leaf-services/scripts/validate_fixtures.py)
(`classify_ra_account_env`). If the fixtures and this file disagree, **this
file wins**; fix the fixtures, unless the disagreement shows this text needs a
change, in which case follow [Change procedure](#change-procedure).

A consumer pins this repository at an explicit commit and replays the
fixtures through its own classifier, compiled from the same source file its
device build uses. See
[`../contracts/leaf-services/README.md`](../contracts/leaf-services/README.md)
for the pinning rule.

---

## Transport

The snapshot is five environment variables, set by the producer **in the
child process after `fork()` and before `exec()`**, and nowhere else.

- The producer's own environment never holds them. Every other child the
  producer starts (apps, services, package-manager probes, RetroArch, an
  unauthorized emulator) has all five **unset**, so an inherited value from
  a stale parent environment cannot reach it.
- The values go into the environment only. They never appear in `argv`, a
  log line, a crash report, a file the producer writes, or an IPC message.
- The RetroArch per-launch handoff (`JAWAKA_CHEEVOS_USERNAME`,
  `JAWAKA_CHEEVOS_PASSWORD`) is a different mechanism for a different
  consumer. The producer unsets it in every standalone child. Its presence
  next to this snapshot is `stale-retroarch-credentials`.

Environment variables are NUL-terminated C strings, so a value can never
contain byte `0x00` on the wire. The classifier still rejects NUL (see
`credential-control-character`), because a consumer may receive the snapshot
through a test harness or a length-prefixed transport where NUL is
representable. There is no NUL fixture in `fixtures.json` for the same
reason in reverse: a fixture is meant to be replayable as a real
environment, and one containing NUL is not. The CR and LF cases cover the
rule.

## Fields

| Variable | Present when | Value |
| --- | --- | --- |
| `UMRK_RA_ACCOUNT_VERSION` | a handoff is produced | exactly `1` |
| `UMRK_RA_ACCOUNT_STATE` | a handoff is produced | one of the five [states](#states) |
| `UMRK_RA_ACCOUNT_USERNAME` | state is `configured`, and only then | 1 to 63 bytes of valid UTF-8, no NUL, CR or LF |
| `UMRK_RA_ACCOUNT_PASSWORD` | state is `configured`, and only then | 1 to 127 bytes of valid UTF-8, no NUL, CR or LF |
| `UMRK_RA_ACCOUNT_REVISION` | state is `configured` or `signed-out`, and only then | a decimal integer in `1..4611686018427387904` (2^62) |

Limits are **bytes**, not characters. The fixture
`configured-utf8-boundary` sits exactly on both ceilings with two-byte
characters.

Credential bytes are opaque data. Quotes, spaces, shell metacharacters,
commas and semicolons are valid (`configured-punctuation`). Neither side
shell-expands, quotes, trims or case-folds them.

### Revision syntax

A consumer accepts a revision that is **one or more ASCII digits** whose
value is in range. Everything else is `revision-invalid`: the empty string,
`0`, a sign (`+7`, `-3`), whitespace, a decimal point, hex, or any non-digit
character. A value above 2^62 is `revision-overflow`, never clamped.

A producer emits the canonical form only: no sign, no leading zeros, no
whitespace. Consumers do not depend on that (leading zeros parse), but a
producer that emits them is out of contract.

Parsers must not use a library routine that skips leading whitespace or
accepts a sign (`strtoll`, `std::stoll`, `int()` in Python). The fixture
`invalid-handoff-revision-non-numeric` (`+7`) exists to catch that.

## States

| State | Meaning | Consumer behavior |
| --- | --- | --- |
| `configured` | the launcher holds a complete, valid account | authenticate as this account, or reuse a token already accepted for this exact account and revision |
| `signed-out` | the user signed out in the launcher; the revision is retained | clear the managed credentials once per revision, record the revision |
| `never-configured` | the launcher was never given an account | leave an independently configured native account alone when no managed state exists; clear managed credentials when it does |
| `invalid` | the launcher's stored values are malformed | run this session without managed authentication, tell the player the account needs fixing, **keep durable data** |
| `unreadable` | the launcher could not read its store | same as `invalid`: not a sign-out, durable data survives |

`invalid` and `unreadable` are valid **handoffs** carrying a negative
verdict. They are not malformed snapshots and they are not sign-out.

### Unmanaged versus malformed

- **Total absence** of every `UMRK_RA_ACCOUNT_*` variable is *unmanaged*:
  the target is not authorized, or the launcher predates this contract. With
  no managed state on disk, the consumer keeps its native account behavior.
  With managed state on disk, it suppresses managed authentication for the
  session instead of running the last imported token.
- **Any** `UMRK_RA_ACCOUNT_*` variable present but the snapshot failing a
  rule below is a *malformed handoff*. It never revives a previously
  accepted account and never quietly becomes an unmanaged session.

## Revision rules

The revision is what lets a consumer tell "the same account I already
accepted" from "a new save, possibly of the same name" and from "a sign-out
I have not applied yet".

1. The producer keeps one revision counter beside the stored account.
2. Every successful **save** and every successful **clear** increments it,
   inside the same checked write transaction as the change. If the
   transaction fails, neither the account nor the revision changes.
3. A sign-out clears the credentials and **retains** the new revision, so
   `signed-out` always carries one.
4. The producer never **fabricates** a revision. When the counter cannot be
   read, initialized or written, the snapshot is `unreadable` (or `invalid`
   for a malformed stored value), never `configured` with a guessed, zero or
   default revision. A pre-contract store that holds a valid account but no
   counter is given revision 1 in a checked write before the first handoff;
   if that write fails, the rule above applies.
5. The producer never emits a revision of `0` or below, and fails a save
   that would move the counter past 2^62 rather than wrapping.
6. A consumer compares revisions for equality only. It does not infer
   anything from one being larger, since a restored backup can legitimately
   go backwards.

## Producer obligations

- Resolve the account **once** per launch into one snapshot, and export
  exactly that snapshot. The five variables never mix values from two reads.
- Export only to authorized targets. Authorization is a launcher decision
  (provider, core, installed package identity, a capability record the
  package ships); it is out of scope here except that an unauthorized
  target receives total absence.
- Validate before export with the same rules as the classifier. A stored
  value that would fail them makes the state `invalid`; it is never
  truncated into something valid.
- Never write a credential into a file, log, argv or IPC message on the way.

## Consumer obligations

- **Copy, validate, unset.** Read all five variables (and the two
  `JAWAKA_CHEEVOS_*` names) into private memory, unset them from the process
  environment, then classify. Do this before starting a helper process,
  before starting threads that could inherit or read the environment, and
  before any log line is written.
- Classify with rules equivalent to `classify_ra_account_env`, and prove it
  by replaying `fixtures.json` through the compiled classifier in CI.
- Persist managed state (whatever marker the consumer uses to remember the
  accepted account and revision) with a checked write: temporary file in
  the same directory, write, flush, `fsync`, close, rename, each checked. A
  failed write leaves the previous accepted state in effect and is reported
  to the player.
- Accept a revision only after the credential the login produced is durably
  stored. A crash between the two must result in a fresh login next time,
  never in an accepted revision with no token.
- On a rejected login, do not fall back to an older account or token.
- An explicit per-game or per-launch "achievements off" setting wins over a
  `configured` handoff. The consumer still consumes and unsets the snapshot.
- Never log or display the password or a token.

## Rejection reasons

Every rule has at least one fixture whose `reason` names it, and the
validator fails if this table and the fixture reasons differ. A snapshot can
violate several rules; the fixture names the one it exists to prove.

| Reason | Rule |
| --- | --- |
| `unsupported-version` | a contract variable is present and `VERSION` is not exactly `1` (including absent) |
| `missing-account-state` | `VERSION` is `1` and `STATE` is absent |
| `unknown-account-state` | `STATE` is not one of the five states |
| `credentials-unexpected` | `USERNAME` or `PASSWORD` present when the state is not `configured` |
| `username-missing` | `configured` without `USERNAME` |
| `password-missing` | `configured` without `PASSWORD` |
| `credential-empty` | a credential is the empty string |
| `username-oversized` | `USERNAME` is longer than 63 bytes |
| `password-oversized` | `PASSWORD` is longer than 127 bytes |
| `credential-control-character` | a credential contains NUL, CR or LF |
| `credential-invalid-utf8` | a credential is not valid UTF-8 |
| `revision-missing` | `configured` or `signed-out` without `REVISION` |
| `revision-invalid` | `REVISION` is not one or more ASCII digits, or its value is 0 |
| `revision-overflow` | `REVISION` is all digits and greater than 2^62 |
| `revision-unexpected` | `REVISION` present on `never-configured`, `invalid` or `unreadable` |
| `stale-retroarch-credentials` | `JAWAKA_CHEEVOS_USERNAME` or `JAWAKA_CHEEVOS_PASSWORD` present in a standalone child |

Two rules have two fixtures, because each covers two code paths a
consumer can get wrong independently: `unsupported-version` (a partial
snapshot with no `VERSION`, and `VERSION=2`) and `revision-invalid` (`0`,
and the signed `+7`).

## Change procedure

`standalone-ra-account-v1` freezes when the first package that consumes it
is published. Until then:

1. Change this file first.
2. Update `fixtures.json` and, if a rule changed, `classify_ra_account_env`,
   then run `python3 contracts/leaf-services/scripts/validate_fixtures.py`.
3. A new rejection rule gets an invalid fixture naming its reason and a row
   in the table above.
4. Every consumer pins a commit and a sha256 of `fixtures.json`; a change
   here is a reviewed pin bump in each of them.
5. After the freeze, a breaking change is `standalone-ra-account-v2` with
   `UMRK_RA_ACCOUNT_VERSION=2`, never a mutation of v1. Version 2 is refused
   by v1 consumers (`invalid-handoff-version-two`), so a new launcher
   paired with an old emulator fails closed.

## Related

- [`../contracts/leaf-services/standalone-ra-account-v1/fixtures.json`](../contracts/leaf-services/standalone-ra-account-v1/fixtures.json): the fixture set
- [`../contracts/leaf-services/README.md`](../contracts/leaf-services/README.md): pinning and the services contract family
