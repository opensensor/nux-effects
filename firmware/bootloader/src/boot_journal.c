#include "boot_journal.h"

#include <stddef.h>

static int backend_is_complete(const boot_journal_backend_t *backend)
{
    return backend != NULL &&
           backend->read != NULL &&
           backend->erase != NULL &&
           backend->program != NULL;
}

static int sequence_is_newer(uint32_t candidate, uint32_t current)
{
    return (int32_t)(candidate - current) > 0;
}

static int bytes_equal(const void *left, const void *right, size_t size)
{
    const uint8_t *left_bytes = (const uint8_t *)left;
    const uint8_t *right_bytes = (const uint8_t *)right;
    uint8_t difference = UINT8_C(0);

    for (size_t index = 0U; index < size; ++index) {
        difference |=
            (uint8_t)(left_bytes[index] ^ right_bytes[index]);
    }
    return difference == UINT8_C(0);
}

static int record_is_erased(const boot_record_t *record)
{
    const uint8_t *bytes = (const uint8_t *)record;

    for (size_t index = 0U; index < sizeof(*record); ++index) {
        if (bytes[index] != UINT8_C(0xFF)) {
            return 0;
        }
    }
    return 1;
}

static int state_equal(const boot_state_t *left,
                       const boot_state_t *right)
{
    return left->sequence == right->sequence &&
           left->confirmed_slot == right->confirmed_slot &&
           left->pending_slot == right->pending_slot &&
           left->trial_count == right->trial_count &&
           left->max_trials == right->max_trials &&
           left->flags == right->flags;
}

static uint32_t record_address(uint32_t base_address,
                               uint8_t sector,
                               uint16_t offset)
{
    return base_address +
           (uint32_t)sector * BOOT_RECORD_SECTOR_SIZE +
           offset;
}

static uint16_t read_record(const boot_journal_backend_t *backend,
                            uint32_t base_address,
                            uint8_t sector,
                            uint16_t offset,
                            boot_record_t *record)
{
    if (backend->read(
            backend->context,
            record_address(base_address, sector, offset),
            record,
            (uint32_t)sizeof(*record)) != 0) {
        return BOOT_JOURNAL_BACKEND_ERROR;
    }
    return BOOT_JOURNAL_OK;
}

static uint16_t find_append_offset(
    const boot_journal_backend_t *backend,
    uint32_t base_address,
    uint8_t sector,
    uint16_t *append_offset)
{
    boot_record_t record;

    for (uint16_t offset = UINT16_C(0);
         offset < BOOT_RECORD_SECTOR_SIZE;
         offset = (uint16_t)(offset + BOOT_RECORD_SIZE)) {
        uint16_t status =
            read_record(
                backend,
                base_address,
                sector,
                offset,
                &record);
        if (status != BOOT_JOURNAL_OK) {
            return status;
        }
        if (record_is_erased(&record)) {
            *append_offset = offset;
            return BOOT_JOURNAL_OK;
        }
    }
    *append_offset = (uint16_t)BOOT_RECORD_SECTOR_SIZE;
    return BOOT_JOURNAL_OK;
}

uint16_t boot_journal_load(const boot_journal_backend_t *backend,
                           uint32_t base_address,
                           boot_state_t *state,
                           boot_journal_location_t *location)
{
    boot_state_t newest;
    boot_journal_location_t newest_location = {
        .offset = UINT16_C(0),
        .sector = UINT8_C(0),
        .found = UINT8_C(0),
    };

    if (!backend_is_complete(backend) ||
        state == NULL ||
        location == NULL) {
        return BOOT_JOURNAL_BACKEND_ERROR;
    }
    boot_state_default(&newest);
    for (uint8_t sector = UINT8_C(0);
         sector < BOOT_JOURNAL_SECTOR_COUNT;
         ++sector) {
        for (uint16_t offset = UINT16_C(0);
             offset < BOOT_RECORD_SECTOR_SIZE;
             offset = (uint16_t)(offset + BOOT_RECORD_SIZE)) {
            boot_record_t record;
            boot_state_t candidate;
            uint16_t status =
                read_record(
                    backend,
                    base_address,
                    sector,
                    offset,
                    &record);

            if (status != BOOT_JOURNAL_OK) {
                return status;
            }
            if (!boot_record_decode(&record, &candidate)) {
                continue;
            }
            if (newest_location.found == UINT8_C(0) ||
                sequence_is_newer(
                    candidate.sequence,
                    newest.sequence)) {
                newest = candidate;
                newest_location.offset = offset;
                newest_location.sector = sector;
                newest_location.found = UINT8_C(1);
            }
        }
    }
    *state = newest;
    *location = newest_location;
    return BOOT_JOURNAL_OK;
}

