#ifndef NCR2_EFFECT_RUNTIME_H
#define NCR2_EFFECT_RUNTIME_H

#include <stddef.h>
#include <stdint.h>

#define EFFECT_RUNTIME_ABI_VERSION UINT16_C(1)
#define EFFECT_RUNTIME_MAX_CHANNELS UINT8_C(8)
#define EFFECT_RUNTIME_MAX_CONTEXT_ALIGNMENT UINT32_C(256)
#define EFFECT_VENDOR_OPEN UINT32_C(0x4F50454E)

typedef struct effect_key {
    uint32_t vendor_id;
    uint32_t effect_id;
} effect_key_t;

typedef struct effect_audio_block {
    float *channels[EFFECT_RUNTIME_MAX_CHANNELS];
    uint32_t frame_count;
    uint8_t channel_count;
} effect_audio_block_t;

enum effect_runtime_status {
    EFFECT_RUNTIME_OK = 0,
    EFFECT_RUNTIME_INVALID_ARGUMENT = 1,
    EFFECT_RUNTIME_INVALID_DESCRIPTOR = 2,
    EFFECT_RUNTIME_DUPLICATE_EFFECT = 3,
    EFFECT_RUNTIME_EFFECT_NOT_FOUND = 4,
    EFFECT_RUNTIME_CHAIN_FULL = 5,
    EFFECT_RUNTIME_ARENA_FULL = 6,
    EFFECT_RUNTIME_CALLBACK_FAILED = 7,
    EFFECT_RUNTIME_PARAMETER_NOT_FOUND = 8,
};

typedef uint16_t (*effect_initialize_fn)(
    void *context,
    uint32_t sample_rate,
    uint32_t maximum_block_frames);
typedef void (*effect_reset_fn)(void *context);
typedef uint16_t (*effect_process_fn)(
    void *context,
    effect_audio_block_t *block);
typedef uint16_t (*effect_set_parameter_fn)(
    void *context,
    uint32_t parameter_id,
    float value);

typedef struct effect_descriptor {
    effect_key_t key;
    const char *name;
    uint16_t abi_version;
    uint16_t parameter_count;
    uint32_t context_size;
    uint32_t context_alignment;
    effect_initialize_fn initialize;
    effect_reset_fn reset;
    effect_process_fn process;
    effect_set_parameter_fn set_parameter;
} effect_descriptor_t;

typedef struct effect_registry {
    const effect_descriptor_t *const *effects;
    size_t count;
} effect_registry_t;

typedef struct effect_instance {
    const effect_descriptor_t *descriptor;
    void *context;
} effect_instance_t;

/*
 * Storage is supplied by the application. The runtime imposes no fixed
 * effect-count limit and performs no dynamic allocation.
 */
typedef struct effect_chain {
    const effect_registry_t *registry;
    effect_instance_t *instances;
    size_t count;
    size_t capacity;
    uint8_t *arena;
    size_t arena_size;
    size_t arena_used;
    uint32_t sample_rate;
    uint32_t maximum_block_frames;
} effect_chain_t;

int effect_key_equal(effect_key_t left, effect_key_t right);
uint16_t effect_registry_validate(
    const effect_registry_t *registry);
const effect_descriptor_t *effect_registry_find(
    const effect_registry_t *registry,
    effect_key_t key);
uint16_t effect_chain_initialize(
    effect_chain_t *chain,
    const effect_registry_t *registry,
    effect_instance_t *instance_storage,
    size_t instance_capacity,
    void *context_arena,
    size_t context_arena_size,
    uint32_t sample_rate,
    uint32_t maximum_block_frames);
uint16_t effect_chain_add(
    effect_chain_t *chain,
    effect_key_t key,
    size_t *instance_index);
void effect_chain_reset(effect_chain_t *chain);
uint16_t effect_chain_process(
    effect_chain_t *chain,
    effect_audio_block_t *block);
uint16_t effect_chain_set_parameter(
    effect_chain_t *chain,
    size_t instance_index,
    uint32_t parameter_id,
    float value);

#endif
