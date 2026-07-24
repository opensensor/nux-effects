import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ProgramRuntimeTests(unittest.TestCase):
    def test_extensible_program_catalog_and_banks(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stddef.h>
#include <stdint.h>

#include "effect_runtime.h"
#include "program_runtime.h"

typedef struct scalar_context {
    float value;
} scalar_context_t;

static uint16_t scalar_init(
    void *opaque, uint32_t sample_rate, uint32_t max_frames)
{
    scalar_context_t *context = (scalar_context_t *)opaque;
    if (sample_rate != 48000U || max_frames != 16U) return 1U;
    context->value = 0.0F;
    return EFFECT_RUNTIME_OK;
}

static uint16_t gain_process(
    void *opaque, effect_audio_block_t *block)
{
    scalar_context_t *context = (scalar_context_t *)opaque;
    for (uint32_t frame = 0U;
         frame < block->frame_count;
         ++frame) {
        block->channels[0][frame] *= context->value;
    }
    return EFFECT_RUNTIME_OK;
}

static uint16_t bias_process(
    void *opaque, effect_audio_block_t *block)
{
    scalar_context_t *context = (scalar_context_t *)opaque;
    for (uint32_t frame = 0U;
         frame < block->frame_count;
         ++frame) {
        block->channels[0][frame] += context->value;
    }
    return EFFECT_RUNTIME_OK;
}

static uint16_t scalar_parameter(
    void *opaque, uint32_t parameter_id, float value)
{
    scalar_context_t *context = (scalar_context_t *)opaque;
    if (parameter_id != 10U) return 1U;
    context->value = value;
    return EFFECT_RUNTIME_OK;
}

static const effect_parameter_descriptor_t scalar_parameters[] = {
    { 10U, "value", "linear", -4.0F, 4.0F, 0.0F },
};

static const effect_descriptor_t gain_effect = {
    .key = { EFFECT_VENDOR_OPEN, 100U },
    .name = "gain",
    .abi_version = EFFECT_RUNTIME_ABI_VERSION,
    .parameter_count = 1U,
    .parameters = scalar_parameters,
    .context_size = sizeof(scalar_context_t),
    .context_alignment = _Alignof(scalar_context_t),
    .initialize = scalar_init,
    .reset = NULL,
    .process = gain_process,
    .set_parameter = scalar_parameter,
};

static const effect_descriptor_t bias_effect = {
    .key = { EFFECT_VENDOR_OPEN, 101U },
    .name = "bias",
    .abi_version = EFFECT_RUNTIME_ABI_VERSION,
    .parameter_count = 1U,
    .parameters = scalar_parameters,
    .context_size = sizeof(scalar_context_t),
    .context_alignment = _Alignof(scalar_context_t),
    .initialize = scalar_init,
    .reset = NULL,
    .process = bias_process,
    .set_parameter = scalar_parameter,
};

int main(void)
{
    const effect_descriptor_t *effect_list[] = {
        &gain_effect,
        &bias_effect,
    };
    effect_registry_t effects = { effect_list, 2U };
    program_parameter_value_t gain_parameters[] = {
        { 10U, 2.0F },
    };
    program_parameter_value_t bias_parameters[] = {
        { 10U, 0.5F },
    };
    program_node_descriptor_t nodes[] = {
        { gain_effect.key, gain_parameters, 1U },
        { bias_effect.key, bias_parameters, 1U },
    };
    program_descriptor_t program_storage[9];
    const program_descriptor_t *programs[9];
    program_key_t bank_programs[5][9];
    program_bank_descriptor_t bank_storage[5];
    const program_bank_descriptor_t *banks[5];
    program_catalog_t catalog = { programs, 9U };
    program_library_t library = { &catalog, banks, 5U };
    effect_instance_t instances[4];
    _Alignas(16) uint8_t arena[64];
    effect_chain_t chain;
    program_cursor_t cursor;
    float samples[] = { 1.0F, -1.0F };
    effect_audio_block_t block = {
        .channels = { samples },
        .frame_count = 2U,
        .channel_count = 1U,
    };

    for (size_t index = 0U; index < 9U; ++index) {
        program_storage[index].key =
            (program_key_t){ EFFECT_VENDOR_OPEN, (uint32_t)index };
        program_storage[index].name = "source program";
        program_storage[index].nodes = index == 0U ? nodes : NULL;
        program_storage[index].node_count = index == 0U ? 2U : 0U;
        programs[index] = &program_storage[index];
        bank_programs[0][index] = program_storage[index].key;
    }
    for (size_t index = 0U; index < 5U; ++index) {
        bank_storage[index].key =
            (program_bank_key_t){
                EFFECT_VENDOR_OPEN,
                (uint32_t)(200U + index)
            };
        bank_storage[index].name = "source bank";
        bank_storage[index].programs = bank_programs[index];
        bank_storage[index].program_count = index == 0U ? 9U : 1U;
        if (index != 0U) {
            bank_programs[index][0] =
                program_storage[index].key;
        }
        banks[index] = &bank_storage[index];
    }

    if (program_catalog_validate(&catalog, &effects) !=
        PROGRAM_RUNTIME_OK) return 1;
    if (program_library_validate(&library, &effects) !=
        PROGRAM_RUNTIME_OK) return 2;
    if (program_catalog_find(
            &catalog,
            (program_key_t){ EFFECT_VENDOR_OPEN, 8U }) !=
        &program_storage[8]) return 3;
    if (program_library_find_bank(
            &library,
            (program_bank_key_t){ EFFECT_VENDOR_OPEN, 204U }) !=
        &bank_storage[4]) return 4;

    if (effect_chain_initialize(
            &chain, &effects, instances, 4U,
            arena, sizeof(arena), 48000U, 16U) !=
        EFFECT_RUNTIME_OK) return 5;
    if (program_prepare(&chain, &program_storage[0]) !=
        PROGRAM_RUNTIME_OK ||
        chain.count != 2U) return 6;
    if (effect_chain_process(&chain, &block) !=
        EFFECT_RUNTIME_OK ||
        samples[0] != 2.5F ||
        samples[1] != -1.5F) return 7;
    if (program_prepare(&chain, &program_storage[0]) !=
        PROGRAM_RUNTIME_CHAIN_NOT_EMPTY) return 8;
    effect_chain_clear(&chain);
    if (chain.count != 0U || chain.arena_used != 0U) return 9;

    if (program_cursor_initialize(
            &cursor, &library, 0U, 8U) !=
        PROGRAM_RUNTIME_OK) return 10;
    if (program_cursor_next_program(&cursor) !=
            PROGRAM_RUNTIME_OK ||
        cursor.program_index != 0U) return 11;
    if (program_cursor_previous_program(&cursor) !=
            PROGRAM_RUNTIME_OK ||
        cursor.program_index != 8U) return 12;
    if (program_cursor_next_bank(&cursor) !=
            PROGRAM_RUNTIME_OK ||
        cursor.bank_index != 1U ||
        cursor.program_index != 0U) return 13;
    if (program_cursor_previous_bank(&cursor) !=
            PROGRAM_RUNTIME_OK ||
        cursor.bank_index != 0U) return 14;
    if (program_cursor_select_bank(
            &cursor,
            (program_bank_key_t){ EFFECT_VENDOR_OPEN, 204U },
            0U) != PROGRAM_RUNTIME_OK ||
        program_cursor_current(&cursor) !=
            &program_storage[4]) return 15;

    {
        const program_descriptor_t *duplicates[] = {
            &program_storage[0],
            &program_storage[0],
        };
        program_catalog_t duplicate_catalog = {
            duplicates, 2U
        };
        if (program_catalog_validate(
                &duplicate_catalog, &effects) !=
            PROGRAM_RUNTIME_DUPLICATE_PROGRAM) return 16;
    }
    {
        program_node_descriptor_t missing_node = {
            { EFFECT_VENDOR_OPEN, 999U }, NULL, 0U
        };
        program_descriptor_t missing_program = {
            { EFFECT_VENDOR_OPEN, 999U },
            "missing effect",
            &missing_node,
            1U
        };
        const program_descriptor_t *missing_list[] = {
            &missing_program
        };
        program_catalog_t missing_catalog = {
            missing_list, 1U
        };
        if (program_catalog_validate(
                &missing_catalog, &effects) !=
            PROGRAM_RUNTIME_EFFECT_NOT_FOUND) return 17;
    }
    {
        program_parameter_value_t duplicate_values[] = {
            { 10U, 1.0F },
            { 10U, 2.0F },
        };
        program_node_descriptor_t bad_node = {
            gain_effect.key, duplicate_values, 2U
        };
        program_descriptor_t bad_program = {
            { EFFECT_VENDOR_OPEN, 998U },
            "bad parameters",
            &bad_node,
            1U
        };
        const program_descriptor_t *bad_list[] = {
            &bad_program
        };
        program_catalog_t bad_catalog = { bad_list, 1U };
        if (program_catalog_validate(
                &bad_catalog, &effects) !=
            PROGRAM_RUNTIME_INVALID_DESCRIPTOR) return 18;
    }
    {
        effect_instance_t one_instance[1];
        effect_chain_t small_chain;
        if (effect_chain_initialize(
                &small_chain, &effects, one_instance, 1U,
                arena, sizeof(arena), 48000U, 16U) !=
            EFFECT_RUNTIME_OK) return 19;
        if (program_prepare(
                &small_chain, &program_storage[0]) !=
            PROGRAM_RUNTIME_CHAIN_ERROR ||
            small_chain.count != 0U ||
            small_chain.arena_used != 0U) return 20;
    }
    {
        const program_bank_descriptor_t *duplicate_banks[] = {
            &bank_storage[0],
            &bank_storage[0],
        };
        program_library_t duplicate_library = {
            &catalog, duplicate_banks, 2U
        };
        if (program_library_validate(
                &duplicate_library, &effects) !=
            PROGRAM_RUNTIME_DUPLICATE_BANK) return 21;
    }
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "program_runtime_test.c"
            executable = directory_path / "program_runtime_test"
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
                    str(
                        ROOT
                        / "firmware"
                        / "app"
                        / "src"
                        / "program_runtime.c"
                    ),
                    str(test_source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([executable], check=True)

    def test_program_runtime_has_no_fixed_catalog_or_allocator(self):
        header = (
            ROOT
            / "firmware"
            / "app"
            / "include"
            / "program_runtime.h"
        ).read_text()
        source = (
            ROOT
            / "firmware"
            / "app"
            / "src"
            / "program_runtime.c"
        ).read_text()
        self.assertIn("size_t bank_count", header)
        self.assertIn("size_t node_count", header)
        for forbidden in (
            "MAX_EFFECTS",
            "MAX_PROGRAMS",
            "MAX_BANKS",
            "malloc(",
            "calloc(",
            "realloc(",
            "free(",
            "FLEXSPI",
        ):
            self.assertNotIn(forbidden, header + source)


if __name__ == "__main__":
    unittest.main()
