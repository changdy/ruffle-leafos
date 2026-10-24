#include "internal/launcher/image_header.h"

#include <stdint.h>
#include <string.h>

static uint32_t jw__ih_be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* CRC-32 (IEEE, reflected) over a handful of bytes; IHDR is 17. */
static uint32_t jw__ih_crc32(const unsigned char *p, size_t n) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool jw_image_png_dims(const unsigned char *data, size_t len, bool strict,
                       unsigned *w, unsigned *h) {
    static const unsigned char sig[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    if (w) *w = 0;
    if (h) *h = 0;
    if (!data || len < (strict ? 33u : 24u) || memcmp(data, sig, 8) != 0 ||
        memcmp(data + 12, "IHDR", 4) != 0)
        return false;
    uint32_t pw = jw__ih_be32(data + 16), ph = jw__ih_be32(data + 20);
    if (strict) {
        if (jw__ih_be32(data + 8) != 13u ||
            jw__ih_be32(data + 29) != jw__ih_crc32(data + 12, 17))
            return false;
        unsigned depth = data[24], color = data[25];
        bool depth_ok;
        switch (color) {
            case 0:  depth_ok = depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16; break;
            case 3:  depth_ok = depth == 1 || depth == 2 || depth == 4 || depth == 8; break;
            case 2: case 4: case 6: depth_ok = depth == 8 || depth == 16; break;
            default: depth_ok = false; break;
        }
        if (!depth_ok || data[26] != 0 || data[27] != 0 || data[28] > 1) return false;
    } else if (pw == 0 || ph == 0) {
        return false;
    }
    if (w) *w = pw;
    if (h) *h = ph;
    return true;
}

bool jw_image_jpeg_dims(FILE *fp, bool strict, unsigned *w, unsigned *h) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (!fp || fseek(fp, 0, SEEK_END) != 0) return false;
    long size = ftell(fp);
    if (size < 2 || fseek(fp, 0, SEEK_SET) != 0) return false;
    if (fgetc(fp) != 0xFF || fgetc(fp) != 0xD8) return false;

    long i = 2;
    while (i < size) {
        int c = fgetc(fp);
        if (c != 0xFF) return false;                 /* a stray byte where a marker belongs */
        i++;
        while (i < size && (c = fgetc(fp)) == 0xFF) i++;
        if (i >= size || c == EOF) return false;
        int marker = c;
        i++;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;   /* TEM, RSTn */
        bool sof = marker >= 0xC0 && marker <= 0xCF &&
                   marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
        bool supported = marker == 0xC0 || marker == 0xC1 || marker == 0xC2;
        if (marker == 0x00 || marker == 0xD8 || marker == 0xD9 || marker == 0xDA ||
            (sof && strict && !supported))
            return false;
        if (i + 2 > size) return false;
        int hi = fgetc(fp), lo = fgetc(fp);
        if (hi == EOF || lo == EOF) return false;
        long length = ((long)hi << 8) | lo;
        if (length < 2 || i + length > size) return false;
        if (sof) {
            unsigned char f[6];
            if (length < 8 || fread(f, 1, sizeof(f), fp) != sizeof(f)) return false;
            unsigned precision = f[0], components = f[5];
            unsigned fh = ((unsigned)f[1] << 8) | f[2];
            unsigned fw = ((unsigned)f[3] << 8) | f[4];
            if (strict ? (precision != 8 || (components != 1 && components != 3) ||
                          length != 8 + 3 * (long)components || fh == 0)
                       : (fw == 0 || fh == 0))
                return false;
            if (w) *w = fw;
            if (h) *h = fh;
            return true;
        }
        if (fseek(fp, length - 2, SEEK_CUR) != 0) return false;
        i += length;
    }
    return false;
}
