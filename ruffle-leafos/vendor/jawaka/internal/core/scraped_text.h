#ifndef JW_CORE_SCRAPED_TEXT_H
#define JW_CORE_SCRAPED_TEXT_H

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Encode one code point as UTF-8 at p; returns the bytes written (0 for an
   out-of-range or surrogate value, which the caller drops). */
static inline size_t jw__utf8_put(char *p, unsigned long cp) {
    if (cp < 0x80) {
        p[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        p[0] = (char)(0xC0 | (cp >> 6));
        p[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        if (cp >= 0xD800 && cp <= 0xDFFF) return 0;   /* lone surrogate */
        p[0] = (char)(0xE0 | (cp >> 12));
        p[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        p[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    if (cp <= 0x10FFFF) {
        p[0] = (char)(0xF0 | (cp >> 18));
        p[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        p[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        p[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* Make one field of scraped metadata safe to draw.
 *
 * ScreenScraper serves its text HTML-escaped and with real line breaks inside
 * it, and Leaf draws that text straight into a wrapped box. So `&quot;` reaches
 * the screen literally, and a newline reaches the font as an unmapped glyph and
 * comes out as tofu. This resolves both: entities decode to their characters,
 * and every control byte becomes a space, after which runs of whitespace
 * collapse and the ends are trimmed.
 *
 * In place. Every transformation shrinks or preserves length -- the shortest
 * entity that decodes to four UTF-8 bytes is eight characters -- so the result
 * always fits the buffer it arrived in. Decoded output is written past the read
 * cursor and never rescanned, so "&amp;quot;" decodes once, to `&quot;`, rather
 * than collapsing all the way to a quote.
 *
 * Paragraph breaks flatten to a single space: the info box is a few lines of
 * wrapped prose with no room to honour them.
 */
static inline void jw_clean_scraped_text(char *s) {
    if (!s) return;

    size_t w = 0;
    for (size_t i = 0; s[i]; ) {
        if (s[i] != '&') { s[w++] = s[i++]; continue; }

        /* Find the terminating ';' within the longest entity worth accepting. */
        size_t end = i + 1;
        while (s[end] && s[end] != ';' && end - i <= 10) end++;
        if (s[end] != ';') { s[w++] = s[i++]; continue; }

        const char *body = s + i + 1;
        size_t body_len = end - i - 1;
        unsigned long cp = 0;
        int ok = 0;

        if (body_len >= 2 && body[0] == '#') {
            int hex = (body[1] == 'x' || body[1] == 'X');
            const char *digits = body + (hex ? 2 : 1);
            /* strtoul would also take a sign or leading space, and an empty
               "&#x;" would read as zero; demand a digit up front instead. */
            int leads = (*digits >= '0' && *digits <= '9') ||
                        (hex && ((*digits >= 'a' && *digits <= 'f') ||
                                 (*digits >= 'A' && *digits <= 'F')));
            if (leads) {
                char *stop = NULL;
                cp = strtoul(digits, &stop, hex ? 16 : 10);
                ok = stop == s + end && cp != 0;
            }
        } else if (body_len == 4 && !strncmp(body, "quot", 4)) { cp = '"';  ok = 1; }
        else if (body_len == 3 && !strncmp(body, "amp", 3))    { cp = '&';  ok = 1; }
        else if (body_len == 2 && !strncmp(body, "lt", 2))     { cp = '<';  ok = 1; }
        else if (body_len == 2 && !strncmp(body, "gt", 2))     { cp = '>';  ok = 1; }
        else if (body_len == 4 && !strncmp(body, "apos", 4))   { cp = '\''; ok = 1; }
        else if (body_len == 4 && !strncmp(body, "nbsp", 4))   { cp = ' ';  ok = 1; }

        if (!ok) { s[w++] = s[i++]; continue; }

        size_t n = jw__utf8_put(s + w, cp);
        w += n;                 /* n == 0 drops an unencodable code point */
        i = end + 1;
    }
    s[w] = '\0';

    /* Control bytes are what render as tofu; fold them in with the spaces. */
    size_t out = 0;
    int pending_space = 0, started = 0;
    for (size_t i = 0; s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c < 0x20 || c == 0x7F || c == ' ') { pending_space = 1; continue; }
        if (pending_space && started) s[out++] = ' ';
        pending_space = 0;
        started = 1;
        s[out++] = (char)c;
    }
    s[out] = '\0';
}

#endif /* JW_CORE_SCRAPED_TEXT_H */
