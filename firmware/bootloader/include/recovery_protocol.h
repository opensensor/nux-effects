#ifndef NCR2_RECOVERY_PROTOCOL_H
#define NCR2_RECOVERY_PROTOCOL_H

#include <stdint.h>

#define RECOVERY_PACKET_MAGIC UINT32_C(0x5846584E)
#define RECOVERY_PROTOCOL_VERSION UINT8_C(1)
#define RECOVERY_PACKET_SIZE 64U
#define RECOVERY_PAYLOAD_SIZE 32U

enum recovery_command {
    RECOVERY_COMMAND_GET_INFO = 1,
    RECOVERY_COMMAND_BEGIN_IMAGE = 2,
    RECOVERY_COMMAND_ERASE_SLOT = 3,
    RECOVERY_COMMAND_WRITE_CHUNK = 4,
    RECOVERY_COMMAND_READ_CHUNK = 5,
    RECOVERY_COMMAND_FINALIZE_IMAGE = 6,
    RECOVERY_COMMAND_SET_PENDING = 7,
    RECOVERY_COMMAND_REBOOT = 8,
    RECOVERY_COMMAND_GET_LOG = 9,
};

enum recovery_status {
    RECOVERY_STATUS_OK = 0,
    RECOVERY_STATUS_BAD_MAGIC = 1,
    RECOVERY_STATUS_BAD_VERSION = 2,
    RECOVERY_STATUS_BAD_COMMAND = 3,
    RECOVERY_STATUS_BAD_LENGTH = 4,
    RECOVERY_STATUS_BAD_HEADER_CRC = 5,
    RECOVERY_STATUS_BAD_PAYLOAD_CRC = 6,
    RECOVERY_STATUS_BAD_SESSION = 7,
    RECOVERY_STATUS_BAD_SEQUENCE = 8,
    RECOVERY_STATUS_BAD_SLOT = 9,
    RECOVERY_STATUS_RANGE_DENIED = 10,
};

typedef struct __attribute__((packed)) recovery_packet {
    uint32_t magic;
    uint8_t version;
    uint8_t command;
    uint16_t flags;
    uint32_t session;
    uint32_t sequence;
    uint32_t offset;
    uint16_t length;
    uint16_t status;
    uint32_t payload_crc32;
    uint32_t header_crc32;
    uint8_t payload[RECOVERY_PAYLOAD_SIZE];
} recovery_packet_t;

_Static_assert(sizeof(recovery_packet_t) == RECOVERY_PACKET_SIZE,
               "recovery packet layout changed");

uint16_t recovery_packet_validate(const recovery_packet_t *packet,
                                  uint32_t expected_session,
                                  uint32_t expected_sequence,
                                  int require_session);
void recovery_packet_finalize(recovery_packet_t *packet);
int recovery_command_is_mutating(uint8_t command);
uint16_t recovery_resolve_range(uint8_t slot,
                                uint32_t offset,
                                uint32_t length,
                                uint32_t *flash_address);

#endif

