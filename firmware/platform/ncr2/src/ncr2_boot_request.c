#include "ncr2_boot_request.h"

#include <stdint.h>

#include "boot_recovery_request.h"
#include "ncr2_flash_layout.h"

_Static_assert(
    sizeof(boot_recovery_mailbox_t) <= NCR2_BOOT_MAILBOX_SIZE,
    "recovery mailbox exceeds the reserved DTCM range");

void ncr2_boot_recovery_arm(void)
{
    boot_recovery_request_arm(
        (boot_recovery_mailbox_t *)(uintptr_t)
            NCR2_BOOT_MAILBOX_ADDRESS);
}
