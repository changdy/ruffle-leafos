#!/usr/bin/env python3
"""Validate every fixture in contracts/leaf-services/ against its schema
and against the specific rejection reason it claims (contracts.md A0
"Verify": every valid/ manifest validates, every invalid/ one fails
with the expected reason).

Exit code 0 and a summary line per section on success; on any
mismatch, prints the offending fixture and exits 1.
"""
from __future__ import annotations

import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import minischema  # noqa: E402

FAILURES: list[str] = []


def fail(msg: str) -> None:
    FAILURES.append(msg)
    print(f"FAIL: {msg}")


def ok(msg: str) -> None:
    print(f"ok:   {msg}")


def load_json(path: str):
    with open(path) as f:
        return json.load(f)


# ---------------------------------------------------------------------------
# SVC-1 manifest business rules (beyond generic JSON-schema shape checks)
# ---------------------------------------------------------------------------

REVERSE_DNS = __import__("re").compile(r"^[a-z0-9]+(\.[a-z0-9]+)+$")


def _has_traversal_or_absolute(p: str) -> str | None:
    if p.startswith("/"):
        return "absolute"
    parts = p.split("/")
    if ".." in parts:
        return "traversal"
    return None


def validate_manifest(manifest: dict, fixture_dir: str) -> set[str]:
    v: set[str] = set()

    top_id = manifest.get("id", "")
    svc = manifest.get("service", {})
    svc_id = svc.get("id", "")

    if not REVERSE_DNS.match(top_id) or not REVERSE_DNS.match(svc_id):
        v.add("malformed-id")
    elif top_id != svc_id:
        v.add("id-mismatch")

    schema_val = svc.get("schema")
    if isinstance(schema_val, bool) or schema_val != 1:
        v.add("unknown-schema")

    run = svc.get("run", {})
    run_path = run.get("path", "")
    problem = _has_traversal_or_absolute(run_path)
    if problem == "absolute":
        v.add("absolute-run-path")
    elif problem == "traversal":
        v.add("path-traversal")
    elif run_path:
        full = os.path.join(fixture_dir, run_path)
        base = os.path.realpath(fixture_dir)
        # realpath resolves EVERY component of the path, not just a symlink
        # at the leaf -- an intermediate symlink (e.g. bin/ itself being a
        # symlink pointing outside the pak) escapes exactly as much as a
        # symlinked leaf does, and realpath resolves lexically even through
        # a dangling symlink, so this also catches a symlink to a missing
        # target without a separate existence check first.
        resolved = os.path.realpath(full)
        escapes = not (resolved == base or resolved.startswith(base + os.sep))
        if escapes:
            v.add("escaping-symlink")
        elif not os.path.exists(resolved):
            v.add("run-path-missing")
        elif not os.path.isfile(resolved):
            v.add("non-regular-executable")
        elif not os.access(resolved, os.X_OK):
            v.add("run-path-not-executable")

    args = run.get("args", [])
    if len(args) > 16:
        v.add("args-count-over-limit")
    # SVC-1 caps each arg at 256 *bytes*, not Unicode code points -- a
    # multibyte UTF-8 string can be under 256 characters and still be well
    # over 256 bytes.
    if any(len(a.encode("utf-8")) > 256 for a in args):
        v.add("args-item-too-long")

    if svc.get("restart") not in ("no", "on-failure"):
        v.add("unknown-restart-policy")

    if svc.get("default_enabled") is not False:
        v.add("default-enabled-true")

    if "stop_grace_ms" in svc:
        sg = svc["stop_grace_ms"]
        if isinstance(sg, bool) or not isinstance(sg, int):
            v.add("stop-grace-ms-non-integer")
        elif sg < 0:
            v.add("stop-grace-ms-negative")

    lifecycle = svc.get("lifecycle", {})
    known_lifecycle = {"game", "stop_on_storage_change", "stop_on_suspend"}
    if set(lifecycle) - known_lifecycle:
        v.add("unknown-lifecycle-key")

    state = manifest.get("state", {})
    state_root = state.get("root", "")

    revoke = state.get("revoke_on_uninstall", [])
    if len(revoke) > 16:
        v.add("revoke-on-uninstall-count-over-limit")
    state_root_full = os.path.join(fixture_dir, state_root) if state_root else fixture_dir
    state_root_is_symlink = bool(state_root) and os.path.islink(state_root_full)
    for rp in revoke:
        problem = _has_traversal_or_absolute(rp)
        if problem == "absolute":
            v.add("revoke-on-uninstall-absolute")
        elif problem == "traversal":
            v.add("revoke-on-uninstall-traversal")
        elif state_root_is_symlink:
            # state.root itself is an existing component of every path
            # beneath it; if it's a symlink, every revoke_on_uninstall entry
            # is affected the same way, so flag it directly rather than only
            # discovering it once per entry below.
            v.add("revoke-on-uninstall-symlink-component")
        else:
            current = state_root_full
            # Walk every component INCLUDING the final one: "no existing
            # component may be a symlink" covers the leaf entry too, not
            # just its parent directories. A symlinked leaf would let
            # revocation delete/quarantine through it instead of the
            # declared file itself.
            for comp in rp.split("/"):
                current = os.path.join(current, comp)
                if os.path.islink(current):
                    v.add("revoke-on-uninstall-symlink-component")
                    break

    retained = state.get("retained_roots", [])
    if len(retained) > 16:
        v.add("retained-roots-count-over-limit")
    for rp in retained:
        problem = _has_traversal_or_absolute(rp)
        if problem == "absolute":
            v.add("retained-roots-absolute")
        elif problem == "traversal":
            v.add("retained-roots-traversal")

    return v


