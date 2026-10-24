#!/bin/sh
# Print RetroArch's config_file_strip_comment and config_file_extract_value,
# byte for byte, from libretro-common/file/config_file.c. Both the vendored
# copy (third_party/retroarch/config_file_parser.inc) and the drift check use
# this, so "verbatim" means one thing.
set -eu
src=${1:?usage: $0 path/to/libretro-common/file/config_file.c}
awk '
/^static char \*config_file_strip_comment\(char \*str\)$/ { on = 1 }
/^static char \*config_file_extract_value\(char \*line\)$/ { on = 1 }
on { print }
on && /^}$/ { on = 0; print "" }
' "$src"
