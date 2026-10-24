#!/usr/bin/env python3
"""Validate every fixture in contracts/leaf-content/ against its schema and
against the specific rejection reason it claims.

Every valid/ manifest must validate; every invalid/ one must fail with the
exact reason named in its expect.json. Same rule for the merge, generation,
and storefront fixtures.

Exit 0 and a line per fixture on success; on any mismatch, print the
offending fixture and exit 1.
"""
from __future__ import annotations

import copy
import hashlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORKSPACE = os.path.dirname(os.path.dirname(ROOT))
sys.path.insert(0, HERE)
# minischema is shared with the SVC-1 contract rather than copied: a second
# copy of a JSON-Schema subset is a second thing to keep in step.
sys.path.insert(0, os.path.join(WORKSPACE, "contracts", "leaf-services", "scripts"))

import art_fixtures  # noqa: E402
import canonical  # noqa: E402
import catalog_model  # noqa: E402
import content_model  # noqa: E402
import minischema  # noqa: E402
import scrape_model  # noqa: E402
import storefront_model  # noqa: E402

FAILURES: list[str] = []
VERBOSE = "-v" in sys.argv or "--verbose" in sys.argv


def fail(msg: str) -> None:
    FAILURES.append(msg)
    print(f"FAIL: {msg}")


def ok(msg: str) -> None:
    if VERBOSE:
        print(f"ok:   {msg}")


def load_json(path: str):
    with open(path) as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# CONTENT-1 manifests
# ---------------------------------------------------------------------------

def run_manifest_fixtures() -> None:
    schema = load_json(os.path.join(ROOT, "content-paks-v1.schema.json"))
    valid_dir = os.path.join(ROOT, "manifests", "valid")
    invalid_dir = os.path.join(ROOT, "manifests", "invalid")

    for name in sorted(os.listdir(valid_dir)):
        fdir = os.path.join(valid_dir, name)
        pak = load_json(os.path.join(fdir, "pak.json"))
        expect = load_json(os.path.join(fdir, "expect.json"))
        context = _context(fdir)

        schema_ok, schema_err = minischema.is_valid(pak, schema)
        violations = content_model.validate_manifest(pak, fdir, context)
        warnings = content_model.manifest_warnings(pak)

        if not schema_ok:
            fail(f"manifests/valid/{name}: expected schema-valid, got: {schema_err}")
            continue
        if violations:
            fail(f"manifests/valid/{name}: expected accepted, got {sorted(violations)}")
            continue
        if sorted(warnings) != sorted(expect.get("warnings", [])):
            fail(f"manifests/valid/{name}: warnings {sorted(warnings)} != "
                 f"expected {sorted(expect.get('warnings', []))}")
            continue
        if "apps_listed" in expect:
            listed = content_model.apps_listed(pak, context)
            if listed != expect["apps_listed"]:
                fail(f"manifests/valid/{name}: apps_listed {listed} != "
                     f"expected {expect['apps_listed']}")
                continue
        ok(f"manifests/valid/{name}")

    seen_reasons: dict[str, list[str]] = {}
    for name in sorted(os.listdir(invalid_dir)):
        fdir = os.path.join(invalid_dir, name)
        pak = load_json(os.path.join(fdir, "pak.json"))
        expect = load_json(os.path.join(fdir, "expect.json"))
        context = _context(fdir)
        expected_reason = expect["reason"]
        seen_reasons.setdefault(expected_reason, []).append(name)

        violations = content_model.validate_manifest(pak, fdir, context)
        if expected_reason not in violations:
            fail(f"manifests/invalid/{name}: expected reason {expected_reason!r}, "
                 f"got {sorted(violations)}")
            continue
        # A refused CONTRIBUTION must not un-list a working app. The shared
        # hybrid is the case that would regress here.
        if "apps_listed" in expect:
            listed = content_model.apps_listed(pak, context)
            if listed != expect["apps_listed"]:
                fail(f"manifests/invalid/{name}: apps_listed {listed} != "
                     f"expected {expect['apps_listed']}")
                continue
        ok(f"manifests/invalid/{name} -> {expected_reason}")

    # One fixture per rule, so a duplicated slug is either a fixture that
    # tests nothing new or a rule with two genuinely different outcomes. The
    # sanctioned exceptions are named, fixture by fixture, so a third claimant
    # of any slug still fails:
    #   - D16's shared rule has two outcomes (pure stays hidden, hybrid stays
    #     launchable).
    #   - A reason slug is vocabulary, not a rule. `frontend` reuses two
    #     existing slugs through rules of its own (retroarch-only field on a
    #     path core; executable check on frontend.path), and each of those
    #     checks has its own call site that only its own fixture can pin.
    sanctioned_duplicates = {
        "shared-content-unsupported": {
            "shared-content-unsupported-pure",
            "shared-content-unsupported-hybrid",
        },
        "core-type-field-mismatch": {
            "core-type-field-mismatch",
            "frontend-on-path-core",
        },
        "not-executable": {
            "not-executable",
            "frontend-not-executable",
        },
    }
    for reason, names in sorted(seen_reasons.items()):
        if len(names) > 1 and set(names) != sanctioned_duplicates.get(reason):
            fail(f"reason {reason!r} claimed by {len(names)} fixtures: {names}")

    print(f"CONTENT-1 manifests: {len(os.listdir(valid_dir))} valid, "
          f"{len(os.listdir(invalid_dir))} invalid, "
          f"{len(seen_reasons)} distinct rejection reasons")


