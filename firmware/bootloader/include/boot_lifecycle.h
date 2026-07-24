#ifndef NCR2_BOOT_LIFECYCLE_H
#define NCR2_BOOT_LIFECYCLE_H

#include <stdint.h>

#include "boot_journal.h"
#include "boot_state.h"

enum boot_lifecycle_status {
    BOOT_LIFECYCLE_OK = 0,
    BOOT_LIFECYCLE_JOURNAL_ERROR = 1,
    BOOT_LIFECYCLE_NOT_PENDING = 2,
    BOOT_LIFECYCLE_WRONG_SLOT = 3,
};

uint16_t boot_lifecycle_prepare(
    const boot_journal_backend_t *backend,
    uint32_t metadata_address,
    boot_state_t *state,
    uint8_t *selected_slot);
uint16_t boot_lifecycle_confirm(
    const boot_journal_backend_t *backend,
    uint32_t metadata_address,
    uint8_t running_slot,
    boot_state_t *state);
uint16_t boot_lifecycle_reject_pending(
    const boot_journal_backend_t *backend,
    uint32_t metadata_address,
    boot_state_t *state);

#endif
