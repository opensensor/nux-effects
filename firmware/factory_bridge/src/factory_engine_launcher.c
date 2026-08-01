/* Copyright-neutral launcher for the four preserved factory engines. */

#include "factory_engine_launcher.h"
#include "factory_engine_request.h"

#include <stddef.h>
#include <stdint.h>

#include "MIMXRT1051.h"

typedef struct {
    uint32_t source;
    uint32_t stack;
    uint32_t reset;
    uint32_t hook_offset;
    uint32_t hook_instruction;
    uint32_t original_led;
    uint32_t original_led_prefix;
} ncr2_factory_engine_descriptor_t;

static const ncr2_factory_engine_descriptor_t g_factory_engines[
    NCR2_FACTORY_ENGINE_COUNT] = {
    {
        UINT32_C(0x60060000), UINT32_C(0x20018000),
        UINT32_C(0x0000f8cd), UINT32_C(0x00001bf8),
        UINT32_C(0xf998f002), UINT32_C(0x00003f2d),
        UINT32_C(0x6a504a47),
    },
    {
        UINT32_C(0x60080000), UINT32_C(0x20018000),
        UINT32_C(0x00019095), UINT32_C(0x00012402),
        UINT32_C(0xfd96f001), UINT32_C(0x00013f33),
        UINT32_C(0x6a504a37),
    },
    {
        UINT32_C(0x600a0000), UINT32_C(0x20020000),
        UINT32_C(0x0000c04d), UINT32_C(0x00003d3e),
        UINT32_C(0xf9d9f001), UINT32_C(0x000050f5),
        UINT32_C(0x6a504a37),
    },
    {
        UINT32_C(0x600c0000), UINT32_C(0x20018000),
        UINT32_C(0x0000e4b5), UINT32_C(0x00005880),
        UINT32_C(0xff57f001), UINT32_C(0x00007733),
        UINT32_C(0x6a504a27),
    },
};

#define NCR2_FACTORY_MONITOR_OFFSET UINT32_C(0x0001df00)
#define NCR2_FACTORY_MONITOR_LED_OFFSET UINT32_C(0x0001dffc)
#define NCR2_FACTORY_MONITOR_CAVE_SIZE UINT32_C(0x00000100)
#define NCR2_FACTORY_GPR7_ADDRESS UINT32_C(0x400f8038)

extern void ncr2_factory_compat_prepare(void);
extern void ncr2_factory_engine_copy_to_itcm(uint32_t source);
extern void ncr2_factory_engine_sync_and_jump(
    uint32_t reset) __attribute__((noreturn));
extern const uint8_t ncr2_factory_return_monitor_start[];
extern const uint8_t ncr2_factory_return_monitor_original_led[];
extern const uint8_t ncr2_factory_return_monitor_end[];

/* Keep GCC's hosted null-object analysis away from valid low ITCM MMIO. */
__attribute__((noinline))
static volatile void *itcm_pointer(uint32_t address)
{
    __asm volatile("" : "+r"(address));
    return (volatile void *)(uintptr_t)address;
}

static uint32_t read_u32_le(const volatile uint8_t *source)
{
    return (uint32_t)source[0] |
        ((uint32_t)source[1] << 8) |
        ((uint32_t)source[2] << 16) |
        ((uint32_t)source[3] << 24);
}

/* Encode a Thumb-2 BL from one even ITCM instruction address to another. */
static uint32_t encode_thumb_bl(uint32_t source, uint32_t target)
{
    const int32_t displacement =
        (int32_t)target - (int32_t)(source + UINT32_C(4));
    const uint32_t encoded = (uint32_t)displacement;
    const uint32_t sign = (encoded >> 24) & UINT32_C(1);
    const uint32_t i1 = (encoded >> 23) & UINT32_C(1);
    const uint32_t i2 = (encoded >> 22) & UINT32_C(1);
    const uint32_t j1 = (~(i1 ^ sign)) & UINT32_C(1);
    const uint32_t j2 = (~(i2 ^ sign)) & UINT32_C(1);
    const uint32_t first =
        UINT32_C(0xf000) |
        (sign << 10) |
        ((encoded >> 12) & UINT32_C(0x03ff));
    const uint32_t second =
        UINT32_C(0xd000) |
        (j1 << 13) |
        (j2 << 11) |
        ((encoded >> 1) & UINT32_C(0x07ff));

    return first | (second << 16);
}

