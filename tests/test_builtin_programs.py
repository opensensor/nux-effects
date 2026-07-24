import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BuiltinProgramsTests(unittest.TestCase):
    def test_starter_bank_is_valid_and_larger_than_factory_modes(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>

#include "effect_runtime.h"
#include "effects_basic.h"
#include "program_runtime.h"
#include "programs_builtin.h"

int main(void)
{
    effect_instance_t instances[4];
    _Alignas(16) uint8_t arena[128];
    effect_chain_t chain;
    int saw_multi_effect_program = 0;

    if (program_library_validate(
            &ncr2_builtin_program_library,
            &ncr2_basic_effect_registry) !=
        PROGRAM_RUNTIME_OK) return 1;
    if (ncr2_builtin_program_catalog.count != 6U ||
        ncr2_starter_program_bank.program_count != 6U) return 2;

    if (effect_chain_initialize(
            &chain,
            &ncr2_basic_effect_registry,
            instances,
            4U,
            arena,
            sizeof(arena),
            48000U,
            32U) != EFFECT_RUNTIME_OK) return 3;

    for (size_t index = 0U;
         index < ncr2_starter_program_bank.program_count;
         ++index) {
        const program_descriptor_t *program =
            program_catalog_find(
                &ncr2_builtin_program_catalog,
                ncr2_starter_program_bank.programs[index]);

        if (program == NULL ||
            program_prepare(&chain, program) !=
                PROGRAM_RUNTIME_OK) return 4;
        if (chain.count > 1U) saw_multi_effect_program = 1;
        effect_chain_clear(&chain);
    }
    if (saw_multi_effect_program == 0) return 5;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "builtin_programs_test.c"
            executable = directory_path / "builtin_programs_test"
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
                    str(
                        ROOT
                        / "firmware"
                        / "app"
                        / "src"
                        / "program_runtime.c"
                    ),
                    str(
                        ROOT
                        / "firmware"
                        / "app"
                        / "src"
                        / "programs_builtin.c"
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