def resolve_stop_grace_ms(manifest: dict) -> int:
    sg = manifest.get("service", {}).get("stop_grace_ms")
    if sg is None:
        return 5000
    return min(sg, 15000)


def run_manifest_fixtures() -> None:
    schema = load_json(os.path.join(ROOT, "app-services-v1.schema.json"))
    valid_dir = os.path.join(ROOT, "manifests", "valid")
    invalid_dir = os.path.join(ROOT, "manifests", "invalid")

    for name in sorted(os.listdir(valid_dir)):
        fdir = os.path.join(valid_dir, name)
        pak = load_json(os.path.join(fdir, "pak.json"))
        expect = load_json(os.path.join(fdir, "expect.json"))
        schema_ok, schema_err = minischema.is_valid(pak, schema)
        violations = validate_manifest(pak, fdir)
        if not schema_ok:
            fail(f"manifests/valid/{name}: expected schema-valid, got: {schema_err}")
        elif violations:
            fail(f"manifests/valid/{name}: expected valid, got violations {sorted(violations)}")
        else:
            if "resolved_stop_grace_ms" in expect:
                resolved = resolve_stop_grace_ms(pak)
                if resolved != expect["resolved_stop_grace_ms"]:
                    fail(f"manifests/valid/{name}: resolved stop_grace_ms {resolved} != expected {expect['resolved_stop_grace_ms']}")
                else:
                    ok(f"manifests/valid/{name} (resolved stop_grace_ms={resolved})")
            else:
                ok(f"manifests/valid/{name}")

    for name in sorted(os.listdir(invalid_dir)):
        fdir = os.path.join(invalid_dir, name)
        expect = load_json(os.path.join(fdir, "expect.json"))

        if name == "duplicate-id":
            pak_a = load_json(os.path.join(fdir, "pak-a.json"))
            pak_b = load_json(os.path.join(fdir, "pak-b.json"))
            va = validate_manifest(pak_a, fdir)
            vb = validate_manifest(pak_b, fdir)
            if va or vb:
                fail(f"manifests/invalid/{name}: expected each manifest individually valid pre-dedup-check, got {sorted(va)} / {sorted(vb)}")
                continue
            if pak_a["service"]["id"] != pak_b["service"]["id"]:
                fail(f"manifests/invalid/{name}: fixture bug, service ids differ")
                continue
            ok(f"manifests/invalid/{name} (duplicate-service-id, both unavailable)")
            continue

        pak = load_json(os.path.join(fdir, "pak.json"))
        expected_reason = expect["reason"]
        violations = validate_manifest(pak, fdir)
        if expected_reason not in violations:
            fail(f"manifests/invalid/{name}: expected reason {expected_reason!r}, got violations {sorted(violations)}")
        else:
            ok(f"manifests/invalid/{name} -> {expected_reason}")


# ---------------------------------------------------------------------------
# PATH-2 fixtures
# ---------------------------------------------------------------------------

def _under_root(item: str, root: str) -> bool:
    """Boundary-safe: '/mnt/sdcard2' is not under '/mnt/sdcard', unlike a
    bare str.startswith(root) check (the bug this replaces)."""
    root = root.rstrip("/")
    return item == root or item.startswith(root + "/")


