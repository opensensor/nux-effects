#include "ncr2_flexspi_nor.h"

#include <stddef.h>
#include <stdint.h>

#include "MIMXRT1051.h"
#include "fsl_flexspi.h"
#include "ncr2_flash_layout.h"

#define NCR2_RAMFUNC \
    __attribute__((section(".ramfunc.ncr2_flexspi"), noinline))

#define NCR2_LUT_SEQUENCE_READ_ID 11U
#define NCR2_LUT_SEQUENCE_WRITE_ENABLE 12U
#define NCR2_LUT_SEQUENCE_READ_STATUS 13U
#define NCR2_LUT_SEQUENCE_ERASE_SECTOR 14U
#define NCR2_LUT_SEQUENCE_PAGE_PROGRAM 15U
#define NCR2_LUT_FIRST_WORD (4U * NCR2_LUT_SEQUENCE_READ_ID)
#define NCR2_LUT_WORD_COUNT 20U

#define NCR2_W25Q64_MANUFACTURER UINT8_C(0xEF)
#define NCR2_W25Q64_MEMORY_TYPE UINT8_C(0x40)
#define NCR2_W25Q64_CAPACITY UINT8_C(0x17)
#define NCR2_W25Q_STATUS_BUSY UINT8_C(0x01)
#define NCR2_W25Q_STATUS_WRITE_ENABLE UINT8_C(0x02)

typedef struct ncr2_flexspi_context {
    uint8_t initialized;
} ncr2_flexspi_context_t;

static ncr2_flexspi_context_t g_context;

static int range_inside(uint32_t offset,
                        uint32_t length,
                        uint32_t region_offset,
                        uint32_t region_size)
{
    return offset >= region_offset &&
           offset - region_offset < region_size &&
           length <= region_size - (offset - region_offset);
}

static int mutation_allowed(uint32_t address, uint32_t length)
{
    uint32_t offset;

    if (length == UINT32_C(0) ||
        address < NCR2_FLASH_XIP_BASE) {
        return 0;
    }
    offset = address - NCR2_FLASH_XIP_BASE;
    if (offset >= NCR2_FLASH_SIZE ||
        length > NCR2_FLASH_SIZE - offset) {
        return 0;
    }
    return range_inside(
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
               NCR2_APPLICATION_SLOT_SIZE);
}

static NCR2_RAMFUNC status_t ram_transfer(
    uint8_t sequence,
    uint32_t flash_offset,
    flexspi_command_type_t command_type,
    uint32_t *data,
    uint32_t data_size)
{
    flexspi_transfer_t transfer;

    transfer.deviceAddress = flash_offset;
    transfer.port = kFLEXSPI_PortA1;
    transfer.cmdType = command_type;
    transfer.seqIndex = sequence;
    transfer.SeqNumber = UINT8_C(1);
    transfer.data = data;
    transfer.dataSize = data_size;
    return FLEXSPI_TransferBlocking(FLEXSPI, &transfer);
}

static NCR2_RAMFUNC status_t ram_read_status(uint8_t *status_byte)
{
    uint32_t status_word = UINT32_C(0);
    status_t status =
        ram_transfer(
            NCR2_LUT_SEQUENCE_READ_STATUS,
            UINT32_C(0),
            kFLEXSPI_Read,
            &status_word,
            UINT32_C(1));

    *status_byte = (uint8_t)status_word;
    return status;
}

static NCR2_RAMFUNC status_t ram_write_enable(
    uint32_t flash_offset)
{
    uint8_t status_byte;
    status_t status =
        ram_transfer(
            NCR2_LUT_SEQUENCE_WRITE_ENABLE,
            flash_offset,
            kFLEXSPI_Command,
            NULL,
            UINT32_C(0));

    if (status != kStatus_Success) {
        return status;
    }
    status = ram_read_status(&status_byte);
    if (status == kStatus_Success &&
        (status_byte & NCR2_W25Q_STATUS_WRITE_ENABLE) == UINT8_C(0)) {
        return kStatus_Fail;
    }
    return status;
}

/*
 * Once an erase/program command may have reached the NOR, returning to XIP
 * before WIP clears is unsafe. Controller read errors are reset and retried
 * here instead of returning into unavailable flash.
 */
static NCR2_RAMFUNC void ram_wait_until_flash_idle(void)
{
    for (;;) {
        uint8_t status_byte;

        if (ram_read_status(&status_byte) == kStatus_Success) {
            if ((status_byte & NCR2_W25Q_STATUS_BUSY) == UINT8_C(0)) {
                return;
            }
        } else {
            FLEXSPI_SoftwareReset(FLEXSPI);
        }
    }
}

