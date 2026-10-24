"""Reference implementation of CONTENT-1 manifest validation and the
CONTENT-1 merge policy.

This is the executable half of
docs/content-paks.md. Jawaka's C implementation
(`jw_content_manifest_validate`, `jw_catalog_compile`) must agree with it
reason for reason; that agreement is what the fixture tree checks.

Every function here returns *reason slugs*, never booleans, because the
contract's promise is that a developer can find out WHY their pak did
nothing -- from diagnostics.json, from the rescan log, and from the Pak Rat
UI. A validator that only answers yes/no cannot back that promise.
"""
from __future__ import annotations

import os
import re

# --------------------------------------------------------------------------
# Shape helpers
# --------------------------------------------------------------------------

SYSTEM_ID_RE = re.compile(r"^[A-Z0-9_]{2,32}$")
CORE_ID_RE = re.compile(r"^[a-z0-9_]{2,64}$")
ROM_EXT_RE = re.compile(r"^[a-z0-9_]{1,16}$")
ARCHIVE_EXT_RE = re.compile(r"^[a-z0-9]{1,8}$")
ROM_ROOT_RE = re.compile(r"^Roms/[A-Za-z0-9_-]{1,32}$")
IMAGE_ROOT_RE = re.compile(r"^Images/[A-Za-z0-9_-]{1,32}$")
BIOS_DIR_RE = re.compile(r"^[A-Za-z0-9_-]{1,32}$")

ARCHIVE_MODES = {"pass_through", "extract", "none"}
M3U_MODES = {"none", "auto", "manual"}
CORE_TYPES = {"retroarch", "path"}
FRONTEND_PROTOCOLS = {"leaf-ra-1"}

FORBIDDEN_FIELDS = {
    "requires_direct_drm": "forbidden-requires-direct-drm",
    "legacy_flat_core": "forbidden-legacy-flat-core",
    "name_map": "forbidden-name-map",
    "status": "forbidden-status",
}

PROVIDES_KEYS = {"schema", "systems", "system_extensions", "cores"}

SYSTEM_KEYS = {
    "id", "name", "patterns", "extensions", "archive_extensions",
    "archive_inner_extensions", "archive_mode", "file_names",
    "ignore_file_names", "playlist_extensions", "m3u_generation",
    "default_core", "alternate_cores", "rom_root", "image_root",
    "bios_notes", "icon_flat", "icon_photographic",
    "screenscraper_platform_ids", "group", "bios_directory",
}

CORE_KEYS = {
    "id", "display_name", "type", "libretro_name", "file_name", "info_name",
    "config_folder", "frontend", "path", "supports_menu", "supports_savestate",
    "supports_disk_control", "needs_swap",
}

FRONTEND_KEYS = {"path", "protocol"}

EXTENSION_KEYS = {"system_id", "add_alternate_cores"}

CORE_BOOL_KEYS = (
    "supports_menu", "supports_savestate", "supports_disk_control", "needs_swap",
)

RETROARCH_ONLY = ("libretro_name", "file_name", "info_name", "config_folder",
                  "frontend")

MAX_ARRAY = 32


def is_int(value) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def is_str_list(value, item_re: re.Pattern | None = None,
                min_items: int = 0, max_items: int = 10**9,
                min_len: int = 1, max_len: int = 10**9) -> bool:
    if not isinstance(value, list):
        return False
    if not (min_items <= len(value) <= max_items):
        return False
    for item in value:
        if not isinstance(item, str):
            return False
        if not (min_len <= len(item) <= max_len):
            return False
        if item_re is not None and not item_re.match(item):
            return False
    return True


def core_folder_is_safe(folder) -> bool:
    """Port of jw_ra_core_folder_is_safe (Jawaka internal/retroarch/catalog.c).

    RetroArch uses this string verbatim as a FAT32 directory component under
    Saves/ and States/, so an unsafe value corrupts the save layout for the
    whole device, not just the offending pak. That is why the contract
    checks against this function rather than a convenient regex: the
    reserved DOS device names and the trailing-dot/space rules are not
    obvious, and getting them wrong is silent.
    """
    if not isinstance(folder, str) or not folder or folder in (".", ".."):
        return False
    if len(folder) > 255 or folder[-1] in (" ", "."):
        return False
    for ch in folder:
        if ord(ch) < 0x20 or ch in '/\\:*?"<>|':
            return False
    stem = folder.split(".", 1)[0]
    if 0 < len(stem) < 16:
        upper = stem.upper()
        if upper in ("CON", "PRN", "AUX", "NUL"):
            return False
        if len(upper) == 4 and upper[:3] in ("COM", "LPT") and upper[3] in "123456789":
            return False
    return True


