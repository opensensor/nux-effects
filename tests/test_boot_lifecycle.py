import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BootLifecycleTests(unittest.TestCase):
    def test_trial_limit_confirmation_and_rejection(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>
#include <string.h>

#include "boot_lifecycle.h"

#define MOCK_BASE 0x60020000u

typedef struct mock_context {
    uint8_t storage[
        BOOT_JOURNAL_SECTOR_COUNT * BOOT_RECORD_SECTOR_SIZE];
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

static void reset_context(mock_context_t *context)
{
    memset(context, 0xff, sizeof(*context));
}

static int seed_pending(
    const boot_journal_backend_t *backend, boot_state_t *state)
{
    boot_state_default(state);
    state->sequence = 1;
    if (boot_journal_store(backend, MOCK_BASE, state) !=
        BOOT_JOURNAL_OK) return -1;
    boot_state_begin_update(state, BOOT_SLOT_B);
    if (boot_journal_store(backend, MOCK_BASE, state) !=
        BOOT_JOURNAL_OK) return -1;
    return 0;
}

int main(void)
{
    mock_context_t context;
    boot_journal_backend_t backend;
    boot_journal_location_t location;
    boot_state_t state;
    uint8_t selected;

    backend.context = &context;
    backend.read = mock_read;
    backend.erase = mock_erase;
    backend.program = mock_program;

    reset_context(&context);
    if (seed_pending(&backend, &state) != 0) return 1;
    for (uint8_t trial = 1; trial <= BOOT_DEFAULT_MAX_TRIALS; ++trial) {
        if (boot_lifecycle_prepare(
                &backend, MOCK_BASE, &state, &selected) !=
            BOOT_LIFECYCLE_OK) return 2;
        if (selected != BOOT_SLOT_B ||
            state.pending_slot != BOOT_SLOT_B ||
            state.trial_count != trial) return 3;
    }
    if (boot_lifecycle_prepare(
            &backend, MOCK_BASE, &state, &selected) !=
        BOOT_LIFECYCLE_OK) return 4;
    if (selected != BOOT_SLOT_A ||
        state.confirmed_slot != BOOT_SLOT_A ||
        state.pending_slot != BOOT_SLOT_NONE ||
        state.trial_count != 0) return 5;
    if (boot_journal_load(
            &backend, MOCK_BASE, &state, &location) !=
        BOOT_JOURNAL_OK) return 6;
    if (state.pending_slot != BOOT_SLOT_NONE) return 7;

    reset_context(&context);
    if (seed_pending(&backend, &state) != 0) return 8;
    if (boot_lifecycle_prepare(
            &backend, MOCK_BASE, &state, &selected) !=
        BOOT_LIFECYCLE_OK) return 9;
    if (boot_lifecycle_confirm(
            &backend, MOCK_BASE, BOOT_SLOT_A, &state) !=
        BOOT_LIFECYCLE_WRONG_SLOT) return 10;
    if (boot_lifecycle_confirm(
            &backend, MOCK_BASE, BOOT_SLOT_B, &state) !=
        BOOT_LIFECYCLE_OK) return 11;
    if (state.confirmed_slot != BOOT_SLOT_B ||
        state.pending_slot != BOOT_SLOT_NONE ||
        state.trial_count != 0) return 12;
    if (boot_lifecycle_confirm(
            &backend, MOCK_BASE, BOOT_SLOT_B, &state) !=
        BOOT_LIFECYCLE_NOT_PENDING) return 13;

    reset_context(&context);
    if (seed_pending(&backend, &state) != 0) return 14;
    if (boot_lifecycle_reject_pending(
            &backend, MOCK_BASE, &state) !=
        BOOT_LIFECYCLE_OK) return 15;
    if (state.confirmed_slot != BOOT_SLOT_A ||
        state.pending_slot != BOOT_SLOT_NONE) return 16;
    if (boot_lifecycle_reject_pending(
            &backend, MOCK_BASE, &state) !=
        BOOT_LIFECYCLE_NOT_PENDING) return 17;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "boot_lifecycle_test.c"
            executable = directory_path / "boot_lifecycle_test"
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
                        / "boot_journal.c"
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
