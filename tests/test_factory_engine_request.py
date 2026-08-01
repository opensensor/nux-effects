import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class FactoryEngineRequestTests(unittest.TestCase):
    def test_one_word_request_is_validated_consumed_and_one_shot(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>

#include "factory_engine_request.h"

int main(void)
{
    ncr2_factory_engine_mailbox_t mailbox = { UINT32_C(0) };
    uint8_t engine = UINT8_C(99);

    if (ncr2_factory_engine_request_consume(&mailbox, &engine) !=
        NCR2_FACTORY_REQUEST_NONE) return 1;
    if (engine != UINT8_C(0) || mailbox.token != UINT32_C(0)) return 2;

    for (uint8_t candidate = UINT8_C(0);
         candidate < NCR2_ENGINE_SLOT_COUNT;
         ++candidate) {
        if (ncr2_factory_engine_request_arm(&mailbox, candidate) !=
            NCR2_FACTORY_REQUEST_OK) return 3;
        if (mailbox.token !=
            (NCR2_FACTORY_REQUEST_MAGIC | (uint32_t)candidate)) return 4;
        if (ncr2_factory_engine_request_consume(&mailbox, &engine) !=
            NCR2_FACTORY_REQUEST_OK) return 5;
        if (engine != candidate || mailbox.token != UINT32_C(0)) return 6;
        if (ncr2_factory_engine_request_consume(&mailbox, &engine) !=
            NCR2_FACTORY_REQUEST_NONE) return 7;
    }

    mailbox.token = UINT32_C(0xffffffff);
    if (ncr2_factory_engine_request_consume(&mailbox, &engine) !=
        NCR2_FACTORY_REQUEST_NONE) return 8;
    if (mailbox.token != UINT32_C(0)) return 9;

    mailbox.token = UINT32_C(0x12345678);
    if (ncr2_factory_engine_request_arm(
            &mailbox, NCR2_ENGINE_SLOT_COUNT) !=
        NCR2_FACTORY_REQUEST_INVALID_ARGUMENT) return 10;
    if (mailbox.token != UINT32_C(0)) return 11;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "factory_request_test.c"
            executable = directory_path / "factory_request_test"
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
                    str(
                        ROOT
                        / "firmware"
                        / "factory_bridge"
                        / "include"
                    ),
                    str(
                        ROOT
                        / "firmware"
                        / "factory_bridge"
                        / "src"
                        / "factory_engine_request.c"
                    ),
                    str(test_source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([executable], check=True)

    def test_rt1051_mailbox_is_exactly_src_gpr10(self):
        layout = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "include"
            / "ncr2_flash_layout.h"
        ).read_text()

        self.assertIn(
            "NCR2_FACTORY_REQUEST_MAILBOX_ADDRESS UINT32_C(0x400F8044)",
            layout,
        )
        self.assertIn(
            "NCR2_FACTORY_REQUEST_MAILBOX_SIZE UINT32_C(0x00000004)",
            layout,
        )


if __name__ == "__main__":
    unittest.main()