def validate_path2_env(env: dict, plural_vars: list[str], singular_aliases: dict[str, str]) -> set[str]:
    v: set[str] = set()
    lists: dict[str, list[str]] = {}

    for pv in plural_vars:
        if pv not in env:
            v.add("incomplete-plural-lists")
            continue
        items = env[pv].split(":")
        lists[pv] = items

        if any(i == "" for i in items):
            v.add("empty-list-item")
        if any(not i.startswith("/") for i in items if i != ""):
            v.add("colon-in-path")
        if len(items) != len(set(items)):
            v.add("duplicate-root")

        # A valid v2 advertisement requires the singular alias to actually
        # be present, not just equal when it happens to exist -- a producer
        # that exports SDCARD_PATHS but drops SDCARD_PATH is not v2-valid
        # either, and treating "alias absent" as "nothing to check" was the
        # gap that let that pass.
        singular_name = singular_aliases[pv]
        if singular_name not in env:
            v.add("primary-singular-mismatch")
        elif items and items[0] != env[singular_name]:
            v.add("primary-singular-mismatch")

    lengths = {pv: len(items) for pv, items in lists.items()}
    if len(set(lengths.values())) > 1:
        v.add("count-mismatch")

    if "SDCARD_PATHS" in lists:
        ref = lists["SDCARD_PATHS"]
        for pv, items in lists.items():
            if pv == "SDCARD_PATHS" or len(items) != len(ref):
                continue
            # Check EVERY index against its own SDCARD_PATHS entry, not just
            # index 0 -- a Secondary-only misalignment (index 1 pointing
            # under the wrong card while index 0 lines up fine) previously
            # went undetected entirely.
            for item, ref_item in zip(items, ref):
                if not item or not ref_item:
                    continue
                if not _under_root(item, ref_item):
                    v.add("order-mismatch")

    return v


def run_path2_fixtures() -> None:
    data = load_json(os.path.join(ROOT, "source-paths-v2", "fixtures.json"))
    plural_vars = data["plural_vars"]
    singular_aliases = data["singular_aliases"]
    for case in data["cases"]:
        violations = validate_path2_env(case["env"], plural_vars, singular_aliases)
        if case["valid"]:
            if violations:
                fail(f"source-paths-v2/{case['name']}: expected valid, got {sorted(violations)}")
            else:
                ok(f"source-paths-v2/{case['name']}")
        else:
            expected = case["reason"]
            if expected not in violations:
                fail(f"source-paths-v2/{case['name']}: expected reason {expected!r}, got {sorted(violations)}")
            else:
                ok(f"source-paths-v2/{case['name']} -> {expected}")


# ---------------------------------------------------------------------------
# standalone-ra-account-v1 fixtures
# ---------------------------------------------------------------------------

RA_ACCOUNT_STATES = ("configured", "never-configured", "signed-out", "invalid", "unreadable")
# The revision is a positive decimal. 2^62 leaves headroom for an int64
# consumer while staying comfortably below any wrap concern; the producer
# fails a save that would pass it instead of emitting a wrapped value.
RA_ACCOUNT_REVISION_MAX = 4611686018427387904  # 2**62
RA_ACCOUNT_USERNAME_MAX_BYTES = 63
RA_ACCOUNT_PASSWORD_MAX_BYTES = 127


def classify_ra_account_env(env: dict[str, bytes]) -> tuple[str, set[str]]:
    """Classify a child-only RA account handoff. Returns
    ("unmanaged" | "valid-handoff" | "invalid-handoff", reasons).

    Case variables arrive as BYTES: the limits are UTF-8 byte counts, and a
    value that is not valid UTF-8 at all must be rejected, never decoded
    with a lossy fallback. Byte-exact fixtures use env_b64 so malformed
    encodings can be expressed where JSON strings cannot."""
    v: set[str] = set()
    prefix = "UMRK_RA_ACCOUNT_"

    if "JAWAKA_CHEEVOS_USERNAME" in env or "JAWAKA_CHEEVOS_PASSWORD" in env:
        # The RetroArch per-launch handoff has no business in a standalone
        # child; its presence means an inherited value survived fork().
        v.add("stale-retroarch-credentials")

    if not any(k.startswith(prefix) for k in env):
        # Total absence is "no supported handoff", the unmanaged case.
        return ("unmanaged", v)

    if env.get("UMRK_RA_ACCOUNT_VERSION") != b"1":
        v.add("unsupported-version")
        return ("invalid-handoff", v)

    state_b = env.get("UMRK_RA_ACCOUNT_STATE")
    if state_b is None:
        v.add("missing-account-state")
        return ("invalid-handoff", v)
    try:
        state = state_b.decode("ascii")
    except UnicodeDecodeError:
        state = ""
    if state not in RA_ACCOUNT_STATES:
        v.add("unknown-account-state")
        return ("invalid-handoff", v)

    user = env.get("UMRK_RA_ACCOUNT_USERNAME")
    passwd = env.get("UMRK_RA_ACCOUNT_PASSWORD")
    if state != "configured" and (user is not None or passwd is not None):
        v.add("credentials-unexpected")

    if state == "configured":
        if user is None:
            v.add("username-missing")
        if passwd is None:
            v.add("password-missing")
        for value, what, limit in (
            (user, "username", RA_ACCOUNT_USERNAME_MAX_BYTES),
            (passwd, "password", RA_ACCOUNT_PASSWORD_MAX_BYTES),
        ):
            if value is None:
                continue
            if value == b"":
                v.add("credential-empty")
            if len(value) > limit:
                v.add(f"{what}-oversized")
            if b"\x00" in value or b"\r" in value or b"\n" in value:
                v.add("credential-control-character")
            try:
                value.decode("utf-8")
            except UnicodeDecodeError:
                v.add("credential-invalid-utf8")

    revision = env.get("UMRK_RA_ACCOUNT_REVISION")
    if state in ("configured", "signed-out"):
        if revision is None:
            v.add("revision-missing")
        else:
            try:
                rev_text = revision.decode("ascii")
            except UnicodeDecodeError:
                rev_text = ""
            if not rev_text.isascii() or not rev_text.isdigit() or not (1 <= int(rev_text or "0") <= RA_ACCOUNT_REVISION_MAX):
                # A non-positive value is never meaningful; an out-of-range
                # positive one is the overflow case the producer must refuse
                # to write. Distinguish them for the one-fixture-per-rule
                # discipline.
                if rev_text.isdigit() and int(rev_text) > RA_ACCOUNT_REVISION_MAX:
                    v.add("revision-overflow")
                else:
                    v.add("revision-invalid")
    elif revision is not None:
        v.add("revision-unexpected")

    return ("valid-handoff" if not v else "invalid-handoff", v)


