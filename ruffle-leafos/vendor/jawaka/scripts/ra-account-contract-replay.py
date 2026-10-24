#!/usr/bin/env python3
"""Replay the pinned standalone-ra-account-v1 fixtures through Jawaka's producer.

1. Verifies fixtures.json in the pinned leaf-contracts checkout against the
   sha256 Jawaka pins, so a moved pin or an edited file fails loudly.
2. Runs build/bin/ra-account-contract-test, which maps every fixture to the
   producer's real input (stored account rows or an inherited environment),
   runs jawakad's resolve and child-environment code, and compares the result
   with what the contract requires.
3. Classifies every environment the producer emitted with the contract's own
   reference classifier (classify_ra_account_env from the pinned checkout):
   each must be a valid handoff or unmanaged, never a malformed one, and
   every fixture must have been replayed.

Prints case names and verdicts only, never a value.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import importlib.util
import json
import os
import subprocess
import sys
import tempfile

FIXTURES_REL = "contracts/leaf-services/standalone-ra-account-v1/fixtures.json"
CLASSIFIER_REL = "contracts/leaf-services/scripts/validate_fixtures.py"


def fail(message: str) -> None:
    print(f"FAIL ra-account-contract-replay: {message}", file=sys.stderr)
    sys.exit(1)


def load_classifier(contracts: str):
    path = os.path.join(contracts, CLASSIFIER_REL)
    spec = importlib.util.spec_from_file_location("leaf_contracts_validate", path)
    if spec is None or spec.loader is None:
        fail(f"cannot load the reference classifier at {path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.classify_ra_account_env


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--contracts", required=True,
                        help="pinned leaf-contracts checkout")
    parser.add_argument("--sha256", required=True,
                        help="expected sha256 of fixtures.json")
    parser.add_argument("--producer", required=True,
                        help="the built ra-account-contract-test binary")
    args = parser.parse_args()

    fixtures = os.path.join(args.contracts, FIXTURES_REL)
    try:
        with open(fixtures, "rb") as handle:
            raw = handle.read()
    except OSError as exc:
        fail(f"pinned fixtures missing ({exc}); check out leaf-contracts at the "
             "pinned commit (LEAF_CONTRACTS_DIR)")
    digest = hashlib.sha256(raw).hexdigest()
    if digest != args.sha256:
        fail(f"fixtures.json sha256 {digest} != pinned {args.sha256}; "
             "bump the pin and the hash together in a reviewed change")
    names = [case["name"] for case in json.loads(raw)["cases"]]

    classify = load_classifier(args.contracts)

    with tempfile.TemporaryDirectory(prefix="jw-ra-contract-") as work:
        out_path = os.path.join(work, "outputs.json")
        result = subprocess.run([args.producer, fixtures, out_path, work])
        if result.returncode != 0:
            fail("the producer did not emit what the contract requires "
                 "(see the lines above)")
        with open(out_path, "rb") as handle:
            outputs = json.load(handle)

    replayed = set()
    for row in outputs:
        env = {k: base64.b64decode(v) for k, v in row["env_b64"].items()}
        kind, violations = classify(env)
        label = f"{row['case']} [{row['mapping']}]"
        if violations or kind != row["expect_kind"]:
            fail(f"{label}: reference classifier says {kind} "
                 f"{sorted(violations)}, expected {row['expect_kind']}")
        replayed.add(row["case"])
        print(f"  ok {label}: {kind}")

    missing = [name for name in names if name not in replayed]
    if missing:
        fail(f"fixtures not replayed: {missing}")
    print(f"PASS ra-account-contract-replay: {len(names)} fixtures, "
          f"{len(outputs)} producer outputs, fixtures sha256 {digest}")


if __name__ == "__main__":
    main()
