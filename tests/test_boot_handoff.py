import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BootHandoffTests(unittest.TestCase):
    def test_pending_handoff_publishes_token_and_arms_watchdog(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>
#include <string.h>

#include "boot_handoff.h"

static void start_watchdog(void *context)
{
    ++*(uint32_t *)context;
}

int main(void)
{
    boot_trial_mailbox_t mailbox;
    boot_handoff_services_t services;
    boot_controller_result_t result;
    uint32_t watchdog_starts = 0U;
    uint8_t slot;
    uint32_t sequence;

    memset(&mailbox, 0xff, sizeof(mailbox));
    memset(&result, 0, sizeof(result));
    services.trial_mailbox = &mailbox;
    services.watchdog_context = &watchdog_starts;
    services.start_trial_watchdog = start_watchdog;

    if (boot_handoff_prepare(&services, &result) !=
        BOOT_HANDOFF_NO_APPLICATION) return 1;
    if (mailbox.token != 0U ||
        watchdog_starts != 0U) return 2;

    result.action = BOOT_CONTROLLER_HANDOFF;
    result.selected_slot = BOOT_SLOT_A;
    result.state.pending_slot = BOOT_SLOT_NONE;
    if (boot_handoff_prepare(&services, &result) !=
        BOOT_HANDOFF_CONFIRMED) return 3;
    if (mailbox.token != 0U ||
        watchdog_starts != 0U) return 4;

    result.selected_slot = BOOT_SLOT_B;
    result.state.pending_slot = BOOT_SLOT_B;
    result.state.sequence = 17U;
    if (boot_handoff_prepare(&services, &result) !=
        BOOT_HANDOFF_TRIAL) return 5;
    if (mailbox.token !=
            (BOOT_TRIAL_HANDOFF_MAGIC | BOOT_SLOT_B) ||
        mailbox.sequence != 17U ||
        watchdog_starts != 1U) return 6;

    if (boot_trial_arm_confirmation(&mailbox) !=
        BOOT_TRIAL_OK) return 7;
    if (boot_trial_consume_confirmation(
            &mailbox, &slot, &sequence) !=
        BOOT_TRIAL_OK) return 8;
    if (slot != BOOT_SLOT_B || sequence != 17U) return 9;

    if (boot_handoff_prepare(NULL, &result) !=
        BOOT_HANDOFF_INVALID_ARGUMENT) return 10;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "boot_handoff_test.c"
            executable = directory_path / "boot_handoff_test"
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
                    str(ROOT / "firmware" / "bootloader" / "include"),
                    str(
                        ROOT
                        / "firmware"
                        / "bootloader"
                        / "src"
                        / "boot_handoff.c"
                    ),
                    str(
                        ROOT
                        / "firmware"
                        / "bootloader"
                        / "src"
                        / "boot_trial.c"
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
