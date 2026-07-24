#include "effects_basic.h"

typedef struct gain_context {
    float gain;
} gain_context_t;

typedef struct soft_clip_context {
    float drive;
    float level;
    float mix;
} soft_clip_context_t;

static uint16_t gain_initialize(
    void *opaque,
    uint32_t sample_rate,
    uint32_t maximum_block_frames)
{
    gain_context_t *context = (gain_context_t *)opaque;

    (void)sample_rate;
    (void)maximum_block_frames;
    context->gain = 1.0F;
    return EFFECT_RUNTIME_OK;
}

static uint16_t gain_process(
    void *opaque,
    effect_audio_block_t *block)
{
    const gain_context_t *context =
        (const gain_context_t *)opaque;

    for (uint8_t channel = UINT8_C(0);
         channel < block->channel_count;
         ++channel) {
        for (uint32_t frame = UINT32_C(0);
             frame < block->frame_count;
             ++frame) {
            block->channels[channel][frame] *=
                context->gain;
        }
    }
    return EFFECT_RUNTIME_OK;
}

static uint16_t gain_set_parameter(
    void *opaque,
    uint32_t parameter_id,
    float value)
{
    gain_context_t *context = (gain_context_t *)opaque;

    if (parameter_id != EFFECT_GAIN_PARAMETER_GAIN) {
        return EFFECT_RUNTIME_PARAMETER_NOT_FOUND;
    }
    context->gain = value;
    return EFFECT_RUNTIME_OK;
}

static uint16_t soft_clip_initialize(
    void *opaque,
    uint32_t sample_rate,
    uint32_t maximum_block_frames)
{
    soft_clip_context_t *context =
        (soft_clip_context_t *)opaque;

    (void)sample_rate;
    (void)maximum_block_frames;
    context->drive = 1.0F;
    context->level = 1.0F;
    context->mix = 1.0F;
    return EFFECT_RUNTIME_OK;
}

static float absolute_value(float value)
{
    return value < 0.0F ? -value : value;
}

static uint16_t soft_clip_process(
    void *opaque,
    effect_audio_block_t *block)
{
    const soft_clip_context_t *context =
        (const soft_clip_context_t *)opaque;

    for (uint8_t channel = UINT8_C(0);
         channel < block->channel_count;
         ++channel) {
        for (uint32_t frame = UINT32_C(0);
             frame < block->frame_count;
             ++frame) {
            float dry = block->channels[channel][frame];
            float driven = dry * context->drive;
            float wet =
                driven /
                (1.0F + absolute_value(driven));

            wet *= context->level;
            block->channels[channel][frame] =
                dry + (wet - dry) * context->mix;
        }
    }
    return EFFECT_RUNTIME_OK;
}

static uint16_t soft_clip_set_parameter(
    void *opaque,
    uint32_t parameter_id,
    float value)
{
    soft_clip_context_t *context =
        (soft_clip_context_t *)opaque;

    if (parameter_id ==
        EFFECT_SOFT_CLIP_PARAMETER_DRIVE) {
        context->drive = value;
    } else if (parameter_id ==
               EFFECT_SOFT_CLIP_PARAMETER_LEVEL) {
        context->level = value;
    } else if (parameter_id ==
               EFFECT_SOFT_CLIP_PARAMETER_MIX) {
        context->mix = value;
    } else {
        return EFFECT_RUNTIME_PARAMETER_NOT_FOUND;
    }
    return EFFECT_RUNTIME_OK;
}

static const effect_parameter_descriptor_t gain_parameters[] = {
    {
        EFFECT_GAIN_PARAMETER_GAIN,
        "Gain",
        "linear",
        0.0F,
        4.0F,
        1.0F,
    },
};

static const effect_parameter_descriptor_t
soft_clip_parameters[] = {
    {
        EFFECT_SOFT_CLIP_PARAMETER_DRIVE,
        "Drive",
        "linear",
        1.0F,
        32.0F,
        1.0F,
    },
    {
        EFFECT_SOFT_CLIP_PARAMETER_LEVEL,
        "Level",
        "linear",
        0.0F,
        4.0F,
        1.0F,
    },
    {
        EFFECT_SOFT_CLIP_PARAMETER_MIX,
        "Mix",
        "ratio",
        0.0F,
        1.0F,
        1.0F,
    },
};

const effect_descriptor_t ncr2_effect_basic_gain = {
    .key = {
        EFFECT_VENDOR_OPEN,
        EFFECT_OPEN_BASIC_GAIN_ID,
    },
    .name = "Basic Gain",
    .abi_version = EFFECT_RUNTIME_ABI_VERSION,
    .parameter_count =
        (uint16_t)(
            sizeof(gain_parameters) /
            sizeof(gain_parameters[0])),
    .parameters = gain_parameters,
    .context_size = sizeof(gain_context_t),
    .context_alignment = _Alignof(gain_context_t),
    .initialize = gain_initialize,
    .reset = NULL,
    .process = gain_process,
    .set_parameter = gain_set_parameter,
};

const effect_descriptor_t ncr2_effect_basic_soft_clip = {
    .key = {
        EFFECT_VENDOR_OPEN,
        EFFECT_OPEN_BASIC_SOFT_CLIP_ID,
    },
    .name = "Basic Soft Clip",
    .abi_version = EFFECT_RUNTIME_ABI_VERSION,
    .parameter_count =
        (uint16_t)(
            sizeof(soft_clip_parameters) /
            sizeof(soft_clip_parameters[0])),
    .parameters = soft_clip_parameters,
    .context_size = sizeof(soft_clip_context_t),
    .context_alignment = _Alignof(soft_clip_context_t),
    .initialize = soft_clip_initialize,
    .reset = NULL,
    .process = soft_clip_process,
    .set_parameter = soft_clip_set_parameter,
};

static const effect_descriptor_t *const
basic_effect_descriptors[] = {
    &ncr2_effect_basic_gain,
    &ncr2_effect_basic_soft_clip,
};

const effect_registry_t ncr2_basic_effect_registry = {
    .effects = basic_effect_descriptors,
    .count =
        sizeof(basic_effect_descriptors) /
        sizeof(basic_effect_descriptors[0]),
};
