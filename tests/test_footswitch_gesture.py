import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class FootswitchGestureTests(unittest.TestCase):
    def test_tap_hold_latch_and_initial_power_hold(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>

#include "footswitch_gesture.h"

static uint8_t update_for(
    ncr2_footswitch_gesture_t *gesture,
    uint8_t pressed,
    uint32_t milliseconds)
{
    uint8_t event = NCR2_FOOTSWITCH_EVENT_NONE;
    for (uint32_t elapsed = UINT32_C(0);
         elapsed < milliseconds;
         elapsed += UINT32_C(10)) {
        const uint8_t current = ncr2_footswitch_gesture_update(
            gesture, pressed, UINT32_C(10));
        if (current != NCR2_FOOTSWITCH_EVENT_NONE) {
            if (event != NCR2_FOOTSWITCH_EVENT_NONE) return UINT8_C(99);
            event = current;
        }
    }
    return event;
}

int main(void)
{
    ncr2_footswitch_gesture_t gesture;

    ncr2_footswitch_gesture_init(&gesture, UINT8_C(0));
    if (update_for(&gesture, UINT8_C(1), UINT32_C(500)) !=
        NCR2_FOOTSWITCH_EVENT_NONE) return 1;
    if (ncr2_footswitch_gesture_update(
            &gesture, UINT8_C(0), UINT32_C(10)) !=
        NCR2_FOOTSWITCH_EVENT_TAP) return 2;

    if (update_for(&gesture, UINT8_C(1), UINT32_C(1990)) !=
        NCR2_FOOTSWITCH_EVENT_NONE) return 3;
    if (ncr2_footswitch_gesture_update(
            &gesture, UINT8_C(1), UINT32_C(10)) !=
        NCR2_FOOTSWITCH_EVENT_HOLD) return 4;
    if (update_for(&gesture, UINT8_C(1), UINT32_C(3000)) !=
        NCR2_FOOTSWITCH_EVENT_NONE) return 5;
    if (ncr2_footswitch_gesture_update(
            &gesture, UINT8_C(0), UINT32_C(10)) !=
        NCR2_FOOTSWITCH_EVENT_NONE) return 6;

    /* A footswitch held during power-on recovery cannot become a hold. */
    ncr2_footswitch_gesture_init(&gesture, UINT8_C(1));
    if (update_for(&gesture, UINT8_C(1), UINT32_C(5000)) !=
        NCR2_FOOTSWITCH_EVENT_NONE) return 7;
    if (ncr2_footswitch_gesture_update(
            &gesture, UINT8_C(0), UINT32_C(10)) !=
        NCR2_FOOTSWITCH_EVENT_NONE) return 8;
    if (update_for(&gesture, UINT8_C(1), UINT32_C(2000)) !=
        NCR2_FOOTSWITCH_EVENT_HOLD) return 9;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "footswitch_test.c"
            executable = directory_path / "footswitch_test"
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
                    str(ROOT / "firmware" / "hardware_app" / "include"),
                    str(
                        ROOT
                        / "firmware"
                        / "hardware_app"
                        / "src"
                        / "footswitch_gesture.c"
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
