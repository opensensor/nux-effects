"""READ_KNOBS is the non-mutating front-panel capture used to measure the
stepped Type ladder. The detent voltages have never been read off hardware,
so every firmware selector threshold to date has been a guess."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]

SOURCE = r"""
#include <stdint.h>
#include <string.h>

#include "boot_state.h"
#include "crc32.h"
#include "recovery_engine.h"

typedef struct mock_context {
    unsigned int knob_calls;
    int forced_length;
} mock_context_t;

static int mock_read(void *opaque,
                     uint32_t address,
                     void *destination,
                     uint32_t length)
{
    (void)opaque;
    (void)address;
    memset(destination, 0, length);
    return 0;
}

static int mock_erase(void *opaque, uint32_t address, uint32_t length)
{
    (void)opaque;
    (void)address;
    (void)length;
    return 0;
}

static int mock_program(void *opaque,
                        uint32_t address,
                        const void *source,
                        uint32_t length)
{
    (void)opaque;
    (void)address;
    (void)source;
    (void)length;
    return 0;
}

static int mock_store(void *opaque, const boot_state_t *state)
{
    (void)opaque;
    (void)state;
    return 0;
}

static int mock_read_knobs(void *opaque,
                           void *destination,
                           uint32_t capacity)
{
    mock_context_t *context = (mock_context_t *)opaque;
    recovery_knob_sample_t sample;

    if (capacity < sizeof(sample)) {
        return -1;
    }
    ++context->knob_calls;
    if (context->forced_length != 0) {
        return context->forced_length;
    }
    memset(&sample, 0, sizeof(sample));
    sample.magic = RECOVERY_KNOB_SAMPLE_MAGIC;
    sample.value[0] = 1111;
    sample.value[1] = 2222;
    sample.value[2] = 3333;
    sample.value[3] = 444;
    sample.selector_min = 3330;
    sample.selector_max = 3336;
    sample.channel[0] = 5;
    sample.channel[1] = 8;
    sample.channel[2] = 9;
    sample.channel[3] = 11;
    sample.burst = 16;
    sample.valid = 1;
    sample.adc_bits = 12;
    sample.sample_index = context->knob_calls;
    memcpy(destination, &sample, sizeof(sample));
    return (int)sizeof(sample);
}

static void make_request(recovery_packet_t *packet,
                         uint8_t command,
                         uint16_t flags,
                         uint32_t session,
                         uint32_t sequence,
                         uint16_t length)
{
    memset(packet, 0, sizeof(*packet));
    packet->magic = RECOVERY_PACKET_MAGIC;
    packet->version = RECOVERY_PROTOCOL_VERSION;
    packet->command = command;
    packet->flags = flags;
    packet->session = session;
    packet->sequence = sequence;
    packet->length = length;
    recovery_packet_finalize(packet);
}

static uint32_t capabilities_of(recovery_engine_t *engine)
{
    recovery_packet_t request;
    recovery_packet_t response;
    recovery_info_t info;

    make_request(&request, RECOVERY_COMMAND_GET_INFO, 0, 0, 0, 0);
    recovery_engine_process(engine, &request, &response);
    memcpy(&info, response.payload, sizeof(info));
    return info.capabilities;
}