static uint8_t source_matches_patch_contract(
    const ncr2_factory_engine_descriptor_t *descriptor)
{
    const volatile uint8_t *const source =
        (const volatile uint8_t *)(uintptr_t)descriptor->source;
    const size_t monitor_size = (size_t)(
        (uintptr_t)ncr2_factory_return_monitor_end -
        (uintptr_t)ncr2_factory_return_monitor_start);
    const size_t led_offset = (size_t)(
        (uintptr_t)ncr2_factory_return_monitor_original_led -
        (uintptr_t)ncr2_factory_return_monitor_start);

    if (monitor_size > NCR2_FACTORY_MONITOR_CAVE_SIZE ||
        led_offset != (size_t)UINT32_C(0xfc) ||
        read_u32_le(source + descriptor->hook_offset) !=
            descriptor->hook_instruction ||
        read_u32_le(source + (descriptor->original_led & ~UINT32_C(1))) !=
            descriptor->original_led_prefix) {
        return UINT8_C(0);
    }
    for (size_t index = 0U;
         index < NCR2_FACTORY_MONITOR_CAVE_SIZE;
         ++index) {
        if (source[NCR2_FACTORY_MONITOR_OFFSET + index] != UINT8_C(0)) {
            return UINT8_C(0);
        }
    }
    return UINT8_C(1);
}

static void patch_factory_return_monitor(
    const ncr2_factory_engine_descriptor_t *descriptor)
{
    volatile uint8_t *const cave =
        (volatile uint8_t *)itcm_pointer(NCR2_FACTORY_MONITOR_OFFSET);
    volatile uint16_t *const hook =
        (volatile uint16_t *)itcm_pointer(descriptor->hook_offset);
    volatile uint32_t *const original_led =
        (volatile uint32_t *)itcm_pointer(
            NCR2_FACTORY_MONITOR_LED_OFFSET);
    const size_t monitor_size = (size_t)(
        (uintptr_t)ncr2_factory_return_monitor_end -
        (uintptr_t)ncr2_factory_return_monitor_start);
    const uint32_t monitor_call = encode_thumb_bl(
        descriptor->hook_offset,
        NCR2_FACTORY_MONITOR_OFFSET);

    for (size_t index = 0U; index < monitor_size; ++index) {
        cave[index] = ncr2_factory_return_monitor_start[index];
    }
    *original_led = descriptor->original_led;
    hook[0] = (uint16_t)monitor_call;
    hook[1] = (uint16_t)(monitor_call >> 16);

    /* GPR7 is only the main-loop monitor's volatile hold counter. */
    *(volatile uint32_t *)(uintptr_t)NCR2_FACTORY_GPR7_ADDRESS =
        UINT32_C(0);
    __DSB();
}

int ncr2_factory_engine_launch(uint8_t engine)
{
    const ncr2_factory_engine_descriptor_t *descriptor;
    const volatile uint32_t *vectors;

    if (engine >= NCR2_FACTORY_ENGINE_COUNT) {
        return NCR2_FACTORY_LAUNCH_INVALID_ENGINE;
    }

    descriptor = &g_factory_engines[engine];
    vectors =
        (const volatile uint32_t *)(uintptr_t)descriptor->source;
    if (vectors[0] != descriptor->stack ||
        vectors[1] != descriptor->reset) {
        return NCR2_FACTORY_LAUNCH_VECTOR_MISMATCH;
    }
    if (source_matches_patch_contract(descriptor) == UINT8_C(0)) {
        return NCR2_FACTORY_LAUNCH_PATCH_MISMATCH;
    }

    /* Stop the open audio ISR before restoring the factory launch state. */
    __disable_irq();
    ncr2_factory_compat_prepare();
    ncr2_factory_engine_copy_to_itcm(descriptor->source);
    patch_factory_return_monitor(descriptor);
    ncr2_factory_engine_sync_and_jump(descriptor->reset);
}
