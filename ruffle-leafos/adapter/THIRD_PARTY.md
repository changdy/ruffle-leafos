# Runtime provenance

This private test package reuses the unchanged Ruffle executable, SDL2,
ALSA library and menu font from the user's existing `leaf-12` package.
The exact input files are recorded in `runtime.lock.json`; the original
package metadata is retained in `provenance/` in the installed pak.

The supplied package contains a binary and metadata, not its corresponding
custom frontend/renderer source. This project does not claim to rebuild that
runtime or establish its exact upstream source revision.

Upstream projects:

- Ruffle: https://github.com/ruffle-rs/ruffle (MIT / Apache-2.0)
- SDL2: https://github.com/libsdl-org/SDL (zlib)
- ALSA: https://github.com/alsa-project/alsa-lib (LGPL)
- Source Han Sans: https://github.com/adobe-fonts/source-han-sans (SIL OFL)

The new exit layer, launch wrapper, original test movie, icon and build tools
are covered by this project's LICENSE. The source archive contains those new
parts, not the unavailable source of the supplied custom runtime.

The ScummVM-pak repository was used as the reference for Leaf CONTENT-1
packaging and runtime-path conventions. No ScummVM binaries or game data are
included. Reference commits and compiler checksum are in references.lock.json.

Public base source candidate: aweigit/ruffle-miyooflip commit
3bc39c5e0fdbc9fcde92681706395f6ef70f991b (MIT / Apache-2.0).
It matches the supplied short revision and frontend paths but lacks the
Leaf-specific changes. The installed runtime is still the unchanged supplied
binary, not a rebuild of this candidate. See player-source.lock.json.