def run_ra_account_fixtures() -> None:
    import base64

    data = load_json(os.path.join(ROOT, "standalone-ra-account-v1", "fixtures.json"))
    for case in data["cases"]:
        env: dict[str, bytes] = {k: s.encode("utf-8") for k, s in case.get("env", {}).items()}
        for k, s in case.get("env_b64", {}).items():
            env[k] = base64.b64decode(s)
        kind, violations = classify_ra_account_env(env)
        expected_kind = case["kind"]
        if kind != expected_kind:
            fail(f"standalone-ra-account-v1/{case['name']}: classified {kind!r} "
                 f"(violations {sorted(violations)}), expected {expected_kind!r}")
            continue
        if expected_kind == "invalid-handoff":
            expected = case["reason"]
            if expected not in violations:
                fail(f"standalone-ra-account-v1/{case['name']}: expected reason "
                     f"{expected!r}, got violations {sorted(violations)}")
                continue
        elif violations:
            fail(f"standalone-ra-account-v1/{case['name']}: expected no violations, "
                 f"got {sorted(violations)}")
            continue
        suffix = f" -> {case['reason']}" if expected_kind == "invalid-handoff" else ""
        ok(f"standalone-ra-account-v1/{case['name']}{suffix}")

    # The normative page lists every rejection reason in one table; each one
    # must be proved by a fixture, and no fixture may name an undocumented one.
    import re

    doc_path = os.path.join(os.path.dirname(os.path.dirname(ROOT)), "docs", "standalone-ra-account.md")
    with open(doc_path, encoding="utf-8") as handle:
        doc = handle.read()
    section = doc.split("## Rejection reasons", 1)[1].split("\n## ", 1)[0]
    documented = set(re.findall(r"^\| `([a-z0-9-]+)` \|", section, re.MULTILINE))
    proved = {c["reason"] for c in data["cases"] if c["kind"] == "invalid-handoff"}
    if documented != proved:
        fail("standalone-ra-account-v1: docs/standalone-ra-account.md reasons "
             f"{sorted(documented - proved)} have no fixture; fixture reasons "
             f"{sorted(proved - documented)} are undocumented")
    else:
        ok(f"standalone-ra-account-v1: all {len(documented)} documented reasons have a fixture")


# ---------------------------------------------------------------------------
# LIFE-1 subscription fixtures
# ---------------------------------------------------------------------------

ACK_MS_CEILING = 1000
WAIT_MS_CEILING = 15000

MEMBERSHIP_TO_REJECTION = {
    "stale-generation": "stale-generation-peer",
    "different-service": "wrong-group-peer",
    "foreground-app": "foreground-app-peer",
}


def decide_subscription(peer_credential: dict) -> tuple[bool, str | None]:
    """Derive accept/reject from peer_credential ALONE -- never from the
    fixture's own declared outcome. This is what makes the check
    meaningful: a case whose 'outcome' contradicts its own
    peer_credential.membership is caught here instead of trusted."""
    if not peer_credential.get("available"):
        return False, "missing-peer-credential"
    membership = peer_credential.get("membership")
    if membership == "matches-current-generation":
        return True, None
    if membership in MEMBERSHIP_TO_REJECTION:
        return False, MEMBERSHIP_TO_REJECTION[membership]
    raise ValueError(f"unknown peer_credential.membership {membership!r}")


