#ifndef NCR2_RECOVERY_ENGINE_H
#define NCR2_RECOVERY_ENGINE_H

#include <stdint.h>

#include "boot_state.h"
#include "recovery_protocol.h"

#define RECOVERY_CAPABILITY_AB_SLOTS UINT32_C(0x00000001)
#define RECOVERY_CAPABILITY_SHA256 UINT32_C(0x00000002)
#define RECOVERY_CAPABILITY_READBACK UINT32_C(0x00000004)
#define RECOVERY_CAPABILITY_RETRY_CACHE UINT32_C(0x00000008)

enum recovery_update_phase {
    RECOVERY_PHASE_IDLE = 0,
    RECOVERY_PHASE_BEGUN = 1,
    RECOVERY_PHASE_ERASED = 2,
    RECOVERY_PHASE_WRITING = 3,
    RECOVERY_PHASE_FINALIZED = 4,
    RECOVERY_PHASE_PENDING = 5,
};

typedef struct recovery_backend {
    void *context;
    int (*read)(void *context,
                uint32_t address,
                void *destination,
                uint32_t length);
    int (*erase)(void *context, uint32_t address, uint32_t length);
    int (*program)(void *context,
                   uint32_t address,
                   const void *source,
                   uint32_t length);
    int (*store_boot_state)(void *context, const boot_state_t *state);
    void (*request_reboot)(void *context);
} recovery_backend_t;

typedef struct __attribute__((packed)) recovery_info {
    uint32_t flash_size;
    uint32_t slot_size;
    uint32_t slot_a_offset;
    uint32_t slot_b_offset;
    uint32_t manifest_size;
    uint8_t confirmed_slot;
    uint8_t pending_slot;
    uint8_t selected_slot;
    uint8_t update_phase;
    uint32_t capabilities;
    uint32_t max_chunk_size;
} recovery_info_t;

typedef struct recovery_engine {
    recovery_backend_t backend;
    boot_state_t boot_state;
    recovery_packet_t previous_request;
    recovery_packet_t previous_response;
    uint32_t session;
    uint32_t expected_sequence;
    uint32_t next_write_offset;
    uint32_t session_seed;
    uint8_t active_slot;
    uint8_t target_slot;
    uint8_t phase;
    uint8_t has_previous;
} recovery_engine_t;

_Static_assert(sizeof(recovery_info_t) == RECOVERY_PAYLOAD_SIZE,
               "recovery info must fill one payload");

void recovery_engine_init(recovery_engine_t *engine,
                          const recovery_backend_t *backend,
                          const boot_state_t *boot_state,
                          uint8_t active_slot,
                          uint32_t session_seed);
void recovery_engine_process(recovery_engine_t *engine,
                             const recovery_packet_t *request,
                             recovery_packet_t *response);

#endif
