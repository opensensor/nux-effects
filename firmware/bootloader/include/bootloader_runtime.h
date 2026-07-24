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
} bootloader_runtime_services_t;

extern volatile uint32_t g_boot_diagnostic;

__attribute__((noreturn))
void bootloader_run(
    bootloader_runtime_services_t *services);

#endif
