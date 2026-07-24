#ifndef NCR2_FACTORY_BOARD_H
#define NCR2_FACTORY_BOARD_H

#include <stdint.h>

enum
{
    NCR2_FACTORY_BOARD_OK = 0,
    NCR2_FACTORY_BOARD_DISABLED = 1,
};

/*
 * Reproduce the factory output-pin configuration while keeping the
 * GPIO1_IO26 candidate control at its initial low level.
 */
uint16_t ncr2_factory_board_prepare_audio(void);

/*
 * Preserve the factory 100 ms low interval, then drive GPIO1_IO26 high.
 * Call only after the digital audio path is ready.
 */
void ncr2_factory_board_release_audio(void);

#endif