# --------------------------------------------------------------------------
# Path rules -- SVC-1's run.path rule verbatim, same reason vocabulary
# --------------------------------------------------------------------------

def check_path(rel, pak_dir: str, require_executable: bool) -> set[str]:
    v: set[str] = set()
    if not isinstance(rel, str) or not rel:
        v.add("missing-file")
        return v
    if rel.startswith("/"):
        v.add("absolute-path")
        return v
    if ".." in rel.split("/"):
        v.add("path-traversal")
        return v

    full = os.path.join(pak_dir, rel)
    base = os.path.realpath(pak_dir)
    # realpath resolves EVERY component, not just the leaf. An intermediate
    # symlink (cores/ itself pointing outside the pak) escapes exactly as
    # much as a symlinked leaf does, and it resolves lexically even through
    # a dangling link, so this catches a link to a missing target without a
    # separate existence check first.
    resolved = os.path.realpath(full)
    if not (resolved == base or resolved.startswith(base + os.sep)):
        v.add("escaping-symlink")
        return v
    if not os.path.exists(resolved):
        v.add("missing-file")
    elif not os.path.isfile(resolved):
        v.add("non-regular-file")
    elif require_executable and not os.access(resolved, os.X_OK):
        v.add("not-executable")
    return v


def _scan_forbidden(node, v: set[str]) -> None:
    """Forbidden fields are rejected wherever they appear, at any depth.

    D15 lists them as fields of a system or core, but a manifest that buries
    `requires_direct_drm` one level deeper is making the same claim, and the
    compiler force-clears by key name regardless of nesting. Matching that
    here keeps validator and compiler from disagreeing.
    """
    if isinstance(node, dict):
        for key, value in node.items():
            if key in FORBIDDEN_FIELDS:
                v.add(FORBIDDEN_FIELDS[key])
            _scan_forbidden(value, v)
    elif isinstance(node, list):
        for item in node:
            _scan_forbidden(item, v)


# --------------------------------------------------------------------------
# CONTENT-1 manifest validation
# --------------------------------------------------------------------------

def validate_manifest(pak: dict, pak_dir: str, context: dict | None = None) -> set[str]:
    """Return every reason this `provides` block is refused. Empty = accepted.

    `context` carries what the manifest cannot state about itself:
      install_lane: "platform" (default) | "shared"
      source_id:    "primary" (default) | anything else
    """
    context = context or {}
    v: set[str] = set()

    if context.get("install_lane", "platform") == "shared":
        v.add("shared-content-unsupported")
    if context.get("source_id", "primary") != "primary":
        v.add("secondary-source-unsupported")

    provides = pak.get("provides")
    if not isinstance(provides, dict):
        v.add("unknown-field")
        return v

    if set(provides) - PROVIDES_KEYS:
        v.add("unknown-field")

    schema = provides.get("schema")
    if isinstance(schema, bool) or schema != 1:
        v.add("unknown-schema")

    _scan_forbidden(provides, v)

    systems = provides.get("systems", [])
    extensions = provides.get("system_extensions", [])
    cores = provides.get("cores", [])

    for value, reason in ((systems, "too-many-systems"),
                          (extensions, "too-many-system-extensions"),
                          (cores, "too-many-cores")):
        if not isinstance(value, list):
            v.add("unknown-field")
        elif len(value) > MAX_ARRAY:
            v.add(reason)

    if not any(isinstance(a, list) and a for a in (systems, extensions, cores)):
        v.add("empty-provides")

    for system in systems if isinstance(systems, list) else []:
        v |= _validate_system(system, pak_dir)
    for core in cores if isinstance(cores, list) else []:
        v |= _validate_core(core, pak_dir)
    for ext in extensions if isinstance(extensions, list) else []:
        v |= _validate_extension(ext)

    return v


