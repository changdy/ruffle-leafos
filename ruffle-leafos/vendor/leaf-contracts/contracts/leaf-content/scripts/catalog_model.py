"""Reference implementation of CAT-1: selector resolution, the two
validation levels, and the publication protocol.

The point of writing this out is that three processes have to agree --
`jawakad` (the producer), the Jawaka catalog readers, and CentralScrutinizer
-- and the failure mode when they don't is invisible: a user silently pinned
to a pre-OTA system list. Every path here therefore ends in an explicit
outcome with an explicit reason, and there is no "serve what we had" branch
anywhere.
"""
from __future__ import annotations

import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import canonical  # noqa: E402

GENERATION_OUTPUTS = ("systems.json", "cores.json", "info")


def resolve(state: dict) -> dict:
    """Structural validation -- what a READER does, per catalog load.

    Deliberately does NOT rehash contributor binaries: rehashing
    multi-megabyte cores on FAT32 would turn a metadata request into
    compiler work (T-1). Freshness is the producer's job.

    Returns {"resolution", "generation", "reason"} where resolution is
    "generation", "release-defaults", or "catalog-error".
    """
    def fallback(reason: str) -> dict:
        # There is no third option. Serving a previously cached generation
        # after validation fails is exactly the stale-catalog outcome the
        # contract exists to prevent.
        if state.get("release_defaults_valid", True):
            return {"resolution": "release-defaults", "generation": None,
                    "reason": reason}
        return {"resolution": "catalog-error", "generation": None,
                "reason": "release-defaults-invalid"}

    installed_release_id = state.get("installed_release_id")
    if not installed_release_id:
        return fallback("release-identity-unavailable")

    raw = state.get("current")
    if raw is None:
        return fallback("selector-missing")

    name = canonical.parse_selector(raw)
    if name is None:
        return fallback("selector-malformed")

    generation = state.get("generations", {}).get(name)
    if generation is None or not generation.get("present", True):
        return fallback("generation-missing")

    stamp = generation.get("stamp")
    if not isinstance(stamp, dict):
        return fallback("stamp-missing")
    if stamp.get("schema") != 1:
        return fallback("stamp-schema-unsupported")
    if stamp.get("platform") != state.get("platform"):
        return fallback("stamp-platform-mismatch")
    if stamp.get("release_id") != installed_release_id:
        return fallback("stamp-release-mismatch")

    missing = [o for o in GENERATION_OUTPUTS
               if o not in generation.get("outputs", GENERATION_OUTPUTS)]
    if missing:
        return fallback("generation-missing")

    return {"resolution": "generation", "generation": name, "reason": None}


def validate_provenance(state: dict) -> dict:
    """Full provenance validation -- what the PRODUCER does at startup and
    before a ROM scan, and what nothing else ever does.

    Returns {"action", "reason", "invalidate_selector_first"}. The
    invalidation ordering matters: `current` is unlinked and the catalog
    directory fsynced BEFORE recompiling, so every new reader falls back to
    release defaults while the compile runs, and a crash mid-compile leaves
    the selector absent rather than pointing at something unverified.
    """
    structural = resolve(state)
    if structural["resolution"] != "generation":
        return {"action": "compile", "reason": structural["reason"],
                "invalidate_selector_first": structural["reason"] not in
                ("selector-missing",)}

    name = structural["generation"]
    stamp = state["generations"][name]["stamp"]
    on_disk = state.get("on_disk", {})

    if on_disk.get("base") is not None and on_disk["base"] != stamp["base"]:
        return {"action": "compile", "reason": "stamp-base-mismatch",
                "invalidate_selector_first": True}

    if on_disk.get("contributors") is not None:
        if on_disk["contributors"] != stamp["contributors"]:
            return {"action": "compile", "reason": "stamp-contributor-mismatch",
                    "invalidate_selector_first": True}

    return {"action": "none", "reason": None, "invalidate_selector_first": False}


def publish(existing_generations: dict, temp_stamp: dict,
            temp_outputs: dict) -> dict:
    """Content-addressed publication.

    `gen-<digest>` is derived from the stamp, which already contains every
    input hash and every output hash, so an identical recompile lands on the
    same name and is free. A name that exists with DIFFERENT bytes is
    corruption, not a collision to work around: the compile fails closed and
    the existing directory is never overwritten.
    """
    name = canonical.generation_name(temp_stamp)
    existing = existing_generations.get(name)
    if existing is None:
        return {"outcome": "published", "generation": name, "reason": None}

    same_stamp = (canonical.canonical_bytes(existing.get("stamp"))
                  == canonical.canonical_bytes(temp_stamp))
    same_outputs = existing.get("outputs_sha256") == temp_outputs
    if same_stamp and same_outputs:
        return {"outcome": "reused", "generation": name, "reason": None}
    return {"outcome": "conflict", "generation": name,
            "reason": "generation-digest-conflict"}


def cleanup(entries: list[str], current: str | None) -> dict:
    """V1 cleans abandoned temp directories only.

    No finalized-generation GC: a CentralScrutinizer process can outlive a
    `jawakad` crash, so even daemon startup is not a reader-free window, and
    "retain the previous generation" breaks after two swaps. Content
    addressing already keeps no-op recompiles from growing the directory.
    """
    selected = canonical.parse_selector(current) if current else None
    removed = [e for e in entries if e.startswith("tmp-")]
    kept = [e for e in entries if not e.startswith("tmp-")]
    return {"removed": sorted(removed), "kept": sorted(kept),
            "selected": selected}