def _context(fixture_dir: str) -> dict:
    path = os.path.join(fixture_dir, "context.json")
    return load_json(path) if os.path.exists(path) else {}


# ---------------------------------------------------------------------------
# CONTENT-SCRAPE-1 companion metadata
# ---------------------------------------------------------------------------

def run_scrape_fixtures() -> None:
    document = load_json(os.path.join(ROOT, "scrape", "fixtures.json"))
    schema = load_json(os.path.join(ROOT, "content-scrape-v1.schema.json"))
    seen_reasons: set[str] = set()

    for case in document["cases"]:
        pak = {"provides": copy.deepcopy(
            case.get("provides", document["base_provides"]))}
        if "content_scrape" in case:
            pak["content_scrape"] = copy.deepcopy(case["content_scrape"])

        schema_ok, schema_err = minischema.is_valid(pak, schema)
        violations = scrape_model.validate(pak)
        if case["valid"]:
            if not schema_ok:
                fail(f"scrape/{case['name']}: expected schema-valid, got {schema_err}")
            elif violations:
                fail(f"scrape/{case['name']}: expected accepted, got "
                     f"{sorted(violations)}")
            else:
                ok(f"scrape/{case['name']}")
            continue

        reason = case["reason"]
        if reason in seen_reasons:
            fail(f"scrape/{case['name']}: duplicate fixture reason {reason}")
            continue
        seen_reasons.add(reason)
        if violations != {reason}:
            fail(f"scrape/{case['name']}: expected only {reason!r}, got "
                 f"{sorted(violations)}")
        elif schema_ok and reason not in {
                "duplicate-content-scrape-system",
                "unknown-content-scrape-system",
                "undeclared-content-scrape-extension"}:
            fail(f"scrape/{case['name']}: static shape error passed the schema")
        else:
            ok(f"scrape/{case['name']} -> {reason}")

    generation_hashes: set[str] = set()
    for case in document["generation_cases"]:
        pak = {
            "provides": copy.deepcopy(document["base_provides"]),
            "content_scrape": copy.deepcopy(case["content_scrape"]),
        }
        systems, fingerprinted = scrape_model.decorate(
            case["merged_systems"], [{"provider": case["provider"], "pak": pak}])
        digest = canonical.canonical_sha256({"systems": systems})
        if systems != case["expected_systems"]:
            fail(f"scrape/generation/{case['name']}: decorated systems differ")
        elif fingerprinted != case["expected_fingerprinted_providers"]:
            fail(f"scrape/generation/{case['name']}: fingerprinted providers "
                 f"{fingerprinted} != {case['expected_fingerprinted_providers']}")
        elif digest != case["expected_systems_sha256"]:
            fail(f"scrape/generation/{case['name']}: systems hash {digest} != "
                 f"{case['expected_systems_sha256']}")
        else:
            ok(f"scrape/generation/{case['name']}")
        generation_hashes.add(digest)

    if len(generation_hashes) != len(document["generation_cases"]):
        fail("scrape/generation: distinct policy/output cases share a systems hash")

    print(f"CONTENT-SCRAPE-1: {len(document['cases'])} validation cases, "
          f"{len(document['generation_cases'])} generation cases, "
          f"{len(seen_reasons)} distinct rejection reasons")


