#ifndef NCR2_BOOT_REQUEST_H
#define NCR2_BOOT_REQUEST_H

/*
 * Arms a one-shot request that the open bootloader consumes after the
 * next warm reset. Calling this function does not reset the processor.
 */
void ncr2_boot_recovery_arm(void);

#endif
