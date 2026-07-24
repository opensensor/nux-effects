import hashlib
import unittest
from pathlib import Path

from tools.nux_dfu import (
    ENGINE_SLOT_SIZE,
    FLASH_BASE,
    RECORD_SIZE,
    build_bina,
    parse_bina,
    reports_from_bina,
    replace_engine_slot,
)


ROOT = Path(__file__).resolve().parents[1]
STOCK_SHA256 = (
    "4263ef41c0745f6e8c00be13b52391b6b04a5f51779b12d0e191abf6888e7a14"
)
TEST_BINA_SHA256 = (
    "bb7f1713268e6fcc7b656c572af9674c28a74ce1f37a48708ba2ad1511dfb868"
)
RESTORE_BINA_SHA256 = (
    "92a30e9878d237f4e8c7f91e4c17e0204dd90fde0620853f6176184885a527bb"
)


class BinaProtocolTests(unittest.TestCase):
    def test_build_parse_and_report_round_trip(self) -> None:
        image = bytes((index * 17) & 0xFF for index in range(4 * 512))
        bina = build_bina(image, b"TEST0001")
        parsed = parse_bina(bina)

        self.assertEqual(parsed.product, b"COREDLX")
        self.assertEqual(parsed.version, b"TEST0001")
        self.assertEqual(parsed.declared_length, len(image))
        self.assertEqual(parsed.image, image)
        self.assertEqual(len(bina) % RECORD_SIZE, 0)

        reports = list(reports_from_bina(bina))
        self.assertTrue(all(len(report) == 64 for report in reports))
        first_record = b"".join(report[4:] for report in reports[:9])
        self.assertEqual(first_record, bina[:RECORD_SIZE])

    def test_generated_artifacts_match_verified_stock(self) -> None:
        required = (
            ROOT / "dump1.bin",
            ROOT / "eng3-slot1.bina",
            ROOT / "restore-stock-slots.bina",
        )
        if not all(path.exists() for path in required):
            self.skipTest("private device artifacts are not present")
        stock = (ROOT / "dump1.bin").read_bytes()
        self.assertEqual(hashlib.sha256(stock).hexdigest(), STOCK_SHA256)
        self.assertEqual(stock[0x20000], 1)

        test_bina = (ROOT / "eng3-slot1.bina").read_bytes()
        restore_bina = (ROOT / "restore-stock-slots.bina").read_bytes()
        self.assertEqual(
            hashlib.sha256(test_bina).hexdigest(), TEST_BINA_SHA256
        )
        self.assertEqual(
            hashlib.sha256(restore_bina).hexdigest(), RESTORE_BINA_SHA256
        )

        test_image = parse_bina(test_bina).image
        restore_image = parse_bina(restore_bina).image
        self.assertEqual(
            test_image[:ENGINE_SLOT_SIZE],
            stock[FLASH_BASE : FLASH_BASE + ENGINE_SLOT_SIZE],
        )
        self.assertEqual(
            test_image[ENGINE_SLOT_SIZE : 2 * ENGINE_SLOT_SIZE],
            stock[
                FLASH_BASE + 3 * ENGINE_SLOT_SIZE :
                FLASH_BASE + 4 * ENGINE_SLOT_SIZE
            ],
        )
        self.assertEqual(
            restore_image,
            stock[FLASH_BASE : FLASH_BASE + 2 * ENGINE_SLOT_SIZE],
        )

    def test_engine_replacement_is_limited_to_selected_slot(self) -> None:
        if not (ROOT / "dump1.bin").exists():
            self.skipTest("private stock dump is not present")
        stock = (ROOT / "dump1.bin").read_bytes()
        image = replace_engine_slot(
            stock, selected_slot=1, source_slot=3
        )
        self.assertEqual(
            image[:ENGINE_SLOT_SIZE],
            stock[FLASH_BASE : FLASH_BASE + ENGINE_SLOT_SIZE],
        )
        self.assertEqual(
            image[ENGINE_SLOT_SIZE:],
            stock[
                FLASH_BASE + 3 * ENGINE_SLOT_SIZE :
                FLASH_BASE + 4 * ENGINE_SLOT_SIZE
            ],
        )


if __name__ == "__main__":
    unittest.main()
