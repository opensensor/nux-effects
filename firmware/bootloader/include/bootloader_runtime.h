#ifndef NCR2_BOOTLOADER_RUNTIME_H
#define NCR2_BOOTLOADER_RUNTIME_H

#include <stdint.h>

#include "boot_controller.h"
#include "boot_journal.h"
#include "boot_recovery_request.h"
#include "boot_trial.h"

typedef struct bootloader_runtime_services {
    const boot_journal_backend_t *journal;
    boot_recovery_request_t recovery;
    boot_trial_mailbox_t *trial_mailbox;
    void *watchdog_context;
    void (*start_trial_watchdog)(void *context);
    void *recovery_context;
    void (*enter_recovery)(
        void *context,
        const boot_controller_result_t *result);
    /*
     * Make the copied application visible to instruction fetch before the
     * handoff. The payload reaches SDRAM through the data path, so on a
     * core with a write-back D-cache the bytes can still be dirty in cache
     * while SDRAM holds stale data. The bootloader's own verification read
     * cannot detect that, because it reads back through the same cache.
     * Optional: host tests leave this NULL.
     */
    void (*sync_application_memory)(void);
} bootloader_runtime_services_t;

extern volatile uint32_t g_boot_diagnostic;

__attribute__((noreturn))
void bootloader_run(
    bootloader_runtime_services_t *services);

#endif
