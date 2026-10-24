# Coverflow source artwork

`system_icons/` contains the source-size PNG masters retained for future layouts.
This `artwork/` tree is not included in launcher packages.

The runtime copies live in
[`res/themes/Jawaka-Coverflow/system_icons/`](../../res/themes/Jawaka-Coverflow/system_icons/)
and have a maximum dimension of 384 pixels, matching `JW_COVER_THUMB_MAX` in the
launcher. Resize copies of these masters when refreshing the runtime assets; do
not resize the masters in place.

See the runtime directory's
[`LICENSE-ASSETS.md`](../../res/themes/Jawaka-Coverflow/system_icons/LICENSE-ASSETS.md)
for attribution and provenance.
