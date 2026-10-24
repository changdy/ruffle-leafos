#!/usr/bin/env python3
"""Generate generations/fixtures.json -- the CAT-1 selector/stamp lifecycle.

Three groups:

  resolution    a catalog directory state -> what a reader (or the producer)
                does with it, and why
  publication   a compile's temp output against what is already on disk ->
                published / reused / conflict
  digest        canonical-bytes and tree-hash rules, locked against
                hand-written literals rather than against the model

The digest group is the one that must not be self-referential: a generated
expectation for a hashing rule proves nothing. So the canonical byte string
is spelled out in this file by hand, and the generator refuses to write
anything if the model disagrees with it.

Run: python3 gen_generation_fixtures.py
"""
from __future__ import annotations

import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import canonical  # noqa: E402
import catalog_model  # noqa: E402

OUT = os.path.join(ROOT, "generations", "fixtures.json")

PLATFORM = "mlp1"
RELEASE_ID = "2026-08-28-gabc1234"

A, B, C = "aa" * 32, "bb" * 32, "cc" * 32
D, E = "dd" * 32, "ee" * 32
O1, O2, O3 = "11" * 32, "22" * 32, "33" * 32


def stamp(**edits) -> dict:
    value = {
        "schema": 1,
        "platform": PLATFORM,
        "release_id": RELEASE_ID,
        "base": {"systems_sha256": A, "cores_sha256": B, "info_sha256": C},
        "contributors": [
            {
                "provider": "mlp1/ScummVM.pak",
                "source_id": "primary",
                "pak_version": "1.0.0",
                "provides_sha256": D,
                "files": [{"rel": "art/SCUMMVM.png", "sha256": E}],
            }
        ],
        "output": {"systems_sha256": O1, "cores_sha256": O2, "info_sha256": O3},
    }
    value.update(edits)
    return value


# The canonical form of stamp() above, written out by hand. Keys sorted by
# Unicode code point, separators "," and ":" with no padding, UTF-8, no BOM,
# non-ASCII unescaped, plus exactly one trailing newline. If the model and
# this literal ever disagree, one of them is wrong and the generator stops.
CANONICAL_LITERAL = (
    '{"base":{"cores_sha256":"' + B + '","info_sha256":"' + C
    + '","systems_sha256":"' + A + '"},'
    '"contributors":[{"files":[{"rel":"art/SCUMMVM.png","sha256":"' + E + '"}],'
    '"pak_version":"1.0.0","provider":"mlp1/ScummVM.pak","provides_sha256":"'
    + D + '","source_id":"primary"}],'
    '"output":{"cores_sha256":"' + O2 + '","info_sha256":"' + O3
    + '","systems_sha256":"' + O1 + '"},'
    '"platform":"mlp1","release_id":"' + RELEASE_ID + '","schema":1}\n'
)

GEN = canonical.generation_name(stamp())
OTHER_STAMP = stamp(release_id="2026-09-01-gdeadbee")
OTHER_GEN = canonical.generation_name(OTHER_STAMP)

OUTPUT_HASHES = {"systems_sha256": O1, "cores_sha256": O2, "info_sha256": O3}


_DEFAULT = object()


def present(name: str, stamp_value=_DEFAULT, **edits) -> dict:
    """stamp_value=None means the stamp file is ABSENT, which is a different
    state from "use the default stamp" -- hence the sentinel."""
    entry = {"present": True,
             "stamp": stamp() if stamp_value is _DEFAULT else stamp_value,
             "outputs": list(catalog_model.GENERATION_OUTPUTS)}
    entry.update(edits)
    return {name: entry}


def base_state(**edits) -> dict:
    state = {
        "platform": PLATFORM,
        "installed_release_id": RELEASE_ID,
        "release_defaults_valid": True,
        "current": canonical.selector_bytes(GEN).decode(),
        "generations": present(GEN),
        "role": "reader",
    }
    state.update(edits)
    return state


