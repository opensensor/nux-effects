import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ProgramSelectorTests(unittest.TestCase):
    def test_selector_supports_arbitrary_program_counts(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>

#include "program_selector.h"

int main(void)
{
    program_selector_t selector;
    int changed;

    if (program_selector_initialize(
            &selector, 12U, 10U,
            PROGRAM_SELECTOR_DEFAULT_HOLD_MS,
            PROGRAM_SELECTOR_DEFAULT_RELEASE_MS) !=
        PROGRAM_SELECTOR_OK) return 1;

    if (program_selector_sample(
            &selector, 1, 4999U, &changed) !=
            PROGRAM_SELECTOR_OK ||
        changed != 0 ||
        selector.current_program != 10U) return 2;
    if (program_selector_sample(
            &selector, 1, 1U, &changed) !=
            PROGRAM_SELECTOR_OK ||
        changed != 1 ||
        selector.current_program != 11U) return 3;

    /* One long hold changes exactly once. */
    if (program_selector_sample(
            &selector, 1, 60000U, &changed) !=
            PROGRAM_SELECTOR_OK ||
        changed != 0 ||
        selector.current_program != 11U) return 4;

    /* A short release does not rearm the gesture. */
    if (program_selector_sample(
            &selector, 0, 49U, &changed) !=
            PROGRAM_SELECTOR_OK) return 5;
    if (program_selector_sample(
            &selector, 1, 5000U, &changed) !=
            PROGRAM_SELECTOR_OK ||
        changed != 0) return 6;

    /* Stable release rearms; twelve programs wrap without a four-mode cap. */
    if (program_selector_sample(
            &selector, 0, 50U, &changed) !=
            PROGRAM_SELECTOR_OK) return 7;
    if (program_selector_sample(
            &selector, 1, 5000U, &changed) !=
            PROGRAM_SELECTOR_OK ||
        changed != 1 ||
        selector.current_program != 0U) return 8;

    /* Bounce before the threshold restarts the deliberate hold. */
    if (program_selector_sample(
            &selector, 0, 50U, &changed) !=
            PROGRAM_SELECTOR_OK) return 9;
    if (program_selector_sample(
            &selector, 1, 4000U, &changed) !=
            PROGRAM_SELECTOR_OK) return 10;
    if (program_selector_sample(
            &selector, 0, 1U, &changed) !=
            PROGRAM_SELECTOR_OK) return 11;
    if (program_selector_sample(
            &selector, 1, 1000U, &changed) !=
            PROGRAM_SELECTOR_OK ||
        changed != 0) return 12;
    if (program_selector_sample(
            &selector, 1, 4000U, &changed) !=
            PROGRAM_SELECTOR_OK ||
        changed != 1 ||
        selector.current_program != 1U) return 13;

    if (program_selector_select(&selector, 11U) !=
        PROGRAM_SELECTOR_OK) return 14;
    if (program_selector_sample(
            &selector, 1, UINT32_MAX, &changed) !=
            PROGRAM_SELECTOR_OK ||
        changed != 1 ||
        selector.current_program != 0U) return 15;

    if (program_selector_initialize(
            &selector, 0U, 0U, 5000U, 50U) !=
        PROGRAM_SELECTOR_INVALID_ARGUMENT) return 16;
    if (program_selector_initialize(
            &selector, 12U, 12U, 5000U, 50U) !=
        PROGRAM_SELECTOR_INVALID_ARGUMENT) return 17;
    if (program_selector_initialize(
            &selector, 1U, 0U, 5000U, 50U) !=
        PROGRAM_SELECTOR_OK) return 18;
    if (program_selector_sample(
            &selector, 1, 5000U, &changed) !=
            PROGRAM_SELECTOR_OK ||
        changed != 0 ||
        selector.current_program != 0U) return 19;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "program_selector_test.c"
            executable = directory_path / "program_selector_test"
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
                        / "program_selector.c"
                    ),
                    str(test_source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([executable], check=True)

    def test_application_has_no_factory_four_mode_enum(self):
        source = (
            ROOT / "firmware" / "app" / "src" / "main.c"
        ).read_text()
        self.assertNotIn("enum effect_mode", source)
        self.assertNotIn("EFFECT_MODE_", source)
        self.assertIn("g_active_program", source)


if __name__ == "__main__":
    unittest.main()
