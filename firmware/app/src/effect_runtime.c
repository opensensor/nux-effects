#include "effect_runtime.h"

#include <stdint.h>

static int alignment_is_valid(uint32_t alignment)
{
    return alignment != UINT32_C(0) &&
           alignment <=
               EFFECT_RUNTIME_MAX_CONTEXT_ALIGNMENT &&
           (alignment & (alignment - UINT32_C(1))) ==
               UINT32_C(0);
}

static int descriptor_is_valid(
    const effect_descriptor_t *descriptor)
{
    if (descriptor == NULL ||
        descriptor->name == NULL ||
        descriptor->abi_version !=
            EFFECT_RUNTIME_ABI_VERSION ||
        !alignment_is_valid(descriptor->context_alignment) ||
        descriptor->process == NULL) {
        return 0;
    }
    if (descriptor->context_size == UINT32_C(0) &&
        descriptor->context_alignment != UINT32_C(1)) {
        return 0;
    }
    if (descriptor->parameter_count != UINT16_C(0) &&
        (descriptor->parameters == NULL ||
         descriptor->set_parameter == NULL)) {
        return 0;
    }
    for (uint16_t index = UINT16_C(0);
         index < descriptor->parameter_count;
         ++index) {
        const effect_parameter_descriptor_t *parameter =
            &descriptor->parameters[index];

        if (parameter->name == NULL ||
            parameter->unit == NULL ||
            parameter->minimum != parameter->minimum ||
            parameter->maximum != parameter->maximum ||
            parameter->default_value !=
                parameter->default_value ||
            parameter->minimum > parameter->maximum ||
            parameter->default_value < parameter->minimum ||
            parameter->default_value > parameter->maximum) {
            return 0;
        }
        for (uint16_t previous = UINT16_C(0);
             previous < index;
             ++previous) {
            if (parameter->parameter_id ==
                descriptor->parameters[previous].parameter_id) {
                return 0;
            }
        }
    }
    return 1;
}

static void bytes_zero(void *memory, size_t length)
{
    uint8_t *bytes = (uint8_t *)memory;

    for (size_t index = 0U; index < length; ++index) {
        bytes[index] = UINT8_C(0);
    }
}

static int block_is_valid(const effect_audio_block_t *block)
{
    if (block == NULL ||
        block->channel_count == UINT8_C(0) ||
        block->channel_count >
            EFFECT_RUNTIME_MAX_CHANNELS) {
        return 0;
    }
    for (uint8_t channel = UINT8_C(0);
         channel < block->channel_count;
         ++channel) {
        if (block->channels[channel] == NULL) {
            return 0;
        }
    }
    return 1;
}

int effect_key_equal(effect_key_t left, effect_key_t right)
{
    return left.vendor_id == right.vendor_id &&
           left.effect_id == right.effect_id;
}

uint16_t effect_registry_validate(
    const effect_registry_t *registry)
{
    if (registry == NULL ||
        (registry->count != 0U &&
         registry->effects == NULL)) {
        return EFFECT_RUNTIME_INVALID_ARGUMENT;
    }

    for (size_t index = 0U;
         index < registry->count;
         ++index) {
        if (!descriptor_is_valid(registry->effects[index])) {
            return EFFECT_RUNTIME_INVALID_DESCRIPTOR;
        }
        for (size_t previous = 0U;
             previous < index;
             ++previous) {
            if (effect_key_equal(
                    registry->effects[index]->key,
                    registry->effects[previous]->key)) {
                return EFFECT_RUNTIME_DUPLICATE_EFFECT;
            }
        }
    }
    return EFFECT_RUNTIME_OK;
}

const effect_descriptor_t *effect_registry_find(
    const effect_registry_t *registry,
    effect_key_t key)
{
    if (registry == NULL || registry->effects == NULL) {
        return NULL;
    }
    for (size_t index = 0U;
         index < registry->count;
         ++index) {
        if (registry->effects[index] != NULL &&
            effect_key_equal(
                registry->effects[index]->key,
                key)) {
            return registry->effects[index];
        }
    }
    return NULL;
}

const effect_parameter_descriptor_t *effect_parameter_find(
    const effect_descriptor_t *effect,
    uint32_t parameter_id)
{
    if (effect == NULL ||
        effect->parameters == NULL) {
        return NULL;
    }

    for (uint16_t index = UINT16_C(0);
         index < effect->parameter_count;
         ++index) {
        if (effect->parameters[index].parameter_id ==
            parameter_id) {
            return &effect->parameters[index];
        }
    }
    return NULL;
}

uint16_t effect_chain_initialize(
    effect_chain_t *chain,
    const effect_registry_t *registry,
    effect_instance_t *instance_storage,
    size_t instance_capacity,
    void *context_arena,
    size_t context_arena_size,
    uint32_t sample_rate,
    uint32_t maximum_block_frames)
{
    uint16_t status;

    if (chain == NULL ||
        instance_storage == NULL ||
        instance_capacity == 0U ||
        context_arena == NULL ||
        context_arena_size == 0U ||
        sample_rate == UINT32_C(0) ||
        maximum_block_frames == UINT32_C(0)) {
        return EFFECT_RUNTIME_INVALID_ARGUMENT;
    }
    status = effect_registry_validate(registry);
    if (status != EFFECT_RUNTIME_OK) {
        return status;
    }

    chain->registry = registry;
    chain->instances = instance_storage;
    chain->count = 0U;
    chain->capacity = instance_capacity;
    chain->arena = (uint8_t *)context_arena;
    chain->arena_size = context_arena_size;
    chain->arena_used = 0U;
    chain->sample_rate = sample_rate;
    chain->maximum_block_frames = maximum_block_frames;
    return EFFECT_RUNTIME_OK;
}

