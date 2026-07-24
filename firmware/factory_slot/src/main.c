#include <stdint.h>

#include "factory_audio.h"
#include "factory_board.h"

volatile uint32_t g_factory_slot_heartbeat;
volatile uint32_t g_factory_slot_audio_status;
volatile uint32_t g_factory_slot_board_status;
volatile uint32_t g_factory_slot_ready;

void application_main(void)
{
    g_factory_slot_board_status =
        (uint32_t)ncr2_factory_board_prepare_audio();
    g_factory_slot_audio_status =
        (uint32_t)ncr2_factory_audio_init();
    if (g_factory_slot_audio_status == NCR2_FACTORY_AUDIO_OK) {
        ncr2_factory_board_release_audio();
    }
    g_factory_slot_ready = UINT32_C(0x4e435232);

    for (;;) {
        ++g_factory_slot_heartbeat;
        __asm volatile("wfi");
    }
}
