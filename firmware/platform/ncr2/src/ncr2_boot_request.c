#include "ncr2_boot_request.h"

#include <stdint.h>

#include "boot_recovery_request.h"
#include "boot_trial.h"
#include "ncr2_flash_layout.h"

_Static_assert(
    sizeof(boot_recovery_mailbox_t) <= NCR2_BOOT_MAILBOX_SIZE,
    "recovery mailbox exceeds its retained SRC GPR range");
_Static_assert(
    sizeof(boot_trial_mailbox_t) <=
        NCR2_BOOT_TRIAL_MAILBOX_SIZE,
    "trial mailbox exceeds its retained SRC GPR range");

void ncr2_boot_recovery_arm(void)
{
    boot_recovery_request_arm(
        (boot_recovery_mailbox_t *)(uintptr_t)
            NCR2_BOOT_MAILBOX_ADDRESS);
}

uint16_t ncr2_boot_confirm_healthy(void)
{
    return boot_trial_arm_confirmation(
        (boot_trial_mailbox_t *)(uintptr_t)
            NCR2_BOOT_TRIAL_MAILBOX_ADDRESS);
}