RESOLUTION_CASES = [
    {
        "name": "resolves-selected-generation",
        "note": "the ordinary path: both consumers agree on whatever `current` "
                "names, and neither of them rehashes a core to get there.",
        "state": base_state(),
        "expect": {"resolution": "generation", "generation": GEN, "reason": None},
    },
    {
        "name": "selector-swap",
        "note": "after a swap both readers resolve the NEW generation. A "
                "reader already inside the old immutable directory may finish "
                "its snapshot; it just never starts a new one there.",
        "state": base_state(
            current=canonical.selector_bytes(OTHER_GEN).decode(),
            installed_release_id="2026-09-01-gdeadbee",
            generations={**present(GEN), **present(OTHER_GEN, OTHER_STAMP)},
        ),
        "expect": {"resolution": "generation", "generation": OTHER_GEN, "reason": None},
    },
    {
        "name": "selector-missing",
        "note": "the state left behind by invalidation, or by a crash "
                "mid-compile. Release defaults, not an error.",
        "state": base_state(current=None),
        "expect": {"resolution": "release-defaults", "generation": None,
                   "reason": "selector-missing"},
    },
    {
        "name": "selector-malformed-no-newline",
        "note": "the grammar is exact. This value names a directory a reader "
                "is about to open, so anything permissive here is a "
                "path-handling bug waiting to happen.",
        "state": base_state(current=GEN),
        "expect": {"resolution": "release-defaults", "generation": None,
                   "reason": "selector-malformed"},
    },
    {
        "name": "selector-malformed-uppercase-hex",
        "state": base_state(current=(canonical.SELECTOR_PREFIX + "A" * 64 + "\n")),
        "expect": {"resolution": "release-defaults", "generation": None,
                   "reason": "selector-malformed"},
    },
    {
        "name": "selector-malformed-traversal",
        "note": "the reason a strict grammar exists at all.",
        "state": base_state(current="../../elsewhere\n"),
        "expect": {"resolution": "release-defaults", "generation": None,
                   "reason": "selector-malformed"},
    },
    {
        "name": "generation-missing",
        "note": "a valid selector naming a directory that is not there.",
        "state": base_state(generations={}),
        "expect": {"resolution": "release-defaults", "generation": None,
                   "reason": "generation-missing"},
    },
    {
        "name": "generation-incomplete-outputs",
        "note": "the directory exists but cores.json does not. A partially "
                "written generation must never be served.",
        "state": base_state(generations=present(GEN, outputs=["systems.json", "info"])),
        "expect": {"resolution": "release-defaults", "generation": None,
                   "reason": "generation-missing"},
    },
    {
        "name": "stamp-missing",
        "state": base_state(generations=present(GEN, None)),
        "expect": {"resolution": "release-defaults", "generation": None,
                   "reason": "stamp-missing"},
    },
    {
        "name": "stamp-schema-unsupported",
        "state": base_state(generations=present(GEN, stamp(schema=2))),
        "expect": {"resolution": "release-defaults", "generation": None,
                   "reason": "stamp-schema-unsupported"},
    },
    {
        "name": "stamp-platform-mismatch",
        "note": "a card moved between devices. The systems list is "
                "platform-specific, so this is a fallback, not a warning.",
        "state": base_state(generations=present(GEN, stamp(platform="tg5040"))),
        "expect": {"resolution": "release-defaults", "generation": None,
                   "reason": "stamp-platform-mismatch"},
    },
    {
        "name": "stamp-release-mismatch-after-ota",
        "note": "D2: the post-OTA system list must match the new release "
                "exactly. A stale catalog that silently pins a user to a "
                "pre-OTA system list is worse than not shipping the feature.",
        "state": base_state(installed_release_id="2026-09-01-gdeadbee"),
        "expect": {"resolution": "release-defaults", "generation": None,
                   "reason": "stamp-release-mismatch"},
    },
    {
        "name": "release-identity-unavailable",
        "note": "T-5: release.json missing or unreadable. The compiler never "
                "guesses an identity.",
        "state": base_state(installed_release_id=None),
        "expect": {"resolution": "release-defaults", "generation": None,
                   "reason": "release-identity-unavailable"},
    },
    {
        "name": "release-defaults-invalid",
        "note": "T-4: when BOTH the effective generation and the release "
                "defaults fail, the answer is an explicit catalog error. "
                "There is no stale-data fallback to reach for.",
        "state": base_state(current=None, release_defaults_valid=False),
        "expect": {"resolution": "catalog-error", "generation": None,
                   "reason": "release-defaults-invalid"},
    },
    {
        "name": "cs-survives-daemon-restart",
        "note": "T-4: a long-lived CS process outliving a jawakad crash. "
                "During the gap the selector is absent, so CS serves release "
                "defaults -- never the generation it had cached a moment ago.",
        "state": base_state(current=None),
        "expect": {"resolution": "release-defaults", "generation": None,
                   "reason": "selector-missing"},
        "cache_note": "CS may cache structural validation only while the exact "
                      "`current` value and installed release_id are unchanged; "
                      "both changed here, so the cache is void.",
    },
]

