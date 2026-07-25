import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BoardSupportSafetyTests(unittest.TestCase):
    def test_board_adapter_is_opt_in_and_not_in_default_boot(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        boot_sources = cmake[
            cmake.index("add_executable(\n    ncr2_bootloader") :
            cmake.index("target_include_directories(\n    ncr2_bootloader")
        ]

        self.assertRegex(
            cmake,
            re.compile(
                r"option\(\s*NCR2_BUILD_MCUX_BOARD_ADAPTER"
                r".*?\s+OFF\s*\)",
                re.DOTALL,
            ),
        )
        self.assertNotIn("ncr2_board.c", boot_sources)
        self.assertNotIn("ncr2_watchdog.c", boot_sources)
        self.assertIn("ncr2_hardware_link_probe", cmake)
        self.assertIn("set(NCR2_EFFECTIVE_USB_VID 0)", cmake)
        self.assertIn("set(NCR2_EFFECTIVE_USB_PID 0)", cmake)

    def test_recovery_pin_mapping_and_polarity_are_exact(self):
        source = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "board"
            / "ncr2_board.c"
        ).read_text()

        self.assertIn(
            "IOMUXC_GPIO_AD_B1_05_GPIO1_IO21",
            source,
        )
        self.assertIn(
            "IOMUXC_GPIO_SD_B1_02_GPIO3_IO02",
            source,
        )
        self.assertIn("primary == UINT32_C(0)", source)
        self.assertIn("guard != UINT32_C(0)", source)
        self.assertNotIn("GPIO_PinWrite", source)
        self.assertNotIn("GPIO_PortSet", source)
        self.assertNotIn("GPIO_PortClear", source)

    def test_retained_mailbox_uses_src_gpr8_and_gpr9(self):
        layout = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "include"
            / "ncr2_flash_layout.h"
        ).read_text()

        self.assertIn(
            "#define NCR2_BOOT_MAILBOX_ADDRESS "
            "UINT32_C(0x400F803C)",
            layout,
        )
        self.assertIn(
            "#define NCR2_BOOT_MAILBOX_SIZE "
            "UINT32_C(0x00000008)",
            layout,
        )
        self.assertNotIn("NCR2_DTCM_USABLE_START", layout)

    def test_trial_mailbox_and_watchdog_are_guarded(self):
        layout = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "include"
            / "ncr2_flash_layout.h"
        ).read_text()
        watchdog = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "board"
            / "ncr2_watchdog.c"
        ).read_text()
        header = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "board"
            / "ncr2_watchdog.h"
        ).read_text()

        self.assertIn(
            "#define NCR2_BOOT_TRIAL_MAILBOX_ADDRESS "
            "UINT32_C(0x400F8028)",
            layout,
        )
        self.assertIn(
            "#define NCR2_BOOT_TRIAL_MAILBOX_SIZE "
            "UINT32_C(0x00000010)",
            layout,
        )
        self.assertIn(
            "NCR2_TRIAL_WATCHDOG_TIMEOUT_VALUE "
            "UINT16_C(0x000F)",
            header,
        )
        self.assertIn("config.enableTimeOutAssert = false", watchdog)
        self.assertIn("config.enableInterrupt = false", watchdog)
        self.assertIn("config.workMode.enableDebug = false", watchdog)
        self.assertIn("WDOG1", watchdog)

    def test_usb_irq_has_a_vector_but_defaults_to_weak_handler(self):
        startup = (
            ROOT
            / "firmware"
            / "bootloader"
            / "src"
            / "startup.S"
        ).read_text()

        self.assertIn(".word USB_OTG1_IRQHandler", startup)
        self.assertIn(".word SysTick_Handler", startup)
        self.assertIn(".rept 13", startup)
        self.assertIn(".rept 113", startup)
        self.assertIn(".rept 46", startup)
        self.assertIn(".weak USB_OTG1_IRQHandler", startup)
        self.assertIn(
            ".thumb_set USB_OTG1_IRQHandler, Default_Handler",
            startup,
        )
        self.assertIn(".weak SysTick_Handler", startup)

        board = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "board"
            / "ncr2_board.c"
        ).read_text()
        self.assertIn("NCR2_RECOVERY_INDICATOR_MASK", board)
        self.assertIn("GPIO2->DR_TOGGLE", board)
        self.assertIn("void SysTick_Handler(void)", board)


