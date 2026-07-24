import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class FactorySlotSourceTests(unittest.TestCase):
    def test_startup_matches_factory_itcm_handoff(self):
        source = (
            ROOT
            / "firmware"
            / "factory_slot"
            / "src"
            / "startup.S"
        ).read_text()

        self.assertIn(".global g_factory_slot_vectors", source)
        self.assertIn(".word __stack_top", source)
        self.assertIn("msr     msp", source)
        self.assertIn("0xe000ed08", source)
        self.assertIn("bl      SystemInit", source)
        self.assertIn("bl      application_main", source)

    def test_linker_enforces_factory_copy_budget(self):
        linker = (
            ROOT
            / "firmware"
            / "factory_slot"
            / "ncr2_factory_slot.ld"
        ).read_text()

        self.assertIn("ORIGIN = 0x00000000", linker)
        self.assertIn("LENGTH = 0x0001e000", linker)
        self.assertIn("ORIGIN = 0x20000000", linker)
        self.assertIn("LENGTH = 0x00020000", linker)
        self.assertIn("__factory_slot_image_end", linker)
        self.assertIn("ASSERT(__factory_slot_image_end", linker)

    def test_factory_slot_is_explicit_and_sdk_pinned(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()

        self.assertIn("NCR2_BUILD_FACTORY_SLOT_APP", cmake)
        self.assertIn("ncr2_factory_slot_app", cmake)
        self.assertIn("EXCLUDE_FROM_ALL", cmake)
        self.assertIn(
            "core/devices/MIMXRT1051/system_MIMXRT1051.c", cmake
        )
        self.assertIn("factory_slot/ncr2_factory_slot.ld", cmake)

    def test_post_link_checker_pins_factory_abi(self):
        checker = (
            ROOT / "tools" / "check_factory_slot.py"
        ).read_text()

        self.assertIn("ITCM_COPY_END = 0x0001E000", checker)
        self.assertIn("DTCM_END = 0x20020000", checker)
        self.assertIn('"g_factory_slot_vectors"', checker)
        self.assertIn('"SystemInit"', checker)


if __name__ == "__main__":
    unittest.main()
