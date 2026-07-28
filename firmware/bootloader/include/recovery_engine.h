#ifndef NCR2_RECOVERY_ENGINE_H
#define NCR2_RECOVERY_ENGINE_H

#include <stdint.h>

#include "boot_state.h"
#include "recovery_protocol.h"

#define RECOVERY_CAPABILITY_AB_SLOTS UINT32_C(0x00000001)
#define RECOVERY_CAPABILITY_SHA256 UINT32_C(0x00000002)
#define RECOVERY_CAPABILITY_READBACK UINT32_C(0x00000004)
#define RECOVERY_CAPABILITY_RETRY_CACHE UINT32_C(0x00000008)
#define RECOVERY_CAPABILITY_BOUNDED_ERASE UINT32_C(0x00000010)
#define RECOVERY_CAPABILITY_FULL_FLASH_RAM UINT32_C(0x00000020)
#define RECOVERY_CAPABILITY_PROGRESSIVE_FULL_ERASE UINT32_C(0x00000040)
/*
 * Reserved until the NOR page-program backend passes physical full-image
 * write and readback validation. The current firmware intentionally does not
 * advertise this bit, so hosts fail closed before a destructive erase.
 */
#define RECOVERY_CAPABILITY_VERIFIED_FULL_PROGRAM UINT32_C(0x00000080)
/*
 * Set only when the backend can sample the four front-panel controls. The
 * Type control is a stepped ladder whose detent voltages have never been
 * measured, so the host needs a non-mutating way to read them before any
 * firmware can quantise the channel correctly.
 */
#define RECOVERY_CAPABILITY_KNOB_SAMPLE UINT32_C(0x00000100)
#define RECOVERY_FULL_FLASH_ERASE_CHUNK_SIZE UINT32_C(0x00010000)

#define RECOVERY_KNOB_SAMPLE_MAGIC UINT32_C(0x424F4E4B)
#define RECOVERY_KNOB_COUNT 4U

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
    int (*get_log)(void *context,
                   void *destination,
                   uint32_t capacity);
    int (*read_knobs)(void *context,
                      void *destination,
                      uint32_t capacity);
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

/*
 * One non-mutating front-panel capture. The selector burst min/max bound the
 * electrical noise at a resting detent, which is exactly the number needed to
 * choose a movement threshold that cannot be tripped by a stationary knob.
 * sample_index increments per capture so a host can tell a fresh reading from
 * a stale HID input report left in the queue by a previous exchange.
 */
typedef struct __attribute__((packed)) recovery_knob_sample {
    uint32_t magic;
    uint16_t value[RECOVERY_KNOB_COUNT];
    uint16_t selector_min;
    uint16_t selector_max;
    uint8_t channel[RECOVERY_KNOB_COUNT];
    uint8_t burst;
    uint8_t valid;
    uint8_t adc_bits;
    uint8_t reserved;
    uint32_t sample_index;
    uint32_t reserved2;
} recovery_knob_sample_t;

_Static_assert(sizeof(recovery_knob_sample_t) == RECOVERY_PAYLOAD_SIZE,
               "knob sample must fill one payload");

typedef struct recovery_engine {
    recovery_backend_t backend;
    boot_state_t boot_state;
    recovery_packet_t previous_request;
    recovery_packet_t previous_response;
    uint32_t session;
    uint32_t expected_sequence;
    uint32_t next_write_offset;
    uint32_t full_erase_offset;
    uint32_t expected_image_size;
    uint32_t session_seed;
    uint8_t expected_image_sha256[32];
    uint8_t active_slot;
    uint8_t target_slot;
    uint8_t phase;
    uint8_t has_previous;
    uint8_t full_flash_enabled;
    uint8_t full_flash_session;
} recovery_engine_t;

_Static_assert(sizeof(recovery_info_t) == RECOVERY_PAYLOAD_SIZE,
               "recovery info must fill one payload");

void recovery_engine_init(recovery_engine_t *engine,
                          const recovery_backend_t *backend,
                          const boot_state_t *boot_state,
                          uint8_t active_slot,
                          uint32_t session_seed);
void recovery_engine_enable_full_flash(recovery_engine_t *engine);
void recovery_engine_process(recovery_engine_t *engine,
                             const recovery_packet_t *request,
                             recovery_packet_t *response);

#endif
