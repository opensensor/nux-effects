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
            "NCR2_USB_DEVICE_BCD UINT16_C(0x0002)",
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
