# Pak Rat Content Paks — Normative Contracts

Date: 2026-08-28

This is the **normative text** for the four interfaces that let a `.pak`
contribute a system and the emulator that runs it:

| Contract | Scope |
| --- | --- |
| **CONTENT-1** | the `provides` block a content pak declares in its `pak.json` |
| **CONTENT-SCRAPE-1** | optional, backward-compatible scraping identity metadata beside `provides` |
| **CONTENT-ART-1** | optional system wordmarks beside `provides` |
| **CONTENT-ART-2** | optional wordmarks, untinted color wordmarks, and Grid icons beside `provides` |
| **CAT-1** | the effective catalog: generation directories, the selector, and the stamp |
| **STORE-CONTENT-1** | the storefront `content[]` lane |

This public document is complete enough to implement against: it states the
contracts field by field and gives the exact rejection reason for every rule.
Private design notes may explain *why*, but are never required to use these
interfaces.

The machine-checkable half — schemas, fixtures, and a validator that asserts
every invalid fixture fails for the reason named here — is
[`../contracts/leaf-content/`](../contracts/leaf-content/). If a schema
or fixture there disagrees with this file, **this file wins**; fix the
schema or fixture, unless the disagreement reveals that this text needs a
change, in which case follow [Change procedure](#change-procedure).

This contract copies the SVC-1 shape wholesale, including its
sibling-checkout-vs-pinned-SHA rule — see
[`../contracts/leaf-services/README.md`](../contracts/leaf-services/README.md).

---

## CONTENT-1 — the `provides` manifest

### Marker

A content pak is an ordinary pak whose `pak.json` carries a top-level
`provides` object. Nothing else marks it. Whether it is *listed in Apps* is a
separate question answered by the installed pak's `launch.sh`, never by
`provides` and never by the storefront lane:

| Installed pak | Listed in Apps | Contributes catalog content |
| --- | --- | --- |
| `launch.sh` executable, no `provides` | yes | no |
| `launch.sh` executable, `provides` present | yes (hybrid) | yes |
| no `launch.sh`, `provides` present | **no** | yes |
| neither | no | no |

### Location

`provides` is honored only for a pak installed under `Apps/<platform>/`.

A pak under `Apps/shared/` is still **classified** — its `provides` block is
read and refused with `shared-content-unsupported` — so it can never slip
through as an unrecognized pure app. A shared pak with `provides` and no
`launch.sh` stays hidden from Apps; a shared hybrid remains an ordinary
launchable app and contributes nothing (D16).

V1 also honors `provides` only on the **primary** storage source. A pak on a
secondary card is refused with `secondary-source-unsupported`: a system whose
emulator disappears when a card is pulled is a bad first experience, and the
stamp would need mount-state tracking to be correct. `source_id` is already
in the stamp so this can be relaxed later without a format change.

### Shape

```json
{
  "name": "ScummVM",
  "platform": "mlp1",
  "pak_version": "1.0.0",
  "min_leaf_version": "0.11.0",

  "provides": {
    "schema": 1,
    "systems": [
      {
        "id": "SCUMMVM",
        "name": "ScummVM",
        "patterns": ["SCUMMVM"],
        "extensions": ["scummvm", "svm"],
        "archive_extensions": [],
        "archive_mode": "pass_through",
        "playlist_extensions": [],
        "m3u_generation": "none",
        "rom_root": "Roms/SCUMMVM",
        "image_root": "Images/SCUMMVM",
        "default_core": "scummvm",
        "icon_flat": "art/SCUMMVM.png",
        "icon_photographic": null,
        "screenscraper_platform_ids": [123],
        "group": "Computer",
        "bios_directory": null
      }
    ],
    "system_extensions": [],
    "cores": [
      {
        "id": "scummvm",
        "display_name": "ScummVM",
        "type": "retroarch",
        "libretro_name": "scummvm",
        "file_name": "cores/scummvm_libretro.so",
        "info_name": "info/scummvm_libretro.info",
        "config_folder": "ScummVM",
        "supports_menu": true,
        "supports_savestate": false,
        "supports_disk_control": false
      }
    ]
  }
}
```

### `provides` — top level

| Field | Required | Rule | Reason on violation |
| --- | --- | --- | --- |
| `schema` | yes | Exactly the integer `1`. A newer schema is **refused, never guessed at**. | `unknown-schema` |
| `systems` | no | Array, ≤ 32 entries. | `too-many-systems` |
| `system_extensions` | no | Array, ≤ 32 entries. | `too-many-system-extensions` |
| `cores` | no | Array, ≤ 32 entries. | `too-many-cores` |
| — | — | At least one of the three arrays is non-empty. | `empty-provides` |
| — | — | No property outside this table. | `unknown-field` |

### `provides.systems[]`

Every field maps onto the release `systems.json` schema-2 shape, except the
five new ones marked **new**.

| Field | Required | Rule | Reason |
| --- | --- | --- | --- |
| `id` | yes | `^[A-Z0-9_]{2,32}$` | `malformed-system-id` |
| `name` | yes | 1–64 characters | `malformed-system-name` |
| `patterns` | yes | ≥ 1 entry, ≤ 32, each 1–64 chars | `malformed-patterns` |
| `extensions` | yes | ≥ 1 entry, ≤ 64, each `^[a-z0-9_]{1,16}$` | `malformed-extensions` |
| `archive_extensions` | no | ≤ 8, each `^[a-z0-9]{1,8}$`; defaults `[]` | `malformed-archive-extensions` |
| `archive_inner_extensions` | no | ≤ 64, same item rule as `extensions`; defaults to `extensions` | `malformed-archive-inner-extensions` |
| `archive_mode` | no | `pass_through` \| `extract` \| `none`; defaults `pass_through` | `unknown-archive-mode` |
| `file_names` | no | ≤ 32 strings; defaults `[]` | `malformed-file-names` |
| `ignore_file_names` | no | ≤ 32 strings; defaults `[]` | `malformed-ignore-file-names` |
| `playlist_extensions` | no | ≤ 8, each `^[a-z0-9]{1,8}$`; defaults `[]` | `malformed-playlist-extensions` |
| `m3u_generation` | no | `none` \| `auto` \| `manual`; defaults `none` | `unknown-m3u-generation` |
| `default_core` | yes | Core-id shape; must resolve after merge | `malformed-core-id`, `unknown-default-core` |
| `alternate_cores` | no | ≤ 16 core ids; defaults `[]` | `malformed-core-id` |
| `rom_root` | yes | `^Roms/[A-Za-z0-9_-]{1,32}$` | `malformed-rom-root` |
| `image_root` | yes | `^Images/[A-Za-z0-9_-]{1,32}$` | `malformed-image-root` |
| `bios_notes` | no | ≤ 8 strings; defaults `[]` | `malformed-bios-notes` |
| `icon_flat` | **new**, yes | Pak-relative path to an existing regular `.png` | path reasons below |
| `icon_photographic` | **new**, no | Same, or `null` | path reasons below |
| `screenscraper_platform_ids` | **new**, no | ≤ 8 integers, each 1–99999 | `malformed-screenscraper-platform-ids` |
| `group` | **new**, no | 1–32 characters, or `null` | `malformed-group` |
| `bios_directory` | **new**, no | `^[A-Za-z0-9_-]{1,32}$`, or `null` | `malformed-bios-directory` |

`rom_root` must respect the canonical public-folder policy — one public
`Roms/` folder per console, per
[`../canonical-user-system-folders.md`](../canonical-user-system-folders.md).

### Launch core selection

`default_core` and `alternate_cores` also decide which core a launch uses:

1. A saved core choice wins when the system lists that core and it is
   available. A game's saved choice masks the system's, even when the game's
   choice turns out to be unavailable.
2. Otherwise the launcher uses `default_core` when it is available.
3. **Only when the default is unavailable**, the launcher uses the first
   available entry of `alternate_cores`, in declared order, whatever its
   `type`.
4. If no listed core is available, the launch fails. The launcher never
   substitutes a core the system does not list.

Available `alternate_cores` are offered in the launcher's core picker; being
an alternate never makes a core start ahead of an available default.

Availability is checked for one core at a time, when the launch is requested,
and covers only these selection checks:

- A `type: "path"` core is available when its executable resolves, it
  supports the content being launched, and any launch preflight specific to
  that core (for example native PICO-8's) passes.
- A `type: "retroarch"` core is available when it is packaged and that exact
  core file resolves. A `frontend` on it does not change this; it is checked
  at launch (see [Pak-supplied RetroArch frontend](#pak-supplied-retroarch-frontend)).

Everything after selection belongs to that launch and never falls back. If a
later check fails — an explicitly chosen BIOS file that is missing or invalid,
a missing RetroArch executable, or the selected core disappearing before it
starts — the launch stops with an error. The launcher does not try the next
core.

An unavailable or no-longer-listed saved choice falls through to steps 2–4
and is kept, not erased, so it applies again once the core is back. Cores a
pak appends through `system_extensions[].add_alternate_cores` follow the same
rule.

This adds no field, schema, or fixture. Leaf releases before the rule may
launch an installed `type: "path"` alternate ahead of a `retroarch` default.
A pak that relies on its `default_core` starting first sets
`min_leaf_version` to the first Leaf release that implements this rule.

### `provides.cores[]`

| Field | Required | Rule | Reason |
| --- | --- | --- | --- |
| `id` | yes | `^[a-z0-9_]{2,64}$` | `malformed-core-id` |
| `display_name` | yes | 1–64 characters | `malformed-core-display-name` |
| `type` | yes | `retroarch` \| `path` | `unknown-core-type` |
| `libretro_name` | `retroarch` only | `^[a-z0-9_]{2,64}$` | `malformed-libretro-name` |
| `file_name` | `retroarch` only | Pak-relative path to an existing regular file | `missing-core-file-name`, path reasons |
| `info_name` | `retroarch` only | Pak-relative path to an existing regular `.info` file | `missing-core-info-name`, path reasons |
| `config_folder` | `retroarch` only | Must pass `jw_ra_core_folder_is_safe` | `missing-config-folder`, `unsafe-config-folder` |
| `frontend` | `retroarch` only, optional | Object with exactly `path` and `protocol`; see [Pak-supplied RetroArch frontend](#pak-supplied-retroarch-frontend) | `malformed-frontend`, `unknown-frontend-protocol`, path reasons |
| `path` | `path` only | Pak-relative path to an existing regular **executable** | `missing-core-path`, path reasons |
| `supports_menu` | no | boolean; defaults `false` | `malformed-core-flag` |
| `supports_savestate` | no | boolean; defaults `false` | `malformed-core-flag` |
| `supports_disk_control` | no | boolean; defaults `false` | `malformed-core-flag` |
| `needs_swap` | no | boolean; defaults `false` | `malformed-core-flag` |

A `retroarch` core may not carry `path`, and a `path` core may not carry
`libretro_name`, `file_name`, `info_name`, `config_folder`, or `frontend`
(`core-type-field-mismatch`).

`config_folder` is used by RetroArch **verbatim as a FAT32 directory
component** for `Saves/` and `States/`. An unsafe value corrupts the save
layout for the whole device, not just the pak, which is why it is checked
against the same function the release catalog is checked against, and not
merely against a regex.

### Pak-supplied RetroArch frontend

A `retroarch` core normally runs in the RetroArch that Leaf ships. When the
core cannot load there (for example a core that only exists for 32-bit ARM),
the pak can ship its own RetroArch build and declare it on the core:

```json
{
  "id": "mysaturn",
  "display_name": "My Saturn Core",
  "type": "retroarch",
  "libretro_name": "mysaturn",
  "file_name": "cores/mysaturn_libretro.so",
  "info_name": "info/mysaturn_libretro.info",
  "config_folder": "MySaturn",
  "frontend": { "path": "frontend/retroarch.sh", "protocol": "leaf-ra-1" }
}
```

| Field | Required | Rule | Reason |
| --- | --- | --- | --- |
| `frontend` | no | An object with exactly `path` and `protocol`. Anything else (a string, an array, an unknown key inside it) is refused. | `malformed-frontend` |
| `frontend.path` | yes | Pak-relative path to an existing regular **executable**. A missing, empty, or non-string value is refused. | `malformed-frontend`, path reasons |
| `frontend.protocol` | yes | Exactly `leaf-ra-1`. Absent, or any other value, is refused. | `unknown-frontend-protocol` |

`frontend` does not replace any other `retroarch` field. `libretro_name`,
`file_name`, `info_name`, and `config_folder` are still required, and the pak
still ships the core file and its `.info`. `config_folder` follows the same
rules as for every `retroarch` core, and it must name the folder RetroArch
sorts the core's saves, states and thumbnails into, which is the name the
core reports to RetroArch as its library name. Leaf and its in-game menu look
for states there (`States/<config_folder>/<content>.stateN` and its `.png`),
so a mismatch hides the player's saves. The validator cannot check it.

**Who launches it.** Leaf does, not the pak. When the player launches a game
on this core, Leaf runs the frontend in place of its own RetroArch binary,
with the same arguments and the same generated configuration file it would
give its own RetroArch. The pak's `launch.sh` is not involved. The session is
a RetroArch session: Leaf's in-game menu, Game Switcher, power-hold save,
boot resume and shader picker apply to it, provided the frontend answers
Leaf's capability probe (a frontend that does not is run as a plain frontend,
with Menu forwarded to it as a Guide tap). The argument list, the
configuration file, the UDP command interface, the exit rules and the
capability probe are specified in
[`leaf-retroarch-frontend.md`](leaf-retroarch-frontend.md) (**LEAF-RA-1**).

**Availability and launch errors.** Availability is unchanged: a `retroarch`
core is available when it is packaged and its core file resolves. The
frontend is checked when the launch starts (it exists, is a regular
executable, and is inside the installed pak). If that check or the exec
fails, the launch stops with an error. As for every check after selection,
Leaf never falls back to its own RetroArch or to another core.

**Catalog.** An accepted core entry is copied into the merged `cores.json`
as written, so `frontend` appears on the merged core row unchanged and still
pak-relative. `frontend.path` is a declared file like `file_name`: it is
fingerprinted in the contributor's CAT-1 `files[]`, so replacing the frontend
without editing `provides` invalidates the stamp.

**Compatibility.** `provides.schema` stays `1`; `frontend` is an optional
addition. A Leaf that predates the field refuses the pak with `unknown-field`
rather than ignoring the field and launching the core in the wrong RetroArch.
A pak that declares `frontend` **must** set `min_leaf_version` to the first
Leaf release that implements it, so the store never offers it to a Leaf that
would refuse it. A pak that should keep working on older Leaf ships an earlier
version without `frontend` and gates the two versions separately.

### Menu on a content-pak path core

For a provider-bound `type: "path"` core, `supports_menu` declares how your
emulator uses the built-in **MENU** button:

- With `false` (the default), a tap requests session exit with SIGTERM. The
  child receives no built-in Menu events, including during declined chords.
  Repeated built-in taps never escalate to SIGKILL.
- With `true`, an unchorded tap delivers one logical Guide tap (`BTN_MODE`):
  a synthetic press followed by a release, after you release physical **MENU**.
  Bind your emulator's menu to Guide normally; no signal handler is required.
  A chord or an escape hold delivers no Guide tap. When Leaf declines a
  **MENU** chord, the other button still reaches the emulator.

With either value, holding built-in **MENU** alone invokes Leaf's escape
gesture: SIGTERM after the escape threshold, then SIGKILL only if the same
eligible hold continues for at least two seconds after SIGTERM was sent and
the originating session is still running. Releasing **MENU** or using another
built-in button, D-pad, stick outside its calibrated deadzone, Volume, or Power
cancels pending escalation. A control already held when **MENU** goes down
also disqualifies that press. Cancellation suppresses its eventual tap;
returning a control to neutral cannot re-arm it. A fresh hold starts a fresh
threshold interval. Screen-off, sleep, input reset, proxy shutdown, and session
exit or replacement cancel the gesture and pending escalation. No Leaf OSD
appears over the standalone session.

The delayed tap, chord suppression, and escape guarantees apply **only to the
built-in Menu button**. External controllers remain direct, ungrabbed devices:

- With `true`, external **Guide** reaches the emulator as its raw press and
  release. Leaf neither quits nor synthesizes an additional virtual tap.
- With `false`, external **Guide** requests SIGTERM on press; another press
  at least two seconds later can request SIGKILL. The child also receives the
  raw events, so leave Guide unbound if you want it to act only as quit.
- External Guide chords are unfiltered, and holding external Guide invokes
  no Leaf escape gesture. Leaf emits no duplicate Guide tap on the virtual pad.

Release-owned standalones and native PICO-8 retain their existing Menu
integrations. A path pak setting `supports_menu: true` **must** set
`min_leaf_version` to the first Leaf release containing this Menu contract
(W4), or a later release it requires. Earlier Leaf versions do not implement
this flag's Menu behavior and may quit the session instead.

### `provides.system_extensions[]`

The **only** way a pak may touch an existing system. It has exactly two
fields, so the validator never has to infer intent from which fields happen
to be present:

```json
{ "system_id": "SNES", "add_alternate_cores": ["my_snes_core"] }
```

| Field | Required | Rule | Reason |
| --- | --- | --- | --- |
| `system_id` | yes | System-id shape | `malformed-system-id` |
| `add_alternate_cores` | yes | 1–16 core ids | `malformed-core-id`, `empty-add-alternate-cores` |
| — | — | No other property | `unknown-extension-field` |

There is **no** shape in which a pak restates an existing system's other
properties. A `systems[]` entry whose `id` already exists is a collision, not
an override (D9).

### Path rules

Every path field in `provides` — `icon_flat`, `icon_photographic`,
`file_name`, `info_name`, `path`, `frontend.path` — is **relative to the pak
root**. This is SVC-1's `run.path` rule verbatim, including its reason slugs:

| Rule | Reason |
| --- | --- |
| Must not be absolute | `absolute-path` |
| Must not contain a `..` component | `path-traversal` |
| Must resolve inside the pak after resolving **every** path component, not just the leaf | `escaping-symlink` |
| Must exist | `missing-file` |
| Must be a regular file | `non-regular-file` |
| A `type: "path"` core's `path` and a `frontend.path` must be executable | `not-executable` |

**Absolute paths are not merely inelegant here — they are broken.** MLP1's SD
card mounts at `/mnt/sdcard` or `/media/sdcard1` and the two swap across
reboots. An absolute root baked into a compiled catalog on that card resolves
to a path that may not exist on the next boot. Every runtime path in the
compiled catalog is therefore stored **pak-relative** and resolved against
the contributing pak's live install path at load time.

### Forbidden fields (D15)

These may not be authored anywhere in `provides`. The validator rejects each
with its own reason, and the compiler force-clears them as defense in depth:

| Field | Reason | Why |
| --- | --- | --- |
| `requires_direct_drm` | `forbidden-requires-direct-drm` | `jw_standalone_policy_requires_direct_drm` honors metadata alone — there is no allowlist gate on that path. A wrong claim can wedge the display. |
| `legacy_flat_core` | `forbidden-legacy-flat-core` | Claims ownership of save/state files from RetroArch's historical flat layout. |
| `name_map` | `forbidden-name-map` | Release-owned arcade-name behavior. |
| `status` | `forbidden-status` | The compiler sets `status` from verified on-disk reality, never from a manifest. |

### Warnings

A warning does not refuse anything; it is written to `diagnostics.json`.

| Condition | Warning |
| --- | --- |
| `patterns` contains redundant case variants (`["SCUMMVM", "scummvm"]`) — matching is already case-insensitive | `redundant-case-variant` |

The generated first-party catalog contains such pairs for historical reasons.
That is data, not a rule to imitate.

---

## Merge policy (D9)

The rule is **unique ownership across contributions**, not literal uniqueness
within one entry. A system listing a value twice is harmless; two systems
claiming the same value is the collision. Matching is case-insensitive at
runtime (`jw_ra_string_list_contains_casefold`), so ownership is resolved
**case-folded** over this namespace set:

- system `id`, every `pattern`, `rom_root`, `image_root`
- core `id`, `config_folder`
- materialized `.info` filenames

| Case | Outcome | Reason recorded |
| --- | --- | --- |
| New system, no namespace collision | Accepted | — |
| `system_extensions[]` appending to an existing system | Accepted, appended | — |
| Any namespace collision with the release base | Contribution refused and dropped, offending value logged | `base-collision` |
| Two paks collide with each other on any namespace value | **Both refused.** Never resolved by `readdir` order — a contract that depends on directory iteration is not a contract | `contributor-collision` |
| A system's `default_core` was refused or dropped during merge | **The system is refused too** — a tile that cannot launch is worse than no tile | `default-core-unavailable` |
| A system's `default_core` names a core that never existed | System refused | `unknown-default-core` |
| `system_extensions[].system_id` names no system after merge | Extension dropped, dangling id logged | `dangling-extension-system` |
| `add_alternate_cores` names a core absent after merge | Extension **accepted**, that one name dropped, logged | `dangling-alternate-core` |

A refusal never fails the whole compile. The contribution is dropped,
everything else merges, and the reason is written to the rescan log **and**
to `diagnostics.json`, so the Pak Rat UI and a developer can both see why a
pak did nothing.

Dependency cleanup is part of the merge, not an afterthought, and it is
**iterated to a fixed point**: refusing a core can refuse a system, which can
in turn strand an extension that named it.

### What an accepted core becomes

An accepted pak core is copied into the merged `cores.json` verbatim, with
three fields the compiler sets: `provider` (the pak's install path),
`status: "packaged"` and `requires_direct_drm: false`. Nothing the pak wrote is
rewritten, and paths stay pak-relative. In particular a `frontend` object
reaches the merged core row exactly as declared:

```json
{
  "id": "mysnes32",
  "type": "retroarch",
  "frontend": { "path": "frontend/retroarch.sh", "protocol": "leaf-ra-1" },
  "provider": "mlp1/MySnes32.pak",
  "status": "packaged",
  "requires_direct_drm": false
}
```

(Other fields omitted.) The launcher resolves `frontend.path` against the
provider's live install path when it launches, exactly as it resolves
`file_name`. The merge fixture `accept-frontend-core` asserts this by hand.

---

## CONTENT-SCRAPE-1 — optional scraping identity

`CONTENT-SCRAPE-1` is a fail-soft companion to CONTENT-1. It lets a pak
describe how ScreenScraper should identify descriptor files without changing
the frozen `provides` object or teaching Jawaka about one system or extension.

The companion is a top-level sibling of `provides`:

```json
{
  "provides": {
    "schema": 1,
    "systems": [
      {
        "id": "SCUMMVM",
        "extensions": ["scummvm", "svm"]
      }
    ]
  },
  "content_scrape": {
    "schema": 1,
    "systems": [
      {
        "id": "SCUMMVM",
        "name_source": "descriptor",
        "lookup_extension": "scummvm"
      }
    ]
  }
}
```

The abbreviated `provides` above omits required CONTENT-1 fields. The complete
pak must still pass CONTENT-1 independently.

Already-released CONTENT-1 validators accept unrelated top-level `pak.json`
properties and therefore ignore `content_scrape`. They accept the contribution
and retain filename scraping. This silent downgrade is intentional because the
companion affects artwork lookup only; it cannot affect discovery, launch,
cores, or whether CONTENT-1 is accepted.

### Shape and rejection reasons

| Field | Required | Rule | Reason |
| --- | --- | --- | --- |
| `schema` | yes | Exactly integer `1`. | `unknown-content-scrape-schema` |
| `systems` | yes | Array of 1-32 entries. | `malformed-content-scrape-systems` |
| `systems[].id` | yes | CONTENT-1 system-id shape; unique in this block. | `malformed-content-scrape-system-id`, `duplicate-content-scrape-system` |
| `systems[].name_source` | yes | Exactly `descriptor` in v1. | `unknown-content-scrape-name-source` |
| `systems[].lookup_extension` | yes | CONTENT-1 ROM-extension shape. | `malformed-content-scrape-lookup-extension` |
| — | — | The block and every entry are objects with no fields beyond those listed. | `malformed-content-scrape`, `malformed-content-scrape-system`, `unknown-content-scrape-field` |
| — | — | `id` names a system in this pak's accepted `provides.systems[]`. | `unknown-content-scrape-system` |
| — | — | `lookup_extension` appears in that system's `extensions`. | `undeclared-content-scrape-extension` |

The extension-membership rule is a v1 safety rail. It prevents optional
metadata from making requests with a suffix the pak does not itself recognize.
Relax it only in a future companion version with a concrete second adopter.

Missing `content_scrape` is valid and produces no warning. An invalid or
unsupported companion records its exact reason in catalog diagnostics but
does **not** refuse a valid CONTENT-1 contribution. The affected system uses
filename behavior.

### Compilation and provenance

A capable producer handles the companion during effective-catalog compilation,
never on a runtime request path:

1. Validate CONTENT-1 normally.
2. Validate `content_scrape` separately and fail-soft.
3. Run the pure CONTENT-1 merge unchanged.
4. For each valid companion entry, find the surviving merged system with the
   same `id` and the same generated `provider` as the contributor.
5. Add these fields to that effective `systems.json` row:

   ```json
   "screenscraper_name_source": "descriptor",
   "screenscraper_lookup_extension": "scummvm"
   ```

6. When at least one entry affects output, include the provider's complete
   `pak.json` as relative path `pak.json` in that contributor's existing CAT-1
   `files[]` fingerprint list. `provides_sha256` remains the canonical hash of
   `provides` alone.

The decorated `systems.json` is covered by `output.systems_sha256`, and the
existing generation digest covers both that output and the `pak.json`
fingerprint. No CAT-1 stamp field, selector rule, or runtime pak read is added.
Structural readers, including Central Scrutinizer, continue consuming only the
selected immutable generation.

An entry for a system refused by CONTENT-1 merge affects no output and adds no
fingerprint. A pak can decorate only a system it contributed; matching an id
owned by the release or another provider is insufficient.

### Descriptor and request semantics

Descriptor mode reads at most 257 bytes, rejects a file longer than 256 bytes,
trims ASCII space/tab around its only non-empty logical line, and accepts a
1-128-byte identity matching:

```text
^[A-Za-z0-9][A-Za-z0-9_.:-]{0,127}$
```

NUL, other control bytes, `/`, `\\`, truncation, read errors, and a second
non-whitespace line invalidate the descriptor. Invalid raw bytes are never
logged.

The ordered, case-insensitively de-duplicated candidates are:

1. descriptor identity plus `lookup_extension`;
2. actual indexed basename;
3. effective display title plus `lookup_extension`.

All descriptor candidates are name-only: the descriptor bytes are never
hashed. Each candidate tries the system's ordered ScreenScraper platform ids.
Within one candidate, an existing hashless/platform recovery may continue when
a game is found without usable media. If any same-identity variant found a
game, the aggregate result is no-media and no weaker candidate is tried. Only
an aggregate game-not-found advances to the next candidate. Cancellation,
quota, authentication, HTTP, parse, and local errors are terminal.

Filename mode keeps its existing per-platform hash then hashless-name request
order and has no effective-title fallback. Its one extra candidate is the
user's manual display name (`game_settings.display_name`, set in Options >
Display Name), never an imported or scanned title. It is tried only when the
game has one and its filename-mode lookups end in game-not-found, as one
name-only request per declared platform: the trimmed display name plus the
extension of the name those lookups sent, or the display name alone when that
name has none. A candidate equal to any name already sent, ignoring case, is
skipped. No-media, cancellation, quota, authentication, HTTP, parse, and local
errors never reach it, and matching stays exact `jeuInfos.php`.

Regardless of the successful candidate, artwork stays keyed to the original
indexed filename stem and neither `games.rom_path` nor the display title is
rewritten.

---

## CONTENT-ART-1 - optional system wordmarks

Your content pak can supply a system wordmark without changing its CONTENT-1
contribution. Add this optional top-level sibling of `provides` to an otherwise
valid manifest:

```json
{
  "content_art": {
    "schema": 1,
    "systems": [
      { "id": "SCUMMVM", "wordmark": "art/SCUMMVM-wordmark.png" }
    ]
  }
}
```

CONTENT-1 stays unchanged. Older consumers ignore this sibling and retain
bundled artwork or a system-name fallback. You don't need to raise your pak's
minimum Leaf version solely for this cosmetic metadata.

Wordmarks are tinted. The launcher draws a wordmark multiplied by the active
theme's text color, so the image supplies only the shape. Author it as white
(#FFFFFF) shapes on a transparent background and let alpha carry the edges.
Any color in the RGB channels is lost or muddied by the tint.

### Shape and diagnostics

| Field | Rule | Reason |
| --- | --- | --- |
| block | Object, when present. `null` is invalid. | `malformed-content-art` |
| `schema` | Required JSON number equal to `1`; `1.0` and `1e0` are equivalent. Booleans and strings are invalid. | `unknown-content-art-schema` |
| `systems` | Required array of 1-32 entries. | `malformed-content-art-systems` |
| entry | Object. | `malformed-content-art-system` |
| `systems[].id` | Required CONTENT-1 system ID, unique within the block. | `malformed-content-art-system-id`, `duplicate-content-art-system` |
| `systems[].wordmark` | Required non-empty string, at most 4096 characters, no NUL. | `malformed-content-art-wordmark` |
| all objects | Only `schema` and `systems` in the block; only `id` and `wordmark` in each entry. | `unknown-content-art-field` |
| wordmark path | Pak-relative, existing regular file; apply CONTENT-1 path checks. | `content-art-absolute-path`, `content-art-path-traversal`, `content-art-escaping-symlink`, `content-art-missing-file`, `content-art-non-regular-file` |
| wordmark format | `.png` suffix (case-insensitive), PNG signature, and an initial 13-byte IHDR chunk. A malformed or truncated header is unsupported. | `unsupported-content-art-image` |
| wordmark dimensions | IHDR width and height each 1-1024. | `invalid-content-art-wordmark-dimensions` |
| wordmark read | File must be readable during compilation. | `unreadable-content-art-image` |

Validate this companion independently from CONTENT-1. Missing metadata is valid
and inert. Record at least one companion violation in catalog diagnostics and
ignore an invalid block as a unit. You may report additional violations. Never drop a system, core, or working app because
optional artwork is invalid. The PNG signature and IHDR checks identify the
format and bound its size; the runtime image loader remains responsible for full
decoding. An undecodable or unsupported PNG falls through to the next artwork
candidate.

If your JSON parser cannot preserve embedded NUL characters, reject the entire
companion with `malformed-content-art` before a string can be silently truncated.
Parsers that preserve NUL may report the specific field violation. In both cases,
keep an otherwise valid CONTENT-1 contribution.

### Eligibility after merge

Run the ordinary CONTENT-1 merge unchanged, then consider valid companions from
accepted contributors. An entry is eligible only when its system survives and:

- The system's generated `provider` equals this pak's install identity; or
- The system is release-owned (no `provider`), and this pak declares a
  `system_extensions` entry for it naming at least one surviving alternate core
  contributed by this same pak. That core must appear in the merged system's
  `alternate_cores`.

A base core, another pak's core, or a refused contributed core cannot authorize
artwork. You cannot decorate another pak's system, even when your extension
adds a working alternate core. A missing or ineligible target produces
`ineligible-content-art-system`. Discard that entry only, retaining unrelated
eligible entries from the same valid block.

If two or more eligible extension paks claim the same release-owned ID, discard
all claims for that ID and report `conflicting-content-art-system` for each
claimant. Resolve conflicts after eligibility, independently of filesystem
order. Ownership, names, discovery, defaults, and icon fields never change.

### Catalog fields and freshness

Decorate each eligible, uncontested system row with both:

```json
"wordmark": "art/SCUMMVM-wordmark.png",
"wordmark_provider": "mlp1/ScummVM.pak"
```

`wordmark_provider` is the existing provider install identity, never a mount
point. It is separate from system `provider`: native PICO-8 artwork can come
from a pak while PICO8 remains release-owned. Resolve the relative `wordmark`
against that provider's live install root using CAT-1 contributor `source_id`
and existing storage-source resolution. CONTENT-1 currently accepts only the
primary storage source. Missing or malformed optional fields disable that
candidate, not the system row. Readers without CONTENT-ART-1 support ignore
both new fields.

For each applied entry, add its relative PNG path and `pak.json` to that
provider's existing CAT-1 `files[]` list, deduplicated and sorted by `rel`, with
the existing SHA-256 file hashes. Entries that don't affect output add no art
fingerprints. `provides_sha256` continues to cover `provides` alone. The
existing `output.systems_sha256` covers decorated rows. Changing only PNG bytes
at the same path must change the contributor fingerprint and generation digest,
even when the serialized system row is identical. Provider removal, missing
files, and root relocation use the existing provenance and selector rules.
No stamp field, stamp schema, or runtime manifest read is added.

### Display fallback

Look up the game-details wordmark in this order. A `.color.png` file and the
catalog `wordmark_color` field are full-color wordmarks drawn without tint; every
other image candidate is tinted as described above.

1. ROM-folder `wordmark.color.png` (untinted)
2. ROM-folder `wordmark.png` (tinted)
3. Selected user-theme `grid/wordmarks/<ID>.color.png` (untinted)
4. Selected user-theme `grid/wordmarks/<ID>.png` (tinted)
5. Accepted catalog `wordmark_color` (untinted, CONTENT-ART-2 only)
6. Accepted catalog `wordmark` (tinted)
7. Bundled `res/grid_wordmarks/<ID>.color.png` (untinted)
8. Bundled `res/grid_wordmarks/<ID>.png` (tinted)
9. The system display name as text

Preserve existing ROM-folder/source resolution and theme identity handling.

The launcher fully decodes a wordmark and then downscales it to at most 512 px
on each edge, so the 512 px figure does not limit decode memory. The 1-1024
dimension rule does: before decoding a candidate from any source, the launcher
reads its IHDR header and refuses one outside 1-1024 on either edge, the same
way it treats grid icons. A missing file, a header outside that range, or a
failed decode advances to the next candidate; an asynchronous pending load is
not a permanent failure. Catalog generation and provider asset changes must
invalidate path memos, textures, and derived thumbnails, including replacement
at the same path. No standalone art-pack format is defined here.

## CONTENT-ART-2 - wordmarks and grid icons

You can provide a separate full-color icon for the system-selection Grid while
retaining your wordmark for game details, and a full-color wordmark that is drawn
without the theme tint. Use `content_art.schema: 2` with
`content-art-v2.schema.json`. CONTENT-1 and CAT-1 stay unchanged, and readers
continue to accept CONTENT-ART-1. Schema values use numeric equality here too:
`2`, `2.0` and `2e0` identify version 2. Booleans and strings are invalid.

```json
"content_art": {
  "schema": 2,
  "systems": [{
    "id": "SCUMMVM",
    "wordmark": "art/SCUMMVM-wordmark.png",
    "wordmark_color": "art/SCUMMVM-wordmark-color.png",
    "grid_icon": "art/SCUMMVM-grid.png"
  }]
}
```

Each system entry requires `id` and at least one of `wordmark`,
`wordmark_color` or `grid_icon`. Any image can be omitted; a present image must
be a nonempty string. The block accepts only `schema` and `systems`, and rows
accept only those four fields. The v1 ID, uniqueness, count, string length, path
containment, regular file, readability and PNG header rules apply to every image
slot. An empty row reports `missing-content-art-image`; an invalid grid path
string reports `malformed-content-art-grid-icon`; an invalid color wordmark path
string reports `malformed-content-art-wordmark-color`. Other malformed fields
retain the v1 reasons.

| Slot | Meaning | Authoring | Dimensions |
| --- | --- | --- | --- |
| `wordmark` | Game-details wordmark, tinted with the theme's text color. | White (#FFFFFF) shapes on a transparent background. | 1-1024 per edge, else `invalid-content-art-wordmark-dimensions` |
| `wordmark_color` | Game-details wordmark drawn without tint. | RGBA with its own colors. | 1-1024 per edge, else `invalid-content-art-wordmark-dimensions` |
| `grid_icon` | Full-color Grid system icon, not tinted. | 512x512 RGBA. | 1-1024 per edge, else `invalid-content-art-grid-dimensions` |

A row may carry both wordmarks. In the [display fallback](#display-fallback)
order the catalog `wordmark_color` is tried before the catalog `wordmark`, so the
tinted image is used only when the color one is absent, over the dimension
limit, or fails to decode.

For every image slot, compilation requires the PNG signature, an initial 13-byte
IHDR chunk and nonzero width/height no greater than 1024. Malformed/truncated
headers report `unsupported-content-art-image`; dimensions outside 1-1024 report
the slot's dimensions reason from the table. Full decoding happens in the
launcher: a corrupt or unsupported image falls through without blocking play.
Keep grid icon proportions and colors; that slot is not tinted as a wordmark.

The optional block fails soft as a unit, including when only one referenced
image is invalid. Its CONTENT-1 contribution remains usable. An older v1-only
reader ignores the entire schema-2 companion, including its wordmark, and uses
its ordinary artwork fallback. Do not raise a minimum Leaf version for these
cosmetic fields. Ship the new reader before migrating a pak's companion.

Post-merge eligibility is the same as v1: your pak must own the surviving system,
or extend a release-owned system with its own surviving alternate core. Resolve
claims independently for each `(system ID, image slot)`, including claims from
mixed v1 and v2 paks. `wordmark`, `wordmark_color` and `grid_icon` are three
separate slots. Ignore every competing claim for that slot. A conflict in one
slot must not discard a unique claim in another: a grid conflict keeps a unique
wordmark, and a `wordmark_color` conflict keeps a unique tinted `wordmark`. Retain
`conflicting-content-art-system`; its detail is the system ID for `wordmark`,
`<ID>:wordmark_color` for color wordmarks, and `<ID>:grid_icon` for grid icons.
Ineligible entries retain the v1 diagnostic.

Decorated system rows may contain `wordmark_color` and `wordmark_color_provider`,
and `grid_icon` and `grid_icon_provider`, alongside `wordmark` and
`wordmark_provider`. Each pair has its own provider identity; none changes
system ownership. Serialize pak-relative image paths and resolve
against the provider's live install root, rechecking containment and a regular
file. A malformed optional pair is ignored without discarding the row or another
valid pair. Fingerprint every applied image and its manifest in CAT-1's existing
contributor file list and cover the decorated output with the systems digest.
No runtime manifest polling is needed.

In the Grid layout, look up a system tile icon in this order:

1. Selected user-theme `grid/icons/<ID>.png`
2. The existing ROM-folder `icon.png` override
3. The user's active built-in icon pack art for that system: the theme's grid
   icon set or the photographic set for Auto, the photographic set for
   Photographic, and the shared flat set for Flat. A pack's art for the system
   always precedes pak art.
4. Accepted pak `grid_icon`
5. The existing generic pak icon-pack candidates (pak-owned systems only)
6. The shared flat baseline icon
7. The placeholder

A user who picked an icon pack keeps that pack's art for a system it covers; a
pak grid icon fills in only where the pack has none. Preserve current theme/ROM
priority and folder resolution. This order and the pak grid icon apply to Grid
system tiles only; every other layout keeps its existing lookup order, and Cover
Flow, Search, Apps icons and full-tile label overlays retain their existing
behavior. Keep a higher-priority asynchronous load pending until it succeeds or
fails. Failed decode advances to the next candidate. A catalog generation change,
including provider update or removal, must invalidate path, failure and texture
caches. A theme or icon pack change must resolve paths again; decoded textures
are keyed by file, so it need not discard them. A reload that leaves the catalog
generation unchanged must not discard any of these caches.
Use catalog generation identity in derived thumbnails so same-path/same-mtime
provider replacements cannot serve older bytes.

## CAT-1 — the effective catalog

### Layout

```text
$UMRK_INTERNAL_DATA_PATH/catalog/          # = $SDCARD_PATH/.umrk/<platform>/catalog/
  current                                  one line: "gen-<digest>\n"  ← atomically replaced
  diagnostics.json                         OUTSIDE any generation
  tmp-<random>/                            in-flight compile; never named by `current`
  gen-<digest>/
    systems.json      merged, schema 2, same `platform` as the base
    cores.json        merged
    info/             merged libretro .info files (release + pak)
    stamp.json        provenance
```

A generation directory is written once and **never mutated**. `current` is a
single small file replaced by `rename()` — that *is* atomic, including on
vfat. Replacing a non-empty *directory* by `rename()` is not; it fails
`ENOTEMPTY`, which is why the naive temp-dir swap does not work.

### Selector grammar

`current` contains exactly `gen-` followed by **64 lowercase hexadecimal
digits** and one trailing `\n`. Nothing else. A reader that cannot parse this
exact grammar falls back to release defaults with `selector-malformed`.

### Canonical bytes and digests

Three primitives, fixed here so producer and readers agree byte for byte:

1. **`canonical_bytes(obj)`** — `obj` serialized as JSON with keys sorted by
   Unicode code point, separators `,` and `:` (no spaces), UTF-8, no BOM, no
   escaping of non-ASCII, followed by exactly one `\n`.

2. **`tree_sha256(dir)`** — for each regular file under `dir`, in ascending
   byte order of its `/`-joined relative path, feed:

   ```text
   rel_path_utf8 || 0x00 || uint64_be(len(file_bytes)) || file_bytes
   ```

   The length prefix is what stops `a` + `bc` from hashing the same as `ab` +
   `c`. An empty directory hashes to SHA-256 of the empty string.

3. **`digest`** — `SHA-256(canonical_bytes(stamp))`, hex, lowercase. Because
   the stamp already contains every input hash **and** every output hash,
   this digest identifies the complete generation. The generation directory
   is named `gen-<digest>`.

Ordering inside the stamp is normative: `contributors[]` ascending by
`provider`, and each contributor's `files[]` ascending by `rel`, both in
UTF-8 byte order.

### `stamp.json`

```json
{
  "schema": 1,
  "platform": "mlp1",
  "release_id": "2026-08-28-gabc1234",
  "base": {
    "systems_sha256": "…",
    "cores_sha256": "…",
    "info_sha256": "…"
  },
  "contributors": [
    {
      "provider": "mlp1/ScummVM.pak",
      "source_id": "primary",
      "pak_version": "1.0.0",
      "provides_sha256": "…",
      "files": [
        { "rel": "art/SCUMMVM.png", "sha256": "…" },
        { "rel": "cores/scummvm_libretro.so", "sha256": "…" },
        { "rel": "info/scummvm_libretro.info", "sha256": "…" }
      ]
    }
  ],
  "output": {
    "systems_sha256": "…",
    "cores_sha256": "…",
    "info_sha256": "…"
  }
}
```

- `provider` is the **install path** — the same identity
  `pakrat_installs.install_path` and `apps.pak_dir` key on. It is never a
  resolved absolute root; the whole point is that no absolute path survives a
  reboot.
- `source_id` records which card the pak came from. It **cannot** identify a
  pak: `jw_storage_source.id` is `"primary"` or an index, so every pak on one
  card shares it.
- `provides_sha256` is `SHA-256(canonical_bytes(provides))`.
- `files[]` fingerprints **every declared file**, a core's `frontend.path`
  included. This is what makes swapping or deleting a `.so` without editing
  `provides` invalidate the stamp — the compiler's `status` output asserted
  that file was present and usable.
- `base.info_sha256` covers the release `.info` directory. Those files are
  *materialized* into the generation, so they are compile inputs like any
  other; leaning on `release_id` alone would miss a partial manual copy that
  replaced info files without changing the release identity.
- `release_id` comes from `$UMRK_INTERNAL_DATA_PATH/release.json` via
  `jw_installed_release_read(...).release_id`. A missing, empty, or invalid
  installed release identity prevents activation of an effective generation:
  use valid release defaults and record `release-identity-unavailable`; if
  those defaults are invalid too, return an explicit catalog error. Native
  tests supply an explicit `release.json` fixture or injected identity — the
  compiler never guesses one.

### Two validation levels

| Level | Who | What | When |
| --- | --- | --- | --- |
| **Full provenance** | `jawakad` only | Rehash the release inputs **and every declared contributor file**; compare against `stamp.json` | At startup, and during the contributor-enumeration pass before a ROM scan |
| **Structural** | Jawaka catalog readers, CS | Parse a safe selector name; require that generation and its `stamp.json`; validate stamp schema, `platform`, and installed `release_id`; load the immutable output | Per catalog load |

Readers **never** rehash contributor binaries on a request path. Rehashing
multi-megabyte cores on FAT32 would make a metadata request do compiler work.

CS may cache the structural result keyed by the exact
(`current` value, installed `release_id`) pair, and must revalidate when
either changes.

`mtime` + size may be recorded or used as a future optimization *hint*. It is
**never** proof of equality: FAT timestamp granularity and same-sized
replacement files can otherwise preserve a stale catalog.

### Publication protocol

1. The compiler writes a `tmp-<random>` directory, fsyncs its files, and
   computes the three output hashes.
2. It writes `stamp.json` in canonical form and computes
   `digest = SHA-256(canonical_bytes(stamp))`.
3. After the temp directory and its parent are fsynced, it is renamed to
   `gen-<digest>`.
4. **If `gen-<digest>` already exists**, the compiler compares the existing
   directory's canonical stamp bytes and output hashes against the temp
   directory's.
   - Exact match → **reuse** the existing generation, discard the temp
     directory. This is what makes a no-op recompile free.
   - Mismatch → corruption. Compilation **fails closed**, records
     `generation-digest-conflict` in `diagnostics.json`, and does **not**
     overwrite the existing directory.
5. `current` is replaced by `rename()` of a temp file in the same directory.

**Invalidation ordering.** On a full-provenance mismatch, `jawakad` first
`unlink`s `current` and fsyncs the catalog directory — making every *new*
reader fall back to release defaults — and only then recompiles. A successful
compile atomically publishes a new selector; a crash or failed compile safely
leaves it absent. A reader that already opened an immutable generation may
finish its existing snapshot.

### Fail-closed rules (D2)

| Situation | Behavior | Reason |
| --- | --- | --- |
| `current` absent | Release defaults | `selector-missing` |
| `current` not the exact selector grammar | Release defaults | `selector-malformed` |
| Named generation directory absent | Release defaults | `generation-missing` |
| Generation present but missing one of `systems.json`, `cores.json`, `info/` | Release defaults — a partially written generation is never served | `generation-missing` |
| `stamp.json` absent or unparseable | Release defaults | `stamp-missing` |
| `stamp.schema` not `1` | Release defaults | `stamp-schema-unsupported` |
| `stamp.platform` ≠ running platform | Release defaults | `stamp-platform-mismatch` |
| `stamp.release_id` ≠ installed `release_id` | Release defaults; producer recompiles | `stamp-release-mismatch` |
| Base hash mismatch (producer only) | Invalidate selector, recompile | `stamp-base-mismatch` |
| Contributor hash mismatch (producer only) | Invalidate selector, recompile | `stamp-contributor-mismatch` |
| Installed release identity unreadable | Release defaults | `release-identity-unavailable` |
| Compilation failed | Release defaults, logged loudly | `compile-failed` |
| Release defaults **also** invalid | **Explicit catalog error** | `release-defaults-invalid` |

A cached stale generation is **never** served after `current` changes or
after failed validation. A stale catalog that silently pins a user to a
pre-OTA system list is worse than not shipping the feature, so there is no
stale-data fallback.

`diagnostics.json` lives **outside** every generation, so a compile that
fails entirely still leaves a readable explanation.

### Lifetime

**No finalized-generation garbage collection in v1.** A CS process may
survive a `jawakad` crash, so even daemon startup is not a reader-free
window. Only abandoned `tmp-*` directories — which could never have been
named by `current` — are cleaned. Content addressing already makes no-op
recompiles reuse the same directory. A later GC requires an explicit
reader-coordination design honored by CS and RetroArch, and is added only if
measured storage growth justifies it.

### Compile timing (P0-2)

Contributor enumeration across the platform and shared app lanes, and the
compile itself, run **before `jw__scan_roms`**. `jw_scan_library` takes
**one** immutable catalog snapshot at the top and threads it through both
`jw__scan_roms` and `jw__scan_apps`. Without this, installing a content pak
adds a system the same scan never indexes, and uninstalling leaves a removed
system's games in the DB until some later scan.

`INFO_PATH` is resolved **per game launch**, from the same generation
snapshot that selected the core — not `setenv`'d once at daemon startup. The
process-wide default is inherited by every child, so after a generation swap
every launch would otherwise point at the previous generation.

---

## STORE-CONTENT-1 — the storefront `content[]` lane

```json
{
  "schema": 1,
  "product": "pak-rat",
  "apps": [ "… unchanged …" ],
  "content": [ { "id": "org.umrk.scummvm", "…": "same package shape" } ]
}
```

| Rule | Reason |
| --- | --- |
| `schema` stays `1`. A gate-unaware client parses `apps[]`, ignores the unknown `content` key, and **never sees a content pak at all** | — |
| The **safe-floor rule does not apply within `content[]`**; instead, its legacy mirror and every `versions[]` entry declare `min_leaf_version` | `ungated-content-version` |
| Immutability and append-only history apply unchanged | `immutable-history-violated` |
| Every `content[]` artifact must actually declare `provides` | `content-artifact-without-provides` |
| An id may appear in **exactly one** lane | `id-in-both-lanes` |
| An `apps[]` artifact that declares `provides` is in the wrong lane | `provides-artifact-in-apps-lane` |
| The `apps[]` safe-floor rule is **unchanged** — the exemption above must not leak into it | `missing-safe-floor` |
| `content[].packages[].platform` names a concrete supported platform; `"shared"` is refused | `shared-platform-in-content` |
| `content[].packages[].runtime` is `"leaf"` | `unknown-runtime` |

Everything else about a package — version grammar, `versions[]` ordering and
limits, `min_leaf_version` grammar, artifact completeness, HTTPS rules — is
shared with the ordinary Pak Rat package rules and enforced by the generator
and publication validator.

### Why a new lane and not `kind` on `apps[]`

Every package in `apps[]` needs an ungated **safe floor**, and the generator
rejects an all-gated history. Every content pak must gate on the release that
ships this contract, so no content pak can satisfy that rule. Worse, on an
old client a content-only pak would install and then show a broken tile,
failing at launch with "app launch.sh missing or not executable". The
safe-floor rule exists precisely to prevent that class of outcome; the
`content[]` lane honors its intent instead of inventing a fake ungated
version to route around it.

### Hybrid packages

PortMaster is both an app and a content provider, so the lanes need an
explicit rule:

- **An id lives in exactly one lane.** Both lanes resolve to the same
  `install_path` (`mlp1/PortMaster.pak`), and two catalog entries pointing at
  one install path is a duplicate-identity bug, not a feature.
- **A package that declares `provides` belongs in `content[]`**, whether or
  not it also has a `launch.sh`. It is gated on this contract by
  construction, so `apps[]` — the gate-unaware lane — is the wrong home.
- **"Open" is derived from the installed pak, never from the lane.** Pak Rat
  offers Open iff the installed `.pak` has an executable `launch.sh`. A
  hybrid gets Open; a pure content pak does not; and the rule stays correct
  for a sideloaded pak that is in no lane at all.
- **Lane migration is deliberate.** When PortMaster moves lanes, gate-unaware
  clients stop seeing it in `apps[]`. That is acceptable: they keep their
  working install, and they could not run the new gated versions anyway. The
  validator flags an id present in both lanes as an error. This migration is
  recorded here so it is not rediscovered during the PortMaster phase.

---

## Change procedure

`content-paks-v1`, `effective-catalog-v1`, and `storefront-content-v1` are
frozen once the first content pak publishes. `content-scrape-v1` freezes when
the first pak using it publishes. `content-art-v1` and `content-art-v2` freeze
together when the first pak using either one publishes. Until the applicable
freeze:

1. Change this file first. It is the normative text.
2. Update the schema and the fixture tree in
   [`../contracts/leaf-content/`](../contracts/leaf-content/) to match,
   and re-run `scripts/validate_fixtures.py`.
3. Every new rejection rule gets **exactly one** invalid fixture whose
   recorded expectation names its reason slug.
4. A breaking change after freeze is a **new** version identifier
   (`content-paks-v2`), never a mutation of v1. `provides.schema` is refused
   rather than guessed at precisely so a v2 pak fails loudly on a v1 device.
5. After the content art freeze, a new `content_art` field or slot is a new
   schema number with its own schema file, never a mutation of
   `content-art-v1` or `content-art-v2`. Fixtures that exercise
   `unknown-content-art-schema` use a number no reader will ever accept
   (`99`), not the next version.

## Related

- [`leaf-retroarch-frontend.md`](leaf-retroarch-frontend.md) — LEAF-RA-1, the protocol a pak-supplied RetroArch frontend speaks
- [`../contracts/leaf-content/`](../contracts/leaf-content/) — schemas, fixtures, and reference models
- [`../contracts/leaf-services/README.md`](../contracts/leaf-services/README.md) — the public SVC-1-family contract guide
