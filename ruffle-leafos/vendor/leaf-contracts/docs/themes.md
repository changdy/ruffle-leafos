# Leaf Themes - Normative Contract

Date: 2026-09-14

This is the **normative text** for the theme package format:

| Contract | Scope |
| --- | --- |
| **THEME-1** | a theme `.zip`: its layout, `theme.json`, images, and archive limits |

This public document is complete enough to implement against: it states the
contract field by field and gives the exact rejection reason for every rule.
Private design notes may explain *why*, but are never required to use this
interface.

The machine-checkable half (a schema, fixture archives, and a reference
validator that asserts every invalid fixture fails for the reason named here)
is [`../contracts/leaf-themes/`](../contracts/leaf-themes/). If the schema or a
fixture there disagrees with this file, **this file wins**; fix the schema or
fixture, unless the disagreement reveals that this text needs a change, in
which case follow [Change procedure](#change-procedure).

---

## THEME-1 - the theme package

### What a theme package is

A theme restyles Leaf's Grid View and Cover Flow: tile icons, labels, system
wordmarks, wallpapers, colors, and the recommended grid size. It contains no
code. One package installs on every Leaf device, so there is no `platform`
anywhere in it.

A package is a zip archive holding exactly one folder. Leaf installs that
folder as `Themes/<id>/` on the primary card, which is where the launcher
discovers user themes. Everything a device reads from the package is listed
in this document; any other file is refused rather than ignored.

### Zip layout

```text
<id>/                              exactly one top-level folder, named by theme.json "id"
  theme.json                       required
  preview.png                      required, the store screenshot
  LICENSE.txt                      optional, full license text
  wallpaper.png|jpg|jpeg           optional, the theme-wide wallpaper
  icons/<ID>.png                   optional, system icons both views use
  grid/
    wallpaper.png|jpg|jpeg         optional, overrides the theme-wide wallpaper in Grid View
    icons/<ID>.png                 optional, system tiles
    labels/<ID>.png                optional, full-tile overlays
    wordmarks/<ID>.png             optional, system wordmark, tinted
    wordmarks/<ID>.color.png       optional, system wordmark, untinted
  coverflow/
    icons/<ID>.png                 optional, Cover Flow system cards
```

The allowlist holds only what the launcher draws today. Cover Flow reads icons
only, so it has no wallpaper, labels or wordmarks here. Adding a file kind later
is not a breaking change: a theme that uses it declares a `min_leaf_version` new
enough to draw it.

`icons/` at the root is one icon set for both views, so a theme whose Grid and
Cover Flow art are the same ships it once. For each system, a view looks in its
own `icons/` folder first, then in the shared one, then draws what it would
without the theme. A theme can therefore share most icons and still override a
few in one view. Only icons are shared: labels and wordmarks stay under `grid/`.
Test builds of Leaf 0.12.0 up to beta 4 predate this folder and refuse a theme
that contains it; 0.12.0 itself and every later release accept it.

Directory entries are optional. When present, a directory entry must name one
of the folders above.

### theme.json

```json
{
  "schema": 1,
  "id": "neon-nights",
  "name": "Neon Nights",
  "author": "Example",
  "version": "1.2.0",
  "min_leaf_version": "0.12.0",
  "license": "CC-BY-4.0",
  "description": "Pink and cyan on black.",
  "grid": { "cols": 4, "rows": 3 },
  "colors": {
    "text": "#F2F2F2",
    "highlight": "#FF4FB8",
    "highlight_text": "#12292B",
    "underlay": "#000000",
    "underlay_opacity": 178,
    "tile_border": "#FFFFFF40",
    "focus_ring": "#2FE6FF",
    "shadow": 96
  },
  "status_style": "light"
}
```

| Field | Required | Rule | Reason on violation |
| --- | --- | --- | --- |
| `schema` | yes | A JSON number equal to `1`; `1.0` is the same value. Booleans and strings are invalid. A newer schema is **refused, never guessed at**: no other field is judged. | `theme-unknown-schema` |
| `id` | yes | `^[a-z0-9][a-z0-9-]{1,39}$`. Names the top-level folder and the install folder. Never changes between versions. | `theme-id-invalid` |
| `name` | yes | 1-40 Unicode characters and at most 95 UTF-8 bytes, no control characters (U+0000-U+001F, U+007F), not only whitespace. | `theme-name-invalid` |
| `author` | yes | 1-60 characters and at most 63 UTF-8 bytes, same character rules as `name`. Display text only. | `theme-author-invalid` |
| `version` | yes | `MAJOR.MINOR.PATCH`, each component 0-9999 with no leading zeros: `^(0\|[1-9][0-9]{0,3})\.(0\|[1-9][0-9]{0,3})\.(0\|[1-9][0-9]{0,3})$`. | `theme-version-invalid` |
| `min_leaf_version` | yes | The `version` grammar, and at least `0.12.0`, the first release with user themes. | `theme-min-leaf-version` |
| `license` | yes | Exactly one of `CC-BY-4.0`, `CC-BY-SA-4.0`, `CC-BY-NC-SA-2.0`, `CC-BY-NC-SA-4.0`, `CC0-1.0`, `redistribution-permitted`. | `theme-unknown-license` |
| `description` | no | A string of at most 300 characters. No control characters except line feed (U+000A). | `theme-description-invalid` |
| `grid` | no | An object with **both** `cols` (integer 1-8) and `rows` (integer 1-6). The launcher applies the recommendation only as a pair; the user's Grid Size setting still wins. | `theme-grid-invalid` |
| `colors` | no | An object. `text`, `highlight`, `highlight_text`, `underlay`, `tile_border`, `focus_ring` are each `#RRGGBB` or `#RRGGBBAA` (hex digits in either case; no alpha means opaque). | `theme-color-invalid` |
| `colors.underlay_opacity`, `colors.shadow` | no | Integers 0-255. `underlay_opacity` replaces the underlay's alpha. | `theme-color-level-invalid` |
| `status_style` | no | `auto`, `light`, or `dark`. `light` draws light status icons for a dark wallpaper, `dark` the reverse, and `auto` samples the wallpaper under each overlay. | `theme-status-style-invalid` |
| - | - | No property outside this table, at the top level or inside `grid` or `colors`. | `theme-unknown-field` |

"Integer" means a JSON number written without a fraction or exponent: `3` is
valid, `3.0` and `3e0` are not. Every color and level is optional; a theme that
omits one keeps the value Leaf would otherwise use.

The byte limits on `name` and `author` match the launcher's fixed buffers, so
an accepted name is never cut off in the middle of a character. The JSON
Schema cannot express them; the reference validator enforces them.

`CC-BY-NC-SA-2.0` names Creative Commons Attribution-NonCommercial-ShareAlike
2.0 Generic. It must not be replaced by `CC-BY-SA-4.0` or
`redistribution-permitted` to fit a reader's license list. Leaf 0.12.0 test
builds up to beta 6 predate this license value and refuse themes using it.

`CC-BY-NC-SA-4.0` names Creative Commons Attribution-NonCommercial-ShareAlike
4.0 International. It is a different license from `CC-BY-NC-SA-2.0`, and
neither may stand in for the other. Leaf 0.12.0 test builds up to beta 7
predate this license value and refuse themes using it.

#### Reading theme.json

| Rule | Reason |
| --- | --- |
| At most 65,536 bytes, the launcher's read limit. | `theme-manifest-too-large` |
| UTF-8 with no byte order mark, one JSON object, no duplicate keys at any depth, no `NaN` or `Infinity`. Duplicate keys are refused because the device's parser keeps the first and most others keep the last, so the store and the device would read different themes. | `theme-malformed-manifest` |

### Path allowlist

Paths below are relative to the top-level folder. Names are compared
**exactly**: `grid/icons/FC.PNG` and `Grid/icons/FC.png` are not on the list.

| Path | Format | Dimensions (width and height) |
| --- | --- | --- |
| `theme.json` | JSON, see above | - |
| `preview.png` | PNG | exactly 960 x 720 |
| `LICENSE.txt` | not inspected | - |
| `wallpaper.png`, `wallpaper.jpg`, `wallpaper.jpeg` | PNG for `.png`, JPEG otherwise | each 1-2048 |
| `grid/wallpaper.png`, `.jpg`, `.jpeg` | PNG for `.png`, JPEG otherwise | each 1-2048 |
| `icons/<ID>.png`, `grid/icons/<ID>.png`, `coverflow/icons/<ID>.png` | PNG | each 1-1024 |
| `grid/labels/<ID>.png` | PNG | each 1-1024 |
| `grid/wordmarks/<ID>.png` | PNG | each 1-1024 |
| `grid/wordmarks/<ID>.color.png` | PNG | each 1-1024 |

| Rule | Reason |
| --- | --- |
| Every file and directory entry is on the allowlist. | `theme-unknown-file` |
| `<ID>` matches `^[A-Z0-9_]{2,32}$`: a system code the catalog knows, or one a content pak adds. The one exception is `_apps`, exactly in lowercase, which names the Apps tile and is allowed as its icon and its Grid label: `icons/_apps.png`, `grid/icons/_apps.png`, `coverflow/icons/_apps.png` and `grid/labels/_apps.png`. The Apps tile has no wordmark. | `theme-system-id-invalid` |
| `<ID>` is not `_default` in any letter case. `_default` is Leaf's final fallback, not a tile, and is never themed. This reason replaces `theme-system-id-invalid` for that name. | `theme-reserved-system-id` |
| At most one wallpaper per folder: the root and `grid/` may each hold one of the three names. | `theme-multiple-wallpapers` |
| `theme.json` is present. | `theme-missing-manifest` |
| `preview.png` is present. | `theme-missing-preview` |

Wordmarks follow the same authoring rules as CONTENT-ART-2 in
[`content-paks.md`](content-paks.md): a `.png` wordmark is white (#FFFFFF) on a
transparent background and is tinted with the theme's text color; a
`.color.png` wordmark keeps its own colors. Icons are best authored at 512 x
512 with transparency.

### Images

Every image is identified from its bytes, never from its extension alone, and
the extension must agree with the format.

| Format | What makes a file that format |
| --- | --- |
| PNG | The 8-byte PNG signature, then a first chunk of length 13 and type `IHDR` whose CRC-32 is correct, with a bit depth legal for its color type (0: 1/2/4/8/16; 2, 4, 6: 8/16; 3: 1/2/4/8), compression method 0, filter method 0, and interlace method 0 or 1. Width and height come from IHDR. |
| JPEG | `FF D8`, then marker segments whose lengths stay inside the file, up to the first frame header. That frame header must be SOF0, SOF1 or SOF2 (baseline, extended sequential, progressive) with 8-bit precision, 1 or 3 components, a segment length of `8 + 3 x components`, and a nonzero height. Any other SOFn, a scan or end-of-image before the frame header, or a stray byte where a marker belongs makes the file unsupported. Width and height come from the frame header. |

| Rule | Reason |
| --- | --- |
| A file whose bytes are not the format its path requires. | `theme-unsupported-image` |
| Dimensions outside the path's range in the allowlist, including zero. | `theme-image-dimensions` |

These header checks bound what a device will be asked to decode; they do not
prove a file decodes. The launcher decodes on use and falls back to its next
artwork candidate when a file fails. The 1024 px art cap matches the launcher's
existing icon and wordmark cap. The 2048 px wallpaper cap exists because the
launcher decodes a wallpaper at full size into one texture, on a device with
1 GB of memory.

### Archive rules

| Rule | Reason |
| --- | --- |
| The archive file is at most 10,485,760 bytes (10 MiB). | `theme-archive-too-large` |
| A well-formed, single-volume zip without ZIP64: an end-of-central-directory record that ends the file, a central directory directly before it, local headers starting at offset 0 that repeat each central name and method and do not overlap, directory entries (names ending in `/`) with an uncompressed size of 0, and every entry's data decompressing to exactly its declared size with a matching CRC-32. | `theme-malformed-archive` |
| At most 512 entries, directory entries included. | `theme-too-many-entries` |
| Every entry is stored (method 0) or deflated (method 8), and none is encrypted (general purpose flag bit 0). | `theme-unsupported-compression` |
| The declared uncompressed sizes of all entries total at most 26,214,400 bytes (25 MiB). | `theme-uncompressed-too-large` |
| No entry's declared uncompressed size exceeds 100 times its compressed size. | `theme-compression-ratio` |
| Every entry name is non-empty, valid UTF-8, and free of control characters (U+0000-U+001F, U+007F), whatever the zip's UTF-8 flag says. | `theme-entry-name-encoding` |
| No name starts with `/` or a drive letter (`C:`). | `theme-absolute-path` |
| No name contains `\`. | `theme-backslash-path` |
| No path component is `.` or `..`. | `theme-path-traversal` |
| No path component starts with `.` (`.DS_Store`, `._theme.json`) or is `__MACOSX`. | `theme-hidden-file` |
| No entry is a symbolic link. For entries made on Unix, the file type in the high 16 bits of the external attributes is not `S_IFLNK`. | `theme-symlink` |
| No entry is another special file: that Unix file type is `S_IFREG`, `S_IFDIR`, or 0, and a directory type appears only on a name ending in `/` (and a regular type only on one that does not). | `theme-special-file` |
| No two entries have the same name after ignoring letter case and a trailing `/`. The card is FAT32, where those names are one file. The later entry is the duplicate. | `theme-duplicate-entry` |
| Every entry lives inside one top-level folder, and nothing sits beside it. An archive with no entries fails this rule. | `theme-not-single-folder` |
| The top-level folder name equals `theme.json` `id`, exactly. | `theme-id-mismatch` |
| `id` is not a reserved install name (below), compared case-insensitively. | `theme-reserved-name` |

The size, count and ratio limits are checked against the sizes the zip
**declares**, before any entry is decompressed. An installer must also stop
extracting any entry that produces more bytes than it declared; the integrity
stage below is where the reference validator does that.

### Reserved install names

A Leaf release ships bundled themes in `Themes/` and replaces each of them
wholesale on every install (`bundled-themes.txt`). A package may not use one of
their names as its `id`:

| Reserved name | Since |
| --- | --- |
| `Sample` | 0.12.0 |

Comparison ignores letter case because the card is FAT32. Adding a bundled
theme to a release adds its name here first.

### Validation order

A validator applies the rules in these stages. When stage 1, 2 or 3 fails,
validation stops and reports that stage's reasons only, so nothing is
decompressed from an archive whose declared sizes are over a limit. Stages 4
and 5 report every violation they find.

1. **Container.** Archive size (`theme-archive-too-large`), then zip structure
   (`theme-malformed-archive`) and entry count (`theme-too-many-entries`).
2. **Declared sizes.** `theme-unsupported-compression`,
   `theme-uncompressed-too-large`, `theme-compression-ratio`.
3. **Integrity.** Every entry decompresses to its declared size and CRC-32
   (`theme-malformed-archive`).
4. **Entries.** For each entry, the first of `theme-entry-name-encoding`,
   `theme-absolute-path`, `theme-backslash-path`, `theme-path-traversal`,
   `theme-hidden-file`, `theme-symlink`, `theme-special-file` that applies. An
   entry that breaks one of these is set aside and not checked further. Then
   `theme-duplicate-entry` over the remaining entries, in central directory
   order, setting duplicates aside the same way. Then
   `theme-not-single-folder`; when that fails, validation stops here. Then the
   allowlist over the remaining entries: `theme-unknown-file`,
   `theme-system-id-invalid`, `theme-reserved-system-id`, and
   `theme-multiple-wallpapers`.
5. **Contents.** `theme-missing-manifest`, `theme-manifest-too-large`,
   `theme-malformed-manifest`, then the field reasons from the `theme.json`
   table. When `schema` and `id` are both valid: `theme-id-mismatch` and
   `theme-reserved-name`. Then `theme-missing-preview`, and
   `theme-unsupported-image` or `theme-image-dimensions` for each image.

A package is accepted when no stage reports a reason.

### Warnings

A warning does not refuse anything. The store pipeline shows it to the
submitter and a reviewer.

| Condition | Warning |
| --- | --- |
| An icon (`icons/<ID>.png`, `grid/icons/<ID>.png` or `coverflow/icons/<ID>.png`) with valid dimensions that are not 512 x 512. The launcher draws it contain-fit. | `theme-icon-off-size` |
| No wallpaper, icon, label, or wordmark at all: a colors-only theme. | `theme-no-art` |

### Outside THEME-1

These rules belong to the store catalog and the submission pipeline, not to a
package, and a package validator cannot check them:

- `id` is unique across every Pak Rat lane (`apps`, `content`, `themes`).
- A Pak Rat `themes` catalog entry's `install_name` equals the theme's `id`,
  so it also equals the archive's top-level folder (the package rule
  `theme-id-mismatch`).
- Each published `version` of an `id` is greater than the last one.
- The GitHub account that first publishes an `id` owns it.
- The submitter confirms the right to share every image under the chosen
  license.

---

## Change procedure

`theme-v1` freezes when Leaf officially announces themes in Pak Rat. Until
then the contract is under active development and may change without a version
bump, and any theme published before the announcement can be withdrawn or
removed so development and testing can continue. Until the freeze:

1. Change this file first. It is the normative text.
2. Update the schema, the fixture generator, and the reference validator in
   [`../contracts/leaf-themes/`](../contracts/leaf-themes/) to match, and re-run
   `scripts/gen_theme_fixtures.py` and `scripts/validate_fixtures.py`.
3. Every new rejection rule gets **exactly one** invalid fixture whose recorded
   expectation names its reason slug.
4. A breaking change after freeze is a **new** version identifier
   (`theme-v2`, `"schema": 2`), never a mutation of v1. `schema` is refused
   rather than guessed at precisely so a v2 theme fails loudly on a v1 device.
   Fixtures that exercise `theme-unknown-schema` use a number no reader will
   ever accept (`99`), not the next version.
5. Adding a reserved install name is not a breaking change. It only refuses a
   new `id`; published themes already own theirs.

## Related

- [`../contracts/leaf-themes/`](../contracts/leaf-themes/) - schema, fixtures, and the reference validator
- [`content-paks.md`](content-paks.md) - CONTENT-ART-2, which shares the wordmark and icon conventions
