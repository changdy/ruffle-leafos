# ROM visibility and game deletion

**Hide Game** in a game's **Options** menu removes that source/path identity
from ordinary browsing. **Settings > Games > Hidden Games** lets you restore
it, including when every game is hidden. ROMs, favorites, history, overrides,
saves and artwork remain in place.

The library stores hidden identities in a `hidden_roms` table added to schema
v6 without changing its version, so earlier Leaf releases and other readers
of `library.db` still open the library; they just ignore the preferences,
which return when you upgrade again. A library that a development build
stamped as v7 returns to v6 on its next writable open. A library from an
earlier release gains the table on its first writable open; on a card that is
already read-only, browsing works with nothing hidden and visibility changes
report the read-only card. Hide Game stores the game's own source/path with an
empty `member`. Scans retain preferences even when a file disappears.
Relocation moves the exact file's keys in its existing transaction. A missing
library row still appears in Hidden Games by its stored path so you can clear
its preference.

The v0.13.0 betas also had **Manage Discs**, which stored keys for individual
discs and M3U `archive.zip#member` selectors (a non-empty `member`). It was
removed in favor of whole-game Hide and Delete.
Those keys stay in the table. They no longer affect anything unless one
matches an indexed row, such as a disc the scanner lists on its own, which
then stays hidden like any game. Hidden Games lists every key, by path when
there's no row, and Unhide clears it.

## Browse query audit

| Surface | Visibility boundary |
| --- | --- |
| Systems, Games and Focus Pick | `jw_db_list_systems`, `jw_db_count_games_for_system`, `jw_db_list_games_for_system` filter before counts, ordering and limits. |
| Favorites and Recents | Both shared list queries filter before limits; their metadata remains stored. |
| Search | Both FTS and fallback searches, including pinyin candidates, exclude hidden games. App results are unchanged. |
| Game switcher | Recents use the shared query. Current-game injection checks the game's identity before adding a tile. Each in-game entry reloads the list. B Resume remains independent of selection, including an empty carousel. |
| Library summary and most-played titles | Browse counts, sample titles and ranked titles exclude hidden games. Lifetime statistics retain their historical totals. |
| Batch artwork scraping | System/game enumeration uses the shared browse queries. Explicit single-game requests still resolve by identity. |

These paths deliberately retain access to hidden games:

- `jw_db_get_game_by_id` and `jw_db_get_game_by_rom_path` serve identity lookup,
  launches, active-session metadata, and inspection.
- Focus's locked screen and Arrange resolve remembered IDs. Pick uses the
  filtered browser; its Clear action removes the remembered selection.
- Boot resume resolves its saved ROM path and can resume a hidden game.
- Scanning, relocation, legacy save migration and direct SQL consumers such as
  Central Scrutinizer retain their existing behavior.
- Daemon startup uses total library statistics to recognize a populated cache
  even when its visible game count is zero.

Hide/Unhide writes directly through the DB. The launcher refreshes its cached
lists, cursors, counts and navigation breadcrumb without changing the daemon's
library generation or requesting a scan. A read-only library returns the
existing storage result and opens the SD card warning with its repair action.

## Delete Game

**Options > Delete Game** deletes the files behind the selected game entry:
its launch file and every file that file names. For a multi-disc game that is
the `.m3u`, each disc it lists and each disc's tracks. Deleting only the
playlist wouldn't work: the next scan would list the discs again or, for
`Name (Disc N)` files, generate a new playlist. The row stays last in the
menu, after **Hide Game**. **Hidden Games** only unhides; to delete a hidden
game, unhide it first.

The confirmation shows the game, how many files and how much space, which
card, that deletion is permanent and might not delete every file the game
uses, and that saves, states and artwork are kept. **Files** lists each file
with its card and size. **Cancel** is selected first. Afterwards the screen
says how many files were deleted; if one fails it says how many of the total
were deleted and which file failed.

### What counts as the game's files

`internal/discovery/content.c` reads the launch file's M3U, CUE, GDI, TOC or
quoted CMD references, without executing commands, and follows nested
descriptors. Nothing else is read: not other playlists, not other cards'
ROM trees. It resolves references with the filesystem's own lookup rules, so
existing paths use their on-disk spelling and missing ones keep their
normalized identity. As in RetroArch, only a `#` directly after `.zip`, `.7z`
or `.apk` starts an archive member; any other `#` is part of the filename, as
in `Game #1 (Disc 1).cue`. Directives, comments and `|label` suffixes name no
files. References may not leave the mounted ROM roots or escape through a
symlink; an explicit absolute path to the other mounted card is followed. An
unreadable or malformed descriptor stops the preview, and the error names the
innermost descriptor.

Deletion never expands a title stem, unpacks a container or removes a folder.
Formats without a reader, such as `.uae`, `.ccd`, `.mds`, `.conf`, `.bat` or
`.sh`, delete only themselves, which is what "might not delete every file"
covers. PCSX ReARMed also reads same-name `.toc`, `.ccd`, `.mds`, `.cue`,
`.sub` and `.sbi` files beside an image; those aren't named by the image, so
they stay.

