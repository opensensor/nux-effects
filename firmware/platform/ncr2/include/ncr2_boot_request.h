#ifndef NCR2_BOOT_REQUEST_H
#define NCR2_BOOT_REQUEST_H

#include <stdint.h>

/*
 * Arms a one-shot request that the open bootloader consumes after the
 * next warm reset. Calling this function does not reset the processor.
 */
void ncr2_boot_recovery_arm(void);

/*
 * Converts a valid pending-image handoff token into a confirmation token.
 * The application must warm-reset only after this succeeds.
 */
uint16_t ncr2_boot_confirm_healthy(void);

#endif
