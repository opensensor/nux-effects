import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class Ncr2NorTests(unittest.TestCase):
    def test_policy_page_splitting_and_verification(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>
#include <string.h>

#include "ncr2_flash_layout.h"
#include "ncr2_nor.h"

typedef struct mock_context {
    uint8_t flash[NCR2_FLASH_SIZE];
    uint32_t erase_calls;
    uint32_t program_calls;
    uint32_t sync_calls;
    uint32_t program_addresses[4];
    uint32_t program_lengths[4];
    int corrupt_next_program;
    int fail_next_program;
} mock_context_t;

static int mock_range(mock_context_t *context,
                      uint32_t address,
                      uint32_t length,
                      uint8_t **pointer)
{
    uint32_t offset;
    (void)context;

    if (address < NCR2_FLASH_XIP_BASE) return -1;
    offset = address - NCR2_FLASH_XIP_BASE;
    if (offset >= NCR2_FLASH_SIZE ||
        length > NCR2_FLASH_SIZE - offset) return -1;
    *pointer = &context->flash[offset];
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

static int mock_erase_sector(void *opaque, uint32_t address)
{
    mock_context_t *context = (mock_context_t *)opaque;
    uint8_t *destination;

    if (mock_range(
            context,
            address,
            NCR2_NOR_SECTOR_SIZE,
            &destination) != 0) return -1;
    memset(destination, 0xff, NCR2_NOR_SECTOR_SIZE);
    ++context->erase_calls;
    return 0;
}

static int mock_program_page(void *opaque,
                             uint32_t address,
                             const void *source,
                             uint32_t length)
{
    mock_context_t *context = (mock_context_t *)opaque;
    const uint8_t *input = (const uint8_t *)source;
    uint8_t *destination;
    uint32_t call = context->program_calls;

    if (length == 0 || length > NCR2_NOR_PAGE_SIZE) return -1;
    if ((address & (NCR2_NOR_PAGE_SIZE - 1u)) + length >
        NCR2_NOR_PAGE_SIZE) return -1;
    if (mock_range(context, address, length, &destination) != 0) return -1;
    if (call < 4) {
        context->program_addresses[call] = address;
        context->program_lengths[call] = length;
    }
    ++context->program_calls;
    if (context->fail_next_program) {
        context->fail_next_program = 0;
        return -1;
    }
    for (uint32_t index = 0; index < length; ++index) {
        destination[index] &= input[index];
    }
    if (context->corrupt_next_program) {
        destination[0] ^= 1u;
        context->corrupt_next_program = 0;
    }
    return 0;
}

static int mock_sync(void *opaque,
                     uint32_t address,
                     uint32_t length)
{
    mock_context_t *context = (mock_context_t *)opaque;
    (void)address;
    (void)length;
    ++context->sync_calls;
    return 0;
}

int main(void)
{
    static mock_context_t context;
    ncr2_nor_operations_t operations;
    ncr2_nor_t nor;
    uint8_t payload[20];
    uint8_t readback[20];
    uint32_t address =
        NCR2_APPLICATION_A_ADDRESS + NCR2_NOR_PAGE_SIZE - 6u;
    uint32_t programs;

    memset(&context, 0, sizeof(context));
    memset(context.flash, 0xff, sizeof(context.flash));
    memset(payload, 0x5a, sizeof(payload));
    operations.context = &context;
    operations.read = mock_read;
    operations.erase_sector = mock_erase_sector;
    operations.program_page = mock_program_page;
    operations.sync_after_mutation = mock_sync;

    if (ncr2_nor_init(&nor, &operations) != NCR2_NOR_OK) return 1;
    if (ncr2_nor_read(
            &nor,
            NCR2_FLASH_XIP_BASE,
            readback,
            sizeof(readback)) != NCR2_NOR_OK) return 2;
    if (ncr2_nor_read(
            &nor,
            NCR2_FLASH_XIP_BASE,
            readback,
            0) != NCR2_NOR_INVALID_ARGUMENT) return 3;

    if (ncr2_nor_erase(
            &nor,
            NCR2_FLASH_XIP_BASE,
            NCR2_NOR_SECTOR_SIZE) !=
        NCR2_NOR_PROTECTED_RANGE) return 4;
    if (context.erase_calls != 0) return 5;
    if (ncr2_nor_erase(
            &nor,
            NCR2_APPLICATION_A_ADDRESS + 1u,
            NCR2_NOR_SECTOR_SIZE) !=
        NCR2_NOR_ALIGNMENT_ERROR) return 6;
    context.flash[NCR2_BOOT_METADATA_OFFSET] = 0;
    if (ncr2_nor_erase(
            &nor,
            NCR2_FLASH_XIP_BASE + NCR2_BOOT_METADATA_OFFSET,
            NCR2_NOR_SECTOR_SIZE) != NCR2_NOR_OK) return 7;
    if (context.erase_calls != 1 || context.sync_calls != 1) return 8;
    if (ncr2_nor_erase(
            &nor,
            NCR2_FLASH_XIP_BASE + NCR2_BOOT_METADATA_OFFSET,
            NCR2_NOR_SECTOR_SIZE) != NCR2_NOR_OK) return 21;
    if (context.erase_calls != 1 || context.sync_calls != 1) return 22;

    if (ncr2_nor_program(
            &nor, address, payload, sizeof(payload)) != NCR2_NOR_OK) {
        return 9;
    }
    if (context.program_calls != 2 || context.sync_calls != 3) return 10;
    if (context.program_addresses[0] != address ||
        context.program_lengths[0] != 6u ||
        context.program_addresses[1] != address + 6u ||
        context.program_lengths[1] != 14u) return 11;
    if (ncr2_nor_read(
            &nor, address, readback, sizeof(readback)) != NCR2_NOR_OK) {
        return 12;
    }
    if (memcmp(payload, readback, sizeof(payload)) != 0) return 13;

    programs = context.program_calls;
    payload[0] = 0xff;
    if (ncr2_nor_program(
            &nor, address, payload, sizeof(payload)) !=
        NCR2_NOR_NEEDS_ERASE) return 14;
    if (context.program_calls != programs) return 15;
    payload[0] = 0x5a;

    if (ncr2_nor_program(
            &nor,
            NCR2_APPLICATION_A_ADDRESS +
                NCR2_APPLICATION_SLOT_SIZE - 4u,
            payload,
            8u) != NCR2_NOR_PROTECTED_RANGE) return 16;

    context.flash[
        NCR2_APPLICATION_B_OFFSET + NCR2_NOR_SECTOR_SIZE] = 0xff;
    context.corrupt_next_program = 1;
    if (ncr2_nor_program(
            &nor,
            NCR2_APPLICATION_B_ADDRESS + NCR2_NOR_SECTOR_SIZE,
            payload,
            sizeof(payload)) != NCR2_NOR_VERIFY_ERROR) return 17;

    context.fail_next_program = 1;
    programs = context.sync_calls;
    if (ncr2_nor_program(
            &nor,
            NCR2_APPLICATION_B_ADDRESS + 2u * NCR2_NOR_SECTOR_SIZE,
            payload,
            sizeof(payload)) != NCR2_NOR_IO_ERROR) return 18;
    if (context.sync_calls != programs + 1u) return 19;

    if (ncr2_nor_erase(
            &nor,
            NCR2_FLASH_XIP_BASE + NCR2_FLASH_SIZE -
                NCR2_NOR_SECTOR_SIZE,
            2u * NCR2_NOR_SECTOR_SIZE) !=
        NCR2_NOR_OUT_OF_RANGE) return 20;

    context.flash[0] = 0;
    if (ncr2_nor_init_full_flash(&nor, &operations) !=
        NCR2_NOR_OK) return 23;
    if (ncr2_nor_erase(
            &nor,
            NCR2_FLASH_XIP_BASE,
            NCR2_NOR_SECTOR_SIZE) != NCR2_NOR_OK) return 24;
    if (context.flash[0] != 0xff) return 25;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "ncr2_nor_test.c"
            executable = directory_path / "ncr2_nor_test"
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
                    str(
                        ROOT
                        / "firmware"
                        / "platform"
                        / "ncr2"
                        / "include"
                    ),
                    str(
                        ROOT
                        / "firmware"
                        / "platform"
                        / "ncr2"
                        / "src"
                        / "ncr2_nor.c"
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
