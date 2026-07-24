import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class EffectRuntimeTests(unittest.TestCase):
    def test_registry_and_caller_sized_chain(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stddef.h>
#include <stdint.h>

#include "effect_runtime.h"

typedef struct gain_context {
    float gain;
} gain_context_t;

typedef struct bias_context {
    float bias;
} bias_context_t;

static uint16_t gain_init(
    void *opaque, uint32_t sample_rate, uint32_t max_frames)
{
    gain_context_t *context = (gain_context_t *)opaque;
    if (sample_rate != 48000U || max_frames != 32U) return 1U;
    context->gain = 1.0F;
    return EFFECT_RUNTIME_OK;
}

static uint16_t gain_process(
    void *opaque, effect_audio_block_t *block)
{
    gain_context_t *context = (gain_context_t *)opaque;
    for (uint8_t channel = 0U;
         channel < block->channel_count;
         ++channel) {
        for (uint32_t frame = 0U;
             frame < block->frame_count;
             ++frame) {
            block->channels[channel][frame] *= context->gain;
        }
    }
    return EFFECT_RUNTIME_OK;
}

static uint16_t gain_parameter(
    void *opaque, uint32_t parameter_id, float value)
{
    gain_context_t *context = (gain_context_t *)opaque;
    if (parameter_id != 0U) return 1U;
    context->gain = value;
    return EFFECT_RUNTIME_OK;
}

static uint16_t bias_init(
    void *opaque, uint32_t sample_rate, uint32_t max_frames)
{
    bias_context_t *context = (bias_context_t *)opaque;
    (void)sample_rate;
    (void)max_frames;
    context->bias = 0.0F;
    return EFFECT_RUNTIME_OK;
}

static uint16_t bias_process(
    void *opaque, effect_audio_block_t *block)
{
    bias_context_t *context = (bias_context_t *)opaque;
    for (uint8_t channel = 0U;
         channel < block->channel_count;
         ++channel) {
        for (uint32_t frame = 0U;
             frame < block->frame_count;
             ++frame) {
            block->channels[channel][frame] += context->bias;
        }
    }
    return EFFECT_RUNTIME_OK;
}

static uint16_t bias_parameter(
    void *opaque, uint32_t parameter_id, float value)
{
    bias_context_t *context = (bias_context_t *)opaque;
    if (parameter_id != 7U) return 1U;
    context->bias = value;
    return EFFECT_RUNTIME_OK;
}

static const effect_parameter_descriptor_t gain_parameters[] = {
    { 0U, "gain", "linear", 0.0F, 4.0F, 1.0F },
};

static const effect_parameter_descriptor_t bias_parameters[] = {
    { 7U, "bias", "linear", -2.0F, 2.0F, 0.0F },
};

static const effect_descriptor_t gain_descriptor = {
    .key = { EFFECT_VENDOR_OPEN, 1U },
    .name = "gain",
    .abi_version = EFFECT_RUNTIME_ABI_VERSION,
    .parameter_count = 1U,
    .parameters = gain_parameters,
    .context_size = sizeof(gain_context_t),
    .context_alignment = _Alignof(gain_context_t),
    .initialize = gain_init,
    .reset = NULL,
    .process = gain_process,
    .set_parameter = gain_parameter,
};

static const effect_descriptor_t bias_descriptor = {
    .key = { EFFECT_VENDOR_OPEN, 2U },
    .name = "bias",
    .abi_version = EFFECT_RUNTIME_ABI_VERSION,
    .parameter_count = 1U,
    .parameters = bias_parameters,
    .context_size = sizeof(bias_context_t),
    .context_alignment = 16U,
    .initialize = bias_init,
    .reset = NULL,
    .process = bias_process,
    .set_parameter = bias_parameter,
};

int main(void)
{
    const effect_descriptor_t *effects[] = {
        &gain_descriptor,
        &bias_descriptor,
    };
    const effect_descriptor_t *duplicates[] = {
        &gain_descriptor,
        &gain_descriptor,
    };
    effect_registry_t registry = { effects, 2U };
    effect_registry_t duplicate_registry = { duplicates, 2U };
    effect_instance_t instances[8];
    _Alignas(16) uint8_t arena[128];
    effect_chain_t chain;
    size_t gain_index;
    size_t bias_index;
    float left[2] = { 1.0F, -1.0F };
    float right[2] = { 0.5F, -0.5F };
    effect_audio_block_t block = {
        .channels = { left, right },
        .frame_count = 2U,
        .channel_count = 2U,
    };

    if (effect_registry_validate(&registry) !=
        EFFECT_RUNTIME_OK) return 1;
    if (effect_registry_validate(&duplicate_registry) !=
        EFFECT_RUNTIME_DUPLICATE_EFFECT) return 2;
    if (effect_registry_find(
            &registry,
            (effect_key_t){ EFFECT_VENDOR_OPEN, 2U }) !=
        &bias_descriptor) return 3;
    if (effect_parameter_find(&gain_descriptor, 0U) !=
        &gain_parameters[0]) return 18;

    if (effect_chain_initialize(
            &chain, &registry, instances, 8U,
            arena, sizeof(arena), 48000U, 32U) !=
        EFFECT_RUNTIME_OK) return 4;
    if (effect_chain_add(
            &chain, gain_descriptor.key, &gain_index) !=
        EFFECT_RUNTIME_OK) return 5;
    if (effect_chain_add(
            &chain, bias_descriptor.key, &bias_index) !=
        EFFECT_RUNTIME_OK) return 6;

    /* The runtime is not limited to the four factory categories. */
    for (size_t index = 0U; index < 4U; ++index) {
        if (effect_chain_add(
                &chain, gain_descriptor.key, NULL) !=
            EFFECT_RUNTIME_OK) return 7;
    }
    if (chain.count != 6U) return 8;
    if (((uintptr_t)instances[bias_index].context & 15U) != 0U)
        return 9;

    if (effect_chain_set_parameter(
            &chain, gain_index, 0U, 2.0F) !=
        EFFECT_RUNTIME_OK) return 10;
    if (effect_chain_set_parameter(
            &chain, bias_index, 7U, 1.0F) !=
        EFFECT_RUNTIME_OK) return 11;
    if (effect_chain_set_parameter(
            &chain, gain_index, 0U, 5.0F) !=
        EFFECT_RUNTIME_PARAMETER_OUT_OF_RANGE) return 19;
    if (effect_chain_process(&chain, &block) !=
        EFFECT_RUNTIME_OK) return 12;
    if (left[0] != 3.0F || left[1] != -1.0F ||
        right[0] != 2.0F || right[1] != 0.0F) return 13;

    if (effect_chain_add(
            &chain,
            (effect_key_t){ EFFECT_VENDOR_OPEN, 99U },
            NULL) != EFFECT_RUNTIME_EFFECT_NOT_FOUND) return 14;
    block.frame_count = 33U;
    if (effect_chain_process(&chain, &block) !=
        EFFECT_RUNTIME_INVALID_ARGUMENT) return 15;

    {
        effect_instance_t one_instance[1];
        uint8_t tiny_arena[1];
        effect_chain_t tiny;
        if (effect_chain_initialize(
                &tiny, &registry, one_instance, 1U,
                tiny_arena, sizeof(tiny_arena),
                48000U, 32U) != EFFECT_RUNTIME_OK) return 16;
        if (effect_chain_add(
                &tiny, gain_descriptor.key, NULL) !=
            EFFECT_RUNTIME_ARENA_FULL) return 17;
    }
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "effect_runtime_test.c"
            executable = directory_path / "effect_runtime_test"
            test_source.write_text(source)
            subprocess.run(
                [
                    compiler,
                    "-std=c17",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wconversion",
                    "-Wshadow",
                    "-Wundef",
                    "-I",
                    str(ROOT / "firmware" / "app" / "include"),
                    str(
                        ROOT
                        / "firmware"
                        / "app"
                        / "src"
                        / "effect_runtime.c"
                    ),
                    str(test_source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([executable], check=True)

    def test_runtime_has_no_allocator_or_flash_dependency(self):
        source = (
            ROOT / "firmware" / "app" / "src" / "effect_runtime.c"
        ).read_text()
        for forbidden in (
            "malloc(",
            "calloc(",
            "realloc(",
            "free(",
            "FLEXSPI",
            "boot_journal",
        ):
            self.assertNotIn(forbidden, source)


if __name__ == "__main__":
    unittest.main()
