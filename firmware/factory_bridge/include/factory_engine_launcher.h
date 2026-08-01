#ifndef NCR2_FACTORY_ENGINE_LAUNCHER_H
#define NCR2_FACTORY_ENGINE_LAUNCHER_H

#include <stdint.h>

enum {
    NCR2_FACTORY_LAUNCH_OK = 0,
    NCR2_FACTORY_LAUNCH_INVALID_ENGINE = 1,
    NCR2_FACTORY_LAUNCH_VECTOR_MISMATCH = 2,
};

/*
 * Validate and launch one preserved factory engine. A successful call does
 * not return; an invalid index or changed factory vector fails closed.
 */
int ncr2_factory_engine_launch(uint8_t engine);

#endif
