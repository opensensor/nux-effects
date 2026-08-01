#include "ncr2_boot_request.h"

#include <stdint.h>

#include "boot_recovery_request.h"
#include "boot_trial.h"
#include "ncr2_flash_layout.h"

#define NCR2_SCB_AIRCR_ADDRESS UINT32_C(0xe000ed0c)
#define NCR2_SCB_AIRCR_VECTKEY UINT32_C(0x05fa0000)
#define NCR2_SCB_AIRCR_PRIGROUP_MASK UINT32_C(0x00000700)
#define NCR2_SCB_AIRCR_SYSRESETREQ UINT32_C(0x00000004)

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

void ncr2_boot_warm_reset(void)
{
    volatile uint32_t *const aircr =
        (volatile uint32_t *)(uintptr_t)
            NCR2_SCB_AIRCR_ADDRESS;

    /*
     * Match CMSIS NVIC_SystemReset without making the slot skeleton depend
     * on the full device SDK. Preserve only the priority grouping field;
     * VECTKEY authorizes the write and SYSRESETREQ performs a warm reset.
     */
    __asm volatile("dsb" ::: "memory");
    *aircr =
        (*aircr & NCR2_SCB_AIRCR_PRIGROUP_MASK) |
        NCR2_SCB_AIRCR_VECTKEY |
        NCR2_SCB_AIRCR_SYSRESETREQ;
    __asm volatile("dsb" ::: "memory");
    for (;;) {
        __asm volatile("wfi");
    }
}

uint16_t ncr2_boot_confirm_healthy_and_reset(void)
{
    const uint16_t status =
        ncr2_boot_confirm_healthy();

    if (status == BOOT_TRIAL_OK) {
        ncr2_boot_warm_reset();
    }
    return status;
}
