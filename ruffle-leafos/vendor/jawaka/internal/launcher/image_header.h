/* Image dimensions from file headers, without decoding.
 *
 * Two strictness levels share one walker. Strict is THEME-1 (leaf-contracts
 * docs/themes.md, "Images"): what a store theme must be, and what the store's
 * reference validator accepts, byte for byte. Loose reads only what the
 * launcher needs to bound a decode, so a hand-made theme keeps drawing any file
 * its decoder accepts; it refuses nothing a decode would have survived except
 * a header it cannot find dimensions in. */
#ifndef JW_LAUNCHER_IMAGE_HEADER_H
#define JW_LAUNCHER_IMAGE_HEADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

/* PNG signature + IHDR. Strict also needs 33 bytes and checks the IHDR CRC,
   the bit depth for the color type, and the compression, filter and interlace
   methods; it reports zero dimensions as read (the caller's range check
   rejects them). Loose needs 24 bytes and fails on a zero dimension. */
bool jw_image_png_dims(const unsigned char *data, size_t len, bool strict,
                       unsigned *w, unsigned *h);

/* JPEG marker walk from SOI to the first frame header, reading `fp` from its
   start. Strict accepts only SOF0/1/2 with 8-bit precision, 1 or 3 components,
   a segment length of 8 + 3 x components and a nonzero height. Loose accepts
   any SOFn with nonzero dimensions. fp's position is left undefined. */
bool jw_image_jpeg_dims(FILE *fp, bool strict, unsigned *w, unsigned *h);

#endif
