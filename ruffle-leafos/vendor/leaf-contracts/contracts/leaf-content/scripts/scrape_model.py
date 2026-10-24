"""Reference validation for the optional CONTENT-SCRAPE-1 companion block."""
from __future__ import annotations

import copy

from content_model import ROM_EXT_RE, SYSTEM_ID_RE

BLOCK_KEYS = {"schema", "systems"}
SYSTEM_KEYS = {"id", "name_source", "lookup_extension"}


def validate(pak: dict) -> set[str]:
    """Return companion reason slugs. Missing metadata is valid and inert."""
    block = pak.get("content_scrape") if isinstance(pak, dict) else None
    if block is None:
        return set()
    if not isinstance(block, dict):
        return {"malformed-content-scrape"}

    violations: set[str] = set()
    if set(block) - BLOCK_KEYS:
        violations.add("unknown-content-scrape-field")
    schema = block.get("schema")
    if isinstance(schema, bool) or schema != 1:
        violations.add("unknown-content-scrape-schema")
    rows = block.get("systems")
    if not isinstance(rows, list) or not 1 <= len(rows) <= 32:
        violations.add("malformed-content-scrape-systems")
        return violations

    provided = {
        row.get("id"): row
        for row in pak.get("provides", {}).get("systems", [])
        if isinstance(row, dict) and isinstance(row.get("id"), str)
    }
    seen: set[str] = set()
    for row in rows:
        if not isinstance(row, dict):
            violations.add("malformed-content-scrape-system")
            continue
        if set(row) - SYSTEM_KEYS:
            violations.add("unknown-content-scrape-field")

        system_id = row.get("id")
        if not isinstance(system_id, str) or not SYSTEM_ID_RE.fullmatch(system_id):
            violations.add("malformed-content-scrape-system-id")
        elif system_id in seen:
            violations.add("duplicate-content-scrape-system")
        else:
            seen.add(system_id)
            target = provided.get(system_id)
            if target is None:
                violations.add("unknown-content-scrape-system")

        if row.get("name_source") != "descriptor":
            violations.add("unknown-content-scrape-name-source")

        extension = row.get("lookup_extension")
        if not isinstance(extension, str) or not ROM_EXT_RE.fullmatch(extension):
            violations.add("malformed-content-scrape-lookup-extension")
        elif isinstance(system_id, str) and system_id in provided:
            if extension not in provided[system_id].get("extensions", []):
                violations.add("undeclared-content-scrape-extension")

    return violations


def decorate(systems: list[dict], contributors: list[dict]) \
        -> tuple[list[dict], list[str]]:
    """Apply valid policies after CONTENT-1 merge.

    Returns the decorated system rows and providers whose pak.json must be
    fingerprinted because their companion metadata affected output.
    """
    output = copy.deepcopy(systems)
    fingerprinted: set[str] = set()
    for contributor in contributors:
        provider = contributor.get("provider")
        pak = contributor.get("pak")
        if not isinstance(provider, str) or not isinstance(pak, dict) or validate(pak):
            continue
        block = pak.get("content_scrape")
        if not isinstance(block, dict):
            continue
        affected = False
        for policy in block["systems"]:
            for system in output:
                if (system.get("id") == policy["id"] and
                        system.get("provider") == provider):
                    system["screenscraper_name_source"] = policy["name_source"]
                    system["screenscraper_lookup_extension"] = \
                        policy["lookup_extension"]
                    affected = True
                    break
        if affected:
            fingerprinted.add(provider)
    return output, sorted(fingerprinted)
