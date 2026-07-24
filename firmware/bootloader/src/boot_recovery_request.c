#include "boot_recovery_request.h"

#include <stddef.h>

void boot_recovery_request_arm(
    boot_recovery_mailbox_t *mailbox)
{
    if (mailbox == NULL) {
        return;
    }
    mailbox->inverse = ~BOOT_RECOVERY_REQUEST_MAGIC;
    mailbox->magic = BOOT_RECOVERY_REQUEST_MAGIC;
}

uint8_t boot_recovery_request_consume(
    const boot_recovery_request_t *request)
{
    int software_requested = 0;
    int physical_requested = 0;

    if (request == NULL) {
        return BOOT_RECOVERY_REQUEST_NONE;
    }
    if (request->mailbox != NULL) {
        software_requested =
            request->mailbox->magic ==
                BOOT_RECOVERY_REQUEST_MAGIC &&
            request->mailbox->inverse ==
                ~BOOT_RECOVERY_REQUEST_MAGIC;

        /*
         * Consume valid and partial tokens alike. This makes the request
         * one-shot and prevents a reset during token creation from
         * becoming a later surprise.
         */
        request->mailbox->magic = UINT32_C(0);
        request->mailbox->inverse = UINT32_C(0);
    }
    if (request->physical_asserted != NULL) {
        physical_requested =
            request->physical_asserted(
                request->physical_context) != 0;
    }
    if (physical_requested) {
        return BOOT_RECOVERY_REQUEST_PHYSICAL;
    }
    if (software_requested) {
        return BOOT_RECOVERY_REQUEST_SOFTWARE;
    }
    return BOOT_RECOVERY_REQUEST_NONE;
}
