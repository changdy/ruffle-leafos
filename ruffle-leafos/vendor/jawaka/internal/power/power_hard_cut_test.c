#include "internal/power/power_hard_cut.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "power-hard-cut-test: FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

int main(void) {
    /* seconds -> bits 5:4, exactly the four the PMIC offers. */
    CHECK(jw_power_hard_cut_bits(6) == 0);
    CHECK(jw_power_hard_cut_bits(8) == 1);
    CHECK(jw_power_hard_cut_bits(10) == 2);
    CHECK(jw_power_hard_cut_bits(12) == 3);
    CHECK(jw_power_hard_cut_bits(0) == -1);
    CHECK(jw_power_hard_cut_bits(7) == -1);
    CHECK(jw_power_hard_cut_bits(14) == -1);
    CHECK(jw_power_hard_cut_bits(-6) == -1);
    CHECK(jw_power_hard_cut_valid_s(10) && !jw_power_hard_cut_valid_s(9));

    /* Stored value parse: the default (10) covers missing and garbage alike. */
    CHECK(jw_power_hard_cut_parse("6") == 6);
    CHECK(jw_power_hard_cut_parse("8") == 8);
    CHECK(jw_power_hard_cut_parse("10") == 10);
    CHECK(jw_power_hard_cut_parse("12") == 12);
    CHECK(jw_power_hard_cut_parse(" 12 ") == 12);
    CHECK(jw_power_hard_cut_parse(NULL) == 10);
    CHECK(jw_power_hard_cut_parse("") == 10);
    CHECK(jw_power_hard_cut_parse("0") == 10);
    CHECK(jw_power_hard_cut_parse("7") == 10);
    CHECK(jw_power_hard_cut_parse("-8") == 10);
    CHECK(jw_power_hard_cut_parse("abc") == 10);
    CHECK(jw_power_hard_cut_parse("8s") == 10);
    CHECK(jw_power_hard_cut_parse("12 sec") == 10);
    CHECK(jw_power_hard_cut_parse("99999999999999999999") == 10);
    CHECK(JW_POWER_HARD_CUT_DEFAULT_S == 10);
    CHECK(strcmp(JW_POWER_HARD_CUT_SETTING_KEY, "power_hard_cut_seconds") == 0);

    /* Register decode. */
    CHECK(jw_power_hard_cut_seconds_from_reg(0x06) == 6);
    CHECK(jw_power_hard_cut_seconds_from_reg(0x16) == 8);
    CHECK(jw_power_hard_cut_seconds_from_reg(0x26) == 10);
    CHECK(jw_power_hard_cut_seconds_from_reg(0x36) == 12);
    CHECK(jw_power_hard_cut_seconds_from_reg(0xff) == 12);
    CHECK(jw_power_hard_cut_seconds_from_reg(0xcf) == 6);

    /* Read-modify-write: the measured stock byte 0x06 becomes 0x26 for 10 s. */
    CHECK(jw_power_hard_cut_apply_reg(0x06, 10) == 0x26);
    CHECK(jw_power_hard_cut_apply_reg(0x06, 6) == 0x06);
    CHECK(jw_power_hard_cut_apply_reg(0x06, 8) == 0x16);
    CHECK(jw_power_hard_cut_apply_reg(0x06, 12) == 0x36);
    CHECK(jw_power_hard_cut_apply_reg(0x26, 6) == 0x06);
    /* Bit 6 (restart action), bit 7 and bits 3:0 travel through untouched. */
    CHECK(jw_power_hard_cut_apply_reg(0xcf, 10) == 0xef);
    CHECK(jw_power_hard_cut_apply_reg(0x4a, 6) == 0x4a);
    CHECK(jw_power_hard_cut_apply_reg(0x7f, 8) == 0x5f);
    CHECK(jw_power_hard_cut_apply_reg(0x06, 7) == -1);
    CHECK(jw_power_hard_cut_apply_reg(0x06, 0) == -1);
    /* Applying what the register already selects is a no-op byte. */
    for (int i = 0; i < JW_POWER_HARD_CUT_OPTION_COUNT; i++) {
        int s = jw_power_hard_cut_options_s[i];
        int reg = jw_power_hard_cut_apply_reg(0x06, s);
        CHECK(reg >= 0 && jw_power_hard_cut_apply_reg((unsigned char)reg, s) == reg);
        CHECK(jw_power_hard_cut_seconds_from_reg((unsigned char)reg) == s);
    }

    if (failures) {
        fprintf(stderr, "power-hard-cut-test: %d failure(s)\n", failures);
        return 1;
    }
    printf("power-hard-cut-test: ok\n");
    return 0;
}
