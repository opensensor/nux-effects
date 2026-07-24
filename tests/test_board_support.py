import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BoardSupportSafetyTests(unittest.TestCase):
    def test_board_adapter_is_opt_in_and_not_in_default_boot(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        boot_sources = cmake[
            cmake.index("add_executable(\n    ncr2_bootloader") :
            cmake.index("target_include_directories(\n    ncr2_bootloader")
        ]

        self.assertRegex(
            cmake,
            re.compile(
                r"option\(\s*NCR2_BUILD_MCUX_BOARD_ADAPTER"
                r".*?\s+OFF\s*\)",
                re.DOTALL,
            ),
        )
        self.assertNotIn("ncr2_board.c", boot_sources)
        self.assertIn("ncr2_hardware_link_probe", cmake)
        self.assertIn("NCR2_OPEN_USB_VID=0", cmake)
        self.assertIn("NCR2_OPEN_USB_PID=0", cmake)

    def test_recovery_pin_mapping_and_polarity_are_exact(self):
        source = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "board"
            / "ncr2_board.c"
        ).read_text()

        self.assertIn(
            "IOMUXC_GPIO_AD_B1_05_GPIO1_IO21",
            source,
        )
        self.assertIn(
            "IOMUXC_GPIO_SD_B1_02_GPIO3_IO02",
            source,
        )
        self.assertIn("primary == UINT32_C(0)", source)
        self.assertIn("guard != UINT32_C(0)", source)
        self.assertNotIn("GPIO_PinWrite", source)
        self.assertNotIn("GPIO_PortSet", source)
        self.assertNotIn("GPIO_PortClear", source)

    def test_retained_mailbox_uses_src_gpr8_and_gpr9(self):
        layout = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "include"
            / "ncr2_flash_layout.h"
        ).read_text()

        self.assertIn(
            "#define NCR2_BOOT_MAILBOX_ADDRESS "
            "UINT32_C(0x400F803C)",
            layout,
        )
        self.assertIn(
            "#define NCR2_BOOT_MAILBOX_SIZE "
            "UINT32_C(0x00000008)",
            layout,
        )
        self.assertNotIn("NCR2_DTCM_USABLE_START", layout)

    def test_usb_irq_has_a_vector_but_defaults_to_weak_handler(self):
        startup = (
            ROOT
            / "firmware"
            / "bootloader"
            / "src"
            / "startup.S"
        ).read_text()

        self.assertIn(".word USB_OTG1_IRQHandler", startup)
        self.assertIn(".rept 127", startup)
        self.assertIn(".rept 46", startup)
        self.assertIn(".weak USB_OTG1_IRQHandler", startup)
        self.assertIn(
            ".thumb_set USB_OTG1_IRQHandler, Default_Handler",
            startup,
        )


if __name__ == "__main__":
    unittest.main()
