#include "ncr2_flexspi_nor.h"

#include <stddef.h>
#include <stdint.h>

#include "MIMXRT1051.h"
#include "fsl_flexspi.h"
#include "fsl_romapi.h"
#include "ncr2_flash_layout.h"

#define NCR2_RAMFUNC \
    __attribute__((section(".ramfunc.ncr2_flexspi"), noinline))

#define NCR2_LUT_SEQUENCE_READ_ID 11U
#define NCR2_LUT_SEQUENCE_WRITE_ENABLE 12U
#define NCR2_LUT_SEQUENCE_READ_STATUS 13U
#define NCR2_LUT_SEQUENCE_ERASE_SECTOR 14U
/*
 * The FlexSPI LUT is 64 words: sixteen four-word sequences, and IPCR1
 * ISEQID is only four bits wide. Sequence 10 sits just below the block
 * this driver already owns; AHB reads use sequence 0, so claiming it does
 * not disturb XIP.
 */
#define NCR2_LUT_SEQUENCE_READ_DATA 10U
#define NCR2_LUT_FIRST_WORD (4U * NCR2_LUT_SEQUENCE_READ_ID)
#define NCR2_LUT_WORD_COUNT 16U
/* FLSHCR0[FLSHSZ] is a 23-bit KiB count; index 0 is port A1. */
#define NCR2_FLASH_SIZE_KIB (NCR2_FLASH_SIZE / UINT32_C(1024))
#define NCR2_FLSHCR0_SIZE_LIMIT_KIB UINT32_C(0x007FFFFF)

_Static_assert(
    NCR2_FLASH_SIZE_KIB <= NCR2_FLSHCR0_SIZE_LIMIT_KIB,
    "flash size must fit the FLSHCR0 FLSHSZ field");

#define NCR2_LUT_WORDS_PER_SEQUENCE 4U
#define NCR2_LUT_SEQUENCE_LIMIT 16U
#define NCR2_LUT_TOTAL_WORDS 64U

_Static_assert(
    NCR2_LUT_SEQUENCE_READ_DATA < NCR2_LUT_SEQUENCE_LIMIT,
    "IPCR1 ISEQID is four bits; sequence indices above 15 alias to 0");
_Static_assert(
    NCR2_LUT_FIRST_WORD + NCR2_LUT_WORD_COUNT <= NCR2_LUT_TOTAL_WORDS,
    "LUT update would run past the end of the 64-word table");
_Static_assert(
    (NCR2_LUT_SEQUENCE_READ_DATA + 1U) * NCR2_LUT_WORDS_PER_SEQUENCE <=
        NCR2_LUT_FIRST_WORD,
    "the read sequence must not overlap the driver's own LUT block");
#define NCR2_IP_READ_CHUNK_SIZE UINT32_C(8)

#define NCR2_W25Q64_MANUFACTURER UINT8_C(0xEF)
#define NCR2_W25Q64_MEMORY_TYPE UINT8_C(0x40)
#define NCR2_W25Q64_CAPACITY UINT8_C(0x17)
#define NCR2_W25Q_STATUS_BUSY UINT8_C(0x01)
#define NCR2_W25Q_STATUS_WRITE_ENABLE UINT8_C(0x02)

#define NCR2_FLEXSPI_POLL_LIMIT UINT32_C(2000000)
#define NCR2_FLEXSPI_RESET_POLL_LIMIT UINT32_C(200000)
#define NCR2_FLASH_BUSY_POLL_LIMIT UINT32_C(200000)
#define NCR2_FLASH_STATUS_ERROR_LIMIT UINT32_C(3)
#define NCR2_FLEXSPI_ERROR_MASK \
    (FLEXSPI_INTR_AHBCMDERR_MASK | \
     FLEXSPI_INTR_IPCMDERR_MASK | \
     FLEXSPI_INTR_AHBCMDGE_MASK | \
     FLEXSPI_INTR_IPCMDGE_MASK)
#define NCR2_BOOT_ROM_START UINT32_C(0x00200000)
#define NCR2_BOOT_ROM_END UINT32_C(0x00220000)
#define NCR2_BOOTLOADER_TREE_POINTER UINT32_C(0x0020001C)
#define NCR2_FLEXSPI_INSTANCE UINT32_C(0)

