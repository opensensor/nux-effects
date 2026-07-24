import importlib.util
from pathlib import Path
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MODULE_PATH = ROOT / "tools" / "ncr2_rom_recover.py"
SPEC = importlib.util.spec_from_file_location("ncr2_rom_recover", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
rom_recover = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(rom_recover)


class RomRecoveryTests(unittest.TestCase):
    def test_vendor_assets_match_pinned_hashes(self):
        rom_recover.validate_vendor_assets()

    def test_validates_complete_imxrt_image(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "image.bin"
            data = bytearray(b"\xff" * rom_recover.FLASH_SIZE)
            data[0:4] = b"FCFB"
            data[rom_recover.IVT_OFFSET : rom_recover.IVT_OFFSET + 4] = (
                b"\xd1\x00\x20\x41"
            )
            image.write_bytes(data)
            digest = rom_recover.validate_flash_image(image)
            self.assertEqual(rom_recover.validate_flash_image(image, digest), digest)

    def test_rejects_wrong_size_or_headers(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / "bad.bin"
            image.write_bytes(b"FCFB")
            with self.assertRaisesRegex(rom_recover.RecoveryError, "exactly"):
                rom_recover.validate_flash_image(image)

            image.write_bytes(b"\xff" * rom_recover.FLASH_SIZE)
            with self.assertRaisesRegex(rom_recover.RecoveryError, "does not begin"):
                rom_recover.validate_flash_image(image)

    def test_flash_parser_defaults_to_verify_and_requires_execute(self):
        parser = rom_recover.build_parser()
        arguments = parser.parse_args(["flash", "--image", "image.bin"])
        self.assertTrue(arguments.verify)
        self.assertFalse(arguments.execute)

        arguments = parser.parse_args(
            ["flash", "--image", "image.bin", "--execute", "--no-verify"]
        )
        self.assertFalse(arguments.verify)
        self.assertTrue(arguments.execute)


if __name__ == "__main__":
    unittest.main()