def resolve_ack_ms(request: dict) -> int:
    return min(request["ack_ms"], ACK_MS_CEILING)


def resolve_wait_ms(request: dict) -> int:
    return min(request["wait_ms"], WAIT_MS_CEILING)


def run_subscription_fixtures() -> None:
    schema = load_json(os.path.join(ROOT, "game-coordination-v1.schema.json"))
    sub_schema = schema["definitions"]["subscribe_request"]
    data = load_json(os.path.join(ROOT, "game-coordination", "subscription-fixtures.json"))
    reasons_seen = set()
    for case in data["cases"]:
        name = case["name"]
        req_ok, err = minischema.is_valid(case["request"], sub_schema, root=schema)
        if not req_ok:
            fail(f"game-coordination/{name}: request payload not schema-valid: {err}")
            continue
        if "check_before_stop" in case["request"] and case["request"]["mode"] != "stop":
            fail(f"game-coordination/{name}: check_before_stop is valid only in stop mode")
            continue

        try:
            derived_accepted, derived_code = decide_subscription(case["peer_credential"])
        except ValueError as e:
            fail(f"game-coordination/{name}: {e}")
            continue

        outcome = case["outcome"]
        if derived_accepted != outcome["accepted"]:
            fail(f"game-coordination/{name}: peer_credential.membership derives "
                 f"accepted={derived_accepted}, but outcome.accepted={outcome['accepted']}")
            continue

        if not outcome["accepted"]:
            actual_code = outcome["response"]["error"]["code"]
            if actual_code != derived_code:
                fail(f"game-coordination/{name}: derived rejection code {derived_code!r} "
                     f"!= outcome's declared code {actual_code!r}")
                continue
            resp_ok, err = minischema.is_valid(outcome["response"], schema["definitions"]["subscribe_reject"], root=schema)
            if not resp_ok:
                fail(f"game-coordination/{name}: reject response not schema-valid: {err}")
                continue
            reasons_seen.add(actual_code)
        else:
            resp_ok, err = minischema.is_valid(outcome["response"], schema["definitions"]["subscribe_ack"], root=schema)
            if not resp_ok:
                fail(f"game-coordination/{name}: ack response not schema-valid: {err}")
                continue

        if "resolved_ack_ms" in case:
            resolved = resolve_ack_ms(case["request"])
            if resolved != case["resolved_ack_ms"]:
                fail(f"game-coordination/{name}: resolved ack_ms {resolved} != expected {case['resolved_ack_ms']}")
                continue
        if "resolved_wait_ms" in case:
            resolved = resolve_wait_ms(case["request"])
            if resolved != case["resolved_wait_ms"]:
                fail(f"game-coordination/{name}: resolved wait_ms {resolved} != expected {case['resolved_wait_ms']}")
                continue

        ok(f"game-coordination/{name}")

    expected_reasons = {"stale-generation-peer", "wrong-group-peer", "foreground-app-peer", "missing-peer-credential"}
    missing = expected_reasons - reasons_seen
    if missing:
        fail(f"game-coordination subscription fixtures: missing rejection reasons {sorted(missing)}")
    else:
        ok("game-coordination subscription fixtures cover all rejection reasons")


# ---------------------------------------------------------------------------
# Wire fixtures: frame round-trip + schema validation where applicable
# ---------------------------------------------------------------------------

