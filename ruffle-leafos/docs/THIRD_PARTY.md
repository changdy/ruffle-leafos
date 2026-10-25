# Runtime provenance

The current 0.7.0 executable is rebuilt from aweigit/ruffle-miyooflip commit 3bc39c5e0fdbc9fcde92681706395f6ef70f991b, with this project's Leaf frontend changes. It does not reproduce every private leaf-12 patch. Ruffle is MIT / Apache-2.0; see licenses/RUFFLE-LICENSE.md. Cargo.lock pins Rust dependencies.

The experimental Vulkan renderer reuses 54 unmodified files from the official ruffle-rs/ruffle `render/wgpu/` at commit 71f18f0d0e3bd118945d5956f4ac4bb2e486e856 (nightly-2026-03-15), under the same MIT / Apache-2.0 licenses. `vendor/ruffle-wgpu.lock.json` records their SHA-256 values and is included in package provenance. The Leaf GBM/DMA-BUF/EGL presentation bridge is this project's MIT code. Vulkan, EGL and GBM driver libraries are provided by the device; no system GPU driver is replaced or redistributed.

SDL2, ALSA and the CJK menu font are reused unchanged from the user-provided leaf-12 pak; their hashes remain in provenance/runtime.lock.json. On Leaf, the launcher preloads the system SDL2 because the bundled fallback does not provide Wayland on this target. SDL2 is zlib-licensed, ALSA is LGPL, and Source Han Sans is SIL OFL. Their source references are https://github.com/libsdl-org/SDL , https://github.com/alsa-project/alsa-lib and https://github.com/adobe-fonts/source-han-sans .

New frontend, wrappers, diagnostics, build scripts and the original demo use this project's MIT license. Additional dependency license files are collected from the pinned build cache into licenses/dependencies. No game SWFs from the user's library are distributed.

The 0.6.0 menu rendering follows the pinned Leaf/Jawaka source in `vendor/jawaka/cmd/jawaka-menu/main.c` (seven pinned reference files; see `vendor/jawaka.lock.json`) (`jw__render_ingame_menu`, underlay and focus helpers), Copyright (c) 2026 Jawaka contributors, MIT license. Its license is included as `licenses/JAWAKA-LICENSE.txt`. Theme colors are read from the device at runtime; no user's theme assets are redistributed.
