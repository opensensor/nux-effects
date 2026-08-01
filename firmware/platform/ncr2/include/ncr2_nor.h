#ifndef NCR2_NOR_H
#define NCR2_NOR_H

#include <stdint.h>

#define NCR2_NOR_SECTOR_SIZE UINT32_C(0x00001000)
#define NCR2_NOR_PAGE_SIZE UINT32_C(0x00000100)
#define NCR2_NOR_DIAGNOSTIC_MAGIC UINT32_C(0x444F524E)
#define NCR2_NOR_DIAGNOSTIC_VERSION UINT16_C(1)

enum ncr2_nor_status {
    NCR2_NOR_OK = 0,
    NCR2_NOR_INVALID_ARGUMENT = 1,
    NCR2_NOR_OUT_OF_RANGE = 2,
    NCR2_NOR_PROTECTED_RANGE = 3,
    NCR2_NOR_ALIGNMENT_ERROR = 4,
    NCR2_NOR_NEEDS_ERASE = 5,
    NCR2_NOR_IO_ERROR = 6,
    NCR2_NOR_VERIFY_ERROR = 7,
};

enum ncr2_nor_operation {
    NCR2_NOR_OPERATION_NONE = 0,
    NCR2_NOR_OPERATION_READ = 1,
    NCR2_NOR_OPERATION_ERASE = 2,
    NCR2_NOR_OPERATION_PROGRAM = 3,
};

enum ncr2_nor_phase {
    NCR2_NOR_PHASE_NONE = 0,
    NCR2_NOR_PHASE_VALIDATE = 1,
    NCR2_NOR_PHASE_PREFLIGHT_READ = 2,
    NCR2_NOR_PHASE_ERASE = 3,
    NCR2_NOR_PHASE_PROGRAM = 4,
    NCR2_NOR_PHASE_SYNC = 5,
    NCR2_NOR_PHASE_VERIFY_READ = 6,
    NCR2_NOR_PHASE_VERIFY_DATA = 7,
    NCR2_NOR_PHASE_COMPLETE = 8,
};

/*
 * Negative backend statuses are preserved verbatim. The RT1051 FlexSPI
 * backend uses these values to distinguish the failing IP-command stage.
 */
enum ncr2_nor_backend_status {
    NCR2_NOR_BACKEND_INVALID = -1,
    NCR2_NOR_BACKEND_WRITE_ENABLE_TRANSFER = -10,
    NCR2_NOR_BACKEND_WRITE_ENABLE_LATCH = -11,
    NCR2_NOR_BACKEND_ERASE_TRANSFER = -12,
    NCR2_NOR_BACKEND_PROGRAM_TRANSFER = -13,
    NCR2_NOR_BACKEND_CONTROLLER_TIMEOUT = -14,
    NCR2_NOR_BACKEND_BUSY_TIMEOUT = -15,
    NCR2_NOR_BACKEND_ROM_API_INVALID = -16,
    NCR2_NOR_BACKEND_ROM_INIT_FAILED = -17,
};

typedef struct __attribute__((packed)) ncr2_nor_diagnostics {
    uint32_t magic;
    uint16_t version;
    uint8_t operation;
    uint8_t phase;
    uint32_t address;
    uint32_t length;
    uint32_t completed_units;
    int32_t backend_status;
    uint16_t status;
    uint16_t reserved;
    uint32_t detail;
} ncr2_nor_diagnostics_t;

typedef struct ncr2_nor_operations {
    void *context;
    int (*read)(void *context,
                uint32_t address,
                void *destination,
                uint32_t length);
    int (*erase_sector)(void *context, uint32_t address);
    int (*program_page)(void *context,
                        uint32_t address,
                        const void *source,
                        uint32_t length);
    int (*sync_after_mutation)(void *context,
                               uint32_t address,
                               uint32_t length);
} ncr2_nor_operations_t;

typedef struct ncr2_nor {
    ncr2_nor_operations_t operations;
    ncr2_nor_diagnostics_t diagnostics;
    uint8_t full_flash_mutation;
} ncr2_nor_t;

_Static_assert(sizeof(ncr2_nor_diagnostics_t) == 32U,
               "NOR diagnostics must fill one recovery payload");

uint16_t ncr2_nor_init(ncr2_nor_t *nor,
                       const ncr2_nor_operations_t *operations);
uint16_t ncr2_nor_init_full_flash(
    ncr2_nor_t *nor,
    const ncr2_nor_operations_t *operations);
uint16_t ncr2_nor_read(ncr2_nor_t *nor,
                       uint32_t address,
                       void *destination,
                       uint32_t length);
uint16_t ncr2_nor_erase(ncr2_nor_t *nor,
                        uint32_t address,
                        uint32_t length);
uint16_t ncr2_nor_program(ncr2_nor_t *nor,
                          uint32_t address,
                          const void *source,
                          uint32_t length);
uint16_t ncr2_nor_get_diagnostics(
    const ncr2_nor_t *nor,
    ncr2_nor_diagnostics_t *diagnostics);

#endif