# ---------------------------------------------------------------------------
# Merge
# ---------------------------------------------------------------------------

def run_merge_fixtures() -> None:
    document = load_json(os.path.join(ROOT, "merge", "fixtures.json"))
    base = document["base"]
    for case in document["cases"]:
        merged, diagnostics = content_model.merge(base, case["contributors"])

        got_reasons = sorted(d["reason"] for d in diagnostics)
        want_reasons = sorted(case["expect_diagnostics"])
        if got_reasons != want_reasons:
            fail(f"merge/{case['name']}: diagnostics {got_reasons} != "
                 f"hand-asserted {want_reasons}")
            continue
        if diagnostics != case["diagnostics"]:
            fail(f"merge/{case['name']}: diagnostic detail differs from the "
                 f"recorded fixture")
            continue
        # Hand-asserted fields on named merged cores, for fields a pass-through
        # rule would otherwise agree with by construction (see
        # gen_merge_fixtures.py).
        core_rows = {c["id"]: c for c in merged["cores"]}
        field_failure = None
        for core_id, fields in case.get("expect_core_fields", {}).items():
            row = core_rows.get(core_id)
            if row is None:
                field_failure = f"core {core_id} is not in the merged output"
            else:
                for key, want in fields.items():
                    if row.get(key) != want:
                        field_failure = (f"core {core_id} field {key} is "
                                         f"{row.get(key)!r}, hand-asserted {want!r}")
        if field_failure:
            fail(f"merge/{case['name']}: {field_failure}")
            continue
        # Byte-identical, not merely equal: Jawaka's compiler has to produce
        # these exact bytes, so the fixture records the hash of them.
        got_bytes = canonical.canonical_bytes(merged)
        want_bytes = canonical.canonical_bytes(case["expected"])
        if got_bytes != want_bytes:
            fail(f"merge/{case['name']}: merged output differs from expected")
            continue
        if canonical.sha256_hex(got_bytes) != case["expected_sha256"]:
            fail(f"merge/{case['name']}: expected_sha256 does not match its "
                 f"own expected output -- the fixture is internally inconsistent")
            continue
        ok(f"merge/{case['name']} -> {want_reasons or 'accepted'}")

    print(f"CONTENT-1 merge: {len(document['cases'])} cases")


# ---------------------------------------------------------------------------
# CAT-1 generations
# ---------------------------------------------------------------------------

