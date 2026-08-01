#ifndef NCR2_BOOT_REQUEST_H
#define NCR2_BOOT_REQUEST_H

#include <stdint.h>

/*
 * Arms a one-shot request that the open bootloader consumes after the
 * next warm reset. Calling this function does not reset the processor.
 */
void ncr2_boot_recovery_arm(void);

/* Requests a Cortex-M7 warm system reset and does not return. */
void ncr2_boot_warm_reset(void);

/*
 * Converts a valid pending-image handoff token into a confirmation token.
 * The application must warm-reset only after this succeeds.
 */
uint16_t ncr2_boot_confirm_healthy(void);

/*
 * Confirms a pending image and immediately warm-resets so the bootloader can
 * commit it. On an ordinary confirmed boot there is no handoff token, so the
 * function returns BOOT_TRIAL_NO_HANDOFF instead of resetting.
 */
uint16_t ncr2_boot_confirm_healthy_and_reset(void);

#endif