static NCR2_RAMFUNC int ram_configure_and_probe(void)
{
    uint32_t lut[NCR2_LUT_WORD_COUNT];
    uint32_t id_word = UINT32_C(0);
    uint8_t *identifier = (uint8_t *)&id_word;

    for (uint32_t index = UINT32_C(0);
         index < NCR2_LUT_WORD_COUNT;
         ++index) {
        lut[index] = UINT32_C(0);
    }
    lut[0] =
        FLEXSPI_LUT_SEQ(
            kFLEXSPI_Command_SDR,
            kFLEXSPI_1PAD,
            0x9FU,
            kFLEXSPI_Command_READ_SDR,
            kFLEXSPI_1PAD,
            0x04U);
    lut[4] =
        FLEXSPI_LUT_SEQ(
            kFLEXSPI_Command_SDR,
            kFLEXSPI_1PAD,
            0x06U,
            kFLEXSPI_Command_STOP,
            kFLEXSPI_1PAD,
            0x00U);
    lut[8] =
        FLEXSPI_LUT_SEQ(
            kFLEXSPI_Command_SDR,
            kFLEXSPI_1PAD,
            0x05U,
            kFLEXSPI_Command_READ_SDR,
            kFLEXSPI_1PAD,
            0x04U);
    lut[12] =
        FLEXSPI_LUT_SEQ(
            kFLEXSPI_Command_SDR,
            kFLEXSPI_1PAD,
            0x20U,
            kFLEXSPI_Command_RADDR_SDR,
            kFLEXSPI_1PAD,
            0x18U);
    lut[16] =
        FLEXSPI_LUT_SEQ(
            kFLEXSPI_Command_SDR,
            kFLEXSPI_1PAD,
            0x02U,
            kFLEXSPI_Command_RADDR_SDR,
            kFLEXSPI_1PAD,
            0x18U);
    lut[17] =
        FLEXSPI_LUT_SEQ(
            kFLEXSPI_Command_WRITE_SDR,
            kFLEXSPI_1PAD,
            0x04U,
            kFLEXSPI_Command_STOP,
            kFLEXSPI_1PAD,
            0x00U);

    FLEXSPI_UpdateLUT(
        FLEXSPI,
        NCR2_LUT_FIRST_WORD,
        lut,
        NCR2_LUT_WORD_COUNT);
    FLEXSPI_SoftwareReset(FLEXSPI);
    if (ram_transfer(
            NCR2_LUT_SEQUENCE_READ_ID,
            UINT32_C(0),
            kFLEXSPI_Read,
            &id_word,
            UINT32_C(3)) != kStatus_Success) {
        return -1;
    }
    if (identifier[0] != NCR2_W25Q64_MANUFACTURER ||
        identifier[1] != NCR2_W25Q64_MEMORY_TYPE ||
        identifier[2] != NCR2_W25Q64_CAPACITY) {
        return 1;
    }
    return 0;
}

static NCR2_RAMFUNC int ram_erase_sector(uint32_t flash_offset)
{
    status_t enable_status = ram_write_enable(flash_offset);
    status_t erase_status;

    if (enable_status != kStatus_Success) {
        return -1;
    }
    erase_status =
        ram_transfer(
            NCR2_LUT_SEQUENCE_ERASE_SECTOR,
            flash_offset,
            kFLEXSPI_Command,
            NULL,
            UINT32_C(0));
    ram_wait_until_flash_idle();
    FLEXSPI_SoftwareReset(FLEXSPI);
    return erase_status == kStatus_Success ? 0 : -1;
}

static NCR2_RAMFUNC int ram_program_page(
    uint32_t flash_offset,
    const uint8_t *source,
    uint32_t length)
{
    uint32_t aligned_data[NCR2_NOR_PAGE_SIZE / sizeof(uint32_t)];
    uint8_t *aligned_bytes = (uint8_t *)aligned_data;
    status_t enable_status;
    status_t program_status;

    for (uint32_t index = UINT32_C(0);
         index < length;
         ++index) {
        aligned_bytes[index] = source[index];
    }
    enable_status = ram_write_enable(flash_offset);
    if (enable_status != kStatus_Success) {
        return -1;
    }
    program_status =
        ram_transfer(
            NCR2_LUT_SEQUENCE_PAGE_PROGRAM,
            flash_offset,
            kFLEXSPI_Write,
            aligned_data,
            length);
    ram_wait_until_flash_idle();
    FLEXSPI_SoftwareReset(FLEXSPI);
    return program_status == kStatus_Success ? 0 : -1;
}

