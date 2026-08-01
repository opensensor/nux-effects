#include <stdint.h>

#include "boot_journal.h"
#include "boot_state.h"
#include "ncr2_board.h"
#include "ncr2_flash_layout.h"
#include "ncr2_flexspi_nor.h"
#include "ncr2_nor.h"
#include "recovery_engine.h"
#include "recovery_storage.h"
#include "recovery_usb.h"

#define NCR2_RAM_RECOVERY_SESSION_SEED UINT32_C(0x52414D46)

typedef struct ncr2_ram_recovery_context {
    ncr2_nor_t nor;
    recovery_storage_t storage;
    recovery_backend_t backend;
    recovery_engine_t engine;
    boot_state_t state;
} ncr2_ram_recovery_context_t;

static ncr2_ram_recovery_context_t g_recovery;

__attribute__((noreturn))
static void stop(void)
{
    for (;;) {
        __asm volatile("wfi");
    }
}

void ram_recovery_main(void)
{
    boot_journal_backend_t journal;
    boot_journal_location_t journal_location;

    if (ncr2_flexspi_nor_init_full_flash(&g_recovery.nor) !=
        NCR2_FLEXSPI_NOR_OK) {
        stop();
    }
    if (recovery_storage_init(
            &g_recovery.storage,
            &g_recovery.nor,
            ncr2_board_warm_reset,
            NULL) != RECOVERY_STORAGE_OK) {
        stop();
    }
    recovery_storage_make_backend(
        &g_recovery.storage,
        &g_recovery.backend);
    /*
     * This RAM personality serves both bounded inactive-slot updates and
     * guarded whole-flash restore. Load the real journal so slot protection,
     * SET_PENDING, trial accounting, and rollback all operate on durable
     * state. Whole-flash sessions never issue SET_PENDING; their image
     * replaces this metadata along with every other flash region.
     */
    recovery_storage_make_journal_backend(
        &g_recovery.storage,
        &journal);
    if (boot_journal_load(
            &journal,
            NCR2_FLASH_XIP_BASE + NCR2_BOOT_METADATA_OFFSET,
            &g_recovery.state,
            &journal_location) != BOOT_JOURNAL_OK) {
        stop();
    }
    /*
     * Reading the front panel touches no flash state, so it is safe to offer
     * even in the whole-flash session that erased the image.
     */
    g_recovery.backend.read_knobs =
        ncr2_board_recovery_read_knobs;
    recovery_engine_init(
        &g_recovery.engine,
        &g_recovery.backend,
        &g_recovery.state,
        boot_state_selected_slot(&g_recovery.state),
        NCR2_RAM_RECOVERY_SESSION_SEED);
    recovery_engine_enable_full_flash(&g_recovery.engine);

    if (ncr2_board_usb_clock_init() != NCR2_BOARD_OK) {
        stop();
    }
    ncr2_board_usb_irq_enable();
    if (ncr2_recovery_usb_start(&g_recovery.engine) !=
        NCR2_RECOVERY_USB_OK) {
        stop();
    }
    ncr2_board_recovery_indicator_init();

    for (;;) {
        ncr2_recovery_usb_service();
        __asm volatile("wfi");
    }
}
