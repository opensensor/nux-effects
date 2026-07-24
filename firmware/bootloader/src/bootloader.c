#include <stddef.h>
#include <stdint.h>

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

static uint32_t crc32(const uint8_t *data, size_t size)
{
    uint32_t value = UINT32_C(0xFFFFFFFF);

    for (size_t index = 0U; index < size; ++index) {
        value ^= data[index];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask =
                (uint32_t)(-(int32_t)(value & UINT32_C(1)));
            value = (value >> 1U) ^
                    (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~value;
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
        crc32((const uint8_t *)manifest,
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

static int try_slot(uint32_t slot_address)
{
    const pedal_image_manifest_t *manifest =
        (const pedal_image_manifest_t *)(uintptr_t)slot_address;
    const uint8_t *payload =
        (const uint8_t *)(uintptr_t)(
            slot_address + NCR2_APPLICATION_MANIFEST_SIZE);

    if (!manifest_is_valid(manifest) ||
        !vector_is_valid(manifest, payload) ||
        !payload_hash_is_valid(manifest, payload)) {
        return 0;
    }

    copy_payload(manifest, payload);
    if (!payload_hash_is_valid(
            manifest,
            (const uint8_t *)(uintptr_t)manifest->load_address)) {
        g_boot_diagnostic = BOOT_DIAGNOSTIC_COPY_FAILED;
        return 0;
    }
    return 1;
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

void bootloader_main(void)
{
    g_boot_diagnostic = BOOT_DIAGNOSTIC_RESET;

    if (try_slot(NCR2_APPLICATION_A_ADDRESS)) {
        jump_to_application();
    }
    g_boot_diagnostic = BOOT_DIAGNOSTIC_SLOT_A_INVALID;

    if (try_slot(NCR2_APPLICATION_B_ADDRESS)) {
        jump_to_application();
    }
    g_boot_diagnostic = BOOT_DIAGNOSTIC_SLOT_B_INVALID;

    /*
     * Recovery USB and the physical recovery input are the next hardware
     * milestone. Until both are verified, an invalid image intentionally
     * stops here and this binary must not be flashed.
     */
    g_boot_diagnostic = BOOT_DIAGNOSTIC_RECOVERY;
    for (;;) {
        __asm volatile("wfi");
    }
}

