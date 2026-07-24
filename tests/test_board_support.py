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

    def test_delay_line_lives_in_the_sdram_region(self):
        application = (
            ROOT / "firmware" / "hardware_app" / "src" / "main.c"
        ).read_text()

        self.assertIn('__attribute__((section(".sdram_bss")))', application)
        # The region is NOLOAD and startup does not clear it, so the buffer
        # must zero itself or replay power-on garbage on the first pass.
        self.assertIn("clear_delay_line()", application)
        self.assertLess(
            application.index("clear_delay_line();"),
            application.index("ncr2_factory_audio_init()"),
        )
