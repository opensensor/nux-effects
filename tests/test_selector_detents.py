"""The Type selector against its physically measured detent ladder.

The detent values in the firmware were read off hardware on 2026-07-27 with
`RECOVERY_COMMAND_READ_KNOBS`. Before that measurement the build quantised
this channel into eight equal 512-count bins, which silently mapped knob
positions 2 and 3 onto one destination and left the eighth unreachable by
three ADC counts. These tests pin the real ladder so no future change can
reintroduce a mapping that the hardware does not actually produce.
"""

import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware" / "hardware_app" / "src" / "main.c"

# Knob position 1 is the topmost printed label; the ladder descends.
MEASURED = (3581, 3071, 2560, 2049, 1538, 1028, 516, 2)
RESTING_NOISE = 5
EFFECT_COUNT = 8


def extract(name: str, text: str) -> str:
    pattern = re.compile(
        r"^(?:static[ \t]+)?(?:const[ \t]+)?[A-Za-z_][\w ]*?[\w*\]][ \t\n]*"
        + re.escape(name)
        + r"[ \t\n]*(?:\([^;{]*\)|\[[^\]]*\])[ \t\n]*(?:=[ \t\n]*)?\{",
        re.M,
    )
    match = pattern.search(text)
    if match is None:
        raise AssertionError(f"{name} is no longer where the test expects it")
    depth = 0
    start = text.index("{", match.start())
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                # A declaration ends `};`, a function body just `}`.
                end = index + 1
                rest = text[end:]
                if rest.lstrip().startswith(";"):
                    end += rest.index(";") + 1
                return text[match.start():end]
    raise AssertionError(f"unbalanced braces in {name}")


HARNESS = r"""
#include <stdint.h>
#include <stdio.h>

#define NCR2_EFFECT_COUNT UINT32_C(8)
#define NCR2_SELECTOR_SETTLE_SAMPLES UINT32_C(3)
#define NCR2_SELECTOR_HYSTERESIS UINT32_C(128)

static uint32_t g_selector_candidate;
static uint32_t g_selector_candidate_samples;

@@DETENTS@@

@@DISTANCE@@

@@QUANTIZE@@

@@UPDATE@@

/* Hold a reading steady long enough to pass the settle filter. */
static uint32_t settle(uint32_t sample, uint32_t current)
{
    for (int i = 0; i < 16; ++i) {
        current = update_selector(sample, current);
    }
    return current;
}

int main(void)
{
    for (uint32_t i = 0; i < NCR2_EFFECT_COUNT; ++i) {
        printf("detent %u %u\n", i, (unsigned)g_selector_detent[i]);
        printf("quantize %u %u\n", i,
               (unsigned)quantize_selector(g_selector_detent[i]));
    }
    /* Walk every detent in both directions from every start. */
    for (uint32_t from = 0; from < NCR2_EFFECT_COUNT; ++from) {
        for (uint32_t to = 0; to < NCR2_EFFECT_COUNT; ++to) {
            uint32_t current = from;
            g_selector_candidate = from;
            g_selector_candidate_samples = 0;
            current = settle(g_selector_detent[to], current);
            printf("walk %u %u %u\n", from, to, (unsigned)current);
        }
    }
    /* Resting noise around a detent must never change the selection. */
    for (uint32_t i = 0; i < NCR2_EFFECT_COUNT; ++i) {
        uint32_t current = i;
        int32_t base = (int32_t)g_selector_detent[i];
        g_selector_candidate = i;
        g_selector_candidate_samples = 0;
        for (int32_t d = -@@NOISE@@; d <= @@NOISE@@; ++d) {
            int32_t v = base + d;
            if (v < 0) { v = 0; }
            current = update_selector((uint32_t)v, current);
        }
        printf("noise %u %u\n", i, (unsigned)current);
    }
    /* Parking the wiper at a midpoint must not flip the selection. */
    for (uint32_t i = 0; i + 1 < NCR2_EFFECT_COUNT; ++i) {
        uint32_t mid =
            ((uint32_t)g_selector_detent[i] +
             (uint32_t)g_selector_detent[i + 1]) / 2U;
        uint32_t current = settle(mid, i);
        printf("mid %u %u\n", i, (unsigned)current);
    }
    return 0;
}
"""


class SelectorDetentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if compiler is None:
            raise unittest.SkipTest("host C compiler is unavailable")
        text = MAIN.read_text()
        source = HARNESS
        for token, value in (
            ("@@DETENTS@@", extract("g_selector_detent", text)),
            ("@@DISTANCE@@", extract("selector_distance", text)),
            ("@@QUANTIZE@@", extract("quantize_selector", text)),
            ("@@UPDATE@@", extract("update_selector", text)),
            ("@@NOISE@@", str(RESTING_NOISE)),
        ):
            source = source.replace(token, value)
        cls._directory = tempfile.TemporaryDirectory()
        directory = Path(cls._directory.name)
        path = directory / "selector.c"
        binary = directory / "selector"
        path.write_text(source)
        subprocess.run(
            [
                compiler, "-std=c17", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wconversion", "-Wshadow", "-Wundef",
                str(path), "-o", str(binary),
            ],
            check=True,
        )
        output = subprocess.run(
            [str(binary)], check=True, capture_output=True, text=True
        ).stdout
        cls.rows = [line.split() for line in output.splitlines()]

    @classmethod
    def tearDownClass(cls):
        cls._directory.cleanup()

    def rows_of(self, kind):
        return [row[1:] for row in self.rows if row[0] == kind]

    def test_table_matches_the_hardware_measurement(self):
        measured = [int(value) for _, value in self.rows_of("detent")]
        self.assertEqual(tuple(measured), MEASURED)

    def test_every_detent_selects_its_own_destination(self):
        for index, destination in self.rows_of("quantize"):
            self.assertEqual(
                int(destination),
                int(index),
                f"detent {index} selects destination {destination}",
            )

    def test_all_eight_destinations_are_reachable_and_distinct(self):
        selected = {
            int(destination) for _, destination in self.rows_of("quantize")
        }
        self.assertEqual(
            selected,
            set(range(EFFECT_COUNT)),
            "equal-width bins previously merged two positions and left one "
            "destination unreachable",
        )

    def test_any_detent_reaches_any_other(self):
        for start, target, landed in self.rows_of("walk"):
            self.assertEqual(
                int(landed),
                int(target),
                f"moving from position {start} to {target} landed on {landed}",
            )

    def test_resting_noise_never_changes_the_selection(self):
        for index, destination in self.rows_of("noise"):
            self.assertEqual(int(destination), int(index))

    def test_a_wiper_parked_between_detents_does_not_flip(self):
        for index, destination in self.rows_of("mid"):
            self.assertEqual(
                int(destination),
                int(index),
                "hysteresis must hold the current destination at a midpoint",
            )

    def test_measured_ladder_is_evenly_spaced_and_clear_of_noise(self):
        gaps = [a - b for a, b in zip(MEASURED, MEASURED[1:])]
        self.assertTrue(all(gap > 8 * RESTING_NOISE for gap in gaps), gaps)
        self.assertLess(max(gaps) - min(gaps), 16, gaps)


if __name__ == "__main__":
    unittest.main()
