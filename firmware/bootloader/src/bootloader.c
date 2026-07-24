#include <stddef.h>
#include <stdint.h>

#include "boot_controller.h"
#include "boot_handoff.h"
#include "boot_recovery_request.h"
#include "bootloader_runtime.h"
#include "boot_trial.h"
#include "crc32.h"
#include "ncr2_flash_layout.h"
#include "pedal_image.h"
#include "sha256.h"

#define SCB_VTOR (*(volatile uint32_t *)UINT32_C(0xE000ED08))

enum boot_diagnostic_code {
    BOOT_DIAGNOSTIC_RESET = 0x42540000U,
    BOOT_DIAGNOSTIC_SLOT_A_INVALID = 0x42540001U,
    BOOT_DIAGNOSTIC_SLOT_B_INVALID = 0x42540002U,
    BOOT_DIAGNOSTIC_COPY_FAILED = 0x42540003U,
    BOOT_DIAGNOSTIC_HANDOFF = 0x42540004U,
    BOOT_DIAGNOSTIC_RECOVERY = 0x4254FFFFU,
};

volatile uint32_t g_boot_diagnostic
    __attribute__((section(".noinit")));

typedef struct boot_runtime_context {
    boot_recovery_request_t recovery;
    boot_trial_mailbox_t *trial;
} boot_runtime_context_t;

static void data_sync_barrier(void)
{
    __asm volatile("dsb 0xf" ::: "memory");
}

static void instruction_sync_barrier(void)
{
    __asm volatile("isb 0xf" ::: "memory");
}

static void disable_interrupts(void)
{
    __asm volatile("cpsid i" ::: "memory");
}

static int bytes_equal(const uint8_t *left,
                       const uint8_t *right,
                       size_t size)
{
    uint8_t difference = UINT8_C(0);

    for (size_t index = 0U; index < size; ++index) {
        difference |= (uint8_t)(left[index] ^ right[index]);
    }
    return difference == UINT8_C(0);
}

static int manifest_is_valid(const pedal_image_manifest_t *manifest)
{
    const uint32_t payload_capacity =
        NCR2_APPLICATION_SLOT_SIZE - NCR2_APPLICATION_MANIFEST_SIZE;
    const uint32_t expected_crc =
        crc32_compute(manifest,
                      offsetof(pedal_image_manifest_t, header_crc32));

    if (manifest->magic != PEDAL_IMAGE_MAGIC ||
        manifest->format_version != PEDAL_IMAGE_FORMAT_VERSION ||
        manifest->header_size != NCR2_APPLICATION_MANIFEST_SIZE ||
        manifest->board_id != PEDAL_IMAGE_BOARD_NCR2 ||
        manifest->load_address != NCR2_APPLICATION_LOAD_ADDRESS ||
        manifest->vector_offset != UINT32_C(0) ||
        manifest->image_size < UINT32_C(8) ||
        manifest->image_size > payload_capacity ||
        manifest->header_crc32 != expected_crc) {
        return 0;
    }
    return 1;
}

static int payload_hash_is_valid(const pedal_image_manifest_t *manifest,
                                 const uint8_t *payload)
{
    sha256_context_t context;
    uint8_t digest[SHA256_DIGEST_SIZE];

    sha256_init(&context);
    sha256_update(&context, payload, manifest->image_size);
    sha256_final(&context, digest);
    return bytes_equal(digest, manifest->image_sha256, sizeof(digest));
}

static int vector_is_valid(const pedal_image_manifest_t *manifest,
                           const uint8_t *payload)
{
    const uint32_t *vectors = (const uint32_t *)payload;
    const uint32_t initial_stack = vectors[0];
    const uint32_t reset_handler = vectors[1];
    const uint32_t reset_address = reset_handler & ~UINT32_C(1);
    const uint32_t image_end =
        manifest->load_address + manifest->image_size;

    if (initial_stack < NCR2_DTCM_START ||
        initial_stack > NCR2_DTCM_END ||
        (initial_stack & UINT32_C(7)) != UINT32_C(0) ||
        (reset_handler & UINT32_C(1)) == UINT32_C(0) ||
        reset_address < manifest->load_address ||
        reset_address >= image_end) {
        return 0;
    }
    return 1;
}

static void copy_payload(const pedal_image_manifest_t *manifest,
                         const uint8_t *payload)
{
    uint8_t *destination =
        (uint8_t *)(uintptr_t)manifest->load_address;

    for (uint32_t index = 0U; index < manifest->image_size; ++index) {
        destination[index] = payload[index];
    }
    data_sync_barrier();
}