/*
 * These first fields intentionally mirror the private interface layout in
 * NXP's RT1051 fsl_romapi.c. Calling through the ROM tree avoids linking a
 * wrapper from XIP while the same NOR is unavailable.
 */

typedef struct ncr2_rom_flexspi_driver {
    uint32_t version;
    status_t (*init)(uint32_t instance,
                     flexspi_nor_config_t *config);
    status_t (*program)(uint32_t instance,
                        flexspi_nor_config_t *config,
                        uint32_t destination,
                        const uint32_t *source);
    status_t (*erase_all)(uint32_t instance,
                          flexspi_nor_config_t *config);
    status_t (*erase)(uint32_t instance,
                      flexspi_nor_config_t *config,
                      uint32_t start,
                      uint32_t length);
    uint32_t reserved1;
    void (*clear_cache)(uint32_t instance);
    status_t (*transfer)(uint32_t instance,
                         flexspi_xfer_t *transfer);
    status_t (*update_lut)(uint32_t instance,
                           uint32_t sequence_index,
                           const uint32_t *lut,
                           uint32_t sequence_count);
    uint32_t reserved2;
} ncr2_rom_flexspi_driver_t;

typedef struct ncr2_bootloader_api_entry {
    void (*run_bootloader)(void *argument);
    uint32_t version;
    const uint8_t *copyright;
    uint32_t reserved0;
    const ncr2_rom_flexspi_driver_t *flexspi_nor_driver;
} ncr2_bootloader_api_entry_t;

_Static_assert(
    offsetof(ncr2_rom_flexspi_driver_t, init) == UINT32_C(4),
    "RT1051 ROM FlexSPI init pointer moved");
_Static_assert(
    offsetof(ncr2_rom_flexspi_driver_t, program) == UINT32_C(8),
    "RT1051 ROM FlexSPI program pointer moved");
_Static_assert(
    offsetof(ncr2_bootloader_api_entry_t, flexspi_nor_driver) ==
        UINT32_C(16),
    "RT1051 bootloader API tree layout changed");
_Static_assert(
    sizeof(flexspi_nor_config_t) == UINT32_C(512),
    "RT1051 ROM FlexSPI configuration must remain 512 bytes");

typedef struct ncr2_flexspi_context {
    uint8_t initialized;
    uint8_t full_flash_mutation;
    flexspi_nor_config_t rom_config;
} ncr2_flexspi_context_t;

static ncr2_flexspi_context_t g_context;

static NCR2_RAMFUNC int ram_rom_range_is_valid(
    uintptr_t address,
    uint32_t length)
{
    const uintptr_t start = (uintptr_t)NCR2_BOOT_ROM_START;
    const uintptr_t end = (uintptr_t)NCR2_BOOT_ROM_END;

    return address >= start &&
           address < end &&
           (uintptr_t)length <= end - address;
}

static NCR2_RAMFUNC int ram_rom_code_is_valid(uintptr_t address)
{
    return (address & (uintptr_t)UINT32_C(1)) != (uintptr_t)0 &&
           ram_rom_range_is_valid(
               address & ~(uintptr_t)UINT32_C(1),
               UINT32_C(2));
}

static NCR2_RAMFUNC const ncr2_rom_flexspi_driver_t *
ram_rom_flexspi_driver(void)
{
    const uint32_t tree_address =
        *(volatile const uint32_t *)(uintptr_t)
            NCR2_BOOTLOADER_TREE_POINTER;
    const ncr2_bootloader_api_entry_t *tree;
    const ncr2_rom_flexspi_driver_t *driver;

    if (!ram_rom_range_is_valid(
            (uintptr_t)tree_address,
            (uint32_t)sizeof(*tree))) {
        return NULL;
    }
    tree = (const ncr2_bootloader_api_entry_t *)(uintptr_t)
        tree_address;
    driver = tree->flexspi_nor_driver;
    if (!ram_rom_range_is_valid(
            (uintptr_t)driver,
            (uint32_t)sizeof(*driver)) ||
        !ram_rom_code_is_valid((uintptr_t)driver->init) ||
        !ram_rom_code_is_valid((uintptr_t)driver->program)) {
        return NULL;
    }
    return driver;
}

