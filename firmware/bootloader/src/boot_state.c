#include "boot_state.h"

#include <stddef.h>

#include "crc32.h"

static int slot_is_valid(uint8_t slot)
{
    return slot == BOOT_SLOT_A || slot == BOOT_SLOT_B;
}

static int sequence_is_newer(uint32_t candidate, uint32_t current)
{
    return (int32_t)(candidate - current) > 0;
}

static int bytes_are_erased(const uint8_t *bytes, size_t size)
{
    for (size_t index = 0U; index < size; ++index) {
        if (bytes[index] != UINT8_C(0xFF)) {
            return 0;
        }
    }
    return 1;
}

void boot_state_default(boot_state_t *state)
{
    state->sequence = UINT32_C(0);
    state->confirmed_slot = BOOT_SLOT_A;
    state->pending_slot = BOOT_SLOT_NONE;
    state->trial_count = UINT8_C(0);
    state->max_trials = BOOT_DEFAULT_MAX_TRIALS;
    state->flags = UINT32_C(0);
    state->found_record = 0;
}

int boot_record_decode(const boot_record_t *record, boot_state_t *state)
{
    const uint32_t expected_crc =
        crc32_compute(record, offsetof(boot_record_t, crc32));

    if (record->magic != BOOT_RECORD_MAGIC ||
        record->format_version != BOOT_RECORD_FORMAT_VERSION ||
        record->record_size != BOOT_RECORD_SIZE ||
        !slot_is_valid(record->confirmed_slot) ||
        (record->pending_slot != BOOT_SLOT_NONE &&
         !slot_is_valid(record->pending_slot)) ||
        record->pending_slot == record->confirmed_slot ||
        record->max_trials == UINT8_C(0) ||
        record->trial_count > record->max_trials ||
        record->crc32 != expected_crc) {
        return 0;
    }

    state->sequence = record->sequence;
    state->confirmed_slot = record->confirmed_slot;
    state->pending_slot = record->pending_slot;
    state->trial_count = record->trial_count;
    state->max_trials = record->max_trials;
    state->flags = record->flags;
    state->found_record = 1;
    return 1;
}

void boot_record_encode(const boot_state_t *state, boot_record_t *record)
{
    record->magic = BOOT_RECORD_MAGIC;
    record->format_version = BOOT_RECORD_FORMAT_VERSION;
    record->record_size = BOOT_RECORD_SIZE;
    record->sequence = state->sequence;
    record->confirmed_slot = state->confirmed_slot;
    record->pending_slot = state->pending_slot;
    record->trial_count = state->trial_count;
    record->max_trials = state->max_trials;
    record->flags = state->flags;
    for (size_t index = 0U; index < sizeof(record->reserved); ++index) {
        record->reserved[index] = UINT8_C(0);
    }
    record->crc32 =
        crc32_compute(record, offsetof(boot_record_t, crc32));
}

static void scan_sector(const boot_record_t *records, boot_state_t *state)
{
    for (size_t index = 0U; index < BOOT_RECORDS_PER_SECTOR; ++index) {
        boot_state_t candidate;
        if (!boot_record_decode(&records[index], &candidate)) {
            continue;
        }
        if (!state->found_record ||
            sequence_is_newer(candidate.sequence, state->sequence)) {
            *state = candidate;
        }
    }
}

void boot_state_scan(const void *sector_a,
                     const void *sector_b,
                     boot_state_t *state)
{
    boot_state_default(state);
    scan_sector((const boot_record_t *)sector_a, state);
    scan_sector((const boot_record_t *)sector_b, state);
}

uint8_t boot_state_selected_slot(const boot_state_t *state)
{
    if (state->pending_slot != BOOT_SLOT_NONE &&
        state->trial_count < state->max_trials) {
        return state->pending_slot;
    }
    return state->confirmed_slot;
}

void boot_state_begin_update(boot_state_t *state, uint8_t slot)
{
    if (!slot_is_valid(slot) || slot == state->confirmed_slot) {
        return;
    }
    ++state->sequence;
    state->pending_slot = slot;
    state->trial_count = UINT8_C(0);
    state->max_trials = BOOT_DEFAULT_MAX_TRIALS;
}

void boot_state_record_trial(boot_state_t *state)
{
    if (state->pending_slot == BOOT_SLOT_NONE ||
        state->trial_count >= state->max_trials) {
        return;
    }
    ++state->sequence;
    ++state->trial_count;
}

void boot_state_confirm_pending(boot_state_t *state)
{
    if (state->pending_slot == BOOT_SLOT_NONE) {
        return;
    }
    ++state->sequence;
    state->confirmed_slot = state->pending_slot;
    state->pending_slot = BOOT_SLOT_NONE;
    state->trial_count = UINT8_C(0);
}

void boot_state_rollback(boot_state_t *state)
{
    if (state->pending_slot == BOOT_SLOT_NONE) {
        return;
    }
    ++state->sequence;
    state->pending_slot = BOOT_SLOT_NONE;
    state->trial_count = UINT8_C(0);
}

size_t boot_state_find_append_offset(const void *sector)
{
    const uint8_t *bytes = (const uint8_t *)sector;
    for (size_t index = 0U; index < BOOT_RECORDS_PER_SECTOR; ++index) {
        const size_t offset = index * BOOT_RECORD_SIZE;
        if (bytes_are_erased(&bytes[offset], BOOT_RECORD_SIZE)) {
            return offset;
        }
    }
    return BOOT_RECORD_SECTOR_SIZE;
}

