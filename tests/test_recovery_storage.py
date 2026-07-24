import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class RecoveryStorageTests(unittest.TestCase):
    def test_nor_policy_journal_and_recovery_backend_bridge(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>
#include <string.h>

#include "boot_journal.h"
#include "boot_state.h"
#include "ncr2_flash_layout.h"
#include "ncr2_nor.h"
#include "recovery_storage.h"

typedef struct mock_flash {
    uint8_t bytes[NCR2_FLASH_SIZE];
    uint32_t reboot_count;
} mock_flash_t;

static int resolve(mock_flash_t *flash,
                   uint32_t address,
                   uint32_t length,
                   uint8_t **pointer)
{
    uint32_t offset;

    if (address < NCR2_FLASH_XIP_BASE) return -1;
    offset = address - NCR2_FLASH_XIP_BASE;
    if (offset >= NCR2_FLASH_SIZE ||
        length > NCR2_FLASH_SIZE - offset) return -1;
    *pointer = &flash->bytes[offset];
    return 0;
}

static int read_flash(void *opaque,
                      uint32_t address,
                      void *destination,
                      uint32_t length)
{
    mock_flash_t *flash = (mock_flash_t *)opaque;
    uint8_t *source;

    if (resolve(flash, address, length, &source) != 0) return -1;
    memcpy(destination, source, length);
    return 0;
}

static int erase_sector(void *opaque, uint32_t address)
{
    mock_flash_t *flash = (mock_flash_t *)opaque;
    uint8_t *destination;

    if (resolve(
            flash,
            address,
            NCR2_NOR_SECTOR_SIZE,
            &destination) != 0) return -1;
    memset(destination, 0xff, NCR2_NOR_SECTOR_SIZE);
    return 0;
}

static int program_page(void *opaque,
                        uint32_t address,
                        const void *source,
                        uint32_t length)
{
    mock_flash_t *flash = (mock_flash_t *)opaque;
    const uint8_t *input = (const uint8_t *)source;
    uint8_t *destination;

    if (resolve(flash, address, length, &destination) != 0) return -1;
    for (uint32_t index = 0; index < length; ++index) {
        destination[index] &= input[index];
    }
    return 0;
}

static int sync_flash(void *opaque,
                      uint32_t address,
                      uint32_t length)
{
    (void)opaque;
    (void)address;
    (void)length;
    return 0;
}

static void reboot(void *opaque)
{
    mock_flash_t *flash = (mock_flash_t *)opaque;
    ++flash->reboot_count;
}

int main(void)
{
    static mock_flash_t flash;
    ncr2_nor_operations_t operations;
    ncr2_nor_t nor;
    recovery_storage_t storage;
    recovery_backend_t recovery;
    boot_journal_backend_t journal;
    boot_journal_location_t location;
    boot_state_t state;
    boot_state_t loaded;
    uint8_t payload[32];
    uint8_t readback[32];
    const uint32_t metadata =
        NCR2_FLASH_XIP_BASE + NCR2_BOOT_METADATA_OFFSET;

    memset(&flash, 0, sizeof(flash));
    memset(flash.bytes, 0xff, sizeof(flash.bytes));
    memset(payload, 0x3c, sizeof(payload));
    operations.context = &flash;
    operations.read = read_flash;
    operations.erase_sector = erase_sector;
    operations.program_page = program_page;
    operations.sync_after_mutation = sync_flash;

    if (ncr2_nor_init(&nor, &operations) != NCR2_NOR_OK) return 1;
    if (recovery_storage_init(
            &storage, &nor, reboot, &flash) != RECOVERY_STORAGE_OK) {
        return 2;
    }
    recovery_storage_make_backend(&storage, &recovery);
    recovery_storage_make_journal_backend(&storage, &journal);

    if (recovery.erase(
            recovery.context,
            NCR2_FLASH_XIP_BASE,
            NCR2_NOR_SECTOR_SIZE) == 0) return 3;
    if (recovery.erase(
            recovery.context,
            NCR2_APPLICATION_B_ADDRESS,
            NCR2_NOR_SECTOR_SIZE) != 0) return 4;
    if (recovery.program(
            recovery.context,
            NCR2_APPLICATION_B_ADDRESS,
            payload,
            sizeof(payload)) != 0) return 5;
    if (recovery.read(
            recovery.context,
            NCR2_APPLICATION_B_ADDRESS,
            readback,
            sizeof(readback)) != 0) return 6;
    if (memcmp(payload, readback, sizeof(payload)) != 0) return 7;

    boot_state_default(&state);
    state.sequence = 1;
    if (recovery.store_boot_state(
            recovery.context, &state) != 0) return 8;
    if (boot_journal_load(
            &journal,
            metadata,
            &loaded,
            &location) != BOOT_JOURNAL_OK) return 9;
    if (!location.found ||
        loaded.sequence != 1 ||
        loaded.confirmed_slot != BOOT_SLOT_A) return 10;

    recovery.request_reboot(recovery.context);
    if (flash.reboot_count != 1) return 11;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "recovery_storage_test.c"
            executable = directory_path / "recovery_storage_test"
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
                        / "bootloader"
                        / "src"
                        / "recovery_storage.c"
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
