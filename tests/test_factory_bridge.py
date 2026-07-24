import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class FactoryBridgeTests(unittest.TestCase):
    def test_bridge_is_original_source_and_fail_closed(self):
        source = (
            ROOT
            / "firmware"
            / "factory_bridge"
            / "src"
            / "startup.S"
        ).read_text()

        self.assertIn("FACTORY_ENGINE_XIP,       0x600c0000", source)
        self.assertIn("FACTORY_ENGINE_COPY_SIZE, 0x0001e000", source)
        self.assertIn("FACTORY_ENGINE_STACK,     0x20018000", source)
        self.assertIn("FACTORY_ENGINE_RESET,     0x0000e4b5", source)
        self.assertIn("cmp     r2, r3", source)
        self.assertIn("bne     bridge_fault", source)
        self.assertIn("dsb     sy", source)
        self.assertIn("isb     sy", source)

    def test_bridge_is_an_explicit_nondefault_target(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        start = cmake.index(
            "add_executable(\n    ncr2_factory_bridge"
        )
        end = cmake.index(
            "if(NCR2_BUILD_MCUX_USB_ADAPTER)",
            start,
        )
        declaration = cmake[start:end]

        self.assertIn("EXCLUDE_FROM_ALL", declaration)
        self.assertIn("factory_bridge/src/startup.S", declaration)
        self.assertIn("app/ncr2_app.ld", declaration)

    def test_post_link_checker_pins_the_audited_engine(self):
        checker = (
            ROOT / "tools" / "check_factory_bridge.py"
        ).read_text()

        self.assertIn('"ncr2_factory_engine_source": 0x600C0000', checker)
        self.assertIn('"ncr2_factory_engine_copy_size": 0x0001E000', checker)
        self.assertIn('"ncr2_factory_engine_stack": 0x20018000', checker)
        self.assertIn('"ncr2_factory_engine_reset": 0x0000E4B5', checker)


if __name__ == "__main__":
    unittest.main()