def _validate_system(system, pak_dir: str) -> set[str]:
    v: set[str] = set()
    if not isinstance(system, dict):
        v.add("unknown-field")
        return v
    if set(system) - SYSTEM_KEYS - set(FORBIDDEN_FIELDS):
        v.add("unknown-field")

    if not isinstance(system.get("id"), str) or not SYSTEM_ID_RE.match(system.get("id", "")):
        v.add("malformed-system-id")
    if not is_str_list([system.get("name")], min_len=1, max_len=64):
        v.add("malformed-system-name")

    if not is_str_list(system.get("patterns"), min_items=1, max_items=32, max_len=64):
        v.add("malformed-patterns")
    if not is_str_list(system.get("extensions"), ROM_EXT_RE, min_items=1, max_items=64):
        v.add("malformed-extensions")

    if "archive_extensions" in system and not is_str_list(
            system["archive_extensions"], ARCHIVE_EXT_RE, max_items=8):
        v.add("malformed-archive-extensions")
    if "archive_inner_extensions" in system and not is_str_list(
            system["archive_inner_extensions"], ROM_EXT_RE, max_items=64):
        v.add("malformed-archive-inner-extensions")
    if "playlist_extensions" in system and not is_str_list(
            system["playlist_extensions"], ARCHIVE_EXT_RE, max_items=8):
        v.add("malformed-playlist-extensions")
    if "file_names" in system and not is_str_list(
            system["file_names"], max_items=32, max_len=128):
        v.add("malformed-file-names")
    if "ignore_file_names" in system and not is_str_list(
            system["ignore_file_names"], max_items=32, max_len=128):
        v.add("malformed-ignore-file-names")
    if "bios_notes" in system and not is_str_list(
            system["bios_notes"], max_items=8, max_len=256):
        v.add("malformed-bios-notes")

    if "archive_mode" in system and system["archive_mode"] not in ARCHIVE_MODES:
        v.add("unknown-archive-mode")
    if "m3u_generation" in system and system["m3u_generation"] not in M3U_MODES:
        v.add("unknown-m3u-generation")

    if not isinstance(system.get("rom_root"), str) or not ROM_ROOT_RE.match(system.get("rom_root", "")):
        v.add("malformed-rom-root")
    if not isinstance(system.get("image_root"), str) or not IMAGE_ROOT_RE.match(system.get("image_root", "")):
        v.add("malformed-image-root")

    default_core = system.get("default_core")
    if not isinstance(default_core, str) or not CORE_ID_RE.match(default_core):
        v.add("malformed-core-id")
    if "alternate_cores" in system and not is_str_list(
            system["alternate_cores"], CORE_ID_RE, max_items=16):
        v.add("malformed-core-id")

    # A default_core naming no core THIS manifest declares is deliberately
    # NOT an error here: it may name a release core, or one another pak
    # ships. It becomes unknown-default-core in merge(), where the full core
    # set is finally known.

    v |= check_path(system.get("icon_flat"), pak_dir, require_executable=False)
    if system.get("icon_photographic") is not None and "icon_photographic" in system:
        v |= check_path(system["icon_photographic"], pak_dir, require_executable=False)

    ids = system.get("screenscraper_platform_ids")
    if ids is not None and "screenscraper_platform_ids" in system:
        if (not isinstance(ids, list) or len(ids) > 8
                or not all(is_int(i) and 1 <= i <= 99999 for i in ids)):
            v.add("malformed-screenscraper-platform-ids")

    if "group" in system and system["group"] is not None:
        if not isinstance(system["group"], str) or not (1 <= len(system["group"]) <= 32):
            v.add("malformed-group")

    if "bios_directory" in system and system["bios_directory"] is not None:
        if (not isinstance(system["bios_directory"], str)
                or not BIOS_DIR_RE.match(system["bios_directory"])):
            v.add("malformed-bios-directory")

    return v