static NCR2_RAMFUNC void ram_build_rom_config(
    flexspi_nor_config_t *config)
{
    uint8_t *bytes = (uint8_t *)config;

    for (uint32_t index = UINT32_C(0);
         index < (uint32_t)sizeof(*config);
         ++index) {
        bytes[index] = UINT8_C(0);
    }
    config->memConfig.tag = FLEXSPI_CFG_BLK_TAG;
    config->memConfig.version = FLEXSPI_CFG_BLK_VERSION;
    config->memConfig.readSampleClkSrc =
        kFLEXSPIReadSampleClk_LoopbackInternally;
    config->memConfig.csHoldTime = UINT8_C(3);
    config->memConfig.csSetupTime = UINT8_C(3);
    config->memConfig.controllerMiscOption =
        FSL_ROM_FLEXSPI_BITMASK(
            kFLEXSPIMiscOffset_SafeConfigFreqEnable);
    config->memConfig.deviceType = kFLEXSPIDeviceType_SerialNOR;
    config->memConfig.sflashPadType = kSerialFlash_4Pads;
    config->memConfig.serialClkFreq = kFLEXSPISerialClk_30MHz;
    config->memConfig.sflashA1Size = NCR2_FLASH_SIZE;
    config->memConfig.lookupTable[
        UINT32_C(4) * NOR_CMD_LUT_SEQ_IDX_READ] =
        FSL_ROM_FLEXSPI_LUT_SEQ(
            CMD_SDR,
            FLEXSPI_1PAD,
            0x03U,
            RADDR_SDR,
            FLEXSPI_1PAD,
            0x18U);
    config->memConfig.lookupTable[
        UINT32_C(4) * NOR_CMD_LUT_SEQ_IDX_READ + UINT32_C(1)] =
        FSL_ROM_FLEXSPI_LUT_SEQ(
            READ_SDR,
            FLEXSPI_1PAD,
            0x04U,
            STOP,
            FLEXSPI_1PAD,
            0x00U);
    config->memConfig.lookupTable[
        UINT32_C(4) * NOR_CMD_LUT_SEQ_IDX_READSTATUS] =
        FSL_ROM_FLEXSPI_LUT_SEQ(
            CMD_SDR,
            FLEXSPI_1PAD,
            0x05U,
            READ_SDR,
            FLEXSPI_1PAD,
            0x01U);
    config->memConfig.lookupTable[
        UINT32_C(4) * NOR_CMD_LUT_SEQ_IDX_WRITEENABLE] =
        FSL_ROM_FLEXSPI_LUT_SEQ(
            CMD_SDR,
            FLEXSPI_1PAD,
            0x06U,
            STOP,
            FLEXSPI_1PAD,
            0x00U);
    config->memConfig.lookupTable[
        UINT32_C(4) * NOR_CMD_LUT_SEQ_IDX_ERASESECTOR] =
        FSL_ROM_FLEXSPI_LUT_SEQ(
            CMD_SDR,
            FLEXSPI_1PAD,
            0x20U,
            RADDR_SDR,
            FLEXSPI_1PAD,
            0x18U);
    config->memConfig.lookupTable[
        UINT32_C(4) * NOR_CMD_LUT_SEQ_IDX_PAGEPROGRAM] =
        FSL_ROM_FLEXSPI_LUT_SEQ(
            CMD_SDR,
            FLEXSPI_1PAD,
            0x02U,
            RADDR_SDR,
            FLEXSPI_1PAD,
            0x18U);
    config->memConfig.lookupTable[
        UINT32_C(4) * NOR_CMD_LUT_SEQ_IDX_PAGEPROGRAM + UINT32_C(1)] =
        FSL_ROM_FLEXSPI_LUT_SEQ(
            WRITE_SDR,
            FLEXSPI_1PAD,
            0x04U,
            STOP,
            FLEXSPI_1PAD,
            0x00U);
    config->pageSize = NCR2_NOR_PAGE_SIZE;
    config->sectorSize = NCR2_NOR_SECTOR_SIZE;
    config->ipcmdSerialClkFreq = kFLEXSPISerialClk_30MHz;
    config->isUniformBlockSize = UINT8_C(1);
    config->serialNorType = kSerialNorType_StandardSPI;
    config->blockSize = UINT32_C(0x00010000);
}

