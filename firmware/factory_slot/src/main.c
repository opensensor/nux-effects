#include <stdint.h>

#include "factory_audio.h"

volatile uint32_t g_factory_slot_heartbeat;
volatile uint32_t g_factory_slot_audio_status;

void application_main(void)
{
    g_factory_slot_audio_status =
        (uint32_t)ncr2_factory_audio_init();

    for (;;) {
        ++g_factory_slot_heartbeat;
        __asm volatile("wfi");
    }
}
