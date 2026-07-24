#ifndef NCR2_FACTORY_AUDIO_H
#define NCR2_FACTORY_AUDIO_H

#include <stddef.h>
#include <stdint.h>

enum
{
    NCR2_FACTORY_AUDIO_OK = 0,
    NCR2_FACTORY_AUDIO_DISABLED = 1,
    NCR2_FACTORY_AUDIO_BAD_STATE = 2,
};

enum
{
    NCR2_FACTORY_AUDIO_SAMPLE_RATE_HZ = 48000,
    NCR2_FACTORY_AUDIO_SLOTS = 4,
    NCR2_FACTORY_AUDIO_FRAMES_PER_BLOCK = 8,
    NCR2_FACTORY_AUDIO_WORDS_PER_BLOCK =
        NCR2_FACTORY_AUDIO_SLOTS *
        NCR2_FACTORY_AUDIO_FRAMES_PER_BLOCK,
};

typedef struct ncr2_factory_audio_counters
{
    volatile uint32_t rx_blocks;
    volatile uint32_t copied_blocks;
    volatile uint32_t unexpected_interrupts;
} ncr2_factory_audio_counters_t;

extern ncr2_factory_audio_counters_t
    g_ncr2_factory_audio_counters;

uint16_t ncr2_factory_audio_init(void);

/*
 * The weak default implementation copies all four factory slots unchanged.
 * A source DSP runtime can override this symbol without changing the ISR or
 * the hardware contract.
 */
void ncr2_factory_audio_process_block(
    const int32_t *input,
    int32_t *output,
    size_t frames);

void DMA0_DMA16_IRQHandler(void);

#endif