Files are not checked against other games. If two playlists list the same
disc, deleting one deletes the disc. If the scanner lists a subfolder disc on
its own as well as inside its parent playlist, deleting the playlist deletes
that disc and its row; deleting the disc's own row breaks the playlist. The
file list shows exactly what goes.

### Which games offer it

`jw_delete_supported()` decides from the catalog alone, so the menu stays
cheap. Every system offers Delete except:

- `ARCADE`, `MAME`, `MAME2003`, `MAME2010`, `NEOGEO`, `NAOMI` and
  `ATOMISWAVE`: deleting a parent ROM set breaks its clones, which the
  warning doesn't describe.
- `PORTS`: PortMaster manages ports, and deleting the script leaves the port.
- Content-pak systems (a non-empty catalog `provider`): the pak owns its
  content.
- Systems with a filename launch contract, such as EasyRPG: the game is a
  folder.

`AMIGA`, `DOS` and `PC98` previews add that progress stored inside a writable
game image is erased with that image.

### Daemon protocol

The launcher sends a source/path identity, never a list of paths.

1. `rom-delete-preview {source_id, rom_relpath}` runs on jawakad's main
   thread. It checks the read-only and busy gates, resolves the files and
   answers with them, their total size and a 16-hex-digit digest of their
   cards, paths, sizes and missing flags. jawakad keeps nothing afterwards,
   so closing the preview needs no request and a restart in between changes
   nothing.
2. `rom-delete {source_id, rom_relpath, digest}` starts a worker and answers
   `COMMITTING`. The worker resolves the files again and refuses a different
   digest ("Your game's files changed"). It then stops affected artwork
   scrapes, checks relocation reservations, removes the files and reconciles
   the library in one transaction. The connection polls `rom-delete-status`
   until `DONE` or `ERROR`.

Once removal starts it finishes even if the connection closes; jawakad still
publishes the library generation. The launcher then says it couldn't confirm
the result and refreshes. Nothing is retried automatically. Only one deletion
runs at a time, and while it runs scans, launches, relocation and unmounting
wait or refuse, as before.

Both the ROM card and the library card must be writable for the preview and
again for every removal. Read-only results open the existing SD card warning
and repair flow.

Files are removed payload first: tracks before the CUE that names them, every
disc before the playlist, the launch file last. Each removal's directory is
synced before the next, so an interruption leaves the launch file to retry
from. A file already gone at removal time counts as absent, not as an error,
and its row is reconciled too. An error stops removal and reports how far it
got. Deletion isn't atomic.

Reconciliation removes the rows for every removed or absent file, including
separately listed discs, along with their settings, favorites, recents and
Focus selections. Visibility keys are cleared for removed identities. The
same orphan/Focus cleanup is used by scan pruning. Re-adding a deleted ROM
doesn't recover that metadata.

## Checks

- `make content-test` covers descriptors, filesystem case behavior, path
  aliases, `#` in filenames, missing references and unsafe or malformed input.
  Run it on both case-sensitive and case-insensitive storage.
- `make schema-v6-test visibility-test relocation-test` covers migration, the
  unchanged v6 version (including a development v7 library), read-only
  libraries from earlier releases, visibility queries, metadata retention,
  rescans and relocation.
- `make visibility-ui-test settings-status-test game-switcher-test` exercises
  the real launcher/settings/menu handlers with disposable databases and SDL's
  dummy renderer, including empty lists, leftover beta disc keys, independent
  B Resume and the longest Options menu at a larger font size.
- `make delete-test` covers eligibility, digest stability, removal order for
  multi-disc games (including a disc in a subfolder), partial failure and
  retry, files that vanish before removal, symlinked launch files, CMD disk
  images, cross-card references and the deepest descriptor chain on
  jawakad's worker stack.
- `make deletion-db-test focus-test` covers exact-file reconciliation and
  transactional metadata/Focus cleanup, including later scan pruning.
- `make delete-ui-test delete-client-test` covers the real deletion screen,
  the preview on the next tick, initial Cancel selection, the file list, no
  replay after a refused or lost delete, read-only warnings, Hidden Games
  without Delete, malformed replies and a quick result followed by Back.
- `make rom-delete-ipc-smoke` exercises the daemon with synthetic ROMs: the
  two requests, the digest refusal, coordination, read-only cards, partial
  failure, a lost result, a restart between preview and delete, a separately
  listed disc, and a generated `(Disc N)` playlist that a rescan doesn't bring
  back.
- `WORKSPACE_ROOT=/path/to/UMRK make phase3-fixture-scan-smoke` checks that
  existing nested-folder grouping remains unchanged. Continue running
  `make storage-sources-test relocation-test` for source and reservation changes.

The design history is in
`umrk-workspace/plans/Jawaka/rom-visibility-and-deletion.md` and
`umrk-workspace/plans/Jawaka/rom-deletion-simplification.md`.