WIRE_SCHEMA_MAP = {
    "life1_subscribe_request": ("game-coordination-v1.schema.json", "subscribe_request"),
    "life1_subscribe_check_request": ("game-coordination-v1.schema.json", "subscribe_request"),
    "life1_subscribe_ack": ("game-coordination-v1.schema.json", "subscribe_ack"),
    "life1_subscribe_reject_stale_generation": ("game-coordination-v1.schema.json", "subscribe_reject"),
    "life1_game_start_event": ("game-coordination-v1.schema.json", "game_start_event"),
    "life1_game_check_event": ("game-coordination-v1.schema.json", "game_check_event"),
    "life1_waiting_status": ("game-coordination-v1.schema.json", "waiting_status"),
    "life1_check_waiting_status": ("game-coordination-v1.schema.json", "check_waiting_status"),
    "life1_ready_status": ("game-coordination-v1.schema.json", "ready_status"),
    "life1_stop_status": ("game-coordination-v1.schema.json", "stop_status"),
    "life1_error_status": ("game-coordination-v1.schema.json", "error_status"),
    "life1_game_cancel_event": ("game-coordination-v1.schema.json", "game_cancel_event"),
    "life1_game_abort_event": ("game-coordination-v1.schema.json", "game_abort_event"),
    "life1_game_finish_event": ("game-coordination-v1.schema.json", "game_finish_event"),
    "life1_game_state_request": ("game-coordination-v1.schema.json", "game_state_request"),
    "life1_game_state_response_active": ("game-coordination-v1.schema.json", "game_state_response_active"),
    "life1_game_state_response_inactive": ("game-coordination-v1.schema.json", "game_state_response_inactive"),
    "ctl1_list_response": ("control-ipc-v1.schema.json", "list_response"),
    "ctl1_status_response": ("control-ipc-v1.schema.json", "status_response"),
    "ctl1_ack_response": ("control-ipc-v1.schema.json", "ack_response"),
    "ctl1_error_unsupported_version": ("control-ipc-v1.schema.json", "error_response"),
    "ctl1_capabilities_response": ("control-ipc-v1.schema.json", "capabilities_response"),
    "ctl1_list_request": ("control-ipc-v1.schema.json", "request"),
    "ctl1_status_request": ("control-ipc-v1.schema.json", "request"),
    "ctl1_run_request": ("control-ipc-v1.schema.json", "request"),
    "ctl1_capabilities_request": ("control-ipc-v1.schema.json", "request"),
    "ctl1_logs_response_at_semantic_ceiling": ("control-ipc-v1.schema.json", "logs_response"),
    "ctl1_logs_response_over_semantic_ceiling": ("control-ipc-v1.schema.json", "logs_response"),
    "life1_error_status_unicode": ("game-coordination-v1.schema.json", "error_status"),
}

# contracts.md: "LIFE-1 and CTL-1 add a semantic payload ceiling of 64 KiB
# inside that transport frame; a larger payload is a protocol error." Used
# as the fallback when a fixture's own .json doesn't carry
# semantic_ceiling_bytes (older fixtures generated before this check existed).
SEMANTIC_CEILING_BYTES_DEFAULT = 65536

_schema_cache: dict[str, dict] = {}


def run_wire_fixtures() -> None:
    wdir = os.path.join(ROOT, "wire-fixtures")
    manifest = load_json(os.path.join(wdir, "MANIFEST.json"))

    for entry in manifest["fixtures"]:
        name = entry["name"]
        bin_path = os.path.join(wdir, f"{name}.bin")
        json_path = os.path.join(wdir, f"{name}.json")
        with open(bin_path, "rb") as f:
            frame_bytes = f.read()
        meta = load_json(json_path)

        if len(frame_bytes) < 4:
            fail(f"wire-fixtures/{name}: frame shorter than length prefix")
            continue
        (length,) = struct.unpack(">I", frame_bytes[:4])
        payload_bytes = frame_bytes[4:]
        if length != len(payload_bytes):
            fail(f"wire-fixtures/{name}: length prefix {length} != actual payload bytes {len(payload_bytes)}")
            continue
        if length != meta["length_prefix_value"]:
            fail(f"wire-fixtures/{name}: length prefix {length} != recorded {meta['length_prefix_value']}")
            continue

        decoded = json.loads(payload_bytes.decode("utf-8"))
        if decoded != meta["message"]:
            fail(f"wire-fixtures/{name}: decoded payload != recorded message")
            continue

        re_encoded_payload = meta["canonical_payload_utf8"].encode("utf-8")
        re_encoded_frame = struct.pack(">I", len(re_encoded_payload)) + re_encoded_payload
        if re_encoded_frame != frame_bytes:
            fail(f"wire-fixtures/{name}: re-encoding canonical_payload_utf8 does not reproduce the original frame bytes")
            continue

        import hashlib
        actual_sha = hashlib.sha256(frame_bytes).hexdigest()
        if actual_sha != entry["sha256_of_frame"]:
            fail(f"wire-fixtures/{name}: sha256 mismatch (frame drifted from MANIFEST.json)")
            continue

        ceiling = meta.get("semantic_ceiling_bytes", SEMANTIC_CEILING_BYTES_DEFAULT)
        expect_violation = meta.get("expect_semantic_ceiling_violation", False)
        exceeds = length > ceiling
        if exceeds != expect_violation:
            fail(f"wire-fixtures/{name}: payload is {length} bytes (ceiling {ceiling}); "
                 f"exceeds={exceeds} but expect_semantic_ceiling_violation={expect_violation}")
            continue

        if name in WIRE_SCHEMA_MAP:
            schema_file, def_name = WIRE_SCHEMA_MAP[name]
            if schema_file not in _schema_cache:
                _schema_cache[schema_file] = load_json(os.path.join(ROOT, schema_file))
            schema_doc = _schema_cache[schema_file]
            sub_schema = schema_doc["definitions"][def_name]
            payload_ok, err = minischema.is_valid(decoded, sub_schema, root=schema_doc)
            if not payload_ok:
                fail(f"wire-fixtures/{name}: payload does not validate against {schema_file}#/definitions/{def_name}: {err}")
                continue

        ok(f"wire-fixtures/{name} (frame round-trip + sha256 + schema)")


