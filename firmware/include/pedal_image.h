#ifndef PEDAL_IMAGE_H
#define PEDAL_IMAGE_H

#include <stdint.h>

#define PEDAL_IMAGE_MAGIC UINT32_C(0x4E45504F)
#define PEDAL_IMAGE_BOARD_NCR2 UINT32_C(0x3252434E)
#define PEDAL_IMAGE_FORMAT_VERSION UINT16_C(1)
#define PEDAL_IMAGE_SHA256_SIZE 32U

typedef struct __attribute__((packed)) pedal_image_manifest {
    uint32_t magic;
    uint16_t format_version;
    uint16_t header_size;
    uint32_t image_size;
    uint32_t load_address;
    uint32_t vector_offset;
    uint32_t board_id;
    uint32_t semantic_version;
    uint32_t build_number;
    uint8_t image_sha256[PEDAL_IMAGE_SHA256_SIZE];
    uint32_t header_crc32;
} pedal_image_manifest_t;

_Static_assert(sizeof(pedal_image_manifest_t) == 68U,
               "pedal image manifest layout changed");

#endif