def _validate_core(core, pak_dir: str) -> set[str]:
    v: set[str] = set()
    if not isinstance(core, dict):
        v.add("unknown-field")
        return v
    if set(core) - CORE_KEYS - set(FORBIDDEN_FIELDS):
        v.add("unknown-field")

    if not isinstance(core.get("id"), str) or not CORE_ID_RE.match(core.get("id", "")):
        v.add("malformed-core-id")
    if not is_str_list([core.get("display_name")], min_len=1, max_len=64):
        v.add("malformed-core-display-name")

    for key in CORE_BOOL_KEYS:
        if key in core and not isinstance(core[key], bool):
            v.add("malformed-core-flag")

    core_type = core.get("type")
    if core_type not in CORE_TYPES:
        v.add("unknown-core-type")
        return v

    if core_type == "retroarch":
        if "path" in core:
            v.add("core-type-field-mismatch")
        libretro_name = core.get("libretro_name")
        if not isinstance(libretro_name, str) or not CORE_ID_RE.match(libretro_name):
            v.add("malformed-libretro-name")
        if "file_name" not in core:
            v.add("missing-core-file-name")
        else:
            v |= check_path(core["file_name"], pak_dir, require_executable=False)
        if "info_name" not in core:
            v.add("missing-core-info-name")
        else:
            v |= check_path(core["info_name"], pak_dir, require_executable=False)
        if "config_folder" not in core:
            v.add("missing-config-folder")
        elif not core_folder_is_safe(core["config_folder"]):
            v.add("unsafe-config-folder")
        if "frontend" in core:
            v |= _validate_frontend(core["frontend"], pak_dir)
    else:  # path
        if any(k in core for k in RETROARCH_ONLY):
            v.add("core-type-field-mismatch")
        if "path" not in core:
            v.add("missing-core-path")
        else:
            v |= check_path(core["path"], pak_dir, require_executable=True)

    return v


def _validate_frontend(frontend, pak_dir: str) -> set[str]:
    """A pak-supplied RetroArch frontend (LEAF-RA-1).

    The shape is checked before the filesystem, so a frontend that is not an
    object, or whose `path` is not a non-empty string, is `malformed-frontend`
    and never reaches check_path (which would call it a missing file). The
    protocol is a separate reason because it is the one field a newer pak can
    legitimately get "wrong": naming a protocol this reader does not speak.
    The path then follows the path-core rule exactly, executable bit included.
    """
    v: set[str] = set()
    if not isinstance(frontend, dict):
        v.add("malformed-frontend")
        return v
    if set(frontend) - FRONTEND_KEYS:
        v.add("malformed-frontend")

    path = frontend.get("path")
    if not isinstance(path, str) or not path:
        v.add("malformed-frontend")
    else:
        v |= check_path(path, pak_dir, require_executable=True)

    protocol = frontend.get("protocol")
    if not isinstance(protocol, str) or protocol not in FRONTEND_PROTOCOLS:
        v.add("unknown-frontend-protocol")
    return v


def _validate_extension(ext) -> set[str]:
    v: set[str] = set()
    if not isinstance(ext, dict):
        v.add("unknown-extension-field")
        return v
    if set(ext) - EXTENSION_KEYS:
        v.add("unknown-extension-field")
    if not isinstance(ext.get("system_id"), str) or not SYSTEM_ID_RE.match(ext.get("system_id", "")):
        v.add("malformed-system-id")
    cores = ext.get("add_alternate_cores")
    if isinstance(cores, list) and not cores:
        v.add("empty-add-alternate-cores")
    elif not is_str_list(cores, CORE_ID_RE, min_items=1, max_items=16):
        v.add("malformed-core-id")
    return v


def manifest_warnings(pak: dict) -> set[str]:
    """Warnings never refuse anything; they land in diagnostics.json."""
    w: set[str] = set()
    provides = pak.get("provides", {})
    for system in provides.get("systems", []) or []:
        if not isinstance(system, dict):
            continue
        patterns = system.get("patterns") or []
        if not isinstance(patterns, list):
            continue
        folded = [p.lower() for p in patterns if isinstance(p, str)]
        if len(folded) != len(set(folded)):
            w.add("redundant-case-variant")
    return w