static NCR2_RAMFUNC int ram_rom_initialize(
    flexspi_nor_config_t *config)
{
    const ncr2_rom_flexspi_driver_t *driver =
        ram_rom_flexspi_driver();
    status_t status;

    if (driver == NULL) {
        return NCR2_NOR_BACKEND_ROM_API_INVALID;
    }
    status = driver->init(NCR2_FLEXSPI_INSTANCE, config);
    __DSB();
    return status == kStatus_Success
               ? 0
               : NCR2_NOR_BACKEND_ROM_INIT_FAILED;
}

static NCR2_RAMFUNC int ram_rom_program_page_call(
    flexspi_nor_config_t *config,
    uint32_t flash_offset,
    const uint32_t *source)
{
    const ncr2_rom_flexspi_driver_t *driver =
        ram_rom_flexspi_driver();
    status_t status;

    if (driver == NULL) {
        return NCR2_NOR_BACKEND_ROM_API_INVALID;
    }
    status = driver->program(
        NCR2_FLEXSPI_INSTANCE,
        config,
        flash_offset,
        source);
    __DSB();
    return status == kStatus_Success
               ? 0
               : NCR2_NOR_BACKEND_PROGRAM_TRANSFER;
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

static int mutation_allowed(const ncr2_flexspi_context_t *context,
                            uint32_t address,
                            uint32_t length)
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
    if (context->full_flash_mutation != UINT8_C(0)) {
        return 1;
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
    uint32_t config_value = UINT32_C(0);
    uint32_t polls;

    FLEXSPI->FLSHCR2[kFLEXSPI_PortA1] |=
        FLEXSPI_FLSHCR2_CLRINSTRPTR_MASK;
    FLEXSPI->INTR =
        NCR2_FLEXSPI_ERROR_MASK |
        FLEXSPI_INTR_IPCMDDONE_MASK |
        FLEXSPI_INTR_IPTXWE_MASK |
        FLEXSPI_INTR_IPRXWA_MASK;
    FLEXSPI->IPCR0 = flash_offset;
    if (command_type != kFLEXSPI_Command &&
        command_type != kFLEXSPI_Read) {
        return kStatus_Fail;
    }
    FLEXSPI->IPTXFCR |= FLEXSPI_IPTXFCR_CLRIPTXF_MASK;
    FLEXSPI->IPRXFCR |= FLEXSPI_IPRXFCR_CLRIPRXF_MASK;
    if (command_type == kFLEXSPI_Read) {
        config_value = FLEXSPI_IPCR1_IDATSZ(data_size);
    }
    config_value |=
        FLEXSPI_IPCR1_ISEQID(sequence) |
        FLEXSPI_IPCR1_ISEQNUM(UINT32_C(0));
    FLEXSPI->IPCR1 = config_value;
    FLEXSPI->IPCMD |= FLEXSPI_IPCMD_TRG_MASK;

    if (command_type == kFLEXSPI_Read) {
        uint8_t *output = (uint8_t *)data;

        if (data == NULL || data_size > UINT32_C(8)) {
            return kStatus_Fail;
        }
        for (polls = UINT32_C(0);
             polls < NCR2_FLEXSPI_POLL_LIMIT;
             ++polls) {
            const uint32_t interrupt = FLEXSPI->INTR;
            const uint32_t available =
                ((FLEXSPI->IPRXFSTS & FLEXSPI_IPRXFSTS_FILL_MASK) >>
                 FLEXSPI_IPRXFSTS_FILL_SHIFT) *
                UINT32_C(8);
            if ((interrupt & NCR2_FLEXSPI_ERROR_MASK) !=
                UINT32_C(0)) {
                return kStatus_Fail;
            }
            if (available >= data_size) {
                break;
            }
        }
        if (polls == NCR2_FLEXSPI_POLL_LIMIT) {
            return kStatus_Timeout;
        }
        for (uint32_t index = UINT32_C(0);
             index < data_size;
             ++index) {
            const uint32_t value =
                FLEXSPI->RFDR[index / UINT32_C(4)];
            output[index] =
                (uint8_t)(
                    value >>
                    ((index & UINT32_C(3)) * UINT32_C(8)));
        }
        FLEXSPI->INTR = FLEXSPI_INTR_IPRXWA_MASK;
    }

    for (polls = UINT32_C(0);
         polls < NCR2_FLEXSPI_POLL_LIMIT;
         ++polls) {
        const uint32_t interrupt = FLEXSPI->INTR;
        if ((interrupt & NCR2_FLEXSPI_ERROR_MASK) != UINT32_C(0)) {
            return kStatus_Fail;
        }
        if ((interrupt & FLEXSPI_INTR_IPCMDDONE_MASK) !=
            UINT32_C(0)) {
            FLEXSPI->INTR = FLEXSPI_INTR_IPCMDDONE_MASK;
            return kStatus_Success;
        }
    }
    return kStatus_Timeout;
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

static NCR2_RAMFUNC int ram_read_data(
    uint32_t flash_offset,
    uint8_t *destination,
    uint32_t length)
{
    while (length != UINT32_C(0)) {
        uint32_t data[2] = {
            UINT32_C(0),
            UINT32_C(0),
        };
        const uint8_t *bytes = (const uint8_t *)data;
        const uint32_t chunk =
            length < NCR2_IP_READ_CHUNK_SIZE
                ? length
                : NCR2_IP_READ_CHUNK_SIZE;

        if (ram_transfer(
                NCR2_LUT_SEQUENCE_READ_DATA,
                flash_offset,
                kFLEXSPI_Read,
                data,
                chunk) != kStatus_Success) {
            return -1;
        }
        for (uint32_t index = UINT32_C(0);
             index < chunk;
             ++index) {
            destination[index] = bytes[index];
        }
        flash_offset += chunk;
        destination += chunk;
        length -= chunk;
    }
    return 0;
}

static NCR2_RAMFUNC status_t ram_software_reset(void)
{
    FLEXSPI->MCR0 |= FLEXSPI_MCR0_SWRESET_MASK;
    for (uint32_t polls = UINT32_C(0);
         polls < NCR2_FLEXSPI_RESET_POLL_LIMIT;
         ++polls) {
        if ((FLEXSPI->MCR0 & FLEXSPI_MCR0_SWRESET_MASK) ==
            UINT32_C(0)) {
            return kStatus_Success;
        }
    }
    return kStatus_Timeout;
}

static NCR2_RAMFUNC int ram_write_enable(
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
        return NCR2_NOR_BACKEND_WRITE_ENABLE_TRANSFER;
    }
    status = ram_read_status(&status_byte);
    if (status != kStatus_Success) {
        return NCR2_NOR_BACKEND_WRITE_ENABLE_TRANSFER;
    }
    if ((status_byte & NCR2_W25Q_STATUS_WRITE_ENABLE) ==
        UINT8_C(0)) {
        return NCR2_NOR_BACKEND_WRITE_ENABLE_LATCH;
    }
    return 0;
}

/*
 * Once an erase/program command may have reached the NOR, returning to XIP
 * before WIP clears is unsafe. Controller read errors are reset and retried
 * here instead of returning into unavailable flash.
 */
static NCR2_RAMFUNC int ram_wait_until_flash_idle(void)
{
    uint32_t status_errors = UINT32_C(0);

    for (uint32_t polls = UINT32_C(0);
         polls < NCR2_FLASH_BUSY_POLL_LIMIT;
         ++polls) {
        uint8_t status_byte;

        if (ram_read_status(&status_byte) == kStatus_Success) {
            status_errors = UINT32_C(0);
            if ((status_byte & NCR2_W25Q_STATUS_BUSY) == UINT8_C(0)) {
                return 0;
            }
        } else {
            ++status_errors;
            if (status_errors >= NCR2_FLASH_STATUS_ERROR_LIMIT ||
                ram_software_reset() != kStatus_Success) {
                return NCR2_NOR_BACKEND_CONTROLLER_TIMEOUT;
            }
        }
    }
    return NCR2_NOR_BACKEND_BUSY_TIMEOUT;
}

static NCR2_RAMFUNC int ram_configure_and_probe(
    ncr2_flexspi_context_t *context)
{
    uint32_t lut[NCR2_LUT_WORD_COUNT];
    uint32_t read_lut[NCR2_LUT_WORDS_PER_SEQUENCE] = { 0U, 0U, 0U, 0U };
    uint32_t id_word = UINT32_C(0);
    uint8_t *identifier = (uint8_t *)&id_word;

    ram_build_rom_config(&context->rom_config);
    if (ram_rom_initialize(&context->rom_config) != 0) {
        return -1;
    }

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
    read_lut[0] =
        FLEXSPI_LUT_SEQ(
            kFLEXSPI_Command_SDR,
            kFLEXSPI_1PAD,
            0x03U,
            kFLEXSPI_Command_RADDR_SDR,
            kFLEXSPI_1PAD,
            0x18U);
    read_lut[1] =
        FLEXSPI_LUT_SEQ(
            kFLEXSPI_Command_READ_SDR,
            kFLEXSPI_1PAD,
            0x04U,
            kFLEXSPI_Command_STOP,
            kFLEXSPI_1PAD,
            0x00U);

    /*
     * The stock boot configuration block declares sflashA1Size = 4 MiB on
     * an 8 MiB W25Q64, so FlexSPI maps only the low half of the chip. Both
     * application slots live above that line, which made every access to
     * them fail: IP commands error out, and an AHB read of 0x60400000 from
     * the bootloader faults before it can hand off or reach recovery.
     * Programming the true size here fixes IP and AHB access alike, and
     * runs before any slot is read.
     */
    FLEXSPI->FLSHCR0[0] = NCR2_FLASH_SIZE_KIB;

    FLEXSPI_UpdateLUT(
        FLEXSPI,
        NCR2_LUT_FIRST_WORD,
        lut,
        NCR2_LUT_WORD_COUNT);
    FLEXSPI_UpdateLUT(
        FLEXSPI,
        NCR2_LUT_WORDS_PER_SEQUENCE * NCR2_LUT_SEQUENCE_READ_DATA,
        read_lut,
        NCR2_LUT_WORDS_PER_SEQUENCE);
    if (ram_software_reset() != kStatus_Success) {
        return -1;
    }
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
    int enable_status = ram_write_enable(UINT32_C(0));
    int wait_status;
    status_t erase_status;
    status_t reset_status;

    if (enable_status != 0) {
        return enable_status;
    }
    erase_status =
        ram_transfer(
            NCR2_LUT_SEQUENCE_ERASE_SECTOR,
            flash_offset,
            kFLEXSPI_Command,
            NULL,
            UINT32_C(0));
    wait_status = ram_wait_until_flash_idle();
    reset_status = ram_software_reset();
    if (wait_status != 0) {
        return wait_status;
    }
    if (reset_status != kStatus_Success) {
        return NCR2_NOR_BACKEND_CONTROLLER_TIMEOUT;
    }
    return erase_status == kStatus_Success
               ? 0
               : NCR2_NOR_BACKEND_ERASE_TRANSFER;
}

static NCR2_RAMFUNC int ram_program_page(
    flexspi_nor_config_t *config,
    uint32_t flash_offset,
    const uint8_t *source,
    uint32_t length)
{
    uint32_t aligned_data[NCR2_NOR_PAGE_SIZE / sizeof(uint32_t)];
    uint8_t *aligned_bytes = (uint8_t *)aligned_data;
    const uint32_t page_offset =
        flash_offset & (NCR2_NOR_PAGE_SIZE - UINT32_C(1));
    const uint32_t page_start = flash_offset - page_offset;
    int program_status;
    int wait_status;
    status_t reset_status;

    if (config == NULL ||
        source == NULL ||
        length == UINT32_C(0) ||
        length > NCR2_NOR_PAGE_SIZE - page_offset) {
        return NCR2_NOR_BACKEND_INVALID;
    }
    for (uint32_t index = UINT32_C(0);
         index < NCR2_NOR_PAGE_SIZE;
         ++index) {
        aligned_bytes[index] = UINT8_C(0xFF);
    }
    /*
     * ProgramPage transmits config->pageSize bytes and aligns its destination
     * down. Staging the requested subrange into a complete erased page keeps
     * arbitrary recovery chunks inside that contract; 0xff cannot disturb
     * already-programmed bytes elsewhere in the physical page.
     */
    for (uint32_t index = UINT32_C(0);
         index < length;
         ++index) {
        aligned_bytes[page_offset + index] = source[index];
    }
    program_status =
        ram_rom_program_page_call(
            config,
            page_start,
            aligned_data);
    wait_status = ram_wait_until_flash_idle();
    reset_status = ram_software_reset();
    if (wait_status != 0) {
        return wait_status;
    }
    if (reset_status != kStatus_Success) {
        return NCR2_NOR_BACKEND_CONTROLLER_TIMEOUT;
    }
    return program_status;
}

static int backend_read(void *opaque,
                        uint32_t address,
                        void *destination,
                        uint32_t length)
{
    ncr2_flexspi_context_t *context =
        (ncr2_flexspi_context_t *)opaque;
    uint32_t interrupt_state;
    int status;

    if (context == NULL ||
        context->initialized == UINT8_C(0) ||
        destination == NULL ||
        address < NCR2_FLASH_XIP_BASE ||
        length == UINT32_C(0) ||
        address - NCR2_FLASH_XIP_BASE >= NCR2_FLASH_SIZE ||
        length > NCR2_FLASH_SIZE -
            (address - NCR2_FLASH_XIP_BASE)) {
        return -1;
    }
    /*
     * Do not verify through the XIP/AHB aperture. Physical v0.03 testing
     * showed that AHB reads can wedge after an IP erase while recovery is
     * executing from SDRAM. A private 0x03 LUT sequence keeps all recovery
     * reads on the same bounded IP-command path as erase and program.
     */
    interrupt_state = __get_PRIMASK();
    __disable_irq();
    status =
        ram_read_data(
            address - NCR2_FLASH_XIP_BASE,
            (uint8_t *)destination,
            length);
    __set_PRIMASK(interrupt_state);
    return status;
}

static int backend_erase_sector(void *opaque, uint32_t address)
{
    ncr2_flexspi_context_t *context =
        (ncr2_flexspi_context_t *)opaque;
    uint32_t interrupt_state;
    int status;

    if (context == NULL ||
        context->initialized == UINT8_C(0) ||
        !mutation_allowed(
            context, address, NCR2_NOR_SECTOR_SIZE) ||
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
    uint32_t flash_offset = address - NCR2_FLASH_XIP_BASE;
    int status;

    if (context == NULL ||
        context->initialized == UINT8_C(0) ||
        source == NULL ||
        length == UINT32_C(0) ||
        length > NCR2_NOR_PAGE_SIZE ||
        length > NCR2_NOR_PAGE_SIZE - page_offset ||
        !mutation_allowed(context, address, length)) {
        return -1;
    }
    interrupt_state = __get_PRIMASK();
    __disable_irq();
    status = ram_program_page(
        &context->rom_config,
        flash_offset,
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
        !mutation_allowed(context, address, length)) {
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

static uint16_t initialize_nor(ncr2_nor_t *nor,
                               uint8_t full_flash_mutation)
{
    ncr2_nor_operations_t operations;
    uint32_t interrupt_state;
    int probe_status;

    if (nor == NULL) {
        return NCR2_FLEXSPI_NOR_INVALID_ARGUMENT;
    }
    g_context.initialized = UINT8_C(0);
    g_context.full_flash_mutation = full_flash_mutation;
    interrupt_state = __get_PRIMASK();
    __disable_irq();
    probe_status = ram_configure_and_probe(&g_context);
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
    if ((full_flash_mutation == UINT8_C(0)
             ? ncr2_nor_init(nor, &operations)
             : ncr2_nor_init_full_flash(nor, &operations)) !=
        NCR2_NOR_OK) {
        g_context.initialized = UINT8_C(0);
        return NCR2_FLEXSPI_NOR_POLICY_INIT_FAILED;
    }
    return NCR2_FLEXSPI_NOR_OK;
}

uint16_t ncr2_flexspi_nor_init(ncr2_nor_t *nor)
{
    return initialize_nor(nor, UINT8_C(0));
}

uint16_t ncr2_flexspi_nor_init_full_flash(ncr2_nor_t *nor)
{
    return initialize_nor(nor, UINT8_C(1));
}
