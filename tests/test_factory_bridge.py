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
        self.assertIn("BOOT_TRIAL_MAILBOX,       0x400f8028", source)
        self.assertIn("SCB_ICIALLU,              0xe000ef50", source)
        self.assertIn("bl      boot_trial_arm_confirmation", source)
        self.assertIn("cmp     r2, r3", source)
        self.assertIn("bne     bridge_fault", source)
        self.assertIn("dsb     sy", source)
        self.assertIn("isb     sy", source)
        self.assertIn("bl      ncr2_factory_compat_prepare", source)
        bridge_copy = source[source.index("bridge_copy:") :]
        reload_index = bridge_copy.index(
            "ldr     r1, =FACTORY_ENGINE_XIP"
        )
        copy_index = bridge_copy.index("ldr     r3, [r1], #4")
        self.assertLess(reload_index, copy_index)

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
        self.assertIn("bootloader/src/boot_trial.c", declaration)
        self.assertIn("app/ncr2_app.ld", declaration)

    def test_compatibility_layer_recreates_launcher_hardware_setup(self):
        source = (
            ROOT
            / "firmware"
            / "factory_bridge"
            / "src"
            / "board_compat.c"
        ).read_text()

        self.assertIn("NCR2_FACTORY_ARM_PLL_DIVIDER UINT32_C(100)", source)
        self.assertIn("NCR2_FACTORY_DCDC_TARGET UINT32_C(0x12)", source)
        self.assertIn("MPU->RBAR = UINT32_C(0xc0000010)", source)
        self.assertIn("MPU->RBAR = UINT32_C(0x20200016)", source)
        self.assertIn("IOMUXC_GPIO_AD_B1_05_GPIO1_IO21", source)
        self.assertIn("IOMUXC_GPIO_SD_B1_02_GPIO3_IO02", source)
        self.assertIn("IOMUXC_GPIO_B0_07_GPIO2_IO07", source)
        self.assertIn("IOMUXC_GPIO_B1_15_GPIO2_IO31", source)
        self.assertIn("CLOCK_InitArmPll(&arm_pll)", source)
        self.assertNotIn("CLOCK_InitSysPll", source)
        self.assertIn("CoreDebug->DEMCR", source)

    def test_compatibility_bridge_is_sdk_gated_and_nondefault(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        start = cmake.index(
            "if(EXISTS\n"
            '   "${NCR2_MCUX_SDK_ROOT}/core/devices/MIMXRT1051/'
            'MIMXRT1051.h")'
        )
        end = cmake.index("if(NCR2_BUILD_FACTORY_SLOT_APP)", start)
        declaration = cmake[start:end]

        self.assertIn("ncr2_factory_compat_bridge", declaration)
        self.assertIn("ncr2_factory_compat_diagnostic", declaration)
        self.assertIn("EXCLUDE_FROM_ALL", declaration)
        self.assertIn("factory_bridge/src/board_compat.c", declaration)
        self.assertIn("NCR2_FACTORY_COMPAT_PREPARE=1", declaration)
        self.assertIn("NCR2_FACTORY_COMPAT_DIAGNOSTIC=1", declaration)

    def test_post_link_checker_pins_the_audited_engine(self):
        checker = (
            ROOT / "tools" / "check_factory_bridge.py"
        ).read_text()

        self.assertIn('"ncr2_factory_engine_source": 0x600C0000', checker)
        self.assertIn('"ncr2_factory_engine_copy_size": 0x0001E000', checker)
        self.assertIn('"ncr2_factory_engine_stack": 0x20018000', checker)
        self.assertIn('"ncr2_factory_engine_reset": 0x0000E4B5', checker)
        self.assertIn("SCB_ICIALLU = 0xE000EF50", checker)
        self.assertIn('"boot_trial_arm_confirmation"', checker)
        self.assertIn("FACTORY_COMPAT_MPU_WORDS", checker)
        self.assertIn('"CLOCK_InitArmPll"', checker)
        self.assertIn(
            "does not reload its caller-clobbered",
            checker,
        )


if __name__ == "__main__":
    unittest.main()