int main(void)
{
    mock_context_t context;
    recovery_backend_t backend;
    recovery_engine_t engine;
    recovery_packet_t request;
    recovery_packet_t response;
    recovery_knob_sample_t sample;
    boot_state_t state;

    memset(&context, 0, sizeof(context));
    boot_state_default(&state);

    backend.context = &context;
    backend.read = mock_read;
    backend.erase = mock_erase;
    backend.program = mock_program;
    backend.get_log = NULL;
    backend.read_knobs = NULL;
    backend.store_boot_state = mock_store;
    backend.request_reboot = NULL;

    /* Without the hook the capability stays clear and the command is
     * unknown, so a host can tell "cannot" apart from "failed". */
    recovery_engine_init(&engine, &backend, &state, BOOT_SLOT_A, 0x1234);
    if ((capabilities_of(&engine) &
         RECOVERY_CAPABILITY_KNOB_SAMPLE) != 0) {
        return 1;
    }
    make_request(&request, RECOVERY_COMMAND_READ_KNOBS, 0, 0, 0, 0);
    recovery_engine_process(&engine, &request, &response);
    if (response.status != RECOVERY_STATUS_BAD_COMMAND) {
        return 2;
    }

    backend.read_knobs = mock_read_knobs;
    recovery_engine_init(&engine, &backend, &state, BOOT_SLOT_A, 0x1234);
    if ((capabilities_of(&engine) &
         RECOVERY_CAPABILITY_KNOB_SAMPLE) == 0) {
        return 3;
    }

    /* Sessionless, exactly like GET_INFO and GET_LOG: calibration must work
     * before any update transaction has been opened. */
    make_request(&request, RECOVERY_COMMAND_READ_KNOBS, 0, 0, 0, 0);
    recovery_engine_process(&engine, &request, &response);
    if (response.status != RECOVERY_STATUS_OK) {
        return 4;
    }
    if (recovery_packet_validate(&response, 0, 0, 0) !=
        RECOVERY_STATUS_OK) {
        return 5;
    }
    if (response.length != sizeof(sample)) {
        return 6;
    }
    memcpy(&sample, response.payload, sizeof(sample));
    if (sample.magic != RECOVERY_KNOB_SAMPLE_MAGIC) return 7;
    if (sample.value[2] != 3333) return 8;
    if (sample.selector_max - sample.selector_min != 6) return 9;
    if (sample.channel[2] != 9) return 10;
    if (sample.valid != 1) return 11;

    /*
     * A byte-identical resend must re-sample rather than replay the retry
     * cache. Sessionless commands must carry sequence zero, so every poll a
     * host makes while a knob sits still is exactly this packet: if the
     * cache served it, the reading would freeze at the first capture.
     */
    make_request(&request, RECOVERY_COMMAND_READ_KNOBS, 0, 0, 0, 0);
    recovery_engine_process(&engine, &request, &response);
    if (response.status != RECOVERY_STATUS_OK) return 12;
    memcpy(&sample, response.payload, sizeof(sample));
    if (sample.sample_index != 2) return 13;
    if (context.knob_calls != 2) return 14;

    /* A short or oversized backend reply is a fault, not a partial read. */
    context.forced_length = 8;
    make_request(&request, RECOVERY_COMMAND_READ_KNOBS, 0, 0, 0, 0);
    recovery_engine_process(&engine, &request, &response);
    if (response.status != RECOVERY_STATUS_BACKEND_ERROR) return 15;
    context.forced_length = 0;

    /* Reading knobs must never be treated as a mutation. */
    if (recovery_command_is_mutating(RECOVERY_COMMAND_READ_KNOBS) != 0) {
        return 16;
    }

    /* The request carries no payload; a host that sends one is rejected. */
    make_request(&request, RECOVERY_COMMAND_READ_KNOBS, 0, 0, 0, 4);
    recovery_engine_process(&engine, &request, &response);
    if (response.status != RECOVERY_STATUS_BAD_LENGTH) return 17;

    /* The full-flash flag belongs only to whole-flash commands. */
    make_request(
        &request, RECOVERY_COMMAND_READ_KNOBS, 0x8000, 0, 0, 0);
    recovery_engine_process(&engine, &request, &response);
    if (response.status != RECOVERY_STATUS_BAD_FLAGS) return 18;

    /* Sampling must not disturb an unrelated command's retry cache. */
    make_request(&request, RECOVERY_COMMAND_GET_INFO, 0, 0, 0, 0);
    recovery_engine_process(&engine, &request, &response);
    if (response.status != RECOVERY_STATUS_OK) return 19;

    return 0;
}
"""


class KnobSamplingTests(unittest.TestCase):
    def test_read_knobs_command(self):
        compiler = shutil.which("cc")
        if compiler is None:
            self.skipTest("host C compiler is unavailable")

        bootloader = ROOT / "firmware" / "bootloader"
        with tempfile.TemporaryDirectory() as directory:
            directory_path = Path(directory)
            test_source = directory_path / "knob_sampling_test.c"
            executable = directory_path / "knob_sampling_test"
            test_source.write_text(SOURCE)
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
                    str(bootloader / "include"),
                    "-I",
                    str(ROOT / "firmware" / "platform" / "ncr2" / "include"),
                    str(bootloader / "src" / "boot_state.c"),
                    str(bootloader / "src" / "crc32.c"),
                    str(bootloader / "src" / "recovery_engine.c"),
                    str(bootloader / "src" / "recovery_protocol.c"),
                    str(bootloader / "src" / "sha256.c"),
                    str(test_source),
                    "-o",
                    str(executable),
                ],
                check=True,
            )
            subprocess.run([executable], check=True)


if __name__ == "__main__":
    unittest.main()
