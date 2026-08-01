import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class ApplicationTrialConfirmationTests(unittest.TestCase):
    def test_application_confirms_only_after_both_health_gates(self):
        source = (
            ROOT / "firmware" / "app" / "src" / "main.c"
        ).read_text()

        library_gate = source.index(
            "g_program_library_status == PROGRAM_RUNTIME_OK &&"
        )
        selector_gate = source.index(
            "g_program_selector_status == PROGRAM_SELECTOR_OK"
        )
        confirmation = source.index(
            "ncr2_boot_confirm_healthy_and_reset()"
        )
        heartbeat = source.index("for (;;)", confirmation)

        self.assertLess(library_gate, confirmation)
        self.assertLess(selector_gate, confirmation)
        self.assertLess(confirmation, heartbeat)

    def test_platform_helper_commits_token_before_reset(self):
        source = (
            ROOT
            / "firmware"
            / "platform"
            / "ncr2"
            / "src"
            / "ncr2_boot_request.c"
        ).read_text()

        confirmation = source.index("ncr2_boot_confirm_healthy();")
        success_gate = source.index("status == BOOT_TRIAL_OK")
        reset_call = source.index("ncr2_boot_warm_reset();")

        self.assertLess(confirmation, success_gate)
        self.assertLess(success_gate, reset_call)
        self.assertIn("NCR2_SCB_AIRCR_VECTKEY", source)
        self.assertIn("NCR2_SCB_AIRCR_SYSRESETREQ", source)
        self.assertIn('__asm volatile("dsb"', source)

    def test_post_confirm_recovery_is_explicitly_opt_in(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        source = (
            ROOT / "firmware" / "app" / "src" / "main.c"
        ).read_text()

        self.assertIn("NCR2_APP_RETURN_TO_RECOVERY_AFTER_CONFIRM", cmake)
        self.assertIn(
            '"Return the source app to USB recovery after a '
            'successful A/B trial"\n'
            "    OFF",
            cmake,
        )
        self.assertIn("#if NCR2_APP_RETURN_TO_RECOVERY_AFTER_CONFIRM", source)
        self.assertIn("g_application_trial_status == BOOT_TRIAL_NO_HANDOFF", source)
        self.assertIn("ncr2_boot_recovery_arm();", source)
        self.assertIn("ncr2_boot_warm_reset();", source)


if __name__ == "__main__":
    unittest.main()
