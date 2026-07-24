import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class FullFlashRecoveryTests(unittest.TestCase):
    def test_guarded_full_flash_transaction(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "boot_state.h"
#include "ncr2_flash_layout.h"
#include "recovery_engine.h"
#include "sha256.h"

typedef struct mock_context {
    uint8_t flash[NCR2_FLASH_SIZE];
    uint32_t erase_calls;
    uint32_t program_calls;
    uint32_t reboot_calls;
} mock_context_t;

static int resolve(uint32_t address,
                   uint32_t length,
                   uint8_t **pointer,
                   mock_context_t *context)
{
    uint32_t offset;
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
    if (resolve(address, length, &source, context) != 0) return -1;
    memcpy(destination, source, length);
    return 0;
}

static int mock_erase(void *opaque,
                      uint32_t address,
                      uint32_t length)
{
    mock_context_t *context = (mock_context_t *)opaque;
    uint8_t *destination;
    if (length != RECOVERY_FULL_FLASH_ERASE_CHUNK_SIZE ||
        resolve(address, length, &destination, context) != 0) return -1;
    memset(destination, 0xff, length);
    ++context->erase_calls;
    return 0;
}

static int mock_program(void *opaque,
                        uint32_t address,
                        const void *source,
                        uint32_t length)
{
    mock_context_t *context = (mock_context_t *)opaque;
    uint8_t *destination;
    if (resolve(address, length, &destination, context) != 0) return -1;
    memcpy(destination, source, length);
    ++context->program_calls;
    return 0;
}

static int mock_store(void *opaque, const boot_state_t *state)
{
    (void)opaque;
    (void)state;
    return -1;
}

static void mock_reboot(void *opaque)
{
    mock_context_t *context = (mock_context_t *)opaque;
    ++context->reboot_calls;
}

static void request(recovery_packet_t *packet,
                    uint8_t command,
                    uint16_t flags,
                    uint32_t session,
                    uint32_t sequence,
                    uint32_t offset,
                    const uint8_t *payload,
                    uint16_t length)
{
    memset(packet, 0, sizeof(*packet));
    packet->magic = RECOVERY_PACKET_MAGIC;
    packet->version = RECOVERY_PROTOCOL_VERSION;
    packet->command = command;
    packet->flags = flags;
    packet->session = session;
    packet->sequence = sequence;
    packet->offset = offset;
    packet->length = length;
    if (payload != NULL) memcpy(packet->payload, payload, length);
    recovery_packet_finalize(packet);
}

static int run(recovery_engine_t *engine,
               recovery_packet_t *request_packet,
               recovery_packet_t *response,
               uint16_t expected)
{
    recovery_engine_process(engine, request_packet, response);
    return response->status == expected ? 0 : -1;
}

int main(void)
{
    static mock_context_t context;
    static uint8_t image[NCR2_FLASH_SIZE];
    recovery_backend_t backend;
    recovery_engine_t engine;
    recovery_packet_t in;
    recovery_packet_t out;
    recovery_info_t info;
    boot_state_t state;
    sha256_context_t sha;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint32_t session;
    uint32_t sequence;

    for (uint32_t index = 0; index < NCR2_FLASH_SIZE; ++index) {
        image[index] = (uint8_t)(index ^ (index >> 8) ^ 0x5aU);
    }
    sha256_init(&sha);
    sha256_update(&sha, image, NCR2_FLASH_SIZE);
    sha256_final(&sha, digest);
    memset(&context, 0xa5, sizeof(context.flash));
    context.erase_calls = 0;
    context.program_calls = 0;
    context.reboot_calls = 0;

    backend.context = &context;
    backend.read = mock_read;
    backend.erase = mock_erase;
    backend.program = mock_program;
    backend.get_log = NULL;
    backend.store_boot_state = mock_store;
    backend.request_reboot = mock_reboot;
    boot_state_default(&state);
    recovery_engine_init(
        &engine, &backend, &state, BOOT_SLOT_NONE, 0x12345678U);

    request(
        &in, RECOVERY_COMMAND_GET_INFO, 0, 0, 0, 0, NULL, 0);
    if (run(&engine, &in, &out, RECOVERY_STATUS_OK) != 0) return 1;
    memcpy(&info, out.payload, sizeof(info));
    if ((info.capabilities & RECOVERY_CAPABILITY_FULL_FLASH_RAM) != 0) {
        return 2;
    }

    request(
        &in,
        RECOVERY_COMMAND_BEGIN_FULL_FLASH,
        RECOVERY_FLAG_FULL_FLASH,
        RECOVERY_FULL_FLASH_UNLOCK,
        0,
        NCR2_FLASH_SIZE,
        digest,
        SHA256_DIGEST_SIZE);
    if (run(
            &engine,
            &in,
            &out,
            RECOVERY_STATUS_FULL_FLASH_DISABLED) != 0) return 3;

    recovery_engine_enable_full_flash(&engine);
    request(
        &in, RECOVERY_COMMAND_GET_INFO, 0, 0, 0, 0, NULL, 0);
    if (run(&engine, &in, &out, RECOVERY_STATUS_OK) != 0) return 4;
    memcpy(&info, out.payload, sizeof(info));
    if ((info.capabilities & RECOVERY_CAPABILITY_FULL_FLASH_RAM) == 0) {
        return 5;
    }
    if ((info.capabilities &
         RECOVERY_CAPABILITY_PROGRESSIVE_FULL_ERASE) == 0) {
        return 15;
    }

    request(
        &in,
        RECOVERY_COMMAND_BEGIN_FULL_FLASH,
        RECOVERY_FLAG_FULL_FLASH,
        RECOVERY_FULL_FLASH_UNLOCK,
        0,
        NCR2_FLASH_SIZE,
        digest,
        SHA256_DIGEST_SIZE);
    if (run(&engine, &in, &out, RECOVERY_STATUS_OK) != 0) return 6;
    session = out.session;
    sequence = 1;

    request(
        &in,
        RECOVERY_COMMAND_ERASE_FULL_FLASH,
        RECOVERY_FLAG_FULL_FLASH,
        session,
        sequence,
        RECOVERY_FULL_FLASH_ERASE_CHUNK_SIZE,
        NULL,
        0);
    if (run(&engine, &in, &out, RECOVERY_STATUS_WRITE_ORDER) != 0) {
        return 17;
    }

    for (uint32_t offset = 0; offset < NCR2_FLASH_SIZE;
         offset += RECOVERY_FULL_FLASH_ERASE_CHUNK_SIZE) {
        request(
            &in,
            RECOVERY_COMMAND_ERASE_FULL_FLASH,
            RECOVERY_FLAG_FULL_FLASH,
            session,
            sequence++,
            offset,
            NULL,
            0);
        if (run(&engine, &in, &out, RECOVERY_STATUS_OK) != 0) return 7;
        if (out.offset !=
            offset + RECOVERY_FULL_FLASH_ERASE_CHUNK_SIZE) return 8;
        if (offset == 0) {
            uint32_t erase_calls = context.erase_calls;
            if (run(
                    &engine,
                    &in,
                    &out,
                    RECOVERY_STATUS_OK) != 0) return 18;
            if (context.erase_calls != erase_calls) return 19;
        }
    }
    if (context.erase_calls !=
        NCR2_FLASH_SIZE /
        RECOVERY_FULL_FLASH_ERASE_CHUNK_SIZE) return 16;

    for (uint32_t offset = 0; offset < NCR2_FLASH_SIZE;
         offset += RECOVERY_PAYLOAD_SIZE) {
        request(
            &in,
            RECOVERY_COMMAND_WRITE_CHUNK,
            RECOVERY_FLAG_FULL_FLASH,
            session,
            sequence++,
            offset,
            &image[offset],
            RECOVERY_PAYLOAD_SIZE);
        if (run(&engine, &in, &out, RECOVERY_STATUS_OK) != 0) return 9;
    }
    if (context.program_calls !=
        NCR2_FLASH_SIZE / RECOVERY_PAYLOAD_SIZE) return 10;

    request(
        &in,
        RECOVERY_COMMAND_FINALIZE_FULL_FLASH,
        RECOVERY_FLAG_FULL_FLASH,
        session,
        sequence++,
        0,
        NULL,
        0);
    if (run(&engine, &in, &out, RECOVERY_STATUS_OK) != 0) return 11;
    if (memcmp(context.flash, image, sizeof(image)) != 0) return 12;

    request(
        &in,
        RECOVERY_COMMAND_REBOOT,
        RECOVERY_FLAG_FULL_FLASH,
        session,
        sequence++,
        0,
        NULL,
        0);
    if (run(&engine, &in, &out, RECOVERY_STATUS_OK) != 0) return 13;
    if (context.reboot_calls != 1) return 14;
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "full_flash_test.c"
            executable = directory_path / "full_flash_test"
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
                    str(ROOT / "firmware" / "include"),
                    "-I",
                    str(ROOT / "firmware" / "platform" / "ncr2" / "include"),
                    str(ROOT / "firmware" / "bootloader" / "src" / "boot_state.c"),
                    str(ROOT / "firmware" / "bootloader" / "src" / "crc32.c"),
                    str(
                        ROOT
                        / "firmware"
                        / "bootloader"
                        / "src"
                        / "recovery_engine.c"
                    ),
                    str(
                        ROOT
                        / "firmware"
                        / "bootloader"
                        / "src"
                        / "recovery_protocol.c"
                    ),
                    str(ROOT / "firmware" / "bootloader" / "src" / "sha256.c"),
                    str(test_source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([executable], check=True)

    def test_ram_target_is_opt_in_and_sdram_linked(self):
        cmake = (ROOT / "firmware" / "CMakeLists.txt").read_text()
        linker = (
            ROOT
            / "firmware"
            / "ram_recovery"
            / "ncr2_ram_recovery.ld"
        ).read_text()
        source = (
            ROOT / "firmware" / "ram_recovery" / "src" / "main.c"
        ).read_text()

        self.assertIn("NCR2_BUILD_RAM_RECOVERY", cmake)
        self.assertIn("NCR2_EMBED_RAM_RECOVERY", cmake)
        self.assertIn("ncr2_ram_recovery", cmake)
        self.assertIn("ORIGIN = 0x80000000", linker)
        self.assertNotIn("0x60002000", linker)
        self.assertIn("ncr2_flexspi_nor_init_full_flash", source)
        self.assertIn("recovery_engine_enable_full_flash", source)
        checker = (
            ROOT / "tools" / "check_hardware_bootloader.py"
        ).read_text()
        self.assertIn("--expect-embedded-ram-recovery", checker)
        self.assertIn(".ram_recovery_blob", checker)


if __name__ == "__main__":
    unittest.main()
