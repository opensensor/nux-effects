#ifndef NCR2_BOOT_TRIAL_H
#define NCR2_BOOT_TRIAL_H

#include <stdint.h>

#define BOOT_TRIAL_HANDOFF_MAGIC UINT32_C(0x484E4400)
#define BOOT_TRIAL_CONFIRM_MAGIC UINT32_C(0x43464D00)

enum boot_trial_status {
    BOOT_TRIAL_OK = 0,
    BOOT_TRIAL_INVALID_ARGUMENT = 1,
    BOOT_TRIAL_NO_HANDOFF = 2,
    BOOT_TRIAL_NO_CONFIRMATION = 3,
};

/*
 * One mailbox has two states:
 *
 *   handoff: bootloader -> pending application
 *   confirmation: healthy application -> bootloader after warm reset
 *
 * The slot is encoded in the low byte of token. Sequence and inverse bind
 * the confirmation to one exact journaled trial.
 */
typedef struct boot_trial_mailbox {
    volatile uint32_t token;
    volatile uint32_t token_inverse;
    volatile uint32_t sequence;
    volatile uint32_t sequence_inverse;
} boot_trial_mailbox_t;

void boot_trial_clear(boot_trial_mailbox_t *mailbox);
uint16_t boot_trial_publish(
    boot_trial_mailbox_t *mailbox,
    uint8_t slot,
    uint32_t sequence);
uint16_t boot_trial_arm_confirmation(
    boot_trial_mailbox_t *mailbox);
uint16_t boot_trial_consume_confirmation(
    boot_trial_mailbox_t *mailbox,
    uint8_t *slot,
    uint32_t *sequence);

#endif
