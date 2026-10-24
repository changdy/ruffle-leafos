#ifndef JW_POWER_HARD_CUT_H
#define JW_POWER_HARD_CUT_H

#include <stdbool.h>

/* Force-off hold time: how long the power button must stay down before the
   PMIC cuts power regardless of software (Settings > System > Force Off Hold).

   On the MLP1 the RK817 keeps it in register 0xf7 (PWRON_KEY), bits 5:4:
   00 = 6 s, 01 = 8 s, 10 = 10 s, 11 = 12 s. Bit 6 picks the action (0 off,
   1 restart) and bits 3:0 are unrelated; both are left alone. U-Boot rewrites
   the register from the device tree on every boot, so the daemon re-applies
   the setting after each start. This unit holds the pure parts (value
   mapping, parse, read-modify-write on a byte); the I2C access lives in the
   MLP1 platform backend. */

#define JW_POWER_HARD_CUT_SETTING_KEY "power_hard_cut_seconds"
#define JW_POWER_HARD_CUT_DEFAULT_S   10   /* also for a missing or invalid value */
#define JW_POWER_HARD_CUT_STOCK_S     6    /* what U-Boot programs; assume it after a failed apply */

#define JW_POWER_HARD_CUT_REG        0xf7
#define JW_POWER_HARD_CUT_SHIFT      4
#define JW_POWER_HARD_CUT_MASK       (0x3 << JW_POWER_HARD_CUT_SHIFT)
#define JW_POWER_HARD_CUT_OPTION_COUNT 4

/* The four hold times the PMIC offers, in register-bits order (6, 8, 10, 12). */
extern const int jw_power_hard_cut_options_s[JW_POWER_HARD_CUT_OPTION_COUNT];

bool jw_power_hard_cut_valid_s(int seconds);

/* Stored setting -> seconds. NULL, empty, non-numeric, trailing junk and any
   value the PMIC does not offer all read as JW_POWER_HARD_CUT_DEFAULT_S. */
int  jw_power_hard_cut_parse(const char *value);

/* seconds -> bits 5:4 value (0..3), or -1 when seconds is not an option. */
int  jw_power_hard_cut_bits(int seconds);

/* Register byte -> the hold time it selects. */
int  jw_power_hard_cut_seconds_from_reg(unsigned char reg);

/* The byte to write so that `reg` selects `seconds`: bits 5:4 replaced,
   bit 6 and bits 3:0 kept. Returns -1 when seconds is not an option. */
int  jw_power_hard_cut_apply_reg(unsigned char reg, int seconds);

#endif /* JW_POWER_HARD_CUT_H */
