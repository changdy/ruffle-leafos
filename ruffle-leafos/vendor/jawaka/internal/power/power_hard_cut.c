#include "internal/power/power_hard_cut.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>

const int jw_power_hard_cut_options_s[JW_POWER_HARD_CUT_OPTION_COUNT] = { 6, 8, 10, 12 };

bool jw_power_hard_cut_valid_s(int seconds) {
    return jw_power_hard_cut_bits(seconds) >= 0;
}

int jw_power_hard_cut_parse(const char *value) {
    if (!value || !value[0]) return JW_POWER_HARD_CUT_DEFAULT_S;
    char *end = NULL;
    errno = 0;
    long seconds = strtol(value, &end, 10);
    if (errno != 0 || end == value) return JW_POWER_HARD_CUT_DEFAULT_S;
    while (*end && isspace((unsigned char)*end)) end++;
    if (*end) return JW_POWER_HARD_CUT_DEFAULT_S;   /* "10s", "10 sec", ... */
    if (seconds < 0 || seconds > 1000) return JW_POWER_HARD_CUT_DEFAULT_S;
    return jw_power_hard_cut_valid_s((int)seconds) ? (int)seconds
                                                   : JW_POWER_HARD_CUT_DEFAULT_S;
}

int jw_power_hard_cut_bits(int seconds) {
    for (int i = 0; i < JW_POWER_HARD_CUT_OPTION_COUNT; i++) {
        if (jw_power_hard_cut_options_s[i] == seconds) return i;
    }
    return -1;
}

int jw_power_hard_cut_seconds_from_reg(unsigned char reg) {
    int bits = (reg & JW_POWER_HARD_CUT_MASK) >> JW_POWER_HARD_CUT_SHIFT;
    return jw_power_hard_cut_options_s[bits];
}

int jw_power_hard_cut_apply_reg(unsigned char reg, int seconds) {
    int bits = jw_power_hard_cut_bits(seconds);
    if (bits < 0) return -1;
    return (reg & ~JW_POWER_HARD_CUT_MASK) | (bits << JW_POWER_HARD_CUT_SHIFT);
}
