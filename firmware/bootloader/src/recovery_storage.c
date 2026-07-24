#include "recovery_storage.h"

#include <stddef.h>

#include "ncr2_flash_layout.h"

static int storage_read(void *opaque,
                        uint32_t address,
                        void *destination,
                        uint32_t length)
{
    recovery_storage_t *storage =
        (recovery_storage_t *)opaque;

    if (storage == NULL || storage->nor == NULL) {
        return -1;
    }
    return ncr2_nor_read(
               storage->nor,
               address,
               destination,
               length) == NCR2_NOR_OK
               ? 0
               : -1;
}

static int storage_erase(void *opaque,
                         uint32_t address,
                         uint32_t length)
{
    recovery_storage_t *storage =
        (recovery_storage_t *)opaque;

    if (storage == NULL || storage->nor == NULL) {
        return -1;
    }
    return ncr2_nor_erase(
               storage->nor,
               address,
               length) == NCR2_NOR_OK
               ? 0
               : -1;
}

static int storage_program(void *opaque,
                           uint32_t address,
                           const void *source,
                           uint32_t length)
{
    recovery_storage_t *storage =
        (recovery_storage_t *)opaque;

    if (storage == NULL || storage->nor == NULL) {
        return -1;
    }
    return ncr2_nor_program(
               storage->nor,
               address,
               source,
               length) == NCR2_NOR_OK
               ? 0
               : -1;
}

void recovery_storage_make_journal_backend(
    recovery_storage_t *storage,
    boot_journal_backend_t *backend)
{
    if (backend == NULL) {
        return;
    }
    backend->context = storage;
    backend->read = storage_read;
    backend->erase = storage_erase;
    backend->program = storage_program;
}

static int storage_store_boot_state(
    void *opaque,
    const boot_state_t *state)
{
    recovery_storage_t *storage =
        (recovery_storage_t *)opaque;
    boot_journal_backend_t backend;

    if (storage == NULL || state == NULL) {
        return -1;
    }
    recovery_storage_make_journal_backend(storage, &backend);
    return boot_journal_store(
               &backend,
               NCR2_FLASH_XIP_BASE + NCR2_BOOT_METADATA_OFFSET,
               state) == BOOT_JOURNAL_OK
               ? 0
               : -1;
}

static void storage_request_reboot(void *opaque)
{
    recovery_storage_t *storage =
        (recovery_storage_t *)opaque;

    if (storage != NULL && storage->request_reboot != NULL) {
        storage->request_reboot(storage->reboot_context);
    }
}

uint16_t recovery_storage_init(
    recovery_storage_t *storage,
    ncr2_nor_t *nor,
    void (*request_reboot)(void *context),
    void *reboot_context)
{
    if (storage == NULL || nor == NULL) {
        return RECOVERY_STORAGE_INVALID_ARGUMENT;
    }
    storage->nor = nor;
    storage->request_reboot = request_reboot;
    storage->reboot_context = reboot_context;
    return RECOVERY_STORAGE_OK;
}

void recovery_storage_make_backend(
    recovery_storage_t *storage,
    recovery_backend_t *backend)
{
    if (backend == NULL) {
        return;
    }
    backend->context = storage;
    backend->read = storage_read;
    backend->erase = storage_erase;
    backend->program = storage_program;
    backend->store_boot_state = storage_store_boot_state;
    backend->request_reboot = storage_request_reboot;
}
