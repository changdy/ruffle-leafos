#ifndef JW_CORE_TITLE_H
#define JW_CORE_TITLE_H

#include <stddef.h>

/* Strip ROM-name decorations from a display title: any (...) and [...] groups
   (region/dump/language tags like "(U)", "[!]", "(En,Fr,Es)") and the resulting
   extra whitespace. In place; the result is always shorter, so it can't overrun.
   Leaves the string untouched if stripping everything would empty it. Shared by
   the in-game menu header and the game-switcher title. */
static inline void jw_clean_rom_title(char *s) {
    if (!s) return;
    size_t j = 0;
    int depth = 0;
    for (size_t i = 0; s[i]; i++) {
        char c = s[i];
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') { if (depth) depth--; }
        else if (!depth) s[j++] = c;
    }
    s[j] = '\0';
    /* collapse runs of whitespace and trim both ends */
    size_t w = 0;
    int pending_space = 0, started = 0;
    for (size_t i = 0; s[i]; i++) {
        if (s[i] == ' ' || s[i] == '\t') { pending_space = 1; continue; }
        if (pending_space && started) s[w++] = ' ';
        pending_space = 0;
        started = 1;
        s[w++] = s[i];
    }
    if (w > 0) s[w] = '\0';
}

#endif /* JW_CORE_TITLE_H */
