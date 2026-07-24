#ifndef NCR2_BOOT_STATE_H
#define NCR2_BOOT_STATE_H

#include <stddef.h>
#include <stdint.h>

#define BOOT_RECORD_MAGIC UINT32_C(0x31545342)
#define BOOT_RECORD_FORMAT_VERSION UINT16_C(1)
#define BOOT_RECORD_SIZE 32U
#define BOOT_RECORD_SECTOR_SIZE 4096U
#define BOOT_RECORDS_PER_SECTOR \
    (BOOT_RECORD_SECTOR_SIZE / BOOT_RECORD_SIZE)

#define BOOT_SLOT_A UINT8_C(0)
#define BOOT_SLOT_B UINT8_C(1)
#define BOOT_SLOT_NONE UINT8_C(0xFF)
#define BOOT_DEFAULT_MAX_TRIALS UINT8_C(3)

typedef struct __attribute__((packed)) boot_record {
    uint32_t magic;
    uint16_t format_version;
    uint16_t record_size;
    uint32_t sequence;
    uint8_t confirmed_slot;
    uint8_t pending_slot;
    uint8_t trial_count;
    uint8_t max_trials;
    uint32_t flags;
    uint8_t reserved[8];
    uint32_t crc32;
} boot_record_t;

typedef struct boot_state {
    uint32_t sequence;
    uint8_t confirmed_slot;
    uint8_t pending_slot;
    uint8_t trial_count;
    uint8_t max_trials;
    uint32_t flags;
    int found_record;
} boot_state_t;

_Static_assert(sizeof(boot_record_t) == BOOT_RECORD_SIZE,
               "boot record layout changed");

void boot_state_default(boot_state_t *state);
int boot_record_decode(const boot_record_t *record, boot_state_t *state);
void boot_record_encode(const boot_state_t *state, boot_record_t *record);
void boot_state_scan(const void *sector_a,
                     const void *sector_b,
                     boot_state_t *state);
uint8_t boot_state_selected_slot(const boot_state_t *state);
void boot_state_begin_update(boot_state_t *state, uint8_t slot);
void boot_state_record_trial(boot_state_t *state);
void boot_state_confirm_pending(boot_state_t *state);
void boot_state_rollback(boot_state_t *state);
size_t boot_state_find_append_offset(const void *sector);

#endif

