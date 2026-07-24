#include <stdint.h>

#include "boot_state.h"
#include "ncr2_board.h"
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

static int unusable_store_boot_state(
    void *context,
    const boot_state_t *state)
{
    (void)context;
    (void)state;
    return -1;
}

__attribute__((noreturn))
static void stop(void)
{
    for (;;) {
        __asm volatile("wfi");
    }
}

void ram_recovery_main(void)
{
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
     * Full-flash mode never writes an A/B journal. Keep the generic
     * engine's backend-completeness invariant while making accidental
     * SET_PENDING fail closed.
     */
    g_recovery.backend.store_boot_state =
        unusable_store_boot_state;
    boot_state_default(&g_recovery.state);
    recovery_engine_init(
        &g_recovery.engine,
        &g_recovery.backend,
        &g_recovery.state,
        BOOT_SLOT_NONE,
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
