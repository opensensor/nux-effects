#ifndef NCR2_BOOT_JOURNAL_H
#define NCR2_BOOT_JOURNAL_H

#include <stdint.h>

#include "boot_state.h"

#define BOOT_JOURNAL_SECTOR_COUNT 2U

enum boot_journal_status {
    BOOT_JOURNAL_OK = 0,
    BOOT_JOURNAL_BACKEND_ERROR = 1,
    BOOT_JOURNAL_INVALID_STATE = 2,
    BOOT_JOURNAL_STALE_SEQUENCE = 3,
    BOOT_JOURNAL_VERIFY_FAILED = 4,
};

typedef struct boot_journal_backend {
    void *context;
    int (*read)(void *context,
                uint32_t address,
                void *destination,
                uint32_t length);
    int (*erase)(void *context, uint32_t address, uint32_t length);
    int (*program)(void *context,
                   uint32_t address,
                   const void *source,
                   uint32_t length);
} boot_journal_backend_t;

typedef struct boot_journal_location {
    uint16_t offset;
    uint8_t sector;
    uint8_t found;
} boot_journal_location_t;

uint16_t boot_journal_load(const boot_journal_backend_t *backend,
                           uint32_t base_address,
                           boot_state_t *state,
                           boot_journal_location_t *location);
uint16_t boot_journal_store(const boot_journal_backend_t *backend,
                            uint32_t base_address,
                            const boot_state_t *state);

#endif
