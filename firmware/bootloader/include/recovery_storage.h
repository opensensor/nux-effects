#ifndef NCR2_RECOVERY_STORAGE_H
#define NCR2_RECOVERY_STORAGE_H

#include <stdint.h>

#include "boot_journal.h"
#include "ncr2_nor.h"
#include "recovery_engine.h"

enum recovery_storage_status {
    RECOVERY_STORAGE_OK = 0,
    RECOVERY_STORAGE_INVALID_ARGUMENT = 1,
};

typedef struct recovery_storage {
    ncr2_nor_t *nor;
    void *reboot_context;
    void (*request_reboot)(void *context);
} recovery_storage_t;

uint16_t recovery_storage_init(
    recovery_storage_t *storage,
    ncr2_nor_t *nor,
    void (*request_reboot)(void *context),
    void *reboot_context);
void recovery_storage_make_backend(
    recovery_storage_t *storage,
    recovery_backend_t *backend);
void recovery_storage_make_journal_backend(
    recovery_storage_t *storage,
    boot_journal_backend_t *backend);

#endif
