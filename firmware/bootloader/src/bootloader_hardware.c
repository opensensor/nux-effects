#include <stddef.h>
#include <stdint.h>

#include "MIMXRT1051.h"
#include "bootloader_runtime.h"
#include "ncr2_board.h"
#include "ncr2_flash_layout.h"
#include "ncr2_flexspi_nor.h"
#include "ncr2_nor.h"
#include "ncr2_watchdog.h"
#include "recovery_engine.h"
#include "recovery_storage.h"
#include "recovery_usb.h"

#ifndef NCR2_HARDWARE_RECOVERY_WRITE_ENABLE
#define NCR2_HARDWARE_RECOVERY_WRITE_ENABLE 0
#endif
#ifndef NCR2_EMBED_RAM_RECOVERY
#define NCR2_EMBED_RAM_RECOVERY 0
#endif

_Static_assert(
    NCR2_HARDWARE_RECOVERY_WRITE_ENABLE == 0 ||
        NCR2_HARDWARE_RECOVERY_WRITE_ENABLE == 1,
    "hardware recovery write gate must be zero or one");
_Static_assert(
    NCR2_EMBED_RAM_RECOVERY == 0 ||
        NCR2_EMBED_RAM_RECOVERY == 1,
    "embedded RAM recovery gate must be zero or one");

#define NCR2_DIAGNOSTIC_HARDWARE_NOR_FAILED \
    UINT32_C(0x4254E001)
#define NCR2_DIAGNOSTIC_HARDWARE_STORAGE_FAILED \
    UINT32_C(0x4254E002)
#define NCR2_DIAGNOSTIC_HARDWARE_USB_CLOCK_FAILED \
    UINT32_C(0x4254E003)
#define NCR2_DIAGNOSTIC_HARDWARE_USB_START_FAILED \
    UINT32_C(0x4254E004)
#define NCR2_DIAGNOSTIC_HARDWARE_RECOVERY_ACTIVE \
    UINT32_C(0x4254E100)

typedef struct ncr2_hardware_boot_context {
    ncr2_nor_t nor;
    recovery_storage_t storage;
    boot_journal_backend_t journal;
    recovery_backend_t recovery;
    recovery_engine_t engine;
} ncr2_hardware_boot_context_t;

static ncr2_hardware_boot_context_t g_hardware;

__attribute__((noreturn))
static void stop_with_diagnostic(uint32_t diagnostic);

#if NCR2_EMBED_RAM_RECOVERY
extern const uint8_t __ram_recovery_blob_start[];
extern const uint8_t __ram_recovery_blob_end[];

__attribute__((noreturn, noinline))
static void launch_embedded_ram_recovery(void)
{
    uint8_t *destination =
        (uint8_t *)(uintptr_t)NCR2_APPLICATION_LOAD_ADDRESS;
    const uint8_t *source = __ram_recovery_blob_start;
    const uint32_t length =
        (uint32_t)(
            __ram_recovery_blob_end - __ram_recovery_blob_start);
    const uint32_t *vectors;
    uint32_t initial_stack;
    uint32_t reset_handler;
    void (*entry)(void);

    if (length < UINT32_C(8) ||
        length > NCR2_APPLICATION_SLOT_SIZE) {
        stop_with_diagnostic(
            NCR2_DIAGNOSTIC_HARDWARE_STORAGE_FAILED);
    }
    for (uint32_t index = UINT32_C(0);
         index < length;
         ++index) {
        destination[index] = source[index];
    }
    __DSB();
    __ISB();

    vectors =
        (const uint32_t *)(uintptr_t)
            NCR2_APPLICATION_LOAD_ADDRESS;
    initial_stack = vectors[0];
    reset_handler = vectors[1];
    if (initial_stack < NCR2_DTCM_START ||
        initial_stack > NCR2_DTCM_END ||
        (initial_stack & UINT32_C(7)) != UINT32_C(0) ||
        (reset_handler & UINT32_C(1)) == UINT32_C(0) ||
        (reset_handler & ~UINT32_C(1)) <
            NCR2_APPLICATION_LOAD_ADDRESS ||
        (reset_handler & ~UINT32_C(1)) >=
            NCR2_APPLICATION_LOAD_ADDRESS + length) {
        stop_with_diagnostic(
            NCR2_DIAGNOSTIC_HARDWARE_STORAGE_FAILED);
    }

    __disable_irq();
    SCB->VTOR = NCR2_APPLICATION_LOAD_ADDRESS;
    __DSB();
    __ISB();
    entry = (void (*)(void))(uintptr_t)reset_handler;
    __asm volatile(
        "msr msp, %0" : : "r"(initial_stack) : "memory");
    entry();
    stop_with_diagnostic(
        NCR2_DIAGNOSTIC_HARDWARE_STORAGE_FAILED);
}
#endif

__attribute__((noreturn))
static void stop_with_diagnostic(uint32_t diagnostic)
{
    g_boot_diagnostic = diagnostic;
    for (;;) {
        __asm volatile("wfi");
    }
}

#if !NCR2_HARDWARE_RECOVERY_WRITE_ENABLE
static int readonly_read(
    void *opaque,
    uint32_t address,
    void *destination,
    uint32_t length)
{
    ncr2_hardware_boot_context_t *hardware =
        (ncr2_hardware_boot_context_t *)opaque;

    if (hardware == NULL) {
        return -1;
    }
    return ncr2_nor_read(
               &hardware->nor,
               address,
               destination,
               length) == NCR2_NOR_OK
               ? 0
               : -1;
}

static int readonly_erase(
    void *opaque,
    uint32_t address,
    uint32_t length)
{
    (void)opaque;
    (void)address;
    (void)length;
    return -1;
}

