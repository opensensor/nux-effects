#include "ncr2_nor.h"

#include <stddef.h>

#include "ncr2_flash_layout.h"

static void diagnostics_begin(ncr2_nor_t *nor,
                              uint8_t operation,
                              uint32_t address,
                              uint32_t length)
{
    nor->diagnostics.magic = NCR2_NOR_DIAGNOSTIC_MAGIC;
    nor->diagnostics.version = NCR2_NOR_DIAGNOSTIC_VERSION;
    nor->diagnostics.operation = operation;
    nor->diagnostics.phase = NCR2_NOR_PHASE_VALIDATE;
    nor->diagnostics.address = address;
    nor->diagnostics.length = length;
    nor->diagnostics.completed_units = UINT32_C(0);
    nor->diagnostics.backend_status = 0;
    nor->diagnostics.status = NCR2_NOR_OK;
    nor->diagnostics.reserved = UINT16_C(0);
    nor->diagnostics.detail = UINT32_C(0);
}

static uint16_t diagnostics_finish(ncr2_nor_t *nor,
                                   uint16_t status)
{
    nor->diagnostics.status = status;
    if (status == NCR2_NOR_OK) {
        nor->diagnostics.phase = NCR2_NOR_PHASE_COMPLETE;
    }
    return status;
}

static int operations_are_complete(
    const ncr2_nor_operations_t *operations)
{
    return operations != NULL &&
           operations->read != NULL &&
           operations->erase_sector != NULL &&
           operations->program_page != NULL &&
           operations->sync_after_mutation != NULL;
}

static uint16_t flash_range(uint32_t address,
                            uint32_t length,
                            uint32_t *offset)
{
    uint32_t candidate;

    if (length == UINT32_C(0) ||
        address < NCR2_FLASH_XIP_BASE) {
        return NCR2_NOR_INVALID_ARGUMENT;
    }
    candidate = address - NCR2_FLASH_XIP_BASE;
    if (candidate >= NCR2_FLASH_SIZE ||
        length > NCR2_FLASH_SIZE - candidate) {
        return NCR2_NOR_OUT_OF_RANGE;
    }
    *offset = candidate;
    return NCR2_NOR_OK;
}

static int range_inside(uint32_t offset,
                        uint32_t length,
                        uint32_t region_offset,
                        uint32_t region_size)
{
    return offset >= region_offset &&
           offset - region_offset < region_size &&
           length <= region_size - (offset - region_offset);
}

static uint16_t mutation_range(const ncr2_nor_t *nor,
                               uint32_t address,
                               uint32_t length)
{
    uint32_t offset;
    uint16_t status = flash_range(address, length, &offset);

    if (status != NCR2_NOR_OK) {
        return status;
    }
    if (nor->full_flash_mutation != UINT8_C(0)) {
        return NCR2_NOR_OK;
    }
    if (range_inside(
            offset,
            length,
            NCR2_BOOT_METADATA_OFFSET,
            NCR2_BOOT_METADATA_SIZE) ||
        range_inside(
            offset,
            length,
            NCR2_APPLICATION_A_OFFSET,
            NCR2_APPLICATION_SLOT_SIZE) ||
        range_inside(
            offset,
            length,
            NCR2_APPLICATION_B_OFFSET,
            NCR2_APPLICATION_SLOT_SIZE)) {
        return NCR2_NOR_OK;
    }
    return NCR2_NOR_PROTECTED_RANGE;
}

static int bytes_equal(const uint8_t *left,
                       const uint8_t *right,
                       uint32_t length)
{
    uint8_t difference = UINT8_C(0);

    for (uint32_t index = UINT32_C(0);
         index < length;
         ++index) {
        difference |= (uint8_t)(left[index] ^ right[index]);
    }
    return difference == UINT8_C(0);
}

uint16_t ncr2_nor_init(ncr2_nor_t *nor,
                       const ncr2_nor_operations_t *operations)
{
    if (nor == NULL || !operations_are_complete(operations)) {
        return NCR2_NOR_INVALID_ARGUMENT;
    }
    nor->operations = *operations;
    nor->full_flash_mutation = UINT8_C(0);
    diagnostics_begin(
        nor,
        NCR2_NOR_OPERATION_NONE,
        UINT32_C(0),
        UINT32_C(0));
    nor->diagnostics.phase = NCR2_NOR_PHASE_NONE;
    return NCR2_NOR_OK;
}

uint16_t ncr2_nor_init_full_flash(
    ncr2_nor_t *nor,
    const ncr2_nor_operations_t *operations)
{
    uint16_t status = ncr2_nor_init(nor, operations);

    if (status == NCR2_NOR_OK) {
        nor->full_flash_mutation = UINT8_C(1);
    }
    return status;
}

