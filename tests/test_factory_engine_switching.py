import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class FactoryEngineSwitchingTests(unittest.TestCase):
    def test_launcher_pins_all_four_preserved_main_loop_hooks(self):
        source = (
            ROOT
            / "firmware"
            / "factory_bridge"
            / "src"
            / "factory_engine_launcher.c"
        ).read_text()

        for descriptor in (
            (
                "0x60060000", "0x0000f8cd", "0x00001bf8",
                "0xf998f002", "0x00003f2d", "0x6a504a47",
            ),
            (
                "0x60080000", "0x00019095", "0x00012402",
                "0xfd96f001", "0x00013f33", "0x6a504a37",
            ),
            (
                "0x600a0000", "0x0000c04d", "0x00003d3e",
                "0xf9d9f001", "0x000050f5", "0x6a504a37",
            ),
            (
                "0x600c0000", "0x0000e4b5", "0x00005880",
                "0xff57f001", "0x00007733", "0x6a504a27",
            ),
        ):
            for value in descriptor:
                self.assertIn(value, source)
        self.assertLess(source.index("vectors[0]"), source.index("__disable_irq"))
        self.assertLess(
            source.index("ncr2_factory_compat_prepare();"),
            source.rindex("ncr2_factory_engine_copy_to_itcm("),
        )
        self.assertLess(
            source.index("source_matches_patch_contract(descriptor)"),
            source.rindex("ncr2_factory_engine_copy_to_itcm("),
        )
        self.assertIn("encode_thumb_bl", source)
        self.assertIn("descriptor->hook_instruction", source)
        self.assertIn("descriptor->original_led_prefix", source)
        self.assertIn("*original_led = descriptor->original_led;", source)
        self.assertLess(
            source.index("hook[0] = (uint16_t)monitor_call;"),
            source.rindex("ncr2_factory_engine_sync_and_jump("),
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
        self.assertIn(
            "g_hardware_app_factory_launch_status !=\n"
            "            NCR2_FACTORY_LAUNCH_OK",
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

        self.assertIn('"ncr2_factory_engine_copy_to_itcm"', checker)
        self.assertIn('"ncr2_factory_engine_launch"', checker)
        self.assertIn('"ncr2_factory_return_monitor"', checker)
        self.assertIn(
            '"ncr2_factory_return_monitor_original_led"', checker
        )
        self.assertIn("check_factory_monitor(arguments.binary, found)", checker)
        self.assertIn("0x401B8008", checker)
        self.assertIn("0x403B002C", checker)
        self.assertIn("0x46414330", checker)
        self.assertIn("forbidden_words", checker)

    def test_factory_monitor_selects_by_knob_or_returns_after_release(self):
        source = (
            ROOT
            / "firmware"
            / "factory_bridge"
            / "src"
            / "factory_return_monitor.S"
        ).read_text()

        self.assertIn("GPIO1_PSR,              0x401b8008", source)
        self.assertNotIn("GPIO1_DR", source)
        self.assertIn("TYPE_ADC_RESULT_3_2,    0x403b002c", source)
        self.assertIn("SRC_GPR7,               0x400f8038", source)
        self.assertIn("SRC_GPR10,              0x400f8044", source)
        self.assertNotIn("SRC_GPR8", source)
        self.assertNotIn("SRC_GPR9", source)
        self.assertIn("SELECT_HOLD_LOOPS,      2000", source)
        self.assertIn("OPEN_HOLD_LOOPS,        5000", source)
        self.assertIn("TYPE_DELAY_MIN,         2815", source)
        self.assertIn("TYPE_REVERB_MIN,        1793", source)
        self.assertIn("TYPE_MOD_MIN,           772", source)
        self.assertIn("FACTORY_REQUEST_MAGIC,  0x46414330", source)
        self.assertIn(
            "ncr2_factory_return_monitor_original_led", source
        )
        self.assertIn("chain_factory_led:", source)
        self.assertIn("bx      r0", source)
        self.assertIn(".org 0xfc", source)
        self.assertLess(
            source.index("released:"),
            source.index("AIRCR_SYSRESETREQ", source.index("released:")),
        )
        self.assertLess(source.index(".ltorg"), source.index("_end:"))


if __name__ == "__main__":
    unittest.main()
