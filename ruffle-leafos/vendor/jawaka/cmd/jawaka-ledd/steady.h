/* jawaka-ledd's write policy for frames that change rarely or never: static,
 * off, and Battery Level. Pure logic, shared by main.c and battery_test.c.
 *
 * Every mmrgball write is ~44 interrupts on the LED's I2C bus and ~20 helper
 * wakeups even when nothing changed, so these effects run a slow pass and
 * write only when the frame changes, after a resume (the chip may have lost
 * its registers in suspend), or on a slow safety refresh. On the MLP1 that
 * measured 0.6 helper wakeups/s for Battery Level against 22.6 for a Static
 * that rewrote its frame every second. */
#ifndef JW_LEDD_STEADY_H
#define JW_LEDD_STEADY_H

#include <stdbool.h>
#include <stdint.h>

#define JW_LEDD_STEADY_PASS_MS  5000LL   /* one pass; Battery Level reads the charge on it */
#define JW_LEDD_REFRESH_MS      60000LL  /* rewrite an unchanged frame */

/* Whether this pass writes the ring. An unchanged frame is skipped; a resume
   writes it anyway, and the slow refresh covers anything else that might have
   touched the registers. */
static inline bool jw_ledd_should_write(bool have_written, uint32_t last,
                                        uint32_t next, bool resumed,
                                        long long since_write_ms) {
    if (!have_written || resumed || next != last) return true;
    return since_write_ms >= JW_LEDD_REFRESH_MS;
}

#endif