static int backend_read(void *opaque,
                        uint32_t address,
                        void *destination,
                        uint32_t length)
{
    ncr2_flexspi_context_t *context =
        (ncr2_flexspi_context_t *)opaque;
    uint8_t *output = (uint8_t *)destination;
    const volatile uint8_t *source =
        (const volatile uint8_t *)(uintptr_t)address;

    if (context == NULL ||
        context->initialized == UINT8_C(0) ||
        destination == NULL) {
        return -1;
    }
    for (uint32_t index = UINT32_C(0);
         index < length;
         ++index) {
        output[index] = source[index];
    }
    return 0;
}

static int backend_erase_sector(void *opaque, uint32_t address)
{
    ncr2_flexspi_context_t *context =
        (ncr2_flexspi_context_t *)opaque;
    uint32_t interrupt_state;
    int status;

    if (context == NULL ||
        context->initialized == UINT8_C(0) ||
        !mutation_allowed(address, NCR2_NOR_SECTOR_SIZE) ||
        (address & (NCR2_NOR_SECTOR_SIZE - UINT32_C(1))) !=
            UINT32_C(0)) {
        return -1;
    }
    interrupt_state = __get_PRIMASK();
    __disable_irq();
    status =
        ram_erase_sector(address - NCR2_FLASH_XIP_BASE);
    __set_PRIMASK(interrupt_state);
    return status;
}

static int backend_program_page(void *opaque,
                                uint32_t address,
                                const void *source,
                                uint32_t length)
{
    ncr2_flexspi_context_t *context =
        (ncr2_flexspi_context_t *)opaque;
    uint32_t interrupt_state;
    uint32_t page_offset =
        address & (NCR2_NOR_PAGE_SIZE - UINT32_C(1));
    int status;

    if (context == NULL ||
        context->initialized == UINT8_C(0) ||
        source == NULL ||
        length == UINT32_C(0) ||
        length > NCR2_NOR_PAGE_SIZE ||
        length > NCR2_NOR_PAGE_SIZE - page_offset ||
        !mutation_allowed(address, length)) {
        return -1;
    }
    interrupt_state = __get_PRIMASK();
    __disable_irq();
    status =
        ram_program_page(
            address - NCR2_FLASH_XIP_BASE,
            (const uint8_t *)source,
            length);
    __set_PRIMASK(interrupt_state);
    return status;
}

static int backend_sync(void *opaque,
                        uint32_t address,
                        uint32_t length)
{
    ncr2_flexspi_context_t *context =
        (ncr2_flexspi_context_t *)opaque;
    uint32_t cache_start =
        address & ~UINT32_C(31);
    uint32_t cache_end =
        (address + length + UINT32_C(31)) & ~UINT32_C(31);

    if (context == NULL ||
        context->initialized == UINT8_C(0) ||
        !mutation_allowed(address, length)) {
        return -1;
    }
    SCB_InvalidateDCache_by_Addr(
        (void *)(uintptr_t)cache_start,
        (int32_t)(cache_end - cache_start));
    SCB_InvalidateICache();
    __DSB();
    __ISB();
    return 0;
}

uint16_t ncr2_flexspi_nor_init(ncr2_nor_t *nor)
{
    ncr2_nor_operations_t operations;
    uint32_t interrupt_state;
    int probe_status;

    if (nor == NULL) {
        return NCR2_FLEXSPI_NOR_INVALID_ARGUMENT;
    }
    g_context.initialized = UINT8_C(0);
    interrupt_state = __get_PRIMASK();
    __disable_irq();
    probe_status = ram_configure_and_probe();
    __set_PRIMASK(interrupt_state);
    if (probe_status < 0) {
        return NCR2_FLEXSPI_NOR_PROBE_FAILED;
    }
    if (probe_status > 0) {
        return NCR2_FLEXSPI_NOR_UNSUPPORTED_DEVICE;
    }

    g_context.initialized = UINT8_C(1);
    operations.context = &g_context;
    operations.read = backend_read;
    operations.erase_sector = backend_erase_sector;
    operations.program_page = backend_program_page;
    operations.sync_after_mutation = backend_sync;
    if (ncr2_nor_init(nor, &operations) != NCR2_NOR_OK) {
        g_context.initialized = UINT8_C(0);
        return NCR2_FLEXSPI_NOR_POLICY_INIT_FAILED;
    }
    return NCR2_FLEXSPI_NOR_OK;
}