def apps_listed(pak: dict, context: dict | None = None) -> bool:
    """"Open" is derived from the installed pak, never from `provides` and
    never from the storefront lane. A hybrid gets it, a pure content pak
    does not, and a sideloaded pak in no lane at all still gets the right
    answer."""
    context = context or {}
    return bool(context.get("launch_sh_executable", False))


# --------------------------------------------------------------------------
# CONTENT-1 merge policy (D9)
# --------------------------------------------------------------------------

NAMESPACE_FIELDS = ("system_id", "pattern", "rom_root", "image_root",
                    "core_id", "config_folder", "info_name")


def _fold(value: str) -> str:
    return value.casefold()


def _system_namespace(system: dict) -> set[tuple[str, str]]:
    """Unique ownership ACROSS contributions, not literal uniqueness within
    one entry: a system listing a value twice is harmless, two systems
    claiming the same value is the collision. Hence a set, built case-folded
    because runtime matching is already case-insensitive
    (jw_ra_string_list_contains_casefold)."""
    claims = {("system_id", _fold(system["id"]))}
    for pattern in system.get("patterns", []):
        claims.add(("pattern", _fold(pattern)))
    claims.add(("rom_root", _fold(system["rom_root"])))
    claims.add(("image_root", _fold(system["image_root"])))
    return claims


def _core_namespace(core: dict) -> set[tuple[str, str]]:
    claims = {("core_id", _fold(core["id"]))}
    if core.get("config_folder"):
        claims.add(("config_folder", _fold(core["config_folder"])))
    if core.get("info_name"):
        claims.add(("info_name", _fold(os.path.basename(core["info_name"]))))
    return claims


def _base_namespace(base: dict) -> set[tuple[str, str]]:
    claims: set[tuple[str, str]] = set()
    for system in base.get("systems", []):
        claims |= _system_namespace(system)
    for core in base.get("cores", []):
        claims |= _core_namespace(core)
    return claims


