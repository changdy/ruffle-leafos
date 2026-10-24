"""Reference validation and post-merge decoration for CONTENT-ART-1 and CONTENT-ART-2."""
from __future__ import annotations

import copy
import os

from content_model import SYSTEM_ID_RE, check_path

PNG_SIGNATURE = b"\x89PNG\r\n\x1a\n"
# Claim slots in decoration order. wordmark_color is an untinted wordmark and
# resolves independently of the tinted one.
SLOTS = ("wordmark", "wordmark_color", "grid_icon")


def validate(pak: dict, pak_dir: str, *, max_schema: int = 2) -> set[str]:
    """Companion errors never participate in CONTENT-1 acceptance."""
    if "content_art" not in pak:
        return set()
    block = pak["content_art"]
    if not isinstance(block, dict):
        return {"malformed-content-art"}
    errors: set[str] = set()
    if set(block) - {"schema", "systems"}:
        errors.add("unknown-content-art-field")
    if type(block.get("schema")) not in (int, float) or block["schema"] not in range(1, max_schema + 1):
        return errors | {"unknown-content-art-schema"}
    rows = block.get("systems")
    if not isinstance(rows, list) or not 1 <= len(rows) <= 32:
        return errors | {"malformed-content-art-systems"}
    slots = SLOTS if block.get("schema") == 2 else ("wordmark",)
    seen: set[str] = set()
    for row in rows:
        if not isinstance(row, dict):
            errors.add("malformed-content-art-system")
            continue
        if set(row) - {"id", *slots}:
            errors.add("unknown-content-art-field")
        system_id = row.get("id")
        if not isinstance(system_id, str) or not SYSTEM_ID_RE.fullmatch(system_id):
            errors.add("malformed-content-art-system-id")
        elif system_id in seen:
            errors.add("duplicate-content-art-system")
        else:
            seen.add(system_id)
        if block.get("schema") == 2 and not any(slot in row for slot in slots):
            errors.add("missing-content-art-image")
        for slot in slots:
            if slot not in row and block.get("schema") == 2:
                continue
            rel = row.get(slot)
            if not isinstance(rel, str) or not 1 <= len(rel) <= 4096 or "\0" in rel:
                errors.add("malformed-content-art-" + slot.replace("_", "-"))
                continue
            path_errors = check_path(rel, pak_dir, require_executable=False)
            if path_errors:
                errors.update("content-art-" + reason for reason in path_errors)
                continue
            try:
                with open(os.path.join(pak_dir, rel), "rb") as image:
                    header = image.read(24)
                if not rel.lower().endswith(".png") or header[:8] != PNG_SIGNATURE \
                        or len(header) != 24 or header[8:16] != b"\0\0\0\rIHDR":
                    errors.add("unsupported-content-art-image")
                elif not all(1 <= int.from_bytes(header[i:i+4], "big") <= 1024 for i in (16, 20)):
                    errors.add("invalid-content-art-grid-dimensions" if slot == "grid_icon"
                               else "invalid-content-art-wordmark-dimensions")
            except OSError:
                errors.add("unreadable-content-art-image")
    return errors


def decorate(catalog: dict, contributors: list[dict]) \
        -> tuple[dict, dict[str, list[str]], list[dict]]:
    """Decorate a fresh CONTENT-1 merge using accepted contributors only.

    Each contributor carries provider, pak, and pak_dir (the live install root).
    Return a copied catalog, additional CAT-1 file paths per provider, and
    companion diagnostics. Callers hash files using CAT-1's existing machinery.
    """
    output = copy.deepcopy(catalog)
    systems = {s["id"]: s for s in output["systems"]}
    cores = {c["id"]: c for c in output["cores"]}
    claims: dict[tuple[str, str], list[tuple[str, str]]] = {}
    diagnostics: list[dict] = []

    def record(provider, reason, detail):
        diagnostics.append({"provider": provider, "reason": reason, "detail": detail})

    for contributor in contributors:
        provider, pak = contributor["provider"], contributor["pak"]
        errors = validate(pak, contributor["pak_dir"])
        if errors:
            for reason in sorted(errors):
                record(provider, reason, "content_art ignored")
            continue
        for entry in pak.get("content_art", {}).get("systems", []):
            system_id = entry["id"]
            system = systems.get(system_id)
            eligible = system is not None and system.get("provider") == provider
            if system is not None and not system.get("provider"):
                eligible = any(
                    ext["system_id"] == system_id and any(
                        cores.get(core_id, {}).get("provider") == provider
                        and core_id in system.get("alternate_cores", [])
                        for core_id in ext["add_alternate_cores"])
                    for ext in pak["provides"].get("system_extensions", []))
            if eligible:
                for slot in SLOTS:
                    if slot in entry:
                        claims.setdefault((system_id, slot), []).append((provider, entry[slot]))
            else:
                record(provider, "ineligible-content-art-system", system_id)

    files: dict[str, set[str]] = {}
    for (system_id, slot), candidates in sorted(claims.items()):
        if len(candidates) > 1:
            for provider, _ in candidates:
                record(provider, "conflicting-content-art-system",
                       system_id if slot == "wordmark" else f"{system_id}:{slot}")
            continue
        provider, rel = candidates[0]
        systems[system_id][slot] = rel
        systems[system_id][slot + "_provider"] = provider
        files.setdefault(provider, set()).update(("pak.json", rel))
    diagnostics.sort(key=lambda d: (d["provider"], d["reason"], d["detail"]))
    return output, {p: sorted(paths) for p, paths in sorted(files.items())}, diagnostics
