#include "boot_controller.h"

#include <stddef.h>

#include "boot_lifecycle.h"

static uint8_t other_slot(uint8_t slot)
{
    return slot == BOOT_SLOT_A ? BOOT_SLOT_B : BOOT_SLOT_A;
}

static void result_default(boot_controller_result_t *result)
{
    boot_state_default(&result->state);
    result->journal_status = BOOT_JOURNAL_OK;
    result->slot_status = BOOT_CONTROLLER_SLOT_INVALID;
    result->action = BOOT_CONTROLLER_RECOVERY;
    result->reason = BOOT_CONTROLLER_REASON_INVALID_ARGUMENT;
    result->selected_slot = BOOT_SLOT_NONE;
    result->primary_slot = BOOT_SLOT_NONE;
    result->confirmation_status =
        BOOT_CONTROLLER_CONFIRMATION_NONE;
}

static uint16_t load_state_without_trial(
    const boot_controller_services_t *services,
    boot_controller_result_t *result)
{
    boot_journal_location_t location;

    result->journal_status =
        boot_journal_load(
            services->journal,
            services->metadata_address,
            &result->state,
            &location);
    return result->journal_status;
}

static int try_load(
    const boot_controller_services_t *services,
    boot_controller_result_t *result,
    uint8_t slot)
{
    result->slot_status =
        services->load_slot(services->context, slot);
    if (result->slot_status != BOOT_CONTROLLER_SLOT_OK) {
        return 0;
    }
    result->action = BOOT_CONTROLLER_HANDOFF;
    result->selected_slot = slot;
    return 1;
}

void boot_controller_run(
    const boot_controller_services_t *services,
    boot_controller_result_t *result)
{
    uint8_t selected_slot;
    uint8_t confirmation_slot = BOOT_SLOT_NONE;
    uint32_t confirmation_sequence = UINT32_C(0);
    int confirmation_available = 0;

    if (result == NULL) {
        return;
    }
    result_default(result);
    if (services == NULL ||
        services->journal == NULL ||
        services->load_slot == NULL) {
        return;
    }

    if (services->consume_confirmation != NULL) {
        confirmation_available =
            services->consume_confirmation(
                services->context,
                &confirmation_slot,
                &confirmation_sequence) != 0;
    }

    if (services->recovery_requested != NULL &&
        services->recovery_requested(services->context) != 0) {
        /*
         * A deliberate recovery entry must not count as an attempted
         * pending boot. Load state for recovery status only.
         */
        (void)load_state_without_trial(services, result);
        result->reason =
            BOOT_CONTROLLER_REASON_FORCED_RECOVERY;
        if (confirmation_available) {
            result->confirmation_status =
                BOOT_CONTROLLER_CONFIRMATION_IGNORED_FOR_RECOVERY;
        }
        return;
    }

    if (confirmation_available) {
        result->journal_status =
            load_state_without_trial(services, result);
        if (result->journal_status != BOOT_JOURNAL_OK) {
            result->reason =
                BOOT_CONTROLLER_REASON_JOURNAL_ERROR;
            return;
        }
        if (result->state.pending_slot == confirmation_slot &&
            result->state.sequence == confirmation_sequence) {
            result->journal_status =
                boot_lifecycle_confirm(
                    services->journal,
                    services->metadata_address,
                    confirmation_slot,
                    &result->state);
            if (result->journal_status != BOOT_LIFECYCLE_OK) {
                result->reason =
                    BOOT_CONTROLLER_REASON_JOURNAL_ERROR;
                return;
            }
            result->confirmation_status =
                BOOT_CONTROLLER_CONFIRMATION_ACCEPTED;
        } else {
            result->confirmation_status =
                BOOT_CONTROLLER_CONFIRMATION_STALE;
        }
    }

    result->journal_status =
        boot_lifecycle_prepare(
            services->journal,
            services->metadata_address,
            &result->state,
            &selected_slot);
    if (result->journal_status != BOOT_LIFECYCLE_OK) {
        result->reason = BOOT_CONTROLLER_REASON_JOURNAL_ERROR;
        return;
    }

    result->primary_slot = selected_slot;
    if (try_load(services, result, selected_slot)) {
        result->reason = BOOT_CONTROLLER_REASON_SELECTED;
        return;
    }

    if (result->state.pending_slot == selected_slot) {
        /*
         * A corrupt pending image cannot become a repeated boot loop.
         * Reject it durably before attempting the confirmed image.
         */
        result->journal_status =
            boot_lifecycle_reject_pending(
                services->journal,
                services->metadata_address,
                &result->state);
        if (result->journal_status != BOOT_LIFECYCLE_OK) {
            result->reason =
                BOOT_CONTROLLER_REASON_JOURNAL_ERROR;
            return;
        }
        selected_slot = result->state.confirmed_slot;
    } else {
        selected_slot = other_slot(selected_slot);
    }

    if (try_load(services, result, selected_slot)) {
        result->reason = BOOT_CONTROLLER_REASON_FALLBACK;
        return;
    }
    result->reason = BOOT_CONTROLLER_REASON_NO_VALID_SLOT;
}
