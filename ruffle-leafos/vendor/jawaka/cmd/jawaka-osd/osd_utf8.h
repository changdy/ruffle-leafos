#ifndef JW_OSD_UTF8_H
#define JW_OSD_UTF8_H

#include <stddef.h>

/* Byte boundaries for shortening translated text. The OSD shortens by bytes
   into fixed buffers; cutting inside a multibyte sequence would hand the font
   renderer invalid UTF-8, which it renders as nothing at all. */

/* Length of the longest prefix of s[0, len) that does not end inside a
   multibyte sequence. Malformed trailing bytes are left as they are. */
static inline size_t jw_osd_utf8_boundary(const char *s, size_t len) {
    if (!s) return 0;
    size_t continuation = 0;
    for (size_t i = len; i > 0 && continuation < 4; i--) {
        unsigned char c = (unsigned char)s[i - 1];
        if ((c & 0xC0) == 0x80) {
            continuation++;
            continue;
        }
        size_t need = c < 0x80 ? 1
                    : (c & 0xE0) == 0xC0 ? 2
                    : (c & 0xF0) == 0xE0 ? 3
                    : (c & 0xF8) == 0xF0 ? 4 : 1;
        return continuation + 1 < need ? i - 1 : len;
    }
    return len;
}

/* Start of the character before byte `pos`. */
static inline size_t jw_osd_utf8_prev(const char *s, size_t pos) {
    if (!s || pos == 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

#endif /* JW_OSD_UTF8_H */
