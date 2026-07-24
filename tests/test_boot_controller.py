import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BootControllerTests(unittest.TestCase):
    def test_controller_and_recovery_request_state_machine(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>
#include <string.h>

#include "boot_controller.h"
#include "boot_recovery_request.h"

#define MOCK_BASE 0x60020000u

typedef struct mock_context {
    uint8_t storage[
        BOOT_JOURNAL_SECTOR_COUNT * BOOT_RECORD_SECTOR_SIZE];
    uint16_t slot_status[2];
    uint32_t load_count[2];
    int forced;
    int fail_reads;
    int confirmation_available;
    uint8_t confirmation_slot;
    uint32_t confirmation_sequence;
} mock_context_t;

static int resolve(mock_context_t *context,
                   uint32_t address,
                   uint32_t length,
                   uint8_t **pointer)
{
    uint32_t offset;

    if (address < MOCK_BASE) return -1;
    offset = address - MOCK_BASE;
    if (offset > sizeof(context->storage) ||
        length > sizeof(context->storage) - offset) return -1;
    *pointer = &context->storage[offset];
    return 0;
}

static int mock_read(void *opaque,
                     uint32_t address,
                     void *destination,
                     uint32_t length)
{
    mock_context_t *context = (mock_context_t *)opaque;
    uint8_t *source;

    if (context->fail_reads) return -1;
    if (resolve(context, address, length, &source) != 0) return -1;
    memcpy(destination, source, length);
    return 0;
}

static int mock_erase(void *opaque,
                      uint32_t address,
                      uint32_t length)
{
    mock_context_t *context = (mock_context_t *)opaque;
    uint8_t *destination;

    if (resolve(context, address, length, &destination) != 0) return -1;
    memset(destination, 0xff, length);
    return 0;
}

static int mock_program(void *opaque,
                        uint32_t address,
                        const void *source,
                        uint32_t length)
{
    mock_context_t *context = (mock_context_t *)opaque;
    const uint8_t *input = (const uint8_t *)source;
    uint8_t *destination;

    if (resolve(context, address, length, &destination) != 0) return -1;
    for (uint32_t index = 0; index < length; ++index) {
        if ((destination[index] & input[index]) != input[index]) return -1;
        destination[index] &= input[index];
    }
    return 0;
}

static int recovery_requested(void *opaque)
{
    mock_context_t *context = (mock_context_t *)opaque;
    return context->forced;
}

static int consume_confirmation(
    void *opaque,
    uint8_t *slot,
    uint32_t *sequence)
{
    mock_context_t *context = (mock_context_t *)opaque;

    if (!context->confirmation_available) return 0;
    context->confirmation_available = 0;
    *slot = context->confirmation_slot;
    *sequence = context->confirmation_sequence;
    return 1;
}

static uint16_t load_slot(void *opaque, uint8_t slot)
{
    mock_context_t *context = (mock_context_t *)opaque;

    if (slot > BOOT_SLOT_B) return BOOT_CONTROLLER_SLOT_INVALID;
    ++context->load_count[slot];
    return context->slot_status[slot];
}

static void reset_context(mock_context_t *context)
{
    memset(context, 0, sizeof(*context));
    memset(context->storage, 0xff, sizeof(context->storage));
    context->slot_status[BOOT_SLOT_A] = BOOT_CONTROLLER_SLOT_OK;
    context->slot_status[BOOT_SLOT_B] = BOOT_CONTROLLER_SLOT_OK;
}

static int seed_state(const boot_journal_backend_t *journal,
                      boot_state_t *state,
                      int pending)
{
    boot_state_default(state);
    state->sequence = 1;
    if (boot_journal_store(journal, MOCK_BASE, state) !=
        BOOT_JOURNAL_OK) return -1;
    if (pending) {
        boot_state_begin_update(state, BOOT_SLOT_B);
        if (boot_journal_store(journal, MOCK_BASE, state) !=
            BOOT_JOURNAL_OK) return -1;
    }
    return 0;
}

static int physical_asserted(void *opaque)
{
    return *(int *)opaque;
}

int main(void)
{
    mock_context_t context;
    boot_journal_backend_t journal;
    boot_controller_services_t services;
    boot_controller_result_t result;
    boot_journal_location_t location;
    boot_state_t state;
    boot_recovery_mailbox_t mailbox;
    boot_recovery_request_t request;
    int physical = 0;

    journal.context = &context;
    journal.read = mock_read;
    journal.erase = mock_erase;
    journal.program = mock_program;
    services.journal = &journal;
    services.metadata_address = MOCK_BASE;
    services.context = &context;
    services.recovery_requested = recovery_requested;
    services.consume_confirmation = consume_confirmation;
    services.load_slot = load_slot;

    /* A normal confirmed boot loads A without mutating state. */
    reset_context(&context);
    if (seed_state(&journal, &state, 0) != 0) return 1;
    boot_controller_run(&services, &result);
    if (result.action != BOOT_CONTROLLER_HANDOFF ||
        result.reason != BOOT_CONTROLLER_REASON_SELECTED ||
        result.selected_slot != BOOT_SLOT_A ||
        context.load_count[BOOT_SLOT_A] != 1) return 2;

    /* Entering recovery must not spend a pending trial. */
    reset_context(&context);
    if (seed_state(&journal, &state, 1) != 0) return 3;
    context.forced = 1;
    context.confirmation_available = 1;
    context.confirmation_slot = BOOT_SLOT_B;
    context.confirmation_sequence = state.sequence;
    boot_controller_run(&services, &result);
    if (result.action != BOOT_CONTROLLER_RECOVERY ||
        result.reason != BOOT_CONTROLLER_REASON_FORCED_RECOVERY ||
        result.confirmation_status !=
            BOOT_CONTROLLER_CONFIRMATION_IGNORED_FOR_RECOVERY ||
        result.state.pending_slot != BOOT_SLOT_B ||
        result.state.trial_count != 0 ||
        context.load_count[BOOT_SLOT_A] != 0 ||
        context.load_count[BOOT_SLOT_B] != 0) return 4;
    if (boot_journal_load(
            &journal, MOCK_BASE, &state, &location) !=
        BOOT_JOURNAL_OK || state.trial_count != 0) return 5;

    /* A pending boot is journaled before its image is loaded. */
    reset_context(&context);
    if (seed_state(&journal, &state, 1) != 0) return 6;
    boot_controller_run(&services, &result);
    if (result.action != BOOT_CONTROLLER_HANDOFF ||
        result.selected_slot != BOOT_SLOT_B ||
        result.state.trial_count != 1) return 7;
    if (boot_journal_load(
            &journal, MOCK_BASE, &state, &location) !=
        BOOT_JOURNAL_OK || state.trial_count != 1) return 8;

    /* A matching healthy-app token confirms that exact journal trial. */
    context.confirmation_available = 1;
    context.confirmation_slot = BOOT_SLOT_B;
    context.confirmation_sequence = result.state.sequence;
    boot_controller_run(&services, &result);
    if (result.action != BOOT_CONTROLLER_HANDOFF ||
        result.selected_slot != BOOT_SLOT_B ||
        result.confirmation_status !=
            BOOT_CONTROLLER_CONFIRMATION_ACCEPTED ||
        result.state.confirmed_slot != BOOT_SLOT_B ||
        result.state.pending_slot != BOOT_SLOT_NONE) return 24;
    if (boot_journal_load(
            &journal, MOCK_BASE, &state, &location) !=
        BOOT_JOURNAL_OK ||
        state.confirmed_slot != BOOT_SLOT_B ||
        state.pending_slot != BOOT_SLOT_NONE) return 25;

    /* A stale sequence cannot confirm a different trial. */
    reset_context(&context);
    if (seed_state(&journal, &state, 1) != 0) return 26;
    boot_controller_run(&services, &result);
    context.confirmation_available = 1;
    context.confirmation_slot = BOOT_SLOT_B;
    context.confirmation_sequence = result.state.sequence - 1U;
    boot_controller_run(&services, &result);
    if (result.action != BOOT_CONTROLLER_HANDOFF ||
        result.selected_slot != BOOT_SLOT_B ||
        result.confirmation_status !=
            BOOT_CONTROLLER_CONFIRMATION_STALE ||
        result.state.pending_slot != BOOT_SLOT_B ||
        result.state.trial_count != 2U) return 27;

    /* A bad pending image is rejected durably before fallback. */
    reset_context(&context);
    if (seed_state(&journal, &state, 1) != 0) return 9;
    context.slot_status[BOOT_SLOT_B] =
        BOOT_CONTROLLER_SLOT_INVALID;
    boot_controller_run(&services, &result);
    if (result.action != BOOT_CONTROLLER_HANDOFF ||
        result.reason != BOOT_CONTROLLER_REASON_FALLBACK ||
        result.primary_slot != BOOT_SLOT_B ||
        result.selected_slot != BOOT_SLOT_A ||
        result.state.pending_slot != BOOT_SLOT_NONE) return 10;
    if (boot_journal_load(
            &journal, MOCK_BASE, &state, &location) !=
        BOOT_JOURNAL_OK ||
        state.pending_slot != BOOT_SLOT_NONE ||
        state.confirmed_slot != BOOT_SLOT_A) return 11;

    /* Three unconfirmed boots are allowed; the fourth rolls back. */
    reset_context(&context);
    if (seed_state(&journal, &state, 1) != 0) return 12;
    for (int attempt = 0; attempt < 3; ++attempt) {
        boot_controller_run(&services, &result);
        if (result.action != BOOT_CONTROLLER_HANDOFF ||
            result.selected_slot != BOOT_SLOT_B) return 13;
    }
    boot_controller_run(&services, &result);
    if (result.action != BOOT_CONTROLLER_HANDOFF ||
        result.selected_slot != BOOT_SLOT_A ||
        result.state.pending_slot != BOOT_SLOT_NONE ||
        result.state.trial_count != 0) return 14;

    /* An invalid confirmed slot gets one emergency fallback. */
    reset_context(&context);
    if (seed_state(&journal, &state, 0) != 0) return 15;
    context.slot_status[BOOT_SLOT_A] =
        BOOT_CONTROLLER_SLOT_INVALID;
    boot_controller_run(&services, &result);
    if (result.action != BOOT_CONTROLLER_HANDOFF ||
        result.reason != BOOT_CONTROLLER_REASON_FALLBACK ||
        result.selected_slot != BOOT_SLOT_B) return 16;

    /* Two invalid slots or an unreadable journal force recovery. */
    context.slot_status[BOOT_SLOT_B] =
        BOOT_CONTROLLER_SLOT_INVALID;
    boot_controller_run(&services, &result);
    if (result.action != BOOT_CONTROLLER_RECOVERY ||
        result.reason != BOOT_CONTROLLER_REASON_NO_VALID_SLOT) return 17;
    context.fail_reads = 1;
    boot_controller_run(&services, &result);
    if (result.action != BOOT_CONTROLLER_RECOVERY ||
        result.reason != BOOT_CONTROLLER_REASON_JOURNAL_ERROR) return 18;

    /* The 64-bit software mailbox is one-shot and torn-write safe. */
    memset(&mailbox, 0, sizeof(mailbox));
    request.mailbox = &mailbox;
    request.physical_context = &physical;
    request.physical_asserted = physical_asserted;
    boot_recovery_request_arm(&mailbox);
    if (boot_recovery_request_consume(&request) !=
        BOOT_RECOVERY_REQUEST_SOFTWARE) return 19;
    if (boot_recovery_request_consume(&request) !=
        BOOT_RECOVERY_REQUEST_NONE) return 20;
    mailbox.magic = BOOT_RECOVERY_REQUEST_MAGIC;
    mailbox.inverse = 0;
    if (boot_recovery_request_consume(&request) !=
        BOOT_RECOVERY_REQUEST_NONE ||
        mailbox.magic != 0 || mailbox.inverse != 0) return 21;

    /* Physical recovery wins, while also consuming a software token. */
    boot_recovery_request_arm(&mailbox);
    physical = 1;
    if (boot_recovery_request_consume(&request) !=
        BOOT_RECOVERY_REQUEST_PHYSICAL) return 22;
    physical = 0;
    if (boot_recovery_request_consume(&request) !=
        BOOT_RECOVERY_REQUEST_NONE) return 23;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "boot_controller_test.c"
            executable = directory_path / "boot_controller_test"
            test_source.write_text(source)
            subprocess.run(
                [
                    compiler,
                    "-std=c17",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-Wconversion",
                    "-Wshadow",
                    "-Wundef",
                    "-I",
                    str(ROOT / "firmware" / "bootloader" / "include"),
                    str(
                        ROOT
                        / "firmware"
                        / "bootloader"
                        / "src"
                        / "boot_controller.c"
                    ),
                    str(
                        ROOT
                        / "firmware"
                        / "bootloader"
                        / "src"
                        / "boot_recovery_request.c"
                    ),
                    str(
                        ROOT
                        / "firmware"
                        / "bootloader"
                        / "src"
                        / "boot_lifecycle.c"
                    ),
                    str(
                        ROOT
                        / "firmware"
                        / "bootloader"
                        / "src"
                        / "boot_journal.c"
                    ),
                    str(
                        ROOT
                        / "firmware"
                        / "bootloader"
                        / "src"
                        / "boot_state.c"
                    ),
                    str(
                        ROOT
                        / "firmware"
                        / "bootloader"
                        / "src"
                        / "crc32.c"
                    ),
                    str(test_source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
