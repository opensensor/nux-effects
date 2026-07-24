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
        self.assertIn("NCR2_OPEN_USB_VID=0", cmake)
        self.assertIn("NCR2_OPEN_USB_PID=0", cmake)

        adapter = (USB_DIR / "recovery_usb.c").read_text()
        self.assertIn("NCR2_OPEN_USB_VID == 0U", adapter)
        self.assertIn("NCR2_OPEN_USB_PID == 0U", adapter)
        self.assertIn(
            "NCR2_OPEN_USB_VID == UINT16_C(0x9527)",
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
        self.assertNotIn("0x9527", descriptor.lower())


if __name__ == "__main__":
    unittest.main()
