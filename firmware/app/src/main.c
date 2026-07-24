#include <stdint.h>

#include "application.h"
#include "program_selector.h"

volatile uint32_t g_application_heartbeat;
volatile uint32_t g_active_program;
volatile uint32_t g_program_change_count;

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
    /*
     * This is deliberately hardware-neutral. A GPIO heartbeat is unsafe
     * until the LED, mute, bypass, and switch pins have been confirmed.
     * A debugger can observe these words during the offline/SWD bring-up.
     */
    (void)application_programs_ready(
        UINT32_C(1),
        UINT32_C(0));
    for (;;) {
        g_application_heartbeat +=
            g_active_program + UINT32_C(1);
        diagnostic_delay();
    }
}
