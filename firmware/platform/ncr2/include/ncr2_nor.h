#ifndef NCR2_NOR_H
#define NCR2_NOR_H

#include <stdint.h>

#define NCR2_NOR_SECTOR_SIZE UINT32_C(0x00001000)
#define NCR2_NOR_PAGE_SIZE UINT32_C(0x00000100)

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
} ncr2_nor_t;

uint16_t ncr2_nor_init(ncr2_nor_t *nor,
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

#endif
