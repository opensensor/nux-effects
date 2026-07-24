import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BootTrialTests(unittest.TestCase):
    def test_retained_trial_token_is_torn_write_and_replay_safe(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>
#include <string.h>

#include "boot_trial.h"

int main(void)
{
    boot_trial_mailbox_t mailbox;
    uint8_t slot = 0xffU;
    uint32_t sequence = 99U;

    memset(&mailbox, 0, sizeof(mailbox));
    if (boot_trial_consume_confirmation(
            &mailbox, &slot, &sequence) !=
        BOOT_TRIAL_NO_CONFIRMATION) return 1;
    if (slot != 0xffU || sequence != 0U) return 2;

    if (boot_trial_publish(&mailbox, 2U, 10U) !=
        BOOT_TRIAL_INVALID_ARGUMENT) return 3;
    if (mailbox.token != 0U ||
        mailbox.token_inverse != 0U ||
        mailbox.sequence != 0U ||
        mailbox.sequence_inverse != 0U) return 4;

    if (boot_trial_publish(&mailbox, 1U, 42U) !=
        BOOT_TRIAL_OK) return 5;
    if (mailbox.token !=
            (BOOT_TRIAL_HANDOFF_MAGIC | 1U) ||
        mailbox.token_inverse != ~mailbox.token ||
        mailbox.sequence != 42U ||
        mailbox.sequence_inverse != ~42U) return 6;

    /* A watchdog reset leaves a handoff, not a confirmation. */
    if (boot_trial_consume_confirmation(
            &mailbox, &slot, &sequence) !=
        BOOT_TRIAL_NO_CONFIRMATION) return 7;
    if (mailbox.token != 0U ||
        mailbox.sequence != 0U) return 8;

    if (boot_trial_publish(&mailbox, 1U, 43U) !=
        BOOT_TRIAL_OK) return 9;
    if (boot_trial_arm_confirmation(&mailbox) !=
        BOOT_TRIAL_OK) return 10;
    if (mailbox.token !=
            (BOOT_TRIAL_CONFIRM_MAGIC | 1U) ||
        mailbox.token_inverse != ~mailbox.token) return 11;
    if (boot_trial_consume_confirmation(
            &mailbox, &slot, &sequence) !=
        BOOT_TRIAL_OK) return 12;
    if (slot != 1U || sequence != 43U) return 13;

    /* A consumed token cannot be replayed. */
    if (boot_trial_consume_confirmation(
            &mailbox, &slot, &sequence) !=
        BOOT_TRIAL_NO_CONFIRMATION) return 14;

    /* Torn sequence and token writes are cleared and rejected. */
    if (boot_trial_publish(&mailbox, 0U, 44U) !=
        BOOT_TRIAL_OK) return 15;
    mailbox.sequence_inverse = 0U;
    if (boot_trial_arm_confirmation(&mailbox) !=
        BOOT_TRIAL_NO_HANDOFF) return 16;
    if (mailbox.token != 0U ||
        mailbox.sequence != 0U) return 17;

    if (boot_trial_publish(&mailbox, 0U, 45U) !=
        BOOT_TRIAL_OK) return 18;
    if (boot_trial_arm_confirmation(&mailbox) !=
        BOOT_TRIAL_OK) return 19;
    mailbox.token_inverse = 0U;
    if (boot_trial_consume_confirmation(
            &mailbox, &slot, &sequence) !=
        BOOT_TRIAL_NO_CONFIRMATION) return 20;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "boot_trial_test.c"
            executable = directory_path / "boot_trial_test"
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
