#include <stdint.h>

enum effect_mode {
    EFFECT_MODE_DELAY = 0,
    EFFECT_MODE_REVERB = 1,
    EFFECT_MODE_MODULATION = 2,
    EFFECT_MODE_DRIVE = 3,
};

volatile uint32_t g_application_heartbeat;
volatile uint32_t g_effect_mode = EFFECT_MODE_DRIVE;

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
    for (;;) {
        g_application_heartbeat += g_effect_mode + UINT32_C(1);
        diagnostic_delay();
    }
}
