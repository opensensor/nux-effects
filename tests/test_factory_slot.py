import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class FactorySlotSourceTests(unittest.TestCase):
    def test_startup_matches_factory_itcm_handoff(self):
        source = (
            ROOT
            / "firmware"
            / "factory_slot"
            / "src"
            / "startup.S"
        ).read_text()

        self.assertIn(".global g_factory_slot_vectors", source)
        self.assertIn(".word __stack_top", source)
        self.assertIn(".word DMA0_DMA16_IRQHandler", source)
        self.assertIn("msr     msp", source)
        self.assertIn("0xe000ed08", source)
        self.assertIn("bl      SystemInit", source)
        self.assertIn("bl      application_main", source)

    def test_linker_enforces_factory_copy_budget(self):
        linker = (
            ROOT
            / "firmware"
            / "factory_slot"
            / "ncr2_factory_slot.ld"
        ).read_text()

        self.assertIn("ORIGIN = 0x00000000", linker)
        self.assertIn("LENGTH = 0x0001e000", linker)
        self.assertIn("ORIGIN = 0x20000000", linker)
        self.assertIn("LENGTH = 0x00020000", linker)
        self.assertIn("__factory_slot_image_end", linker)
        self.assertIn("ASSERT(__factory_slot_image_end", linker)

    def test_factory_slot_is_explicit_and_sdk_pinned(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()

        self.assertIn("NCR2_BUILD_FACTORY_SLOT_APP", cmake)
        self.assertIn("ncr2_factory_slot_app", cmake)
        self.assertIn("EXCLUDE_FROM_ALL", cmake)
        self.assertIn(
            "core/devices/MIMXRT1051/system_MIMXRT1051.c", cmake
        )
        self.assertIn("factory_slot/ncr2_factory_slot.ld", cmake)

    def test_post_link_checker_pins_factory_abi(self):
        checker = (
            ROOT / "tools" / "check_factory_slot.py"
        ).read_text()

        self.assertIn("ITCM_COPY_END = 0x0001E000", checker)
        self.assertIn("DTCM_END = 0x20020000", checker)
        self.assertIn('"g_factory_slot_vectors"', checker)
        self.assertIn('"DMA0_DMA16_IRQHandler"', checker)
        self.assertIn('"SystemInit"', checker)
        self.assertIn("vectors[16]", checker)

    def test_audio_passthrough_is_explicitly_opt_in(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        source = (
            ROOT
            / "firmware"
            / "factory_slot"
            / "src"
            / "audio_passthrough.c"
        ).read_text()

        self.assertIn("NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH", cmake)
        self.assertIn(
            'option(\n'
            '    NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH\n'
            '    "Enable the recovered unapproved SAI1/eDMA '
            'factory-slot passthrough"\n'
            '    OFF\n'
            ')',
            cmake,
        )
        self.assertIn("#if NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH", source)
        self.assertIn("SAI1->TCR4 = UINT32_C(0x00031f1b)", source)
        self.assertIn("NCR2_AUDIO_RX_DMAMUX_SOURCE UINT32_C(19)", source)
        self.assertIn("NCR2_AUDIO_TX_DMAMUX_SOURCE UINT32_C(20)", source)

    def test_factory_board_controls_are_separately_opt_in(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        source = (
            ROOT
            / "firmware"
            / "factory_slot"
            / "src"
            / "board_control.c"
        ).read_text()

        self.assertIn(
            'option(\n'
            '    NCR2_FACTORY_SLOT_BOARD_CONTROLS\n'
            '    "Enable recovered unapproved factory-slot GPIO '
            'startup levels"\n'
            '    OFF\n'
            ')',
            cmake,
        )
        self.assertIn("#if NCR2_FACTORY_SLOT_BOARD_CONTROLS", source)
        self.assertIn(
            "factory-slot board controls require the audio passthrough",
            cmake,
        )
        self.assertIn("IOMUXC_GPIO_AD_B1_10_GPIO1_IO26", source)
        self.assertIn("NCR2_GPIO1_INITIAL_HIGH", source)
        self.assertIn("NCR2_GPIO2_INITIAL_HIGH", source)
        self.assertIn("NCR2_BOARD_RELEASE_DELAY_US", source)

    def test_bicolor_indicator_uses_recovered_active_low_pair(self):
        header = (
            ROOT
            / "firmware"
            / "factory_slot"
            / "include"
            / "factory_board.h"
        ).read_text()
        source = (
            ROOT
            / "firmware"
            / "factory_slot"
            / "src"
            / "board_control.c"
        ).read_text()
        application = (
            ROOT
            / "firmware"
            / "hardware_app"
            / "src"
            / "main.c"
        ).read_text()

        self.assertIn("NCR2_FACTORY_INDICATOR_IO24", header)
        self.assertIn("NCR2_FACTORY_INDICATOR_IO31", header)
        self.assertIn("NCR2_FACTORY_INDICATOR_BOTH_LOW", header)
        self.assertIn("GPIO1->DR_SET = indicator_mask", source)
        self.assertIn("GPIO1->DR_CLEAR = low_mask", source)
        self.assertIn(
            "NCR2_FACTORY_BOARD_CANDIDATE_COUNT UINT32_C(9)",
            header,
        )
        self.assertIn("g_candidates[] = {", source)
        self.assertIn("_Static_assert(", source)

    def test_board_sweep_covers_every_traced_factory_output(self):
        source = (
            ROOT
            / "firmware"
            / "factory_slot"
            / "src"
            / "board_control.c"
        ).read_text()
        table = source.split("g_candidates[] = {", 1)[1]
        table = table.split("};", 1)[0]

        # The sweep is only conclusive if it covers every output the stock
        # engine is known to drive; a shrunken table would silently clear
        # candidate pins without testing them.
        for mask in (
            "NCR2_GPIO1_IO24_MASK",
            "NCR2_GPIO1_IO26_MASK",
            "NCR2_GPIO1_IO31_MASK",
            "NCR2_GPIO2_IO11_MASK",
            "NCR2_GPIO2_IO23_MASK",
            "NCR2_GPIO2_IO24_MASK",
            "NCR2_GPIO2_IO25_MASK",
            "NCR2_GPIO2_IO26_MASK",
            "NCR2_GPIO2_IO27_MASK",
        ):
            self.assertIn(mask, table)

        # Idle levels must match the recovered factory startup state, since
        # the sweep pulses relative to idle rather than assuming active-low.
        for entry, idle_high in (
            ("NCR2_GPIO1_IO24_MASK", "1"),
            ("NCR2_GPIO1_IO26_MASK", "0"),
            ("NCR2_GPIO1_IO31_MASK", "1"),
            ("NCR2_GPIO2_IO26_MASK", "1"),
            ("NCR2_GPIO2_IO27_MASK", "0"),
        ):
            self.assertIn(
                f"{entry}, UINT8_C({idle_high}) }}",
                table,
            )

    def test_bringup_delay_cannot_spin_forever_on_a_stopped_counter(self):
        source = (
            ROOT
            / "firmware"
            / "factory_slot"
            / "src"
            / "board_control.c"
        ).read_text()

        self.assertIn("cycle_counter_usable(", source)
        self.assertIn("NCR2_DWT_UNLOCK_KEY", source)
        self.assertIn("NCR2_FALLBACK_CYCLES_PER_ITERATION", source)
        self.assertIn("NCR2_DELAY_MAX_MILLISECONDS", source)

        # SystemCoreClock is a compile-time default, so a wait can
        # outlast the trial watchdog. Refreshing inside the loop makes
        # a miscalibrated delay slow rather than fatal.
        self.assertIn("service_watchdog()", source)
        body = source.split("static void delay_milliseconds", 1)[1]
        self.assertIn("service_watchdog();", body.split("\n}", 1)[0])

    def test_bringup_app_proves_execution_over_recovery(self):
        application = (
            ROOT
            / "firmware"
            / "hardware_app"
            / "src"
            / "main.c"
        ).read_text()

        self.assertIn("ncr2_factory_board_set_relay(", application)
        self.assertIn("g_delay_line[", application)
        self.assertIn("BOOT_RECOVERY_REQUEST_MAGIC", application)
        self.assertIn("NCR2_BOOT_MAILBOX_ADDRESS", application)
        self.assertIn("NVIC_SystemReset()", application)
        self.assertIn("enable_hang_watchdog()", application)

        # A trial boot arms an eight second watchdog, far shorter than the
        # sweep, so every wait must service it and the image must mark
        # itself healthy on the way in.
        self.assertIn("boot_trial_arm_confirmation(", application)
        self.assertIn("WDOG1->WSR = NCR2_WDOG_REFRESH_FIRST", application)
        self.assertIn("WDOG1->WSR = NCR2_WDOG_REFRESH_SECOND", application)
        self.assertNotIn(
            "ncr2_factory_board_delay_ms(NCR2_LED_",
            application,
        )

        # The whole point of this build is that nothing can stall before
        # the sweep reports, so audio must never be started here.
        self.assertLess(
            application.index("ncr2_factory_board_set_relay("),
            application.index("reset_into_recovery()"),
        )

        # Silence must never be ambiguous: audio init and live DMA
        # blocks are reported on the indicator before the relay test.
        self.assertIn("g_hardware_app_processed_blocks", application)
        self.assertLess(
            application.index("flash_code("),
            application.index("NCR2_METER_DURATION_MS;"),
        )

        # A latching relay coil expects a brief pulse, not a steady
        # drive, so the relay hunt must not reuse the LED dwell times.
        self.assertIn("NCR2_INPUT_FLOOR", application)

    def test_source_audio_emulator_is_offline_only(self):
        source = (
            ROOT / "tools" / "emulate_source_audio.py"
        ).read_text()

        self.assertNotIn("/dev/", source)
        self.assertNotIn("hidraw", source)
        self.assertNotIn("usb.core", source)
        self.assertIn("EXPECTED_SAI", source)
        self.assertIn("_validate_passthrough_isr", source)


if __name__ == "__main__":
    unittest.main()
