#include "recovery_protocol.h"

#include <stddef.h>

#include "boot_state.h"
#include "crc32.h"
#include "ncr2_flash_layout.h"

static int command_is_valid(uint8_t command)
{
    return command >= RECOVERY_COMMAND_GET_INFO &&
           command <= RECOVERY_COMMAND_FINALIZE_FULL_FLASH;
}

int recovery_command_is_mutating(uint8_t command)
{
    return command == RECOVERY_COMMAND_BEGIN_IMAGE ||
           command == RECOVERY_COMMAND_ERASE_SLOT ||
           command == RECOVERY_COMMAND_WRITE_CHUNK ||
           command == RECOVERY_COMMAND_FINALIZE_IMAGE ||
           command == RECOVERY_COMMAND_SET_PENDING ||
           command == RECOVERY_COMMAND_REBOOT ||
           command == RECOVERY_COMMAND_BEGIN_FULL_FLASH ||
           command == RECOVERY_COMMAND_ERASE_FULL_FLASH ||
           command == RECOVERY_COMMAND_FINALIZE_FULL_FLASH;
}

void recovery_packet_finalize(recovery_packet_t *packet)
{
    if (packet->length <= RECOVERY_PAYLOAD_SIZE) {
        packet->payload_crc32 =
            crc32_compute(packet->payload, packet->length);
    } else {
        packet->payload_crc32 = UINT32_C(0);
    }
    packet->header_crc32 =
        crc32_compute(packet, offsetof(recovery_packet_t, header_crc32));
}

uint16_t recovery_packet_validate(const recovery_packet_t *packet,
                                  uint32_t expected_session,
                                  uint32_t expected_sequence,
                                  int require_session)
{
    const uint32_t header_crc =
        crc32_compute(packet, offsetof(recovery_packet_t, header_crc32));
    uint32_t payload_crc;

    if (packet->magic != RECOVERY_PACKET_MAGIC) {
        return RECOVERY_STATUS_BAD_MAGIC;
    }
    if (packet->version != RECOVERY_PROTOCOL_VERSION) {
        return RECOVERY_STATUS_BAD_VERSION;
    }
    if (!command_is_valid(packet->command)) {
        return RECOVERY_STATUS_BAD_COMMAND;
    }
    if (packet->length > RECOVERY_PAYLOAD_SIZE) {
        return RECOVERY_STATUS_BAD_LENGTH;
    }
    if (packet->header_crc32 != header_crc) {
        return RECOVERY_STATUS_BAD_HEADER_CRC;
    }
    payload_crc = crc32_compute(packet->payload, packet->length);
    if (packet->payload_crc32 != payload_crc) {
        return RECOVERY_STATUS_BAD_PAYLOAD_CRC;
    }
    if (require_session != 0 &&
        packet->session != expected_session) {
        return RECOVERY_STATUS_BAD_SESSION;
    }
    if (require_session != 0 &&
        packet->sequence != expected_sequence) {
        return RECOVERY_STATUS_BAD_SEQUENCE;
    }
    return RECOVERY_STATUS_OK;
}

uint16_t recovery_resolve_range(uint8_t slot,
                                uint32_t offset,
                                uint32_t length,
                                uint32_t *flash_address)
{
    uint32_t slot_offset;

    if (slot == BOOT_SLOT_A) {
        slot_offset = NCR2_APPLICATION_A_OFFSET;
    } else if (slot == BOOT_SLOT_B) {
        slot_offset = NCR2_APPLICATION_B_OFFSET;
    } else {
        return RECOVERY_STATUS_BAD_SLOT;
    }
    if (length == UINT32_C(0) ||
        offset >= NCR2_APPLICATION_SLOT_SIZE ||
        length > NCR2_APPLICATION_SLOT_SIZE - offset) {
        return RECOVERY_STATUS_RANGE_DENIED;
    }
    *flash_address = NCR2_FLASH_XIP_BASE + slot_offset + offset;
    return RECOVERY_STATUS_OK;
}
