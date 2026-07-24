import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class BootJournalTests(unittest.TestCase):
    def test_append_rotation_and_power_loss_recovery(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>
#include <string.h>

#include "boot_journal.h"

#define MOCK_BASE 0x60020000u

typedef struct mock_context {
    uint8_t storage[
        BOOT_JOURNAL_SECTOR_COUNT * BOOT_RECORD_SECTOR_SIZE];
    unsigned int erase_calls;
    unsigned int program_calls;
    unsigned int fail_erase_call;
    int partial_next_program;
} mock_context_t;

static int mock_range(mock_context_t *context,
                      uint32_t address,
                      uint32_t length,
                      uint8_t **pointer)
{
    uint32_t offset;
    (void)context;

    if (address < MOCK_BASE) return -1;
    offset = address - MOCK_BASE;
    if (offset >
        sizeof(context->storage) ||
        length > sizeof(context->storage) - offset) {
        return -1;
    }
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
    if (mock_range(context, address, length, &source) != 0) return -1;
    memcpy(destination, source, length);
    return 0;
}

static int mock_erase(void *opaque,
                      uint32_t address,
                      uint32_t length)
{
    mock_context_t *context = (mock_context_t *)opaque;
    uint8_t *destination;
    if (mock_range(context, address, length, &destination) != 0) return -1;
    ++context->erase_calls;
    if (context->fail_erase_call == context->erase_calls) return -1;
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
    uint32_t write_length = length;

    if (mock_range(context, address, length, &destination) != 0) return -1;
    ++context->program_calls;
    if (context->partial_next_program) {
        write_length = length / 2;
        context->partial_next_program = 0;
    }
    for (uint32_t index = 0; index < write_length; ++index) {
        if ((destination[index] & input[index]) != input[index]) return -1;
        destination[index] &= input[index];
    }
    return write_length == length ? 0 : -1;
}

static int sector_is_erased(
    const mock_context_t *context, unsigned int sector)
{
    const uint8_t *bytes =
        &context->storage[sector * BOOT_RECORD_SECTOR_SIZE];
    for (uint32_t index = 0; index < BOOT_RECORD_SECTOR_SIZE; ++index) {
        if (bytes[index] != 0xffu) return 0;
    }
    return 1;
}

int main(void)
{
    mock_context_t context;
    boot_journal_backend_t backend;
    boot_journal_location_t location;
    boot_state_t state;
    boot_state_t loaded;
    uint32_t sequence;

    memset(&context, 0, sizeof(context));
    memset(context.storage, 0xff, sizeof(context.storage));
    backend.context = &context;
    backend.read = mock_read;
    backend.erase = mock_erase;
    backend.program = mock_program;

    boot_state_default(&state);
    state.sequence = 1;
    if (boot_journal_store(&backend, MOCK_BASE, &state) !=
        BOOT_JOURNAL_OK) return 1;
    if (boot_journal_load(&backend, MOCK_BASE, &loaded, &location) !=
        BOOT_JOURNAL_OK) return 2;
    if (!location.found || location.sector != 0 || location.offset != 0) {
        return 3;
    }
    if (loaded.sequence != 1 ||
        loaded.confirmed_slot != BOOT_SLOT_A) return 4;

    boot_state_begin_update(&state, BOOT_SLOT_B);
    if (boot_journal_store(&backend, MOCK_BASE, &state) !=
        BOOT_JOURNAL_OK) return 5;
    if (state.sequence != 2) return 6;

    boot_state_record_trial(&state);
    context.partial_next_program = 1;
    if (boot_journal_store(&backend, MOCK_BASE, &state) !=
        BOOT_JOURNAL_BACKEND_ERROR) return 7;
    if (boot_journal_load(&backend, MOCK_BASE, &loaded, &location) !=
        BOOT_JOURNAL_OK) return 8;
    if (loaded.sequence != 2 || location.offset != BOOT_RECORD_SIZE) {
        return 9;
    }
    if (boot_journal_store(&backend, MOCK_BASE, &state) !=
        BOOT_JOURNAL_OK) return 10;
    if (boot_journal_load(&backend, MOCK_BASE, &loaded, &location) !=
        BOOT_JOURNAL_OK) return 11;
    if (loaded.sequence != 3 ||
        location.offset != 3 * BOOT_RECORD_SIZE) return 12;

    {
        boot_state_t stale = loaded;
        stale.sequence = 2;
        if (boot_journal_store(&backend, MOCK_BASE, &stale) !=
            BOOT_JOURNAL_STALE_SEQUENCE) return 13;
    }

    state = loaded;
    sequence = state.sequence;
    while (boot_state_find_append_offset(context.storage) <
           BOOT_RECORD_SECTOR_SIZE) {
        ++sequence;
        state.sequence = sequence;
        state.flags = sequence;
        if (boot_journal_store(&backend, MOCK_BASE, &state) !=
            BOOT_JOURNAL_OK) return 14;
    }
    if (sector_is_erased(&context, 0)) return 15;
    if (!sector_is_erased(&context, 1)) return 16;

    ++sequence;
    state.sequence = sequence;
    state.flags = sequence;
    context.partial_next_program = 1;
    if (boot_journal_store(&backend, MOCK_BASE, &state) !=
        BOOT_JOURNAL_BACKEND_ERROR) return 17;
    if (boot_journal_load(&backend, MOCK_BASE, &loaded, &location) !=
        BOOT_JOURNAL_OK) return 18;
    if (loaded.sequence != sequence - 1 || location.sector != 0) return 19;

    context.fail_erase_call = context.erase_calls + 2;
    if (boot_journal_store(&backend, MOCK_BASE, &state) !=
        BOOT_JOURNAL_OK) return 20;
    if (boot_journal_load(&backend, MOCK_BASE, &loaded, &location) !=
        BOOT_JOURNAL_OK) return 21;
    if (loaded.sequence != sequence ||
        loaded.flags != sequence ||
        location.sector != 1 ||
        location.offset != 0) return 22;
    if (sector_is_erased(&context, 0)) return 23;

    ++sequence;
    state.sequence = sequence;
    state.flags = sequence;
    if (boot_journal_store(&backend, MOCK_BASE, &state) !=
        BOOT_JOURNAL_OK) return 24;
    if (boot_journal_load(&backend, MOCK_BASE, &loaded, &location) !=
        BOOT_JOURNAL_OK) return 25;
    if (loaded.sequence != sequence ||
        location.sector != 1 ||
        location.offset != BOOT_RECORD_SIZE) return 26;

    if (boot_journal_store(&backend, MOCK_BASE, &state) !=
        BOOT_JOURNAL_OK) return 27;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "boot_journal_test.c"
            executable = directory_path / "boot_journal_test"
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
