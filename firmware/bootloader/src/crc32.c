#include "crc32.h"

uint32_t crc32_compute(const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t value = UINT32_C(0xFFFFFFFF);

    for (size_t index = 0U; index < size; ++index) {
        value ^= bytes[index];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask =
                (uint32_t)(-(int32_t)(value & UINT32_C(1)));
            value = (value >> 1U) ^
                    (UINT32_C(0xEDB88320) & mask);
        }
    }
    return ~value;
}

