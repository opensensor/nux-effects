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
        self.assertIn("NCR2_GPIO2_AUDIO_ACTIVE_HIGH", source)
        self.assertIn(
            "NCR2_GPIO2_IO24_MASK | NCR2_GPIO2_IO25_MASK",
            source,
        )
        self.assertIn("NCR2_BOARD_RELEASE_DELAY_US", source)
        self.assertIn("IOMUXC_GPIO_AD_B1_05_GPIO1_IO21", source)
        self.assertIn("GPIO1->GDIR &= ~NCR2_GPIO1_IO21_MASK", source)
        self.assertIn("ncr2_factory_board_switch_pressed(", source)

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
        self.assertIn(
            "ncr2_factory_board_set_relay(UINT8_C(1))",
            application,
        )
        self.assertIn(
            "ncr2_factory_board_set_relay(UINT8_C(0))",
            application,
        )
        self.assertIn("NCR2_SAFE_OUTPUT_PEAK", application)
        self.assertNotIn("NCR2_DRIVE_LIMIT", application)
        self.assertNotIn("NCR2_DRIVE_GATE", application)
        self.assertIn("process_selected_effect(", application)
        self.assertIn("shape_drive(", application)
        self.assertIn("clamp_symmetric(", application)
        self.assertIn("NCR2_EFFECT_RAMP_STEP", application)
        self.assertIn("NCR2_SELECTOR_RAMP_STEP", application)
        self.assertIn("initialize_knob_adc()", application)
        self.assertIn("sample_knobs(", application)
        self.assertIn(
            "NCR2_KNOB_AMOUNT_CHANNEL UINT32_C(5)",
            application,
        )
        self.assertIn(
            "NCR2_KNOB_CHARACTER_CHANNEL UINT32_C(8)",
            application,
        )
        self.assertIn(
            "NCR2_KNOB_SELECTOR_CHANNEL UINT32_C(9)",
            application,
        )
        self.assertIn(
            "NCR2_KNOB_OUTPUT_CHANNEL UINT32_C(11)",
            application,
        )
        self.assertIn("NCR2_SWITCH_DEBOUNCE_MS", application)
        self.assertIn("ncr2_factory_board_switch_pressed()", application)
        self.assertIn("debounced_pressed != raw_pressed", application)
        self.assertIn("g_hardware_app_enable_effect =", application)
        self.assertIn("NCR2_LED_OFF", application)
        self.assertIn("g_hardware_app_ready = UINT32_C(0x46555A5A)", application)
        self.assertIn("ncr2_codec_probe(", application)
        self.assertIn(
            "ncr2_factory_board_restore_audio_active(",
            application,
        )
        self.assertIn(
            "NCR2_FACTORY_BOARD_CANDIDATE_COUNT);",
            application,
        )
        self.assertIn("NCR2_AK4619_REG_DAC_ROUTE", application)
        self.assertIn("NCR2_AK4619_DAC_ROUTE_FACTORY", application)
        self.assertLess(
            application.index("ncr2_codec_configure()"),
            application.index(
                "ncr2_factory_board_restore_audio_active("
            ),
        )
        restore_index = application.index(
            "ncr2_factory_board_restore_audio_active("
        )
        self.assertLess(
            restore_index,
            application.index(
                "g_hardware_app_codec_power_readback = power;",
                restore_index,
            ),
        )
        # A codec held in reset never acknowledges, so PDN
        # candidates must be released before the scan.
        self.assertLess(
            application.index(
                "ncr2_factory_board_release_reset_candidates()"
            ),
            application.index("ncr2_codec_probe("),
        )
        self.assertIn("BOOT_RECOVERY_REQUEST_MAGIC", application)
        self.assertIn("NCR2_BOOT_MAILBOX_ADDRESS", application)
        self.assertIn("NVIC_SystemReset()", application)
        self.assertIn("enable_hang_watchdog()", application)

        # A trial boot arms an eight second watchdog, far shorter than the
        # sweep, so every wait must service it and the image must mark
        # itself healthy on the way in.
        self.assertIn("boot_trial_arm_confirmation(", application)
        self.assertIn(
            "g_hardware_app_trial_status == BOOT_TRIAL_OK",
            application,
        )
        self.assertIn("clear_recovery_request()", application)
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
        self.assertIn(
            "g_ncr2_factory_audio_counters.tx_blocks",
            application,
        )
        self.assertLess(
            application.index("flash_code("),
            application.index("ncr2_codec_probe("),
        )

        # A latching relay coil expects a brief pulse, not a steady
        # drive, so the relay hunt must not reuse the LED dwell times.
        self.assertIn("NCR2_INPUT_FLOOR", application)

    def test_hardware_app_exposes_eight_bounded_artist_presets(self):
        application = (
            ROOT
            / "firmware"
            / "hardware_app"
            / "src"
            / "main.c"
        ).read_text()

        for effect in (
            "NCR2_EFFECT_SHINE_DRIVE",
            "NCR2_EFFECT_WALL_FUZZ",
            "NCR2_EFFECT_BREATHE_VIBE",
            "NCR2_EFFECT_ECHOES_TAPE",
            "NCR2_EFFECT_RAGE_DRIVE",
            "NCR2_EFFECT_COCKED_WAH",
            "NCR2_EFFECT_GUERRILLA_TREM",
            "NCR2_EFFECT_WHAMMY_FUZZ",
        ):
            self.assertIn(effect, application)
        self.assertIn("NCR2_EFFECT_COUNT UINT32_C(8)", application)
        self.assertIn("quantize_selector(", application)
        self.assertIn("NCR2_SELECTOR_SETTLE_SAMPLES", application)
        self.assertIn("NCR2_SELECTOR_HYSTERESIS", application)
        self.assertIn("g_selector_candidate_samples", application)
        self.assertIn("g_selector_detent", application)
        # The equal-width bins this replaced merged two knob positions and
        # left one effect unreachable; see tests/test_selector_detents.py.
        self.assertNotIn(
            "(sample * NCR2_EFFECT_COUNT) /",
            application,
        )
        self.assertIn(
            "const int32_t transitioned = blend_samples_q15(",
            application,
        )
        self.assertNotIn(
            "sample * (int64_t)g_selector_ramp",
            application,
        )
        self.assertIn('section(".sdram_bss")', application)
        self.assertIn("initialize_effect_processor()", application)
        self.assertIn(
            "NCR2_SAFE_OUTPUT_PEAK INT32_C(0x10000000)",
            application,
        )
        self.assertIn("NCR2_SHINE_DRIVE_Q12_RANGE", application)
        self.assertIn("NCR2_WALL_FUZZ_Q12_RANGE", application)
        self.assertIn("NCR2_RAGE_DRIVE_Q12_RANGE", application)
        self.assertIn("NCR2_WHAMMY_WINDOW_FRAMES", application)
        self.assertIn("phase_a + UINT32_C(0x80000000)", application)

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