def run_generation_fixtures() -> None:
    document = load_json(os.path.join(ROOT, "generations", "fixtures.json"))
    stamp_schema = load_json(os.path.join(ROOT, "effective-catalog-v1.schema.json"))
    count = 0

    for case in document["resolution"]:
        got = catalog_model.resolve(case["state"])
        if got != case["expect"]:
            fail(f"generations/resolution/{case['name']}: {got} != {case['expect']}")
        else:
            ok(f"generations/resolution/{case['name']} -> "
               f"{got['resolution']} ({got['reason']})")
        count += 1

    for case in document["provenance"]:
        got = catalog_model.validate_provenance(case["state"])
        if got != case["expect"]:
            fail(f"generations/provenance/{case['name']}: {got} != {case['expect']}")
        else:
            ok(f"generations/provenance/{case['name']} -> {got['action']}")
        count += 1

    for case in document["publication"]:
        if case["temp_stamp"] is None:
            # A compile that produced nothing still has to leave an
            # explanation, which is why diagnostics.json lives outside every
            # generation.
            if not case.get("diagnostics_survive_failure"):
                fail(f"generations/publication/{case['name']}: a failed compile "
                     f"must still leave readable diagnostics")
            elif "/gen-" in case.get("diagnostics_path", ""):
                fail(f"generations/publication/{case['name']}: diagnostics must "
                     f"not live inside a generation directory")
            else:
                ok(f"generations/publication/{case['name']} -> compile-failed")
            count += 1
            continue
        got = catalog_model.publish(case["existing"], case["temp_stamp"],
                                    case["temp_outputs"])
        if got != case["expect"]:
            fail(f"generations/publication/{case['name']}: {got} != {case['expect']}")
        else:
            ok(f"generations/publication/{case['name']} -> {got['outcome']}")
        count += 1

    for case in document["cleanup"]:
        got = catalog_model.cleanup(case["entries"], case["current"])
        if got != case["expect"]:
            fail(f"generations/cleanup/{case['name']}: {got} != {case['expect']}")
        elif any(e.startswith(canonical.SELECTOR_PREFIX) for e in got["removed"]):
            fail(f"generations/cleanup/{case['name']}: v1 must never prune a "
                 f"finalized generation")
        else:
            ok(f"generations/cleanup/{case['name']}")
        count += 1

    for case in document["digest"]:
        count += 1
        name = case["name"]

        if "canonical_bytes" in case:
            computed = canonical.canonical_bytes(case["stamp"]).decode()
            if computed != case["canonical_bytes"]:
                fail(f"generations/digest/{name}: canonical bytes differ from "
                     f"the recorded literal")
                continue
            digest = hashlib.sha256(case["canonical_bytes"].encode()).hexdigest()
            if digest != case["digest"] or case["generation"] != "gen-" + digest:
                fail(f"generations/digest/{name}: digest or generation name "
                     f"does not follow from the canonical bytes")
                continue
            schema_ok, schema_err = minischema.is_valid(case["stamp"], stamp_schema)
            if not schema_ok:
                fail(f"generations/digest/{name}: stamp fails its own schema: "
                     f"{schema_err}")
                continue
            ok(f"generations/digest/{name}")
            continue

        if "sorted_providers" in case:
            def build(providers):
                return {"contributors": [{"provider": p} for p in providers]}
            left = canonical.canonical_sha256(build(case["sorted_providers"]))
            right = canonical.canonical_sha256(build(case["unsorted_providers"]))
            if (left == right) == case["digests_must_differ"]:
                fail(f"generations/digest/{name}: contributor order does not "
                     f"change the digest, so sorting could not be normative")
            else:
                ok(f"generations/digest/{name}")
            continue

        if "left" in case:
            left = canonical.tree_sha256_from_entries(
                {k: v.encode() for k, v in case["left"].items()})
            right = canonical.tree_sha256_from_entries(
                {k: v.encode() for k, v in case["right"].items()})
            if (left == right) == case["digests_must_differ"]:
                fail(f"generations/digest/{name}: the length prefix is not "
                     f"doing its job -- two different trees hash the same")
            else:
                ok(f"generations/digest/{name}")
            continue

        got = canonical.tree_sha256_from_entries(
            {k: v.encode() for k, v in case["entries"].items()})
        if got != case["digest"]:
            fail(f"generations/digest/{name}: {got} != {case['digest']}")
        else:
            ok(f"generations/digest/{name}")

    print(f"CAT-1 generations: {count} cases")


# ---------------------------------------------------------------------------
# STORE-CONTENT-1
# ---------------------------------------------------------------------------

