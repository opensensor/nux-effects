#include "boot_lifecycle.h"

static uint16_t load_state(
    const boot_journal_backend_t *backend,
    uint32_t metadata_address,
    boot_state_t *state)
{
    boot_journal_location_t location;

    if (boot_journal_load(
            backend,
            metadata_address,
            state,
            &location) != BOOT_JOURNAL_OK) {
        return BOOT_LIFECYCLE_JOURNAL_ERROR;
    }
    return BOOT_LIFECYCLE_OK;
}

static uint16_t store_state(
    const boot_journal_backend_t *backend,
    uint32_t metadata_address,
    const boot_state_t *state)
{
    if (boot_journal_store(
            backend,
            metadata_address,
            state) != BOOT_JOURNAL_OK) {
        return BOOT_LIFECYCLE_JOURNAL_ERROR;
    }
    return BOOT_LIFECYCLE_OK;
}

uint16_t boot_lifecycle_prepare(
    const boot_journal_backend_t *backend,
    uint32_t metadata_address,
    boot_state_t *state,
    uint8_t *selected_slot)
{
    uint16_t status;

    if (state == NULL || selected_slot == NULL) {
        return BOOT_LIFECYCLE_JOURNAL_ERROR;
    }
    status = load_state(backend, metadata_address, state);
    if (status != BOOT_LIFECYCLE_OK) {
        return status;
    }

    if (state->pending_slot == BOOT_SLOT_NONE) {
        *selected_slot = state->confirmed_slot;
        return BOOT_LIFECYCLE_OK;
    }
    if (state->trial_count >= state->max_trials) {
        boot_state_rollback(state);
        status =
            store_state(backend, metadata_address, state);
        if (status != BOOT_LIFECYCLE_OK) {
            return status;
        }
        *selected_slot = state->confirmed_slot;
        return BOOT_LIFECYCLE_OK;
    }

    boot_state_record_trial(state);
    status = store_state(backend, metadata_address, state);
    if (status != BOOT_LIFECYCLE_OK) {
        return status;
    }
    *selected_slot = state->pending_slot;
    return BOOT_LIFECYCLE_OK;
}

uint16_t boot_lifecycle_confirm(
    const boot_journal_backend_t *backend,
    uint32_t metadata_address,
    uint8_t running_slot,
    boot_state_t *state)
{
    uint16_t status;

    if (state == NULL) {
        return BOOT_LIFECYCLE_JOURNAL_ERROR;
    }
    status = load_state(backend, metadata_address, state);
    if (status != BOOT_LIFECYCLE_OK) {
        return status;
    }
    if (state->pending_slot == BOOT_SLOT_NONE) {
        return BOOT_LIFECYCLE_NOT_PENDING;
    }
    if (running_slot != state->pending_slot) {
        return BOOT_LIFECYCLE_WRONG_SLOT;
    }
    boot_state_confirm_pending(state);
    return store_state(backend, metadata_address, state);
}

uint16_t boot_lifecycle_reject_pending(
    const boot_journal_backend_t *backend,
    uint32_t metadata_address,
    boot_state_t *state)
{
    uint16_t status;

    if (state == NULL) {
        return BOOT_LIFECYCLE_JOURNAL_ERROR;
    }
    status = load_state(backend, metadata_address, state);
    if (status != BOOT_LIFECYCLE_OK) {
        return status;
    }
    if (state->pending_slot == BOOT_SLOT_NONE) {
        return BOOT_LIFECYCLE_NOT_PENDING;
    }
    boot_state_rollback(state);
    return store_state(backend, metadata_address, state);
}
