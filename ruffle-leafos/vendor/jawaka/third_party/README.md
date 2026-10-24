# Jawaka Third-Party Dependencies

Jawaka keeps small vendored dependencies here when they are part of the current
build contract.

## Current Contents

- `cjson/` is vendored and compiled into the Jawaka binaries.
- `stb/` (stb_image, public domain/MIT), `miniz/` (MIT), and `md5/` (public
  domain, RFC 1321-based) are vendored for the ScreenScraper scrape engine
  (`internal/scrape/`): JPEG/PNG decode, PNG encode and zip access, and ROM
  hashing. Copied from Helaas's nextui-scrapegoat-pak (MIT).
- `retroarch/config_file_parser.inc` is **test-only**: RetroArch's
  `config_file_strip_comment` and `config_file_extract_value`, copied verbatim
  from libretro-common/file/config_file.c at v1.22.2 (MIT, notice in the file).
  `make retroarch-config-test` parses Jawaka's generated config with it, so a
  value such as a RetroAchievements password is checked against RetroArch's
  real rules. To re-vendor after a RetroArch bump, replace the text between the
  VERBATIM markers with the output of
  `scripts/retroarch-config-parser-extract.sh <RetroArch>/libretro-common/file/config_file.c`;
  `scripts/check-retroarch-config-parser.sh` (run by the test target) fails
  when the copy and a fetched tree differ.
- `catastrophe/` is a placeholder for a future submodule, but the active local
  workflow uses an adjacent `../Catastrophe` checkout or an explicit
  `CATASTROPHE_DIR`.

## Catastrophe Checkout

The root Makefile resolves Catastrophe in this order:

1. `CATASTROPHE_DIR`, when set.
2. `../Catastrophe`, when present.
3. `third_party/catastrophe`, for a future submodule checkout.

For normal UMRK sibling workspaces, no submodule setup is required:

```sh
export CATASTROPHE_DIR=../Catastrophe
make
```

If this repo is ever used outside the umbrella workspace, either set
`CATASTROPHE_DIR=/path/to/Catastrophe` or populate `third_party/catastrophe`
with the Catastrophe source.