static uint16_t verify_record(
    const boot_journal_backend_t *backend,
    uint32_t base_address,
    uint8_t sector,
    uint16_t offset,
    const boot_record_t *expected)
{
    boot_record_t actual;
    boot_state_t decoded;
    uint16_t status =
        read_record(
            backend,
            base_address,
            sector,
            offset,
            &actual);

    if (status != BOOT_JOURNAL_OK) {
        return status;
    }
    if (!bytes_equal(&actual, expected, sizeof(actual)) ||
        !boot_record_decode(&actual, &decoded)) {
        return BOOT_JOURNAL_VERIFY_FAILED;
    }
    return BOOT_JOURNAL_OK;
}

static uint16_t program_record(
    const boot_journal_backend_t *backend,
    uint32_t base_address,
    uint8_t sector,
    uint16_t offset,
    const boot_record_t *record)
{
    int program_status =
        backend->program(
            backend->context,
            record_address(base_address, sector, offset),
            record,
            (uint32_t)sizeof(*record));
    uint16_t verify_status =
        verify_record(
            backend,
            base_address,
            sector,
            offset,
            record);

    if (verify_status == BOOT_JOURNAL_OK) {
        return BOOT_JOURNAL_OK;
    }
    if (program_status != 0) {
        return BOOT_JOURNAL_BACKEND_ERROR;
    }
    return verify_status;
}

uint16_t boot_journal_store(const boot_journal_backend_t *backend,
                            uint32_t base_address,
                            const boot_state_t *state)
{
    boot_record_t encoded;
    boot_state_t validated;
    boot_state_t current;
    boot_journal_location_t location;
    uint16_t append_offset;
    uint16_t status;
    uint8_t active_sector;

    if (!backend_is_complete(backend) || state == NULL) {
        return BOOT_JOURNAL_BACKEND_ERROR;
    }
    boot_record_encode(state, &encoded);
    if (!boot_record_decode(&encoded, &validated) ||
        !state_equal(state, &validated)) {
        return BOOT_JOURNAL_INVALID_STATE;
    }

    status =
        boot_journal_load(
            backend,
            base_address,
            &current,
            &location);
    if (status != BOOT_JOURNAL_OK) {
        return status;
    }
    if (location.found != UINT8_C(0)) {
        if (state_equal(state, &current)) {
            return BOOT_JOURNAL_OK;
        }
        if (!sequence_is_newer(
                state->sequence,
                current.sequence)) {
            return BOOT_JOURNAL_STALE_SEQUENCE;
        }
        active_sector = location.sector;
    } else {
        active_sector = UINT8_C(0);
    }

    status =
        find_append_offset(
            backend,
            base_address,
            active_sector,
            &append_offset);
    if (status != BOOT_JOURNAL_OK) {
        return status;
    }
    if (append_offset < BOOT_RECORD_SECTOR_SIZE) {
        return program_record(
            backend,
            base_address,
            active_sector,
            append_offset,
            &encoded);
    }

    {
        const uint8_t target_sector =
            (uint8_t)(active_sector ^ UINT8_C(1));
        const uint32_t target_address =
            base_address +
            (uint32_t)target_sector * BOOT_RECORD_SECTOR_SIZE;
        const uint32_t old_address =
            base_address +
            (uint32_t)active_sector * BOOT_RECORD_SECTOR_SIZE;

        if (backend->erase(
                backend->context,
                target_address,
                BOOT_RECORD_SECTOR_SIZE) != 0) {
            return BOOT_JOURNAL_BACKEND_ERROR;
        }
        status =
            program_record(
                backend,
                base_address,
                target_sector,
                UINT16_C(0),
                &encoded);
        if (status != BOOT_JOURNAL_OK) {
            return status;
        }

        /*
         * The new record is already durable. Failure or power loss while
         * cleaning the old sector is safe because scanning both sectors
         * selects the newer sequence.
         */
        (void)backend->erase(
            backend->context,
            old_address,
            BOOT_RECORD_SECTOR_SIZE);
    }
    return BOOT_JOURNAL_OK;
}
