#ifndef NCR2_FACTORY_ENGINE_REQUEST_H
#define NCR2_FACTORY_ENGINE_REQUEST_H

#include <stdint.h>

#define NCR2_FACTORY_ENGINE_COUNT UINT8_C(4)
#define NCR2_FACTORY_REQUEST_MAGIC UINT32_C(0x46414330)
#define NCR2_FACTORY_REQUEST_ENGINE_MASK UINT32_C(0x00000003)

typedef struct {
    volatile uint32_t token;
} ncr2_factory_engine_mailbox_t;

enum {
    NCR2_FACTORY_REQUEST_OK = 0,
    NCR2_FACTORY_REQUEST_NONE = 1,
    NCR2_FACTORY_REQUEST_INVALID_ARGUMENT = 2,
};

/* Publish one engine number in the retained, one-word warm-reset mailbox. */
int ncr2_factory_engine_request_arm(
    ncr2_factory_engine_mailbox_t *mailbox,
    uint8_t engine);

/*
 * Read and clear a request. Clearing before validation makes every value,
 * including a corrupt one, strictly one-shot.
 */
int ncr2_factory_engine_request_consume(
    ncr2_factory_engine_mailbox_t *mailbox,
    uint8_t *engine);

#endif
