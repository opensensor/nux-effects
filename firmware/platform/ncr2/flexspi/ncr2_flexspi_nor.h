#ifndef NCR2_FLEXSPI_NOR_H
#define NCR2_FLEXSPI_NOR_H

#include <stdint.h>

#include "ncr2_nor.h"

enum ncr2_flexspi_nor_status {
    NCR2_FLEXSPI_NOR_OK = 0,
    NCR2_FLEXSPI_NOR_INVALID_ARGUMENT = 1,
    NCR2_FLEXSPI_NOR_PROBE_FAILED = 2,
    NCR2_FLEXSPI_NOR_UNSUPPORTED_DEVICE = 3,
    NCR2_FLEXSPI_NOR_POLICY_INIT_FAILED = 4,
};

uint16_t ncr2_flexspi_nor_init(ncr2_nor_t *nor);

#endif