uint16_t ncr2_nor_read(ncr2_nor_t *nor,
                       uint32_t address,
                       void *destination,
                       uint32_t length)
{
    uint32_t offset;
    uint16_t status;

    if (nor == NULL) {
        return NCR2_NOR_INVALID_ARGUMENT;
    }
    diagnostics_begin(
        nor, NCR2_NOR_OPERATION_READ, address, length);
    if (destination == NULL ||
        !operations_are_complete(&nor->operations)) {
        return diagnostics_finish(
            nor, NCR2_NOR_INVALID_ARGUMENT);
    }
    status = flash_range(address, length, &offset);
    if (status != NCR2_NOR_OK) {
        return diagnostics_finish(nor, status);
    }
    (void)offset;
    nor->diagnostics.phase = NCR2_NOR_PHASE_PREFLIGHT_READ;
    if (nor->operations.read(
            nor->operations.context,
            address,
            destination,
            length) != 0) {
        nor->diagnostics.backend_status =
            NCR2_NOR_BACKEND_INVALID;
        return diagnostics_finish(nor, NCR2_NOR_IO_ERROR);
    }
    return diagnostics_finish(nor, NCR2_NOR_OK);
}

static uint16_t verify_erased(ncr2_nor_t *nor,
                              uint32_t address)
{
    uint8_t buffer[NCR2_NOR_PAGE_SIZE];

    for (uint32_t offset = UINT32_C(0);
         offset < NCR2_NOR_SECTOR_SIZE;
         offset += NCR2_NOR_PAGE_SIZE) {
        if (nor->operations.read(
                nor->operations.context,
                address + offset,
                buffer,
                NCR2_NOR_PAGE_SIZE) != 0) {
            return NCR2_NOR_IO_ERROR;
        }
        for (uint32_t index = UINT32_C(0);
             index < NCR2_NOR_PAGE_SIZE;
             ++index) {
            if (buffer[index] != UINT8_C(0xFF)) {
                return NCR2_NOR_VERIFY_ERROR;
            }
        }
    }
    return NCR2_NOR_OK;
}

uint16_t ncr2_nor_erase(ncr2_nor_t *nor,
                        uint32_t address,
                        uint32_t length)
{
    uint16_t status;

    if (nor == NULL) {
        return NCR2_NOR_INVALID_ARGUMENT;
    }
    diagnostics_begin(
        nor, NCR2_NOR_OPERATION_ERASE, address, length);
    if (!operations_are_complete(&nor->operations)) {
        return diagnostics_finish(
            nor, NCR2_NOR_INVALID_ARGUMENT);
    }
    status = mutation_range(nor, address, length);
    if (status != NCR2_NOR_OK) {
        return diagnostics_finish(nor, status);
    }
    if ((address & (NCR2_NOR_SECTOR_SIZE - UINT32_C(1))) !=
            UINT32_C(0) ||
        (length & (NCR2_NOR_SECTOR_SIZE - UINT32_C(1))) !=
            UINT32_C(0)) {
        return diagnostics_finish(
            nor, NCR2_NOR_ALIGNMENT_ERROR);
    }

    for (uint32_t offset = UINT32_C(0);
         offset < length;
         offset += NCR2_NOR_SECTOR_SIZE) {
        uint32_t sector_address = address + offset;
        int mutation_status;
        int sync_status;

        nor->diagnostics.detail = sector_address;
        nor->diagnostics.phase = NCR2_NOR_PHASE_PREFLIGHT_READ;
        status = verify_erased(nor, sector_address);
        if (status == NCR2_NOR_OK) {
            ++nor->diagnostics.completed_units;
            continue;
        }
        if (status == NCR2_NOR_IO_ERROR) {
            return diagnostics_finish(nor, status);
        }

        nor->diagnostics.phase = NCR2_NOR_PHASE_ERASE;
        mutation_status =
            nor->operations.erase_sector(
                nor->operations.context,
                sector_address);
        nor->diagnostics.backend_status = mutation_status;
        nor->diagnostics.phase = NCR2_NOR_PHASE_SYNC;
        sync_status =
            nor->operations.sync_after_mutation(
                nor->operations.context,
                sector_address,
                NCR2_NOR_SECTOR_SIZE);

        if (mutation_status != 0 || sync_status != 0) {
            if (mutation_status == 0) {
                nor->diagnostics.backend_status = sync_status;
            } else {
                nor->diagnostics.phase = NCR2_NOR_PHASE_ERASE;
            }
            return diagnostics_finish(nor, NCR2_NOR_IO_ERROR);
        }
        nor->diagnostics.phase = NCR2_NOR_PHASE_VERIFY_DATA;
        status = verify_erased(nor, sector_address);
        if (status != NCR2_NOR_OK) {
            return diagnostics_finish(nor, status);
        }
        ++nor->diagnostics.completed_units;
    }
    return diagnostics_finish(nor, NCR2_NOR_OK);
}

