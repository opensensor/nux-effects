import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class HardwareBootloaderSafetyTests(unittest.TestCase):
    def test_hardware_bootloader_is_opt_in_and_readonly_by_default(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        source = (
            ROOT
            / "firmware"
            / "bootloader"
            / "src"
            / "bootloader_hardware.c"
        ).read_text()

        self.assertRegex(
            cmake,
            re.compile(
                r"option\(\s*NCR2_BUILD_HARDWARE_BOOTLOADER"
                r".*?\s+OFF\s*\)",
                re.DOTALL,
            ),
        )
        self.assertRegex(
            cmake,
            re.compile(
                r"option\(\s*NCR2_HARDWARE_RECOVERY_WRITE_ENABLE"
                r".*?\s+OFF\s*\)",
                re.DOTALL,
            ),
        )
        self.assertIn(
            "#if NCR2_HARDWARE_RECOVERY_WRITE_ENABLE",
            source,
        )
        self.assertIn("readonly_erase", source)
        self.assertIn("readonly_program", source)
        self.assertIn("readonly_store_boot_state", source)
        self.assertIn(
            "ncr2_hardware_recovery_readonly",
            source,
        )

    def test_default_bootloader_excludes_hardware_main(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        default_sources = cmake[
            cmake.index("add_executable(\n    ncr2_bootloader") :
            cmake.index(
                "target_include_directories(\n    ncr2_bootloader"
            )
        ]
        hardware_start = cmake.index(
            "add_executable(\n            ncr2_hardware_bootloader"
        )
        hardware_end = cmake.index(
            "target_include_directories(\n"
            "            ncr2_hardware_bootloader",
            hardware_start,
        )
        hardware_sources = cmake[hardware_start:hardware_end]

        self.assertIn("bootloader_offline.c", default_sources)
        self.assertNotIn(
            "bootloader_hardware.c",
            default_sources,
        )
        self.assertIn(
            "bootloader_hardware.c",
            hardware_sources,
        )
        self.assertNotIn(
            "bootloader_offline.c",
            hardware_sources,
        )

    def test_usb_enumeration_is_separately_gated(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        usb = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "usb"
            / "recovery_usb.c"
        ).read_text()

        self.assertRegex(
            cmake,
            re.compile(
                r"option\(\s*NCR2_ENABLE_HARDWARE_USB_ENUMERATION"
                r".*?\s+OFF\s*\)",
                re.DOTALL,
            ),
        )
        self.assertIn(
            "NUX VID 0x9527 is forbidden",
            cmake,
        )
        self.assertIn(
            "NCR2_EFFECTIVE_USB_VID 0",
            cmake,
        )
        self.assertIn(
            "NCR2_OPEN_USB_VID == UINT16_C(0x9527)",
            usb,
        )
        self.assertIn(
            "core/components/osa/fsl_os_abstraction_bm.c",
            cmake,
        )
        self.assertIn("DATA_SECTION_IS_CACHEABLE=0", cmake)

    def test_usb_dma_layout_is_part_of_the_hardware_gate(self):
        checker = (
            ROOT / "tools" / "check_hardware_bootloader.py"
        ).read_text()

        self.assertIn("USB_DMA_SYMBOL_ALIGNMENTS", checker)
        self.assertIn('"qh_buffer": 2048', checker)
        self.assertIn('"s_UsbDeviceEhciDtd": 32', checker)
        self.assertIn("DTCM_START <= address < DTCM_TOP", checker)
        self.assertIn("verify_flash_load_segments", checker)

    def test_startup_installs_the_boot_vector_table(self):
        startup = (
            ROOT / "firmware" / "bootloader" / "src" / "startup.S"
        ).read_text()
        checker = (
            ROOT / "tools" / "check_hardware_bootloader.py"
        ).read_text()

        self.assertIn("0xE000ED08", startup)
        self.assertIn("g_boot_vectors", startup)
        self.assertIn("reset_installs_vtor", checker)


if __name__ == "__main__":
    unittest.main()