def merge(base: dict, contributors: list[dict]) -> tuple[dict, list[dict]]:
    """base: {"platform", "systems": [...], "cores": [...]}
    contributors: [{"provider": "...", "provides": {...}}, ...]

    Returns (merged_catalog, diagnostics). A refusal NEVER fails the whole
    compile: the contribution is dropped, everything else merges, and the
    reason is recorded so the Pak Rat UI and a developer can both see why a
    pak did nothing.
    """
    diagnostics: list[dict] = []

    def record(provider: str, reason: str, detail: str) -> None:
        diagnostics.append({"provider": provider, "reason": reason, "detail": detail})

    base_claims = _base_namespace(base)

    # Stage 1: collect every candidate with its namespace claims.
    candidates: list[dict] = []
    for contributor in contributors:
        provider = contributor["provider"]
        provides = contributor.get("provides", {})
        for system in provides.get("systems", []):
            candidates.append({
                "kind": "system", "provider": provider, "entry": system,
                "key": ("system", _fold(system["id"])),
                "claims": _system_namespace(system),
            })
        for core in provides.get("cores", []):
            candidates.append({
                "kind": "core", "provider": provider, "entry": core,
                "key": ("core", _fold(core["id"])),
                "claims": _core_namespace(core),
            })

    # A core refused by a collision and a core that never existed are
    # different diagnoses with different developer fixes, so the refusals are
    # tracked by id rather than inferred from "did this provider have any
    # collision at all".
    refused_core_ids: set[str] = set()

    # Stage 2: refuse anything colliding with the release base.
    surviving: list[dict] = []
    for cand in candidates:
        hit = sorted(cand["claims"] & base_claims)
        if hit:
            if cand["kind"] == "core":
                refused_core_ids.add(_fold(cand["entry"]["id"]))
            record(cand["provider"], "base-collision",
                   f"{cand['kind']} {cand['entry']['id']} claims "
                   + ", ".join(f"{k}={val}" for k, val in hit)
                   + " already owned by the release catalog")
        else:
            surviving.append(cand)

    # Stage 3: contributor-vs-contributor collisions refuse BOTH sides.
    # Never resolved by readdir order -- a contract that depends on directory
    # iteration is not a contract.
    owners: dict[tuple[str, str], list[dict]] = {}
    for cand in surviving:
        for claim in cand["claims"]:
            owners.setdefault(claim, []).append(cand)

    refused: set[int] = set()
    for claim, claimants in sorted(owners.items()):
        distinct = {id(c): c for c in claimants}
        if len(distinct) < 2:
            continue
        providers = sorted({c["provider"] for c in distinct.values()})
        if len(providers) < 2:
            continue
        for cand in distinct.values():
            if id(cand) in refused:
                continue
            refused.add(id(cand))
            if cand["kind"] == "core":
                refused_core_ids.add(_fold(cand["entry"]["id"]))
            record(cand["provider"], "contributor-collision",
                   f"{cand['kind']} {cand['entry']['id']} claims "
                   f"{claim[0]}={claim[1]}, also claimed by "
                   + ", ".join(p for p in providers if p != cand["provider"])
                   + "; both refused")
    surviving = [c for c in surviving if id(c) not in refused]

    # Stage 4: dependency cleanup, iterated to a fixed point. Refusing a core
    # can refuse a system, which can in turn strand an extension that named
    # it, so one pass is not enough.
    core_ids = {_fold(c["id"]) for c in base.get("cores", [])}
    core_ids |= {_fold(c["entry"]["id"]) for c in surviving if c["kind"] == "core"}

    while True:
        dropped = False
        for cand in list(surviving):
            if cand["kind"] != "system":
                continue
            default_core = cand["entry"]["default_core"]
            if _fold(default_core) in core_ids:
                continue
            surviving.remove(cand)
            dropped = True
            # An unknown default_core and a refused one are different
            # diagnoses: the first is an authoring mistake, the second is a
            # collision the developer can usually fix by renaming.
            reason = ("default-core-unavailable"
                      if _fold(default_core) in refused_core_ids
                      else "unknown-default-core")
            record(cand["provider"], reason,
                   f"system {cand['entry']['id']} needs core {default_core}, "
                   "which is not available after merge; the system is refused "
                   "too rather than producing a tile that cannot launch")
        if not dropped:
            break

    merged_systems = [dict(s) for s in base.get("systems", [])]
    merged_cores = [dict(c) for c in base.get("cores", [])]
    for cand in surviving:
        entry = dict(cand["entry"])
        entry["provider"] = cand["provider"]
        # The compiler sets `status` from verified on-disk reality; a manifest
        # may not author it (D15), and it is force-cleared before this point.
        if cand["kind"] == "core":
            entry["status"] = "packaged"
            entry["requires_direct_drm"] = False
            merged_cores.append(entry)
        else:
            merged_systems.append(entry)

    system_by_id = {_fold(s["id"]): s for s in merged_systems}
    final_core_ids = {_fold(c["id"]) for c in merged_cores}

    # Stage 5: extensions -- the only way a pak may touch an existing system.
    for contributor in contributors:
        provider = contributor["provider"]
        for ext in contributor.get("provides", {}).get("system_extensions", []):
            target = system_by_id.get(_fold(ext["system_id"]))
            if target is None:
                record(provider, "dangling-extension-system",
                       f"system_extensions names {ext['system_id']}, "
                       "which no system provides after merge; extension dropped")
                continue
            existing = list(target.get("alternate_cores", []))
            for core_id in ext["add_alternate_cores"]:
                if _fold(core_id) not in final_core_ids:
                    # The extension itself is still accepted: one bad name in
                    # a list of three should not cost the developer the other
                    # two.
                    record(provider, "dangling-alternate-core",
                           f"system_extensions for {ext['system_id']} names core "
                           f"{core_id}, absent after merge; that name dropped, "
                           "extension accepted")
                    continue
                if any(_fold(e) == _fold(core_id) for e in existing):
                    continue
                existing.append(core_id)
            target["alternate_cores"] = existing

    merged = {
        "platform": base["platform"],
        "systems": sorted(merged_systems, key=lambda s: s["id"]),
        "cores": sorted(merged_cores, key=lambda c: c["id"]),
    }
    diagnostics.sort(key=lambda d: (d["provider"], d["reason"], d["detail"]))
    return merged, diagnostics
