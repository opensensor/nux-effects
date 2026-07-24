#ifndef NCR2_SHA256_H
#define NCR2_SHA256_H

#include <stddef.h>
#include <stdint.h>

#define SHA256_DIGEST_SIZE 32U
#define SHA256_BLOCK_SIZE 64U

typedef struct sha256_context {
    uint32_t state[8];
    uint64_t bit_count;
    uint8_t block[SHA256_BLOCK_SIZE];
    size_t block_size;
} sha256_context_t;

void sha256_init(sha256_context_t *context);
void sha256_update(sha256_context_t *context, const void *data, size_t size);
void sha256_final(sha256_context_t *context,
                  uint8_t digest[SHA256_DIGEST_SIZE]);

#endif

