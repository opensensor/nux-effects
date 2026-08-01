import re
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

    def test_open_app_consumes_before_recovery_and_maps_eight_destinations(self):
        source = (
            ROOT / "firmware" / "hardware_app" / "src" / "main.c"
        ).read_text()

        consume = source.index("ncr2_factory_engine_request_consume(")
        recovery = source.index("arm_recovery_request();", consume)
        self.assertLess(consume, recovery)
        self.assertIn("g_hardware_app_selector_position", source)
        self.assertIn("(uint8_t)selector_position", source)
        self.assertIn("g_open_engine_effects", source)
        table_start = source.index("g_open_engine_effects")
        table_end = source.index("};", table_start)
        table = source[table_start:table_end]
        for effect in (
            "NCR2_EFFECT_SHINE_DRIVE",
            "NCR2_EFFECT_BREATHE_VIBE",
            "NCR2_EFFECT_ECHOES_TAPE",
            "NCR2_EFFECT_GUERRILLA_TREM",
        ):
            self.assertIn(effect, table)
        self.assertIn(
            "g_hardware_app_factory_launch_status !=\n"
            "            NCR2_FACTORY_LAUNCH_OK",
            source,
        )

        self.assertIn("[NCR2_OPEN_ENGINE_COUNT][NCR2_EFFECT_COUNT]", table)
        assignments = re.findall(
            r"^[ \t]+(NCR2_EFFECT_[A-Z0-9_]+),$",
            table,
            re.M,
        )
        self.assertEqual(len(assignments), 32)
        self.assertEqual(len(set(assignments)), 32)
        self.assertIn(
            "requested_engine_slot < NCR2_FACTORY_ENGINE_COUNT",
            source,
        )
        self.assertIn(
            "requested_engine_slot - NCR2_OPEN_ENGINE_FIRST",
            source,
        )
        hold = source.index(
            "footswitch_event == NCR2_FOOTSWITCH_EVENT_HOLD"
        )
        armed = source.index("engine_select_armed = UINT8_C(1)", hold)
        released = source.index(
            "debounced_pressed == UINT8_C(0)", armed
        )
        reset = source.index("reset_into_engine_slot(engine_slot)", released)
        self.assertLess(hold, armed)
        self.assertLess(armed, released)
        self.assertLess(released, reset)

        reset_helper = source[
            source.index("static void reset_into_engine_slot") :
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

    def test_factory_monitor_maps_one_hold_across_all_eight_positions(self):
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
        self.assertNotIn("OPEN_HOLD_LOOPS", source)
        expected_thresholds = (3326, 2815, 2304, 1793, 1283, 772, 259)
        for index, threshold in enumerate(expected_thresholds, start=1):
            self.assertIn(
                f"TYPE_SLOT_{index}_MIN,        {threshold}",
                source,
            )
        self.assertIn("FACTORY_REQUEST_MAGIC,  0x46414330", source)
        self.assertNotIn("stage_open", source)
        thresholds = [
            int(re.search(
                rf"\.equ TYPE_SLOT_{index}_MIN,\s+(\d+)", source
            ).group(1))
            for index in range(1, 8)
        ]

        def destination(adc):
            for index, threshold in enumerate(thresholds):
                if adc >= threshold:
                    return index
            return 7

        measured = (3581, 3071, 2560, 2049, 1538, 1028, 516, 2)
        self.assertEqual(
            [destination(adc) for adc in measured],
            list(range(8)),
        )
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