static int readonly_program(
    void *opaque,
    uint32_t address,
    const void *source,
    uint32_t length)
{
    (void)opaque;
    (void)address;
    (void)source;
    (void)length;
    return -1;
}

static int readonly_store_boot_state(
    void *opaque,
    const boot_state_t *state)
{
    (void)opaque;
    (void)state;
    return -1;
}

static void make_readonly_journal(
    ncr2_hardware_boot_context_t *hardware)
{
    hardware->journal.context = hardware;
    hardware->journal.read = readonly_read;
    hardware->journal.erase = readonly_erase;
    hardware->journal.program = readonly_program;
}

static void make_readonly_recovery(
    ncr2_hardware_boot_context_t *hardware)
{
    hardware->recovery.context = hardware;
    hardware->recovery.read = readonly_read;
    hardware->recovery.erase = readonly_erase;
    hardware->recovery.program = readonly_program;
    hardware->recovery.store_boot_state =
        readonly_store_boot_state;
    hardware->recovery.request_reboot = NULL;
}
#endif

#if !NCR2_EMBED_RAM_RECOVERY
static uint8_t active_slot_for_recovery(
    const boot_controller_result_t *result)
{
    if (result->state.confirmed_slot == BOOT_SLOT_A ||
        result->state.confirmed_slot == BOOT_SLOT_B) {
        return result->state.confirmed_slot;
    }
    return BOOT_SLOT_A;
}

static uint32_t recovery_session_seed(
    const boot_controller_result_t *result)
{
    return UINT32_C(0x4E584658) ^
           result->state.sequence ^
           ((uint32_t)
                ncr2_board_watchdog_reset_status()
            << 16U);
}
#endif

/*
 * The application payload is copied into SDRAM through the data path while
 * the bootloader itself executes XIP from flash. Cortex-M7 instruction
 * fetch does not snoop the D-cache, so dirty lines must be written back and
 * any stale instruction lines discarded before the handoff. Without this
 * the core fetches whatever SDRAM held before the copy and faults into the
 * application's default handler with no outward sign at all.
 */
static void sync_application_memory(void)
{
#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    SCB_CleanInvalidateDCache();
#endif
#if defined(__ICACHE_PRESENT) && (__ICACHE_PRESENT == 1U)
    SCB_InvalidateICache();
#endif
    __DSB();
    __ISB();
}

static void enter_hardware_recovery(
    void *opaque,
    const boot_controller_result_t *result)
{
    ncr2_hardware_boot_context_t *hardware =
        (ncr2_hardware_boot_context_t *)opaque;

    if (hardware == NULL || result == NULL) {
        stop_with_diagnostic(
            NCR2_DIAGNOSTIC_HARDWARE_STORAGE_FAILED);
    }

#if NCR2_EMBED_RAM_RECOVERY
    (void)hardware;
    (void)result;
    launch_embedded_ram_recovery();
#else
#if NCR2_HARDWARE_RECOVERY_WRITE_ENABLE
    recovery_storage_make_backend(
        &hardware->storage,
        &hardware->recovery);
#else
    make_readonly_recovery(hardware);
#endif

    recovery_engine_init(
        &hardware->engine,
        &hardware->recovery,
        &result->state,
        active_slot_for_recovery(result),
        recovery_session_seed(result));

    if (ncr2_board_usb_clock_init() != NCR2_BOARD_OK) {
        stop_with_diagnostic(
            NCR2_DIAGNOSTIC_HARDWARE_USB_CLOCK_FAILED);
    }
    ncr2_board_usb_irq_enable();
    if (ncr2_recovery_usb_start(&hardware->engine) !=
        NCR2_RECOVERY_USB_OK) {
        stop_with_diagnostic(
            NCR2_DIAGNOSTIC_HARDWARE_USB_START_FAILED);
    }
    ncr2_board_recovery_indicator_init();

    g_boot_diagnostic =
        NCR2_DIAGNOSTIC_HARDWARE_RECOVERY_ACTIVE;
    for (;;) {
        ncr2_recovery_usb_service();
        __asm volatile("wfi");
    }
#endif
}

#if NCR2_HARDWARE_RECOVERY_WRITE_ENABLE
void ncr2_hardware_recovery_write_enabled(void)
{
}
#else
void ncr2_hardware_recovery_readonly(void)
{
}
#endif

void bootloader_main(void)
{
    bootloader_runtime_services_t services;

    ncr2_board_recovery_input_init();
    if (ncr2_flexspi_nor_init(&g_hardware.nor) !=
        NCR2_FLEXSPI_NOR_OK) {
        stop_with_diagnostic(
            NCR2_DIAGNOSTIC_HARDWARE_NOR_FAILED);
    }
    if (recovery_storage_init(
            &g_hardware.storage,
            &g_hardware.nor,
            ncr2_board_warm_reset,
            NULL) != RECOVERY_STORAGE_OK) {
        stop_with_diagnostic(
            NCR2_DIAGNOSTIC_HARDWARE_STORAGE_FAILED);
    }

#if NCR2_HARDWARE_RECOVERY_WRITE_ENABLE
    recovery_storage_make_journal_backend(
        &g_hardware.storage,
        &g_hardware.journal);
#else
    make_readonly_journal(&g_hardware);
#endif

    services.journal = &g_hardware.journal;
    ncr2_board_make_recovery_request(&services.recovery);
    services.trial_mailbox =
        (boot_trial_mailbox_t *)(uintptr_t)
            NCR2_BOOT_TRIAL_MAILBOX_ADDRESS;
    services.watchdog_context = NULL;
    services.start_trial_watchdog =
        ncr2_board_watchdog_start_trial;
    services.recovery_context = &g_hardware;
    services.enter_recovery =
        enter_hardware_recovery;
    services.sync_application_memory =
        sync_application_memory;
    bootloader_run(&services);
}
