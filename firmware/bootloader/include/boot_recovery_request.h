#ifndef NCR2_BOOT_RECOVERY_REQUEST_H
#define NCR2_BOOT_RECOVERY_REQUEST_H

#include <stdint.h>

#define BOOT_RECOVERY_REQUEST_MAGIC UINT32_C(0x4652584E)

enum boot_recovery_request_source {
    BOOT_RECOVERY_REQUEST_NONE = 0,
    BOOT_RECOVERY_REQUEST_SOFTWARE = 1,
    BOOT_RECOVERY_REQUEST_PHYSICAL = 2,
};

typedef struct boot_recovery_mailbox {
    volatile uint32_t magic;
    volatile uint32_t inverse;
} boot_recovery_mailbox_t;

typedef struct boot_recovery_request {
    boot_recovery_mailbox_t *mailbox;
    void *physical_context;
    int (*physical_asserted)(void *context);
} boot_recovery_request_t;

void boot_recovery_request_arm(
    boot_recovery_mailbox_t *mailbox);
uint8_t boot_recovery_request_consume(
    const boot_recovery_request_t *request);

#endif
