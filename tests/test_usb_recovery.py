import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
USB_DIR = ROOT / "firmware" / "platform" / "ncr2" / "usb"


class UsbRecoverySafetyTests(unittest.TestCase):
    def test_adapter_is_opt_in_and_ids_default_to_unassigned(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        self.assertRegex(
            cmake,
            r"option\(\s*NCR2_BUILD_MCUX_USB_ADAPTER\s+"
            r'"[^"]+"\s+OFF\s*\)',
        )
        self.assertIn("set(NCR2_EFFECTIVE_USB_VID 0)", cmake)
        self.assertIn("set(NCR2_EFFECTIVE_USB_PID 0)", cmake)

        adapter = (USB_DIR / "recovery_usb.c").read_text()
        self.assertIn("NCR2_OPEN_USB_VID == 0U", adapter)
        self.assertIn("NCR2_OPEN_USB_PID == 0U", adapter)
        self.assertIn(
            "NCR2_OPEN_USB_VID == UINT16_C(0x9527)",
            adapter,
        )
        self.assertIn(
            "NCR2_ALLOW_BORROWED_NUX_DFU_ID == 0",
            adapter,
        )

    def test_open_descriptor_uses_protocol_report_size_and_endpoints(self):
        header = (USB_DIR / "recovery_usb_descriptor.h").read_text()

        def decimal_define(name):
            match = re.search(
                rf"#define {name} ([0-9]+)U",
                header,
            )
            self.assertIsNotNone(match, name)
            return int(match.group(1))

        self.assertEqual(decimal_define("NCR2_USB_REPORT_SIZE"), 64)
        self.assertEqual(decimal_define("NCR2_USB_ENDPOINT_IN"), 1)
        self.assertEqual(decimal_define("NCR2_USB_ENDPOINT_OUT"), 2)

        descriptor = (USB_DIR / "recovery_usb_descriptor.c").read_text()
        self.assertIn("NCR2_OPEN_USB_VID", descriptor)
        self.assertIn("NCR2_OPEN_USB_PID", descriptor)
        self.assertIn(
            "NCR2_USB_DEVICE_BCD UINT16_C(0x0005)",
            descriptor,
        )
        self.assertNotIn("0x9527", descriptor.lower())

    def test_flexspi_adapter_is_opt_in_and_device_guarded(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        self.assertRegex(
            cmake,
            r"option\(\s*NCR2_BUILD_MCUX_FLEXSPI_ADAPTER\s+"
            r'"[^"]+"\s+OFF\s*\)',
        )
        source = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "flexspi"
            / "ncr2_flexspi_nor.c"
        ).read_text()
        self.assertIn('section(".ramfunc.ncr2_flexspi")', source)
        self.assertIn("NCR2_W25Q64_MANUFACTURER UINT8_C(0xEF)", source)
        self.assertIn("NCR2_W25Q64_MEMORY_TYPE UINT8_C(0x40)", source)
        self.assertIn("NCR2_W25Q64_CAPACITY UINT8_C(0x17)", source)
        self.assertIn(
            "mutation_allowed(context, address, length)",
            source,
        )
        self.assertIn(
            "ncr2_flexspi_nor_init_full_flash",
            source,
        )
        # The stock FCFB declares only 4 MiB on an 8 MiB part, which put
        # both application slots outside the mapped window. Reprogramming
        # the port size must happen during init, before any slot is read.
        self.assertIn("FLEXSPI->FLSHCR0[0] = NCR2_FLASH_SIZE_KIB", source)
        self.assertIn("NCR2_FLASH_SIZE_KIB", source)
        self.assertLess(
            source.index("FLEXSPI->FLSHCR0[0]"),
            source.index("FLEXSPI_UpdateLUT("),
        )

        self.assertIn("NCR2_LUT_SEQUENCE_READ_DATA 10U", source)
        # The custom IP TX FIFO implementation failed physical readback.
        # Programming now delegates a full staged page to the immutable
        # RT1051 boot-ROM driver, with IP commands explicitly at 30 MHz.
        self.assertIn(
            '#include "fsl_romapi.h"',
            source,
        )
        self.assertIn(
            "NCR2_BOOTLOADER_TREE_POINTER UINT32_C(0x0020001C)",
            source,
        )
        self.assertIn(
            "config->ipcmdSerialClkFreq = kFLEXSPISerialClk_30MHz",
            source,
        )
        self.assertIn("ram_rom_program_page_call(", source)
        self.assertIn("aligned_bytes[index] = UINT8_C(0xFF)", source)
        self.assertNotIn("FLEXSPI->TFDR[", source)
        # ISEQID is four bits and the LUT is 64 words; both limits are
        # enforced at compile time after a sequence index of 16 silently
        # aliased to sequence 0 on hardware.
        self.assertIn("NCR2_LUT_SEQUENCE_LIMIT", source)
        self.assertIn("NCR2_LUT_TOTAL_WORDS", source)
        self.assertIn("ram_read_data(", source)
        self.assertNotIn(
            "const volatile uint8_t *source",
            source,
        )

        linker = (
            ROOT
            / "firmware"
            / "bootloader"
            / "ncr2_bootloader.ld"
        ).read_text()
        self.assertIn("DEFINED(FLEXSPI_UpdateLUT) ?", linker)
        for obsolete in (
            "FLEXSPI_ReadBlocking",
            "FLEXSPI_WriteBlocking",
            "FLEXSPI_TransferBlocking",
        ):
            self.assertNotIn(f"DEFINED({obsolete}) ?", linker)
        for bounded_primitive in (
            "NCR2_FLEXSPI_POLL_LIMIT",
            "NCR2_FLEXSPI_RESET_POLL_LIMIT",
            "NCR2_FLASH_BUSY_POLL_LIMIT",
        ):
            self.assertIn(bounded_primitive, source)


if __name__ == "__main__":
    unittest.main()
