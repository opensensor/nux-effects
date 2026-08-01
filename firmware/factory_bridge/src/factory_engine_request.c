#include "factory_engine_request.h"

#include <stdint.h>

_Static_assert(
    (NCR2_FACTORY_REQUEST_MAGIC &
     NCR2_FACTORY_REQUEST_ENGINE_MASK) == UINT32_C(0),
    "factory request signature overlaps the engine field");

static void clear_mailbox(ncr2_factory_engine_mailbox_t *mailbox)
{
    if (mailbox != (ncr2_factory_engine_mailbox_t *)0) {
        mailbox->token = UINT32_C(0);
    }
}

int ncr2_factory_engine_request_arm(
    ncr2_factory_engine_mailbox_t *mailbox,
    uint8_t engine)
{
    if (mailbox == (ncr2_factory_engine_mailbox_t *)0 ||
        engine >= NCR2_FACTORY_ENGINE_COUNT) {
        clear_mailbox(mailbox);
        return NCR2_FACTORY_REQUEST_INVALID_ARGUMENT;
    }

    mailbox->token =
        NCR2_FACTORY_REQUEST_MAGIC | (uint32_t)engine;
    return NCR2_FACTORY_REQUEST_OK;
}

int ncr2_factory_engine_request_consume(
    ncr2_factory_engine_mailbox_t *mailbox,
    uint8_t *engine)
{
    uint32_t token;

    if (mailbox == (ncr2_factory_engine_mailbox_t *)0 ||
        engine == (uint8_t *)0) {
        clear_mailbox(mailbox);
        return NCR2_FACTORY_REQUEST_INVALID_ARGUMENT;
    }

    token = mailbox->token;
    mailbox->token = UINT32_C(0);
    *engine = UINT8_C(0);

    if ((token & ~NCR2_FACTORY_REQUEST_ENGINE_MASK) !=
        NCR2_FACTORY_REQUEST_MAGIC) {
        return NCR2_FACTORY_REQUEST_NONE;
    }

    *engine = (uint8_t)(token & NCR2_FACTORY_REQUEST_ENGINE_MASK);
    return NCR2_FACTORY_REQUEST_OK;
}