if __name__ == "__main__":
    unittest.main()


class SdramAudioBufferTests(unittest.TestCase):
    """Large audio buffers must not enter the flashed image."""

    def test_sdram_buffer_region_is_noload_and_bounded(self):
        linker = (
            ROOT / "firmware" / "app" / "ncr2_app.ld"
        ).read_text()

        self.assertIn(".sdram_bss", linker)
        self.assertIn("(NOLOAD)", linker.split(".sdram_bss", 1)[1])
        # Placed clear of the loaded application, and asserted both ways so
        # neither the image nor the buffers can silently overrun.
        self.assertIn(
            "loaded application overruns the SDRAM audio buffer region",
            linker,
        )
        self.assertIn(
            "SDRAM audio buffers exceed the mapped region",
            linker,
        )


class CodecProbeSafetyTests(unittest.TestCase):
    """The bus scan must not touch pads the running system depends on."""

    def setUp(self):
        self.source = (
            ROOT / "firmware" / "hardware_app" / "src" / "codec_probe.c"
        ).read_text()

    def test_scan_excludes_semc_and_flexspi_pads(self):
        # The application executes from SDRAM behind the SEMC and recovery
        # depends on the FlexSPI NOR, so muxing either as GPIO mid-scan
        # would fault the core or strand the device.
        # SD_B1_04/05 are allowed (FlexSPI SS1/DQS, unused here);
        # SD_B1_10/11 carry FlexSPI data and must stay out.
        for forbidden in ("GPIO_EMC_", "GPIO_SD_B1_10", "GPIO_SD_B1_11"):
            self.assertNotIn(forbidden, self.source)

    def test_scan_covers_the_documented_candidate_count(self):
        header = (
            ROOT / "firmware" / "hardware_app" / "include" / "codec_probe.h"
        ).read_text()

        self.assertIn(
            "NCR2_CODEC_BUS_CANDIDATE_COUNT UINT32_C(7)", header
        )
        self.assertIn("_Static_assert(", self.source)
        self.assertEqual(self.source.count("case "), 6)

    def test_probe_tries_both_cad_addresses(self):
        header = (
            ROOT / "firmware" / "hardware_app" / "include" / "codec_probe.h"
        ).read_text()

        # The AK4619 answers at 0x10 or 0x11 depending on the CAD pin.
        self.assertIn("NCR2_AK4619_ADDRESS_BASE UINT8_C(0x10)", header)
        self.assertIn("+ UINT8_C(1)", self.source)

    def test_clock_stretching_is_bounded(self):
        # A pad pair with no device can sit low forever; the scan must not
        # hang on it.
        self.assertIn("NCR2_I2C_STRETCH_LIMIT", self.source)
        self.assertIn("open drain", self.source.lower())

    def test_probe_rejects_stuck_low_and_requires_codec_signature(self):
        # A stuck-low SDA makes every byte look acknowledged and every read
        # look like zero. The probe must require idle-high and the AK4619's
        # nonzero reset-default MIC gain value.
        self.assertIn(
            "read_line(bus, bus->sda_bit) == UINT32_C(0)",
            self.source,
        )
        self.assertIn(
            "NCR2_AK4619_SIGNATURE_REGISTER UINT8_C(0x04)",
            self.source,
        )
        self.assertIn(
            "NCR2_AK4619_SIGNATURE_VALUE UINT8_C(0x22)",
            self.source,
        )
