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
        self.assertIn(".word DMA0_DMA16_IRQHandler", source)
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
        self.assertIn('"DMA0_DMA16_IRQHandler"', checker)
        self.assertIn('"SystemInit"', checker)
        self.assertIn("vectors[16]", checker)

    def test_audio_passthrough_is_explicitly_opt_in(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        source = (
            ROOT
            / "firmware"
            / "factory_slot"
            / "src"
            / "audio_passthrough.c"
        ).read_text()

        self.assertIn("NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH", cmake)
        self.assertIn(
            'option(\n'
            '    NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH\n'
            '    "Enable the recovered unapproved SAI1/eDMA '
            'factory-slot passthrough"\n'
            '    OFF\n'
            ')',
            cmake,
        )
        self.assertIn("#if NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH", source)
        self.assertIn("SAI1->TCR4 = UINT32_C(0x00031f1b)", source)
        self.assertIn("NCR2_AUDIO_RX_DMAMUX_SOURCE UINT32_C(19)", source)
        self.assertIn("NCR2_AUDIO_TX_DMAMUX_SOURCE UINT32_C(20)", source)

    def test_factory_board_controls_are_separately_opt_in(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        source = (
            ROOT
            / "firmware"
            / "factory_slot"
            / "src"
            / "board_control.c"
        ).read_text()

        self.assertIn(
            'option(\n'
            '    NCR2_FACTORY_SLOT_BOARD_CONTROLS\n'
            '    "Enable recovered unapproved factory-slot GPIO '
            'startup levels"\n'
            '    OFF\n'
            ')',
            cmake,
        )
        self.assertIn("#if NCR2_FACTORY_SLOT_BOARD_CONTROLS", source)
        self.assertIn(
            "factory-slot board controls require the audio passthrough",
            cmake,
        )
        self.assertIn("IOMUXC_GPIO_AD_B1_10_GPIO1_IO26", source)
        self.assertIn("NCR2_GPIO1_INITIAL_HIGH", source)
        self.assertIn("NCR2_GPIO2_INITIAL_HIGH", source)
        self.assertIn("NCR2_BOARD_RELEASE_DELAY_US", source)

    def test_source_audio_emulator_is_offline_only(self):
        source = (
            ROOT / "tools" / "emulate_source_audio.py"
        ).read_text()

        self.assertNotIn("/dev/", source)
        self.assertNotIn("hidraw", source)
        self.assertNotIn("usb.core", source)
        self.assertIn("EXPECTED_SAI", source)
        self.assertIn("_validate_passthrough_isr", source)


if __name__ == "__main__":
    unittest.main()
