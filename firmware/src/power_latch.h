#ifndef POWER_LATCH_H
#define POWER_LATCH_H

/* Battery power latch (TCA9554 I/O expander pin 6, system I2C bus).
 * The PWR button only feeds power while held; the firmware must latch it. */

#ifdef __cplusplus
extern "C" {
#endif

/* Engage the latch. Call as early as possible after I2C init, while the
 * user is still holding the PWR button. */
void power_latch_init(void);

/* Release the latch (powers the board off when running on battery). */
void power_latch_off(void);

/* Call from loop(): powers off after the PWR button (GPIO16) is held ~3s.
 * Armed only after the boot press is first released. */
void power_latch_poll(void);

#ifdef __cplusplus
}
#endif

#endif
