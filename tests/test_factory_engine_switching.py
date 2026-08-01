import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class FactoryEngineSwitchingTests(unittest.TestCase):
    def test_launcher_pins_all_four_preserved_vector_pairs(self):
        source = (
            ROOT
            / "firmware"
            / "factory_bridge"
            / "src"
            / "factory_engine_launcher.c"
        ).read_text()

        for descriptor in (
            "0x60060000), UINT32_C(0x20018000), UINT32_C(0x0000f8cd)",
            "0x60080000), UINT32_C(0x20018000), UINT32_C(0x00019095)",
            "0x600a0000), UINT32_C(0x20020000), UINT32_C(0x0000c04d)",
            "0x600c0000), UINT32_C(0x20018000), UINT32_C(0x0000e4b5)",
        ):
            self.assertIn(descriptor, source)
        self.assertLess(source.index("vectors[0]"), source.index("__disable_irq"))
        self.assertLess(
            source.index("ncr2_factory_compat_prepare();"),
            source.rindex("ncr2_factory_engine_copy_and_jump("),
        )

    def test_copy_is_bounded_and_invalidates_instruction_cache(self):
        source = (
            ROOT
            / "firmware"
            / "factory_bridge"
            / "src"
            / "factory_engine_copy.S"
        ).read_text()

        self.assertIn("FACTORY_ENGINE_COPY_SIZE, 0x0001e000", source)
        self.assertIn("SCB_ICIALLU,              0xe000ef50", source)
        self.assertIn("cpsid   i", source)
        self.assertIn("dsb     sy", source)
        self.assertIn("isb     sy", source)
        self.assertIn("bx      r1", source)

    def test_open_app_consumes_before_recovery_and_maps_detent_pairs(self):
        source = (
            ROOT / "firmware" / "hardware_app" / "src" / "main.c"
        ).read_text()

        consume = source.index("ncr2_factory_engine_request_consume(")
        recovery = source.index("arm_recovery_request();", consume)
        self.assertLess(consume, recovery)
        self.assertIn(
            "g_hardware_app_effect_index / UINT32_C(2)",
            source,
        )

        reset_helper = source[
            source.index("static void reset_into_factory_engine") :
            source.index("static void enable_hang_watchdog")
        ]
        self.assertLess(
            reset_helper.index("clear_recovery_request();"),
            reset_helper.index("ncr2_factory_engine_request_arm("),
        )
        self.assertLess(
            reset_helper.index("ncr2_factory_engine_request_arm("),
            reset_helper.index("NVIC_SystemReset();"),
        )

    def test_hardware_post_link_contract_requires_dynamic_launcher(self):
        checker = (ROOT / "tools" / "check_hardware_audio_app.py").read_text()

        self.assertIn('"ncr2_factory_engine_copy_and_jump"', checker)
        self.assertIn('"ncr2_factory_engine_launch"', checker)


if __name__ == "__main__":
    unittest.main()