static uint16_t try_slot_address(uint32_t slot_address)
{
    const pedal_image_manifest_t *manifest =
        (const pedal_image_manifest_t *)(uintptr_t)slot_address;
    const uint8_t *payload =
        (const uint8_t *)(uintptr_t)(
            slot_address + NCR2_APPLICATION_MANIFEST_SIZE);

    if (!manifest_is_valid(manifest) ||
        !vector_is_valid(manifest, payload) ||
        !payload_hash_is_valid(manifest, payload)) {
        return BOOT_CONTROLLER_SLOT_INVALID;
    }

    copy_payload(manifest, payload);
    if (!payload_hash_is_valid(
            manifest,
            (const uint8_t *)(uintptr_t)manifest->load_address)) {
        g_boot_diagnostic = BOOT_DIAGNOSTIC_COPY_FAILED;
        return BOOT_CONTROLLER_SLOT_COPY_FAILED;
    }
    return BOOT_CONTROLLER_SLOT_OK;
}

static uint16_t load_slot(void *context, uint8_t slot)
{
    uint16_t status;

    (void)context;
    if (slot == BOOT_SLOT_A) {
        status = try_slot_address(NCR2_APPLICATION_A_ADDRESS);
        if (status == BOOT_CONTROLLER_SLOT_INVALID) {
            g_boot_diagnostic =
                BOOT_DIAGNOSTIC_SLOT_A_INVALID;
        }
        return status;
    }
    if (slot == BOOT_SLOT_B) {
        status = try_slot_address(NCR2_APPLICATION_B_ADDRESS);
        if (status == BOOT_CONTROLLER_SLOT_INVALID) {
            g_boot_diagnostic =
                BOOT_DIAGNOSTIC_SLOT_B_INVALID;
        }
        return status;
    }
    return BOOT_CONTROLLER_SLOT_INVALID;
}

static int recovery_requested(void *context)
{
    boot_runtime_context_t *runtime =
        (boot_runtime_context_t *)context;

    return boot_recovery_request_consume(&runtime->recovery) !=
           BOOT_RECOVERY_REQUEST_NONE;
}

static int consume_confirmation(
    void *context,
    uint8_t *slot,
    uint32_t *sequence)
{
    boot_runtime_context_t *runtime =
        (boot_runtime_context_t *)context;

    return boot_trial_consume_confirmation(
               runtime->trial,
               slot,
               sequence) == BOOT_TRIAL_OK;
}

__attribute__((noreturn))
static void jump_to_application(void)
{
    const uint32_t *vectors =
        (const uint32_t *)(uintptr_t)NCR2_APPLICATION_LOAD_ADDRESS;
    const uint32_t initial_stack = vectors[0];
    const uint32_t reset_handler = vectors[1];
    void (*entry)(void) =
        (void (*)(void))(uintptr_t)reset_handler;

    g_boot_diagnostic = BOOT_DIAGNOSTIC_HANDOFF;
    disable_interrupts();
    SCB_VTOR = NCR2_APPLICATION_LOAD_ADDRESS;
    data_sync_barrier();
    instruction_sync_barrier();
    __asm volatile("msr msp, %0" : : "r"(initial_stack) : "memory");
    entry();

    for (;;) {
        __asm volatile("wfi");
    }
}

__attribute__((noreturn))
void bootloader_run(
    bootloader_runtime_services_t *platform)
{
    boot_runtime_context_t runtime;
    boot_controller_services_t services;
    boot_controller_result_t result;
    boot_handoff_services_t handoff;

    g_boot_diagnostic = BOOT_DIAGNOSTIC_RESET;
    if (platform == NULL ||
        platform->journal == NULL ||
        platform->trial_mailbox == NULL) {
        g_boot_diagnostic = BOOT_DIAGNOSTIC_RECOVERY;
        for (;;) {
            __asm volatile("wfi");
        }
    }

    runtime.recovery = platform->recovery;
    runtime.trial = platform->trial_mailbox;
    services.journal = platform->journal;
    services.metadata_address =
        NCR2_FLASH_XIP_BASE + NCR2_BOOT_METADATA_OFFSET;
    services.context = &runtime;
    services.recovery_requested = recovery_requested;
    services.consume_confirmation = consume_confirmation;
    services.load_slot = load_slot;
    handoff.trial_mailbox = runtime.trial;
    handoff.watchdog_context = platform->watchdog_context;
    handoff.start_trial_watchdog =
        platform->start_trial_watchdog;

    boot_controller_run(&services, &result);
    if (result.action == BOOT_CONTROLLER_HANDOFF) {
        (void)boot_handoff_prepare(&handoff, &result);
        if (platform->sync_application_memory != NULL) {
            platform->sync_application_memory();
        }
        jump_to_application();
    }

    g_boot_diagnostic = BOOT_DIAGNOSTIC_RECOVERY;
    if (platform->enter_recovery != NULL) {
        platform->enter_recovery(
            platform->recovery_context,
            &result);
    }
    for (;;) {
        __asm volatile("wfi");
    }
}