PROVENANCE_CASES = [
    {
        "name": "producer-fresh",
        "note": "everything hashes as recorded; no recompile.",
        "state": base_state(role="producer", on_disk={
            "base": stamp()["base"],
            "contributors": stamp()["contributors"],
        }),
        "expect": {"action": "none", "reason": None,
                   "invalidate_selector_first": False},
    },
    {
        "name": "producer-base-mismatch",
        "note": "an OTA or a manual drag-the-ZIP replaced the release "
                "defaults. base.info_sha256 is in the stamp precisely so a "
                "partial manual copy that left release_id alone is still caught.",
        "state": base_state(role="producer", on_disk={
            "base": {"systems_sha256": A, "cores_sha256": B, "info_sha256": O3},
            "contributors": stamp()["contributors"],
        }),
        "expect": {"action": "compile", "reason": "stamp-base-mismatch",
                   "invalidate_selector_first": True},
    },
    {
        "name": "producer-contributor-mismatch-swapped-so",
        "note": "P1-7: the .so was swapped without touching `provides`. "
                "Fingerprinting every DECLARED FILE, not just the manifest, "
                "is what catches this -- the compiler's status output asserted "
                "that exact file was present and usable.",
        "state": base_state(role="producer", on_disk={
            "base": stamp()["base"],
            "contributors": [dict(stamp()["contributors"][0],
                                  files=[{"rel": "art/SCUMMVM.png", "sha256": O1}])],
        }),
        "expect": {"action": "compile", "reason": "stamp-contributor-mismatch",
                   "invalidate_selector_first": True},
    },
    {
        "name": "producer-invalidates-selector-before-recompiling",
        "note": "the ordering is the contract: unlink `current`, fsync, THEN "
                "recompile. During the gap every new reader gets release "
                "defaults, and a crash mid-compile leaves no selector rather "
                "than one pointing at unverified output.",
        "state": base_state(role="producer", on_disk={
            "base": {"systems_sha256": O1, "cores_sha256": B, "info_sha256": C},
            "contributors": stamp()["contributors"],
        }),
        "expect": {"action": "compile", "reason": "stamp-base-mismatch",
                   "invalidate_selector_first": True},
    },
]

PUBLICATION_CASES = [
    {
        "name": "publish-new-generation",
        "existing": {},
        "temp_stamp": stamp(),
        "temp_outputs": OUTPUT_HASHES,
        "expect": {"outcome": "published", "generation": GEN, "reason": None},
    },
    {
        "name": "identical-generation-reuse",
        "note": "T-2: a no-op recompile lands on the same content-addressed "
                "name, is verified, and reuses the existing directory. No "
                "counter, no garbage, no second copy.",
        "existing": {GEN: {"stamp": stamp(), "outputs_sha256": OUTPUT_HASHES}},
        "temp_stamp": stamp(),
        "temp_outputs": OUTPUT_HASHES,
        "expect": {"outcome": "reused", "generation": GEN, "reason": None},
    },
    {
        "name": "generation-digest-conflict",
        "note": "the same name holding different bytes is corruption. Fail "
                "closed, diagnose it, and do NOT overwrite a directory a "
                "reader may be inside.",
        "existing": {GEN: {"stamp": stamp(),
                           "outputs_sha256": dict(OUTPUT_HASHES, cores_sha256=A)}},
        "temp_stamp": stamp(),
        "temp_outputs": OUTPUT_HASHES,
        "expect": {"outcome": "conflict", "generation": GEN,
                   "reason": "generation-digest-conflict"},
    },
    {
        "name": "compile-failure-leaves-readable-diagnostics",
        "note": "diagnostics.json lives OUTSIDE every generation, so a "
                "compile that produced no directory at all still leaves an "
                "explanation a developer and the Pak Rat UI can read.",
        "existing": {},
        "temp_stamp": None,
        "temp_outputs": None,
        "expect": {"outcome": "failed", "generation": None,
                   "reason": "compile-failed"},
        "diagnostics_path": "catalog/diagnostics.json",
        "diagnostics_survive_failure": True,
    },
]