# ---------------------------------------------------------------------------
# Supervisor-state / transaction ordering fixtures: structural sanity
# ---------------------------------------------------------------------------

def run_supervisor_state_fixtures() -> None:
    data = load_json(os.path.join(ROOT, "supervisor-state", "generation-lease.json"))
    required_categories = {
        "clean-acquisition",
        "old-generation-lock-contention",
        "acquisition-after-last-old-holder-exits",
        "stale-ownership-record-report-only",
    }
    seen = {s["category"] for s in data["scenarios"]}
    missing = required_categories - seen
    if missing:
        fail(f"supervisor-state/generation-lease.json: missing categories {sorted(missing)}")
    else:
        ok("supervisor-state/generation-lease.json covers required categories")
    for s in data["scenarios"]:
        for key in ("name", "category", "precondition", "action", "outcome", "signalling_permitted"):
            if key not in s:
                fail(f"supervisor-state/generation-lease.json: scenario {s.get('name', '?')} missing key {key}")


def decide_p1_recovery(case: dict) -> str:
    """Derive the recovery_class FROM on_disk + install_record alone, per
    contracts.md: the install record's commit_token is the discriminator.
    A match means the live tree already IS the tree the record names (any
    stray staged/moved-aside tree is discarded, but nothing is rolled
    forward or back). A mismatch means the transaction never reached its
    commit point, so recovery restores the moved-aside tree the record
    still names and discards whatever was promoted after it. This function
    -- not the case's own prose fields -- is what run_transaction_fixtures()
    checks recovery_class against, so a case that asserts the wrong
    direction is caught instead of trusted."""
    on_disk = case["on_disk"]
    install_token = case["install_record"]["commit_token"]
    live_token = on_disk.get("live_tree_commit_token")
    moved_aside = on_disk.get("moved_aside_tree")
    staged = on_disk.get("staged_tree")
    if live_token == install_token:
        return "discard-staged" if (staged or moved_aside) else "none"
    if moved_aside:
        return "rollback-to-moved-aside"
    raise ValueError(f"case {case['name']!r}: live tree token {live_token!r} mismatches "
                      f"install record {install_token!r} with no moved-aside tree to roll back to")


def run_transaction_fixtures() -> None:
    p1 = load_json(os.path.join(ROOT, "transactions", "p1-promote-recovery.json"))
    if not any(c["name"] == "same-version-repair" for c in p1["cases"]):
        fail("transactions/p1-promote-recovery.json: missing same-version-repair case")
    else:
        ok("transactions/p1-promote-recovery.json includes same-version repair")
    for c in p1["cases"]:
        for key in ("on_disk", "install_record", "crash_point", "recovery_class", "recovery_action", "resulting_state"):
            if key not in c:
                fail(f"transactions/p1-promote-recovery.json: case {c.get('name', '?')} missing key {key}")
                continue
        try:
            derived = decide_p1_recovery(c)
        except ValueError as e:
            fail(f"transactions/p1-promote-recovery.json: {e}")
            continue
        if derived != c["recovery_class"]:
            fail(f"transactions/p1-promote-recovery.json: case {c['name']} declares "
                 f"recovery_class {c['recovery_class']!r} but on_disk/install_record derive {derived!r}")
        else:
            ok(f"transactions/p1-promote-recovery.json case {c['name']} -> {derived}")

    for fname, required_order in (
        ("txn1-ordering.json", list(range(1, 8))),
        ("pkg1-quiesce-ordering.json", list(range(1, 6))),
    ):
        doc = load_json(os.path.join(ROOT, "transactions", fname))
        steps = sorted(s["step"] for s in doc["steps"])
        if steps != required_order:
            fail(f"transactions/{fname}: steps {steps} != expected {required_order}")
            continue
        ok_so_far = True
        for s in doc["steps"]:
            if "if_crash_here" not in s:
                fail(f"transactions/{fname}: step {s['step']} missing if_crash_here")
                ok_so_far = False
            if fname == "txn1-ordering.json":
                # txn1-ordering.json is genuinely two directions (update,
                # uninstall) sharing one step list. A step whose behavior
                # differs by direction must say so explicitly via
                # action_by_direction rather than one prose "action" string
                # that then has to be reconciled against contradicting
                # recovery text -- that mismatch (step 4/7 describing both
                # directions in one action string while the recovery prose
                # said "update does not revoke") is exactly what let the
                # update/uninstall conflation ship silently.
                applies_to = s.get("applies_to")
                if not applies_to:
                    fail(f"transactions/{fname}: step {s['step']} missing applies_to")
                    ok_so_far = False
                    continue
                has_single_action = "action" in s
                has_direction_action = "action_by_direction" in s
                if has_single_action == has_direction_action:
                    fail(f"transactions/{fname}: step {s['step']} must have exactly one "
                         f"of 'action' or 'action_by_direction', not {has_single_action} and {has_direction_action}")
                    ok_so_far = False
                    continue
                if has_direction_action:
                    keys = set(s["action_by_direction"])
                    if keys != set(applies_to):
                        fail(f"transactions/{fname}: step {s['step']} action_by_direction keys "
                             f"{sorted(keys)} != applies_to {sorted(applies_to)}")
                        ok_so_far = False
                elif set(applies_to) != {"update", "uninstall"} and "skipped_for" not in s:
                    fail(f"transactions/{fname}: step {s['step']} applies_to {applies_to} is "
                         f"narrower than both directions but has no skipped_for explaining the other")
                    ok_so_far = False
        if ok_so_far:
            ok(f"transactions/{fname} steps well-formed")

    pair_dir = os.path.join(ROOT, "transactions", "floor-real-pairs")
    pairs = sorted(f for f in os.listdir(pair_dir) if f.endswith(".json"))
    scenarios = set()
    for fname in pairs:
        doc = load_json(os.path.join(pair_dir, fname))
        scenarios.add(doc["scenario"])
        if "installed" not in doc or "candidate" not in doc or "expected_outcome" not in doc:
            fail(f"transactions/floor-real-pairs/{fname}: missing installed/candidate/expected_outcome")
    if {"floor-to-real", "real-to-floor"} - scenarios:
        fail(f"transactions/floor-real-pairs: missing scenarios, have {sorted(scenarios)}")
    else:
        ok("transactions/floor-real-pairs covers both directions")