def run_storefront_fixtures() -> None:
    document = load_json(os.path.join(ROOT, "storefront", "fixtures.json"))
    schema = load_json(os.path.join(ROOT, "storefront-content-v1.schema.json"))

    for case in document["cases"]:
        storefront = case["storefront"]
        violations = storefront_model.validate(storefront, case.get("artifacts"))

        if case["valid"]:
            schema_ok, schema_err = minischema.is_valid(storefront, schema)
            if not schema_ok:
                fail(f"storefront/{case['name']}: expected schema-valid, got "
                     f"{schema_err}")
                continue
            if violations:
                fail(f"storefront/{case['name']}: expected accepted, got "
                     f"{sorted(violations)}")
                continue
            if "expect_visible_to_gate_unaware" in case:
                visible = storefront_model.visible_to_gate_unaware(storefront)
                if visible != case["expect_visible_to_gate_unaware"]:
                    fail(f"storefront/{case['name']}: gate-unaware view {visible} "
                         f"!= expected {case['expect_visible_to_gate_unaware']}")
                    continue
            ok(f"storefront/{case['name']}")
            continue

        expected_reason = case["reason"]
        # shared-platform-in-content is caught twice on purpose: the schema
        # forbids the value outright, and the model catches it for a
        # generator that never ran the schema.
        schema_ok, _ = minischema.is_valid(storefront, schema)
        if expected_reason not in violations and schema_ok:
            fail(f"storefront/{case['name']}: expected reason "
                 f"{expected_reason!r}, got {sorted(violations)} and the "
                 f"schema accepted it")
            continue
        ok(f"storefront/{case['name']} -> {expected_reason}")

    for case in document["open_button_cases"]:
        got = storefront_model.offers_open(case["installed_pak"])
        if got != case["expect_open"]:
            fail(f"storefront/open/{case['name']}: offers_open {got} != "
                 f"{case['expect_open']}")
        else:
            ok(f"storefront/open/{case['name']}")

    print(f"STORE-CONTENT-1 storefront: {len(document['cases'])} cases, "
          f"{len(document['open_button_cases'])} Open-button cases")


# ---------------------------------------------------------------------------
# Schema self-checks
# ---------------------------------------------------------------------------

def run_schema_selfchecks() -> None:
    """minischema silently ignores keywords it does not implement, so a
    schema that starts using one would look like it is enforcing a rule it
    is not. Fail loudly instead."""
    supported = {
        "$schema", "$id", "$ref", "title", "description", "definitions",
        "type", "const", "enum", "not", "allOf", "anyOf", "pattern",
        "minLength", "maxLength", "minimum", "maximum", "minItems",
        "maxItems", "items", "properties", "required", "additionalProperties",
    }

    def walk(node, path: str, out: set[str]) -> None:
        if isinstance(node, dict):
            for key, value in node.items():
                if path.endswith(".properties") or path.endswith(".definitions"):
                    walk(value, f"{path}.{key}", out)
                    continue
                if key not in supported and not path.endswith(".properties"):
                    out.add(f"{path}.{key}")
                walk(value, f"{path}.{key}", out)
        elif isinstance(node, list):
            for i, item in enumerate(node):
                walk(item, f"{path}[{i}]", out)

    for name in ("content-paks-v1.schema.json",
                 "content-scrape-v1.schema.json",
                 "content-art-v1.schema.json",
                 "content-art-v2.schema.json",
                 "effective-catalog-v1.schema.json",
                 "storefront-content-v1.schema.json"):
        unsupported: set[str] = set()
        walk(load_json(os.path.join(ROOT, name)), name, unsupported)
        if unsupported:
            fail(f"{name} uses JSON-Schema keywords minischema does not "
                 f"implement (they would be silently ignored): "
                 f"{sorted(unsupported)}")
        else:
            ok(f"{name}: every keyword is enforced")

    print("Schemas: keyword support verified against minischema")


def main() -> None:
    run_schema_selfchecks()
    run_manifest_fixtures()
    run_scrape_fixtures()
    art_fixtures.run(ROOT, fail)
    art_fixtures.run(ROOT, fail, version=2)
    run_merge_fixtures()
    run_generation_fixtures()
    run_storefront_fixtures()

    if FAILURES:
        print(f"\n{len(FAILURES)} failure(s).")
        sys.exit(1)
    print("\nAll leaf-content fixtures pass.")


if __name__ == "__main__":
    main()
