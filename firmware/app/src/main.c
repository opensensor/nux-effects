#include <stdint.h>

#include "application.h"
#include "boot_trial.h"
#include "effects_basic.h"
#include "ncr2_boot_request.h"
#include "program_runtime.h"
#include "program_selector.h"
#include "programs_builtin.h"

#ifndef NCR2_APP_RETURN_TO_RECOVERY_AFTER_CONFIRM
#define NCR2_APP_RETURN_TO_RECOVERY_AFTER_CONFIRM 0
#endif

_Static_assert(
    NCR2_APP_RETURN_TO_RECOVERY_AFTER_CONFIRM == 0 ||
        NCR2_APP_RETURN_TO_RECOVERY_AFTER_CONFIRM == 1,
    "post-confirm recovery gate must be zero or one");

volatile uint32_t g_application_heartbeat;
volatile uint32_t g_active_program;
volatile uint32_t g_program_change_count;
volatile uint32_t g_program_library_status;
volatile uint32_t g_program_selector_status;
volatile uint32_t g_application_trial_status;

static program_selector_t g_program_selector;

uint16_t application_programs_ready(
    uint32_t program_count,
    uint32_t initial_program)
{
    uint16_t status =
        program_selector_initialize(
            &g_program_selector,
            program_count,
            initial_program,
            PROGRAM_SELECTOR_DEFAULT_HOLD_MS,
            PROGRAM_SELECTOR_DEFAULT_RELEASE_MS);

    if (status == PROGRAM_SELECTOR_OK) {
        g_active_program = initial_program;
        g_program_change_count = UINT32_C(0);
    }
    return status;
}

uint16_t application_navigation_sample(
    int pressed,
    uint32_t elapsed_ms)
{
    int changed;
    uint16_t status =
        program_selector_sample(
            &g_program_selector,
            pressed,
            elapsed_ms,
            &changed);

    if (status == PROGRAM_SELECTOR_OK && changed != 0) {
        g_active_program =
            g_program_selector.current_program;
        ++g_program_change_count;
    }
    return status;
}

static void diagnostic_delay(void)
{
    for (uint32_t count = 0U; count < UINT32_C(100000); ++count) {
        __asm volatile("nop");
    }
}

void application_main(void)
{
    uint32_t program_count = UINT32_C(1);

    /*
     * This is deliberately hardware-neutral. A GPIO heartbeat is unsafe
     * until the LED, mute, bypass, and switch pins have been confirmed.
     * A debugger can observe these words during the offline/SWD bring-up.
     */
    g_program_library_status =
        program_library_validate(
            &ncr2_builtin_program_library,
            &ncr2_basic_effect_registry);
    if (g_program_library_status ==
        PROGRAM_RUNTIME_OK) {
        program_count =
            (uint32_t)
                ncr2_starter_program_bank.program_count;
    }
    g_program_selector_status =
        (uint32_t)application_programs_ready(
            program_count,
            UINT32_C(0));

    /*
     * A pending slot earns confirmation only after both source-level health
     * gates pass. The helper resets only when a valid trial handoff exists;
     * on an already confirmed boot it returns and normal execution begins.
     */
    g_application_trial_status = UINT32_MAX;
    if (g_program_library_status == PROGRAM_RUNTIME_OK &&
        g_program_selector_status == PROGRAM_SELECTOR_OK) {
        g_application_trial_status =
            (uint32_t)
                ncr2_boot_confirm_healthy_and_reset();
    }
#if NCR2_APP_RETURN_TO_RECOVERY_AFTER_CONFIRM
    /*
     * Bench-only end-to-end trial proof: after the first reset has durably
     * confirmed this slot, return to USB recovery so the host can inspect
     * the journal. The normal source application compiles this branch out.
     */
    if (g_application_trial_status == BOOT_TRIAL_NO_HANDOFF) {
        ncr2_boot_recovery_arm();
        ncr2_boot_warm_reset();
    }
#endif
    for (;;) {
        g_application_heartbeat +=
            g_active_program + UINT32_C(1);
        diagnostic_delay();
    }
}
