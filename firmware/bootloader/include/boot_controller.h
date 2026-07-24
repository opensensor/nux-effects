#ifndef NCR2_BOOT_CONTROLLER_H
#define NCR2_BOOT_CONTROLLER_H

#include <stdint.h>

#include "boot_journal.h"
#include "boot_state.h"

enum boot_controller_action {
    BOOT_CONTROLLER_RECOVERY = 0,
    BOOT_CONTROLLER_HANDOFF = 1,
};

enum boot_controller_reason {
    BOOT_CONTROLLER_REASON_SELECTED = 0,
    BOOT_CONTROLLER_REASON_FALLBACK = 1,
    BOOT_CONTROLLER_REASON_FORCED_RECOVERY = 2,
    BOOT_CONTROLLER_REASON_JOURNAL_ERROR = 3,
    BOOT_CONTROLLER_REASON_NO_VALID_SLOT = 4,
    BOOT_CONTROLLER_REASON_INVALID_ARGUMENT = 5,
};

enum boot_controller_slot_status {
    BOOT_CONTROLLER_SLOT_OK = 0,
    BOOT_CONTROLLER_SLOT_INVALID = 1,
    BOOT_CONTROLLER_SLOT_COPY_FAILED = 2,
};

enum boot_controller_confirmation_status {
    BOOT_CONTROLLER_CONFIRMATION_NONE = 0,
    BOOT_CONTROLLER_CONFIRMATION_ACCEPTED = 1,
    BOOT_CONTROLLER_CONFIRMATION_STALE = 2,
    BOOT_CONTROLLER_CONFIRMATION_IGNORED_FOR_RECOVERY = 3,
};

typedef struct boot_controller_services {
    const boot_journal_backend_t *journal;
    uint32_t metadata_address;
    void *context;
    int (*recovery_requested)(void *context);
    int (*consume_confirmation)(
        void *context,
        uint8_t *slot,
        uint32_t *sequence);
    uint16_t (*load_slot)(void *context, uint8_t slot);
} boot_controller_services_t;

typedef struct boot_controller_result {
    boot_state_t state;
    uint16_t journal_status;
    uint16_t slot_status;
    uint8_t action;
    uint8_t reason;
    uint8_t selected_slot;
    uint8_t primary_slot;
    uint8_t confirmation_status;
} boot_controller_result_t;

void boot_controller_run(
    const boot_controller_services_t *services,
    boot_controller_result_t *result);

#endif
