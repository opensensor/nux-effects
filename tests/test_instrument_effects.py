import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class InstrumentEffectsTests(unittest.TestCase):
    def test_all_instrument_voices_register_and_render_strict_c(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>

#include "effect_runtime.h"
#include "effects_instrument.h"

int main(void)
{
    effect_instance_t instance;
    _Alignas(16) uint8_t arena[4096];
    effect_chain_t chain;
    float mono[64];
    effect_audio_block_t block = {
        .channels = { mono },
        .frame_count = 64U,
        .channel_count = 1U,
    };
    float phase = 0.0F;

    if (effect_registry_validate(&ncr2_instrument_effect_registry) !=
        EFFECT_RUNTIME_OK) return 1;
    if (ncr2_instrument_effect_registry.count != 8U) return 2;

    for (size_t voice = 0U;
         voice < ncr2_instrument_effect_registry.count;
         ++voice) {
        size_t index;
        float energy = 0.0F;

        if (effect_chain_initialize(
                &chain,
                &ncr2_instrument_effect_registry,
                &instance,
                1U,
                arena,
                sizeof(arena),
                48000U,
                64U) != EFFECT_RUNTIME_OK) return 3;
        if (effect_chain_add(
                &chain,
                ncr2_instrument_effect_registry.effects[voice]->key,
                &index) != EFFECT_RUNTIME_OK) return 4;
        if (effect_chain_set_parameter(
                &chain,
                index,
                EFFECT_INSTRUMENT_PARAMETER_MIX,
                1.0F) != EFFECT_RUNTIME_OK) return 5;
        if (effect_chain_set_parameter(
                &chain,
                index,
                EFFECT_INSTRUMENT_PARAMETER_SENSITIVITY,
                0.003F) != EFFECT_RUNTIME_OK) return 6;

        for (uint32_t block_index = 0U;
             block_index < 750U;
             ++block_index) {
            for (uint32_t frame = 0U; frame < 64U; ++frame) {
                phase += 220.0F / 48000.0F;
                if (phase >= 1.0F) phase -= 1.0F;
                mono[frame] = phase < 0.5F ? 0.18F : -0.18F;
            }
            if (effect_chain_process(&chain, &block) !=
                EFFECT_RUNTIME_OK) return 7;
            if (block_index > 500U) {
                for (uint32_t frame = 0U; frame < 64U; ++frame) {
                    const float sample = mono[frame];
                    if (sample != sample || sample > 1.1F || sample < -1.1F)
                        return 8;
                    energy += sample < 0.0F ? -sample : sample;
                }
            }
        }
        if (energy < 1.0F) return 9;
        effect_chain_clear(&chain);
    }
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            test_source = base / "instrument_effects_test.c"
            executable = base / "instrument_effects_test"
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
                    str(ROOT / "firmware" / "app" / "src" / "effect_runtime.c"),
                    str(ROOT / "firmware" / "app" / "src" / "effects_instrument.c"),
                    str(test_source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
