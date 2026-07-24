import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BasicEffectsTests(unittest.TestCase):
    def test_gain_and_soft_clip_are_descriptor_driven(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>

#include "effect_runtime.h"
#include "effects_basic.h"

static int close_enough(float actual, float expected)
{
    float difference = actual - expected;
    if (difference < 0.0F) difference = -difference;
    return difference < 0.00001F;
}

int main(void)
{
    effect_instance_t instances[2];
    _Alignas(16) uint8_t arena[64];
    effect_chain_t chain;
    size_t gain_index;
    size_t clip_index;
    float left[] = { 0.25F, -0.25F, 1.0F };
    float right[] = { 0.5F, -0.5F, 0.0F };
    effect_audio_block_t block = {
        .channels = { left, right },
        .frame_count = 3U,
        .channel_count = 2U,
    };

    if (effect_registry_validate(
            &ncr2_basic_effect_registry) !=
        EFFECT_RUNTIME_OK) return 1;
    if (ncr2_basic_effect_registry.count != 2U) return 2;
    if (effect_chain_initialize(
            &chain,
            &ncr2_basic_effect_registry,
            instances,
            2U,
            arena,
            sizeof(arena),
            48000U,
            16U) != EFFECT_RUNTIME_OK) return 3;
    if (effect_chain_add(
            &chain,
            ncr2_effect_basic_gain.key,
            &gain_index) != EFFECT_RUNTIME_OK) return 4;
    if (effect_chain_add(
            &chain,
            ncr2_effect_basic_soft_clip.key,
            &clip_index) != EFFECT_RUNTIME_OK) return 5;
    if (effect_chain_set_parameter(
            &chain,
            gain_index,
            EFFECT_GAIN_PARAMETER_GAIN,
            2.0F) != EFFECT_RUNTIME_OK) return 6;
    if (effect_chain_set_parameter(
            &chain,
            clip_index,
            EFFECT_SOFT_CLIP_PARAMETER_DRIVE,
            4.0F) != EFFECT_RUNTIME_OK) return 7;
    if (effect_chain_set_parameter(
            &chain,
            clip_index,
            EFFECT_SOFT_CLIP_PARAMETER_LEVEL,
            1.0F) != EFFECT_RUNTIME_OK) return 8;
    if (effect_chain_set_parameter(
            &chain,
            clip_index,
            EFFECT_SOFT_CLIP_PARAMETER_MIX,
            1.0F) != EFFECT_RUNTIME_OK) return 9;

    if (effect_chain_process(&chain, &block) !=
        EFFECT_RUNTIME_OK) return 10;

    /* gain(2x), then x/(1+abs(x)) at drive 4x */
    if (!close_enough(left[0], 2.0F / 3.0F) ||
        !close_enough(left[1], -2.0F / 3.0F) ||
        !close_enough(left[2], 8.0F / 9.0F) ||
        !close_enough(right[0], 4.0F / 5.0F) ||
        !close_enough(right[1], -4.0F / 5.0F) ||
        !close_enough(right[2], 0.0F)) return 11;

    if (effect_chain_set_parameter(
            &chain,
            clip_index,
            EFFECT_SOFT_CLIP_PARAMETER_DRIVE,
            33.0F) !=
        EFFECT_RUNTIME_PARAMETER_OUT_OF_RANGE) return 12;
    if (effect_parameter_find(
            &ncr2_effect_basic_soft_clip,
            EFFECT_SOFT_CLIP_PARAMETER_MIX) == NULL) return 13;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "basic_effects_test.c"
            executable = directory_path / "basic_effects_test"
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
                        / "effects_basic.c"
                    ),
                    str(test_source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
