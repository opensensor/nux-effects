#include "boot_trial.h"

#include <stddef.h>

#include "boot_state.h"

static int slot_is_valid(uint8_t slot)
{
    return slot == BOOT_SLOT_A || slot == BOOT_SLOT_B;
}

static uint32_t make_token(uint32_t magic, uint8_t slot)
{
    return magic | (uint32_t)slot;
}

static int token_is_valid(
    uint32_t token,
    uint32_t inverse,
    uint32_t magic,
    uint8_t *slot)
{
    const uint8_t candidate =
        (uint8_t)(token & UINT32_C(0xFF));

    if (inverse != ~token ||
        !slot_is_valid(candidate) ||
        token != make_token(magic, candidate)) {
        return 0;
    }
    *slot = candidate;
    return 1;
}

void boot_trial_clear(boot_trial_mailbox_t *mailbox)
{
    if (mailbox == NULL) {
        return;
    }
    mailbox->token = UINT32_C(0);
    mailbox->token_inverse = UINT32_C(0);
    mailbox->sequence = UINT32_C(0);
    mailbox->sequence_inverse = UINT32_C(0);
}

uint16_t boot_trial_publish(
    boot_trial_mailbox_t *mailbox,
    uint8_t slot,
    uint32_t sequence)
{
    uint32_t token;

    if (mailbox == NULL || !slot_is_valid(slot)) {
        boot_trial_clear(mailbox);
        return BOOT_TRIAL_INVALID_ARGUMENT;
    }

    token = make_token(BOOT_TRIAL_HANDOFF_MAGIC, slot);
    boot_trial_clear(mailbox);
    mailbox->sequence_inverse = ~sequence;
    mailbox->sequence = sequence;
    mailbox->token_inverse = ~token;
    mailbox->token = token;
    return BOOT_TRIAL_OK;
}

uint16_t boot_trial_arm_confirmation(
    boot_trial_mailbox_t *mailbox)
{
    uint32_t token;
    uint32_t inverse;
    uint32_t sequence;
    uint32_t sequence_inverse;
    uint8_t slot;

    if (mailbox == NULL) {
        return BOOT_TRIAL_INVALID_ARGUMENT;
    }

    token = mailbox->token;
    inverse = mailbox->token_inverse;
    sequence = mailbox->sequence;
    sequence_inverse = mailbox->sequence_inverse;
    if (!token_is_valid(
            token,
            inverse,
            BOOT_TRIAL_HANDOFF_MAGIC,
            &slot) ||
        sequence_inverse != ~sequence) {
        boot_trial_clear(mailbox);
        return BOOT_TRIAL_NO_HANDOFF;
    }

    token = make_token(BOOT_TRIAL_CONFIRM_MAGIC, slot);
    /*
     * Invalidate the old handoff first and commit the confirmation token
     * last. A reset at any intermediate write produces no confirmation.
     */
    mailbox->token = UINT32_C(0);
    mailbox->token_inverse = ~token;
    mailbox->token = token;
    return BOOT_TRIAL_OK;
}

uint16_t boot_trial_consume_confirmation(
    boot_trial_mailbox_t *mailbox,
    uint8_t *slot,
    uint32_t *sequence)
{
    uint32_t token;
    uint32_t inverse;
    uint32_t candidate_sequence;
    uint32_t sequence_inverse;
    uint8_t candidate_slot = BOOT_SLOT_NONE;
    int valid;

    if (mailbox == NULL || slot == NULL || sequence == NULL) {
        return BOOT_TRIAL_INVALID_ARGUMENT;
    }

    token = mailbox->token;
    inverse = mailbox->token_inverse;
    candidate_sequence = mailbox->sequence;
    sequence_inverse = mailbox->sequence_inverse;
    boot_trial_clear(mailbox);

    valid =
        token_is_valid(
            token,
            inverse,
            BOOT_TRIAL_CONFIRM_MAGIC,
            &candidate_slot) &&
        sequence_inverse == ~candidate_sequence;
    if (!valid) {
        *slot = BOOT_SLOT_NONE;
        *sequence = UINT32_C(0);
        return BOOT_TRIAL_NO_CONFIRMATION;
    }

    *slot = candidate_slot;
    *sequence = candidate_sequence;
    return BOOT_TRIAL_OK;
}
