"""Reference implementation of STORE-CONTENT-1: the storefront `content[]`
lane and the rules that only make sense across both lanes.

Only the content-lane-specific rules live here. Everything a package shares
with `apps[]` -- version grammar, versions[] ordering, artifact completeness,
HTTPS -- is enforced by the existing generator and CI validator.
"""
from __future__ import annotations


def _versions(package: dict) -> list[dict]:
    return package.get("versions") or []


def validate(storefront: dict, artifacts: dict | None = None) -> set[str]:
    """Return every reason this storefront is rejected. Empty = accepted.

    `artifacts` maps package id -> {"declares_provides": bool}, standing in
    for what the generator learns by opening the built .pak.zip. The lane a
    package sits in is a claim; whether the artifact actually declares
    `provides` is the fact, and the generator checks the fact.
    """
    artifacts = artifacts or {}
    v: set[str] = set()

    apps = storefront.get("apps", []) or []
    content = storefront.get("content", []) or []

    app_ids = {a["id"] for a in apps}
    content_ids = {c["id"] for c in content}
    if app_ids & content_ids:
        # Both lanes resolve to the same install_path, so an id in both is a
        # duplicate-identity bug, not a way to serve two audiences.
        v.add("id-in-both-lanes")

    for package in content:
        info = artifacts.get(package["id"])
        if info is not None and not info.get("declares_provides", False):
            v.add("content-artifact-without-provides")
        for entry in package.get("packages", []):
            if entry.get("platform") == "shared":
                # Cores and standalone emulator binaries are
                # platform-specific; there is nothing a shared content pak
                # could correctly ship (D16).
                v.add("shared-platform-in-content")
            if entry.get("runtime") != "leaf":
                v.add("unknown-runtime")
            if ("min_leaf_version" not in entry or
                    any("min_leaf_version" not in ver
                        for ver in _versions(entry))):
                v.add("ungated-content-version")
            v |= _history_violations(entry, require_safe_floor=False)

    for package in apps:
        info = artifacts.get(package["id"])
        if info is not None and info.get("declares_provides", False):
            # A pak that declares `provides` is gated on this contract by
            # construction, so apps[] -- the gate-unaware lane -- is the
            # wrong home for it.
            v.add("provides-artifact-in-apps-lane")
        for entry in package.get("packages", []):
            v |= _history_violations(entry, require_safe_floor=True)

    return v


def _history_violations(entry: dict, require_safe_floor: bool) -> set[str]:
    v: set[str] = set()
    versions = _versions(entry)
    if not versions:
        return v

    if require_safe_floor:
        # The safe floor protects gate-unaware clients. It is exempted inside
        # content[] because every content package is gated by construction --
        # and because that lane is invisible to those clients anyway.
        if not any("min_leaf_version" not in ver for ver in versions):
            v.add("missing-safe-floor")

    published = entry.get("published_history")
    if published:
        by_version = {ver["version"]: ver for ver in versions}
        for old in published:
            current = by_version.get(old["version"])
            if current is None or current != old:
                # Exact-version Restore depends on a published entry never
                # changing: version, gate, URL, name, archive kind, sizes,
                # and hash are all frozen once shipped.
                v.add("immutable-history-violated")
    return v


def visible_to_gate_unaware(storefront: dict) -> list[str]:
    """What a client that predates this contract sees: `apps[]` only.

    It parses the same document, ignores the unknown `content` key, and
    never learns a content pak exists. That is the whole reason the lane is
    a new key rather than a `kind` field on apps[].
    """
    return sorted(a["id"] for a in storefront.get("apps", []) or [])


def offers_open(installed_pak: dict) -> bool:
    """"Open" comes from the installed pak, never from the lane.

    A hybrid gets it, a pure content pak does not, and a sideloaded pak that
    is in no lane at all still gets the right answer.
    """
    return bool(installed_pak.get("launch_sh_executable", False))