def run_decision_table_fixture() -> None:
    doc = load_json(os.path.join(ROOT, "game-coordination", "decision-table.json"))
    rows = doc["rows"]
    orders = [r["order"] for r in rows]
    if orders != sorted(orders) or orders != list(range(1, len(rows) + 1)):
        fail(f"game-coordination/decision-table.json: orders not sequential from 1: {orders}")
    glossary = set(doc["actions_glossary"])
    for r in rows:
        for key in ("condition", "action", "note"):
            if key not in r:
                fail(f"game-coordination/decision-table.json: row {r.get('order')} missing {key}")
        if r.get("action") not in glossary:
            fail(f"game-coordination/decision-table.json: row {r.get('order')} action {r.get('action')!r} not in actions_glossary")
    if not FAILURES or all("decision-table" not in f for f in FAILURES):
        ok(f"game-coordination/decision-table.json ({len(rows)} rows, in order)")


def run_redaction_fixture() -> None:
    schema = load_json(os.path.join(ROOT, "control-ipc-v1.schema.json"))
    logs_schema = schema["definitions"]["logs_response"]
    doc = load_json(os.path.join(ROOT, "wire-fixtures", "ctl1_logs_redaction_case.json"))
    for c in doc["cases"]:
        for key in ("raw_line", "redacted_line"):
            if key not in c:
                fail(f"wire-fixtures/ctl1_logs_redaction_case.json: case {c.get('name', '?')} missing {key}")
    secret_kinds = {"api-key", "bearer-token", "cookie", "pin", "password", "password-hash", "private-key", "shadow-entry"}
    seen = {c["name"].rsplit("-in-log-line", 1)[0] for c in doc["cases"] if c["name"] != "no-secret-present"}
    missing = secret_kinds - seen
    if missing:
        fail(f"wire-fixtures/ctl1_logs_redaction_case.json: missing secret kinds {sorted(missing)}")
    resp_ok, err = minischema.is_valid(doc["sample_logs_response"], logs_schema, root=schema)
    if not resp_ok:
        fail(f"wire-fixtures/ctl1_logs_redaction_case.json: sample_logs_response invalid: {err}")
    if not missing and resp_ok:
        ok("wire-fixtures/ctl1_logs_redaction_case.json covers every redaction category")


def main() -> None:
    run_manifest_fixtures()
    run_path2_fixtures()
    run_ra_account_fixtures()
    run_subscription_fixtures()
    run_wire_fixtures()
    run_supervisor_state_fixtures()
    run_transaction_fixtures()
    run_decision_table_fixture()
    run_redaction_fixture()

    print()
    if FAILURES:
        print(f"{len(FAILURES)} FAILURE(S)")
        sys.exit(1)
    print("All fixtures validated.")


if __name__ == "__main__":
    main()
