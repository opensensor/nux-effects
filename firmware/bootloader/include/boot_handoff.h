#ifndef NCR2_BOOT_HANDOFF_H
#define NCR2_BOOT_HANDOFF_H

#include <stdint.h>

#include "boot_controller.h"
#include "boot_trial.h"

enum boot_handoff_status {
    BOOT_HANDOFF_CONFIRMED = 0,
    BOOT_HANDOFF_TRIAL = 1,
    BOOT_HANDOFF_NO_APPLICATION = 2,
    BOOT_HANDOFF_INVALID_ARGUMENT = 3,
};

typedef struct boot_handoff_services {
    boot_trial_mailbox_t *trial_mailbox;
    void *watchdog_context;
    void (*start_trial_watchdog)(void *context);
} boot_handoff_services_t;

uint16_t boot_handoff_prepare(
    const boot_handoff_services_t *services,
    const boot_controller_result_t *result);

#endif