static uint16_t preflight_program(ncr2_nor_t *nor,
                                  uint32_t address,
                                  const uint8_t *source,
                                  uint32_t length)
{
    uint8_t current[NCR2_NOR_PAGE_SIZE];
    uint32_t remaining = length;
    uint32_t offset = UINT32_C(0);

    while (remaining != UINT32_C(0)) {
        uint32_t chunk = NCR2_NOR_PAGE_SIZE;

        if (chunk > remaining) {
            chunk = remaining;
        }
        if (nor->operations.read(
                nor->operations.context,
                address + offset,
                current,
                chunk) != 0) {
            return NCR2_NOR_IO_ERROR;
        }
        for (uint32_t index = UINT32_C(0);
             index < chunk;
             ++index) {
            if ((current[index] & source[offset + index]) !=
                source[offset + index]) {
                return NCR2_NOR_NEEDS_ERASE;
            }
        }
        remaining -= chunk;
        offset += chunk;
    }
    return NCR2_NOR_OK;
}

uint16_t ncr2_nor_program(ncr2_nor_t *nor,
                          uint32_t address,
                          const void *source,
                          uint32_t length)
{
    const uint8_t *bytes = (const uint8_t *)source;
    uint8_t verify[NCR2_NOR_PAGE_SIZE];
    uint32_t remaining = length;
    uint32_t offset = UINT32_C(0);
    uint16_t status;

    if (nor == NULL) {
        return NCR2_NOR_INVALID_ARGUMENT;
    }
    diagnostics_begin(
        nor, NCR2_NOR_OPERATION_PROGRAM, address, length);
    if (source == NULL ||
        !operations_are_complete(&nor->operations)) {
        return diagnostics_finish(
            nor, NCR2_NOR_INVALID_ARGUMENT);
    }
    status = mutation_range(nor, address, length);
    if (status != NCR2_NOR_OK) {
        return diagnostics_finish(nor, status);
    }
    nor->diagnostics.phase = NCR2_NOR_PHASE_PREFLIGHT_READ;
    status = preflight_program(nor, address, bytes, length);
    if (status != NCR2_NOR_OK) {
        return diagnostics_finish(nor, status);
    }

    while (remaining != UINT32_C(0)) {
        const uint32_t page_offset =
            (address + offset) &
            (NCR2_NOR_PAGE_SIZE - UINT32_C(1));
        uint32_t chunk = NCR2_NOR_PAGE_SIZE - page_offset;
        int mutation_status;
        int sync_status;

        if (chunk > remaining) {
            chunk = remaining;
        }
        nor->diagnostics.detail = address + offset;
        nor->diagnostics.phase = NCR2_NOR_PHASE_PROGRAM;
        mutation_status =
            nor->operations.program_page(
                nor->operations.context,
                address + offset,
                &bytes[offset],
                chunk);
        nor->diagnostics.backend_status = mutation_status;
        nor->diagnostics.phase = NCR2_NOR_PHASE_SYNC;
        sync_status =
            nor->operations.sync_after_mutation(
                nor->operations.context,
                address + offset,
                chunk);
        if (mutation_status != 0 || sync_status != 0) {
            if (mutation_status == 0) {
                nor->diagnostics.backend_status = sync_status;
            } else {
                nor->diagnostics.phase = NCR2_NOR_PHASE_PROGRAM;
            }
            return diagnostics_finish(nor, NCR2_NOR_IO_ERROR);
        }
        nor->diagnostics.phase = NCR2_NOR_PHASE_VERIFY_READ;
        if (nor->operations.read(
                nor->operations.context,
                address + offset,
                verify,
                chunk) != 0) {
            nor->diagnostics.backend_status =
                NCR2_NOR_BACKEND_INVALID;
            return diagnostics_finish(
                nor, NCR2_NOR_IO_ERROR);
        }
        nor->diagnostics.phase = NCR2_NOR_PHASE_VERIFY_DATA;
        if (!bytes_equal(verify, &bytes[offset], chunk)) {
            return diagnostics_finish(
                nor, NCR2_NOR_VERIFY_ERROR);
        }
        remaining -= chunk;
        offset += chunk;
        ++nor->diagnostics.completed_units;
    }
    return diagnostics_finish(nor, NCR2_NOR_OK);
}

uint16_t ncr2_nor_get_diagnostics(
    const ncr2_nor_t *nor,
    ncr2_nor_diagnostics_t *diagnostics)
{
    if (nor == NULL || diagnostics == NULL) {
        return NCR2_NOR_INVALID_ARGUMENT;
    }
    *diagnostics = nor->diagnostics;
    return NCR2_NOR_OK;
}