CLEANUP_CASES = [
    {
        "name": "temp-only-cleanup",
        "note": "S-4/T-4: v1 removes abandoned temp directories and NOTHING "
                "else. Finalized generations persist across daemon restarts "
                "because a CS process may still be reading one.",
        "entries": ["tmp-a1b2c3", "tmp-d4e5f6", GEN, OTHER_GEN],
        "current": canonical.selector_bytes(GEN).decode(),
        "expect": {"removed": ["tmp-a1b2c3", "tmp-d4e5f6"],
                   "kept": sorted([GEN, OTHER_GEN]),
                   "selected": GEN},
    },
]

EMPTY_SHA256 = hashlib.sha256(b"").hexdigest()

DIGEST_CASES = [
    {
        "name": "canonical-stamp-bytes",
        "note": "the exact bytes hashed to name a generation, spelled out by "
                "hand. Producer and both readers must agree on this or they "
                "disagree about which generation is current.",
        "stamp": stamp(),
        "canonical_bytes": CANONICAL_LITERAL,
        "digest": hashlib.sha256(CANONICAL_LITERAL.encode()).hexdigest(),
        "generation": GEN,
    },
    {
        "name": "contributor-ordering-is-normative",
        "note": "arrays keep their order through canonicalization, so the "
                "producer must sort contributors by provider (and each "
                "contributor's files by rel) BEFORE hashing. Unsorted input "
                "produces a different, wrong digest.",
        "sorted_providers": ["mlp1/Alpha.pak", "mlp1/Beta.pak"],
        "unsorted_providers": ["mlp1/Beta.pak", "mlp1/Alpha.pak"],
        "digests_must_differ": True,
    },
    {
        "name": "tree-hash-length-prefix",
        "note": "without the uint64 length prefix, {'a': 'x', 'b': 'yz'} and "
                "{'a': 'xy', 'b': 'z'} hash identically -- an .info directory "
                "could be rewritten across a file boundary and keep a valid "
                "stamp.",
        "left": {"a": "x", "b": "yz"},
        "right": {"a": "xy", "b": "z"},
        "digests_must_differ": True,
    },
    {
        "name": "tree-hash-empty",
        "note": "a missing .info directory and an empty one contribute the "
                "same nothing, so they hash the same.",
        "entries": {},
        "digest": EMPTY_SHA256,
    },
]


def main() -> None:
    computed = canonical.canonical_bytes(stamp()).decode()
    if computed != CANONICAL_LITERAL:
        raise SystemExit(
            "canonical_bytes() disagrees with the hand-written literal.\n"
            f"model:   {computed!r}\nliteral: {CANONICAL_LITERAL!r}"
        )

    for case in RESOLUTION_CASES:
        got = catalog_model.resolve(case["state"])
        if got != case["expect"]:
            raise SystemExit(f"{case['name']}: model {got} != expected {case['expect']}")
    for case in PROVENANCE_CASES:
        got = catalog_model.validate_provenance(case["state"])
        if got != case["expect"]:
            raise SystemExit(f"{case['name']}: model {got} != expected {case['expect']}")
    for case in PUBLICATION_CASES:
        if case["temp_stamp"] is None:
            continue
        got = catalog_model.publish(case["existing"], case["temp_stamp"],
                                    case["temp_outputs"])
        if got != case["expect"]:
            raise SystemExit(f"{case['name']}: model {got} != expected {case['expect']}")
    for case in CLEANUP_CASES:
        got = catalog_model.cleanup(case["entries"], case["current"])
        if got != case["expect"]:
            raise SystemExit(f"{case['name']}: model {got} != expected {case['expect']}")

    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    document = {
        "contract": "effective-catalog-v1",
        "note": "selector/stamp lifecycle. `resolution` is what a reader does "
                "per catalog load (structural validation only); `provenance` "
                "is what jawakad alone does at startup and before a ROM scan "
                "(full hashing); `publication` and `cleanup` cover the "
                "content-addressed publish protocol; `digest` locks the "
                "canonical-bytes and tree-hash rules against hand-written "
                "literals.",
        "platform": PLATFORM,
        "installed_release_id": RELEASE_ID,
        "resolution": RESOLUTION_CASES,
        "provenance": PROVENANCE_CASES,
        "publication": PUBLICATION_CASES,
        "cleanup": CLEANUP_CASES,
        "digest": DIGEST_CASES,
    }
    with open(OUT, "w") as f:
        json.dump(document, f, indent=2)
        f.write("\n")
    total = sum(len(document[k]) for k in
                ("resolution", "provenance", "publication", "cleanup", "digest"))
    print(f"Generated {total} generation fixtures.")


if __name__ == "__main__":
    main()
