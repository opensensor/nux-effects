#include <stddef.h>
#include <stdint.h>

#include "bootloader_runtime.h"
#include "ncr2_flash_layout.h"

static int metadata_read(
    void *context,
    uint32_t address,
    void *destination,
    uint32_t length)
{
    const uint8_t *source =
        (const uint8_t *)(uintptr_t)address;
    uint8_t *output = (uint8_t *)destination;

    (void)context;
    if (destination == NULL) {
        return -1;
    }
    for (uint32_t index = UINT32_C(0);
         index < length;
         ++index) {
        output[index] = source[index];
    }
    return 0;
}

static int metadata_mutation_disabled(
    void *context,
    uint32_t address,
    uint32_t length)
{
    (void)context;
    (void)address;
    (void)length;
    return -1;
}

static int metadata_program_disabled(
    void *context,
    uint32_t address,
    const void *source,
    uint32_t length)
{
    (void)context;
    (void)address;
    (void)source;
    (void)length;
    return -1;
}

void bootloader_main(void)
{
    boot_journal_backend_t journal = {
        .context = NULL,
        .read = metadata_read,
        .erase = metadata_mutation_disabled,
        .program = metadata_program_disabled,
    };
    bootloader_runtime_services_t services = {
        .journal = &journal,
        .recovery = {
            .mailbox =
                (boot_recovery_mailbox_t *)(uintptr_t)
                    NCR2_BOOT_MAILBOX_ADDRESS,
            .physical_context = NULL,
            .physical_asserted = NULL,
        },
        .trial_mailbox =
            (boot_trial_mailbox_t *)(uintptr_t)
                NCR2_BOOT_TRIAL_MAILBOX_ADDRESS,
        .watchdog_context = NULL,
        .start_trial_watchdog = NULL,
        .recovery_context = NULL,
        .enter_recovery = NULL,
    };

    bootloader_run(&services);
}
