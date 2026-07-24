import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class RecoveryEngineTests(unittest.TestCase):
    def test_guarded_ab_update_transaction(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        source = r"""
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "boot_state.h"
#include "crc32.h"
#include "ncr2_flash_layout.h"
#include "ncr2_nor.h"
#include "pedal_image.h"
#include "recovery_engine.h"
#include "sha256.h"

typedef struct mock_context {
    uint8_t slots[2][NCR2_APPLICATION_SLOT_SIZE];
    boot_state_t stored_state;
    unsigned int erase_calls;
    unsigned int program_calls;
    unsigned int state_calls;
    unsigned int reboot_calls;
    uint32_t erased_address;
    uint32_t erased_length;
} mock_context_t;

static int resolve_mock(uint32_t address,
                        uint32_t length,
                        uint8_t **pointer,
                        mock_context_t *context)
{
    uint32_t offset;
    unsigned int slot;

    if (address >= NCR2_APPLICATION_A_ADDRESS &&
        address < NCR2_APPLICATION_A_ADDRESS +
                  NCR2_APPLICATION_SLOT_SIZE) {
        slot = 0;
        offset = address - NCR2_APPLICATION_A_ADDRESS;
    } else if (address >= NCR2_APPLICATION_B_ADDRESS &&
               address < NCR2_APPLICATION_B_ADDRESS +
                         NCR2_APPLICATION_SLOT_SIZE) {
        slot = 1;
        offset = address - NCR2_APPLICATION_B_ADDRESS;
    } else {
        return -1;
    }
    if (length > NCR2_APPLICATION_SLOT_SIZE - offset) {
        return -1;
    }
    *pointer = &context->slots[slot][offset];
    return 0;
}

static int mock_read(void *opaque,
                     uint32_t address,
                     void *destination,
                     uint32_t length)
{
    mock_context_t *context = (mock_context_t *)opaque;
    uint8_t *source;

    if (resolve_mock(address, length, &source, context) != 0) {
        return -1;
    }
    memcpy(destination, source, length);
    return 0;
}

static int mock_erase(void *opaque,
                      uint32_t address,
                      uint32_t length)
{
    mock_context_t *context = (mock_context_t *)opaque;
    uint8_t *destination;

    if (resolve_mock(address, length, &destination, context) != 0) {
        return -1;
    }
    memset(destination, 0xff, length);
    context->erased_address = address;
    context->erased_length = length;
    ++context->erase_calls;
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

    if (resolve_mock(address, length, &destination, context) != 0) {
        return -1;
    }
    for (uint32_t index = 0; index < length; ++index) {
        if ((destination[index] & input[index]) != input[index]) {
            return -1;
        }
        destination[index] &= input[index];
    }
    ++context->program_calls;
    return 0;
}

static int mock_store_state(void *opaque, const boot_state_t *state)
{
    mock_context_t *context = (mock_context_t *)opaque;
    context->stored_state = *state;
    ++context->state_calls;
    return 0;
}

static void mock_reboot(void *opaque)
{
    mock_context_t *context = (mock_context_t *)opaque;
    ++context->reboot_calls;
}

static void make_request(recovery_packet_t *packet,
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
    if (payload != NULL && length != 0) {
        memcpy(packet->payload, payload, length);
    }
    recovery_packet_finalize(packet);
}

static int process_expect(recovery_engine_t *engine,
                          recovery_packet_t *request,
                          recovery_packet_t *response,
                          uint16_t expected_status)
{
    recovery_engine_process(engine, request, response);
    if (response->status != expected_status) {
        return -1;
    }
    if (recovery_packet_validate(response, 0, 0, 0) !=
        RECOVERY_STATUS_OK) {
        return -2;
    }
    return 0;
}

int main(void)
{
    static mock_context_t context;
    static uint8_t image[NCR2_APPLICATION_MANIFEST_SIZE + 16];
    recovery_backend_t backend;
    recovery_engine_t engine;
    recovery_packet_t request;
    recovery_packet_t response;
    recovery_packet_t first_response;
    boot_state_t state;
    pedal_image_manifest_t *manifest;
    sha256_context_t sha;
    uint32_t *vectors;
    uint32_t session;
    uint32_t sequence;
    uint32_t write_offset;
    unsigned int programs_after_first;

    memset(&context, 0, sizeof(context));
    memset(context.slots, 0xa5, sizeof(context.slots));
    boot_state_default(&state);
    state.sequence = 1;
    state.found_record = 1;

    backend.context = &context;
    backend.read = mock_read;
    backend.erase = mock_erase;
    backend.program = mock_program;
    backend.get_log = NULL;
    backend.store_boot_state = mock_store_state;
    backend.request_reboot = mock_reboot;
    recovery_engine_init(&engine, &backend, &state, BOOT_SLOT_A, 0x1234);

    make_request(
        &request, RECOVERY_COMMAND_GET_INFO, 0, 0, 0, 0, NULL, 0);
    if (process_expect(
            &engine, &request, &response, RECOVERY_STATUS_OK) != 0) {
        return 1;
    }
    if (response.length != sizeof(recovery_info_t)) return 2;

    make_request(
        &request, RECOVERY_COMMAND_BEGIN_IMAGE, 0, 0, 0, 0, NULL, 0);
    if (process_expect(
            &engine,
            &request,
            &response,
            RECOVERY_STATUS_ACTIVE_SLOT) != 0) {
        return 3;
    }

    make_request(
        &request,
        RECOVERY_COMMAND_BEGIN_IMAGE,
        1,
        0,
        0,
        sizeof(image),
        NULL,
        0);
    if (process_expect(
            &engine, &request, &response, RECOVERY_STATUS_OK) != 0) {
        return 4;
    }
    session = response.session;
    if (session == 0) return 5;
    first_response = response;
    if (process_expect(
            &engine, &request, &response, RECOVERY_STATUS_OK) != 0) {
        return 6;
    }
    if (memcmp(&first_response, &response, sizeof(response)) != 0) return 7;

    sequence = 1;
    make_request(
        &request,
        RECOVERY_COMMAND_ERASE_SLOT,
        1,
        session,
        sequence++,
        0,
        NULL,
        0);
    if (process_expect(
            &engine, &request, &response, RECOVERY_STATUS_OK) != 0) {
        return 8;
    }
    if (context.erase_calls != 1) return 9;
    if (context.erased_address != NCR2_APPLICATION_B_ADDRESS ||
        context.erased_length != NCR2_APPLICATION_MANIFEST_SIZE +
                                 NCR2_NOR_SECTOR_SIZE) return 27;

    memset(image, 0xff, sizeof(image));
    manifest = (pedal_image_manifest_t *)image;
    vectors = (uint32_t *)&image[NCR2_APPLICATION_MANIFEST_SIZE];
    vectors[0] = NCR2_DTCM_END;
    vectors[1] = NCR2_APPLICATION_LOAD_ADDRESS | 1u;
    vectors[2] = 0xbf00bf00u;
    vectors[3] = 0xbf00bf00u;
    manifest->magic = PEDAL_IMAGE_MAGIC;
    manifest->format_version = PEDAL_IMAGE_FORMAT_VERSION;
    manifest->header_size = NCR2_APPLICATION_MANIFEST_SIZE;
    manifest->image_size = 16;
    manifest->load_address = NCR2_APPLICATION_LOAD_ADDRESS;
    manifest->vector_offset = 0;
    manifest->board_id = PEDAL_IMAGE_BOARD_NCR2;
    manifest->semantic_version = 0x00010000u;
    manifest->build_number = 7;
    sha256_init(&sha);
    sha256_update(
        &sha, &image[NCR2_APPLICATION_MANIFEST_SIZE], 16);
    sha256_final(&sha, manifest->image_sha256);
    manifest->header_crc32 = crc32_compute(
        manifest, offsetof(pedal_image_manifest_t, header_crc32));

    make_request(
        &request,
        RECOVERY_COMMAND_WRITE_CHUNK,
        1,
        session,
        sequence,
        32,
        image,
        32);
    if (process_expect(
            &engine,
            &request,
            &response,
            RECOVERY_STATUS_WRITE_ORDER) != 0) {
        return 10;
    }

    write_offset = 0;
    while (write_offset < sizeof(image)) {
        uint16_t chunk = RECOVERY_PAYLOAD_SIZE;
        if (chunk > sizeof(image) - write_offset) {
            chunk = (uint16_t)(sizeof(image) - write_offset);
        }
        make_request(
            &request,
            RECOVERY_COMMAND_WRITE_CHUNK,
            1,
            session,
            sequence++,
            write_offset,
            &image[write_offset],
            chunk);
        if (process_expect(
                &engine, &request, &response, RECOVERY_STATUS_OK) != 0) {
            return 11;
        }
        if (write_offset == 0) {
            programs_after_first = context.program_calls;
            first_response = response;
            if (process_expect(
                    &engine,
                    &request,
                    &response,
                    RECOVERY_STATUS_OK) != 0) {
                return 12;
            }
            if (context.program_calls != programs_after_first) return 13;
            if (memcmp(
                    &first_response, &response, sizeof(response)) != 0) {
                return 14;
            }
        }
        write_offset += chunk;
    }

    make_request(
        &request,
        RECOVERY_COMMAND_SET_PENDING,
        1,
        session,
        sequence,
        0,
        NULL,
        0);
    if (process_expect(
            &engine,
            &request,
            &response,
            RECOVERY_STATUS_NOT_FINALIZED) != 0) {
        return 15;
    }
    if (context.state_calls != 0) return 16;

    make_request(
        &request,
        RECOVERY_COMMAND_FINALIZE_IMAGE,
        1,
        session,
        sequence++,
        0,
        NULL,
        0);
    if (process_expect(
            &engine, &request, &response, RECOVERY_STATUS_OK) != 0) {
        return 17;
    }

    make_request(
        &request,
        RECOVERY_COMMAND_READ_CHUNK,
        1,
        session,
        sequence++,
        NCR2_APPLICATION_MANIFEST_SIZE,
        NULL,
        16);
    if (process_expect(
            &engine, &request, &response, RECOVERY_STATUS_OK) != 0) {
        return 18;
    }
    if (response.length != 16 ||
        memcmp(response.payload, vectors, 16) != 0) {
        return 19;
    }

    make_request(
        &request,
        RECOVERY_COMMAND_SET_PENDING,
        1,
        session,
        sequence++,
        0,
        NULL,
        0);
    if (process_expect(
            &engine, &request, &response, RECOVERY_STATUS_OK) != 0) {
        return 20;
    }
    if (context.state_calls != 1) return 21;
    if (context.stored_state.confirmed_slot != BOOT_SLOT_A) return 22;
    if (context.stored_state.pending_slot != BOOT_SLOT_B) return 23;

    make_request(
        &request,
        RECOVERY_COMMAND_REBOOT,
        1,
        session,
        sequence++,
        0,
        NULL,
        0);
    if (process_expect(
            &engine, &request, &response, RECOVERY_STATUS_OK) != 0) {
        return 24;
    }
    if (context.reboot_calls != 1) return 25;

    for (uint32_t index = 0; index < NCR2_APPLICATION_SLOT_SIZE; ++index) {
        if (context.slots[0][index] != 0xa5) return 26;
    }
    return 0;
}
"""

        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "recovery_engine_test.c"
            executable = directory_path / "recovery_engine_test"
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
                    str(ROOT / "firmware" / "include"),
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
                    str(
                        ROOT
                        / "firmware"
                        / "bootloader"
                        / "src"
                        / "sha256.c"
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
