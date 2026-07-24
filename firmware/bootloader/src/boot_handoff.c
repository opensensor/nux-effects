#include "boot_handoff.h"

#include <stddef.h>

uint16_t boot_handoff_prepare(
    const boot_handoff_services_t *services,
    const boot_controller_result_t *result)
{
    uint16_t status;

    if (services == NULL ||
        services->trial_mailbox == NULL ||
        result == NULL) {
        return BOOT_HANDOFF_INVALID_ARGUMENT;
    }
    if (result->action != BOOT_CONTROLLER_HANDOFF) {
        boot_trial_clear(services->trial_mailbox);
        return BOOT_HANDOFF_NO_APPLICATION;
    }
    if (result->state.pending_slot != result->selected_slot) {
        boot_trial_clear(services->trial_mailbox);
        return BOOT_HANDOFF_CONFIRMED;
    }

    status =
        boot_trial_publish(
            services->trial_mailbox,
            result->selected_slot,
            result->state.sequence);
    if (status != BOOT_TRIAL_OK) {
        return BOOT_HANDOFF_INVALID_ARGUMENT;
    }
    if (services->start_trial_watchdog != NULL) {
        services->start_trial_watchdog(
            services->watchdog_context);
    }
    return BOOT_HANDOFF_TRIAL;
}
