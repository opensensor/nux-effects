/* Copyright-neutral launcher for the four preserved factory engines. */

#include "factory_engine_launcher.h"

#include <stddef.h>
#include <stdint.h>

#include "MIMXRT1051.h"

#include "factory_engine_request.h"

typedef struct {
    uint32_t source;
    uint32_t stack;
    uint32_t reset;
} ncr2_factory_engine_descriptor_t;

static const ncr2_factory_engine_descriptor_t g_factory_engines[
    NCR2_FACTORY_ENGINE_COUNT] = {
    { UINT32_C(0x60060000), UINT32_C(0x20018000), UINT32_C(0x0000f8cd) },
    { UINT32_C(0x60080000), UINT32_C(0x20018000), UINT32_C(0x00019095) },
    { UINT32_C(0x600a0000), UINT32_C(0x20020000), UINT32_C(0x0000c04d) },
    { UINT32_C(0x600c0000), UINT32_C(0x20018000), UINT32_C(0x0000e4b5) },
};

extern void ncr2_factory_compat_prepare(void);
extern void ncr2_factory_engine_copy_and_jump(
    uint32_t source,
    uint32_t reset) __attribute__((noreturn));

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

    /* Stop the open audio ISR before restoring the factory launch state. */
    __disable_irq();
    ncr2_factory_compat_prepare();
    ncr2_factory_engine_copy_and_jump(
        descriptor->source,
        descriptor->reset);
}
