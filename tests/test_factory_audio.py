import pathlib
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]


class FactoryAudioEvidenceTests(unittest.TestCase):
    def test_executed_audio_contract_is_recorded(self):
        document = (
            ROOT / "docs" / "hardware" / "FACTORY_AUDIO.md"
        ).read_text()

        self.assertIn("**SAI1 + eDMA**", document)
        self.assertIn("48,000 Hz", document)
        self.assertIn("Words per frame | 4", document)
        self.assertIn("0x2000bf40", document)
        self.assertIn("8 audio frames", document)

    def test_emulator_is_offline_and_pins_verified_inputs(self):
        source = (
            ROOT / "tools" / "emulate_factory_audio.py"
        ).read_text()

        self.assertIn("VERIFIED_DUMP_SHA256", source)
        self.assertIn("factory engine does not match", source)
        self.assertIn("audio_init_complete", source)
        self.assertNotIn("/dev/hidraw", source)
        self.assertNotIn("usb.core", source)

    def test_active_path_is_not_reported_as_sai2_or_sai3(self):
        readme = (ROOT / "README.md").read_text()
        hardware_map = (
            ROOT / "docs" / "hardware" / "HARDWARE_MAP.md"
        ).read_text()

        self.assertIn(
            "SAI1 with eDMA for the executed stock audio path",
            readme,
        )
        self.assertIn(
            "SAI2 and SAI3 (shared SDK code present; not executed",
            hardware_map,
        )


if __name__ == "__main__":
    unittest.main()