uint16_t effect_chain_add(
    effect_chain_t *chain,
    effect_key_t key,
    size_t *instance_index)
{
    const effect_descriptor_t *descriptor;
    uintptr_t base;
    uintptr_t aligned;
    size_t padding;
    size_t required;
    void *context;

    if (chain == NULL ||
        chain->instances == NULL ||
        chain->arena == NULL ||
        chain->arena_used > chain->arena_size) {
        return EFFECT_RUNTIME_INVALID_ARGUMENT;
    }
    if (chain->count >= chain->capacity) {
        return EFFECT_RUNTIME_CHAIN_FULL;
    }

    descriptor =
        effect_registry_find(chain->registry, key);
    if (descriptor == NULL) {
        return EFFECT_RUNTIME_EFFECT_NOT_FOUND;
    }

    base =
        (uintptr_t)&chain->arena[chain->arena_used];
    aligned =
        (base + descriptor->context_alignment -
         UINT32_C(1)) &
        ~(uintptr_t)(
            descriptor->context_alignment - UINT32_C(1));
    padding = (size_t)(aligned - base);
    if (padding > chain->arena_size - chain->arena_used ||
        descriptor->context_size >
            chain->arena_size - chain->arena_used - padding) {
        return EFFECT_RUNTIME_ARENA_FULL;
    }
    required = padding + descriptor->context_size;
    context = (void *)aligned;
    bytes_zero(context, descriptor->context_size);

    if (descriptor->initialize != NULL &&
        descriptor->initialize(
            context,
            chain->sample_rate,
            chain->maximum_block_frames) !=
            EFFECT_RUNTIME_OK) {
        return EFFECT_RUNTIME_CALLBACK_FAILED;
    }
    for (uint16_t parameter = UINT16_C(0);
         parameter < descriptor->parameter_count;
         ++parameter) {
        if (descriptor->set_parameter(
                context,
                descriptor->parameters[parameter].parameter_id,
                descriptor->parameters[parameter].default_value) !=
            EFFECT_RUNTIME_OK) {
            return EFFECT_RUNTIME_CALLBACK_FAILED;
        }
    }

    chain->instances[chain->count].descriptor = descriptor;
    chain->instances[chain->count].context = context;
    if (instance_index != NULL) {
        *instance_index = chain->count;
    }
    ++chain->count;
    chain->arena_used += required;
    return EFFECT_RUNTIME_OK;
}

void effect_chain_reset(effect_chain_t *chain)
{
    if (chain == NULL || chain->instances == NULL) {
        return;
    }
    for (size_t index = 0U; index < chain->count; ++index) {
        if (chain->instances[index].descriptor != NULL &&
            chain->instances[index].descriptor->reset != NULL) {
            chain->instances[index].descriptor->reset(
                chain->instances[index].context);
        }
    }
}

void effect_chain_clear(effect_chain_t *chain)
{
    if (chain == NULL || chain->instances == NULL) {
        return;
    }

    effect_chain_reset(chain);
    for (size_t index = 0U; index < chain->count; ++index) {
        chain->instances[index].descriptor = NULL;
        chain->instances[index].context = NULL;
    }
    chain->count = 0U;
    chain->arena_used = 0U;
}

uint16_t effect_chain_process(
    effect_chain_t *chain,
    effect_audio_block_t *block)
{
    if (chain == NULL ||
        chain->instances == NULL ||
        !block_is_valid(block) ||
        block->frame_count >
            chain->maximum_block_frames) {
        return EFFECT_RUNTIME_INVALID_ARGUMENT;
    }

    for (size_t index = 0U; index < chain->count; ++index) {
        const effect_instance_t *instance =
            &chain->instances[index];
        if (instance->descriptor == NULL ||
            instance->descriptor->process == NULL ||
            instance->descriptor->process(
                instance->context,
                block) != EFFECT_RUNTIME_OK) {
            return EFFECT_RUNTIME_CALLBACK_FAILED;
        }
    }
    return EFFECT_RUNTIME_OK;
}

uint16_t effect_chain_set_parameter(
    effect_chain_t *chain,
    size_t instance_index,
    uint32_t parameter_id,
    float value)
{
    effect_instance_t *instance;
    const effect_parameter_descriptor_t *parameter;

    if (chain == NULL ||
        chain->instances == NULL ||
        instance_index >= chain->count) {
        return EFFECT_RUNTIME_INVALID_ARGUMENT;
    }
    instance = &chain->instances[instance_index];
    if (instance->descriptor == NULL ||
        instance->descriptor->set_parameter == NULL) {
        return EFFECT_RUNTIME_PARAMETER_NOT_FOUND;
    }
    parameter =
        effect_parameter_find(
            instance->descriptor,
            parameter_id);
    if (parameter == NULL) {
        return EFFECT_RUNTIME_PARAMETER_NOT_FOUND;
    }
    if (value != value ||
        value < parameter->minimum ||
        value > parameter->maximum) {
        return EFFECT_RUNTIME_PARAMETER_OUT_OF_RANGE;
    }
    if (instance->descriptor->set_parameter(
            instance->context,
            parameter_id,
            value) != EFFECT_RUNTIME_OK) {
        return EFFECT_RUNTIME_CALLBACK_FAILED;
    }
    return EFFECT_RUNTIME_OK;
}
