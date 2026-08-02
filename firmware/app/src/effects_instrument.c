#include "effects_instrument.h"

#include <stddef.h>
#include <stdint.h>

#define INSTRUMENT_TRACKER_SAMPLES UINT32_C(512)
#define INSTRUMENT_TRACKER_MASK \
    (INSTRUMENT_TRACKER_SAMPLES - UINT32_C(1))
#define INSTRUMENT_ANALYSIS_WINDOW UINT32_C(192)
#define INSTRUMENT_MIN_LAG UINT32_C(9)
#define INSTRUMENT_MAX_LAG UINT32_C(180)
#define INSTRUMENT_ANALYSIS_INTERVAL UINT32_C(32)
#define INSTRUMENT_DECIMATION UINT32_C(4)

enum instrument_voice {
    INSTRUMENT_BOWED_ENSEMBLE = 0,
    INSTRUMENT_CELLO = 1,
    INSTRUMENT_VIOLIN = 2,
    INSTRUMENT_TONEWHEEL_ORGAN = 3,
    INSTRUMENT_CLARINET = 4,
    INSTRUMENT_SYNTH_BRASS = 5,
    INSTRUMENT_SYNTH_BASS = 6,
    INSTRUMENT_BELL_MARIMBA = 7,
};

typedef struct instrument_channel_state {
    float phase_a;
    float phase_b;
    float phase_c;
    float tone;
    float auxiliary;
} instrument_channel_state_t;

typedef struct instrument_context {
    uint32_t sample_rate;
    uint32_t voice;
    float articulation;
    float character;
    float mix;
    float sensitivity;
    float input_envelope;
    float synth_envelope;
    float tracked_frequency;
    float tracking_confidence;
    float decimation_filter;
    float tracker[INSTRUMENT_TRACKER_SAMPLES];
    uint32_t tracker_write;
    uint32_t tracker_count;
    uint32_t decimation_count;
    uint32_t decimation_factor;
    uint32_t analysis_count;
    instrument_channel_state_t channels[EFFECT_RUNTIME_MAX_CHANNELS];
} instrument_context_t;

static float instrument_absolute(float value)
{
    return value < 0.0F ? -value : value;
}

static float instrument_clamp(float value, float lower, float upper)
{
    if (value < lower) return lower;
    if (value > upper) return upper;
    return value;
}

static float instrument_wrap(float phase)
{
    while (phase >= 1.0F) phase -= 1.0F;
    while (phase < 0.0F) phase += 1.0F;
    return phase;
}

static float instrument_saw(float phase)
{
    return phase * 2.0F - 1.0F;
}

static float instrument_square(float phase)
{
    return phase < 0.5F ? 1.0F : -1.0F;
}

/* A continuous parabolic sine approximation avoids a large wavetable and
 * libm while sounding much less synthetic than a triangle oscillator. */
static float instrument_sine(float phase)
{
    const float bipolar = phase * 2.0F - 1.0F;
    return 4.0F * bipolar * (1.0F - instrument_absolute(bipolar));
}

static float instrument_tracker_sample(
    const instrument_context_t *context,
    uint32_t offset)
{
    const uint32_t index =
        (context->tracker_write + INSTRUMENT_TRACKER_SAMPLES -
         UINT32_C(1) - offset) & INSTRUMENT_TRACKER_MASK;
    return context->tracker[index];
}

static void instrument_analyze_pitch(instrument_context_t *context)
{
    float energy = 0.0F;
    float best_difference = 1.0e30F;
    float best_score = 1.0e30F;
    float previous_difference = 1.0e30F;
    float two_back_difference = 1.0e30F;
    uint32_t best_lag = UINT32_C(0);
    float best_lag_fraction = 0.0F;
    float predicted_lag = 0.0F;

    if (context->tracker_count <
        INSTRUMENT_ANALYSIS_WINDOW + INSTRUMENT_MAX_LAG) {
        context->tracking_confidence = 0.0F;
        return;
    }
    for (uint32_t sample = UINT32_C(0);
         sample < INSTRUMENT_ANALYSIS_WINDOW;
         ++sample) {
        energy += instrument_absolute(
            instrument_tracker_sample(context, sample));
    }
    if (energy < context->sensitivity *
                 (float)INSTRUMENT_ANALYSIS_WINDOW) {
        context->tracking_confidence = 0.0F;
        return;
    }
    if (context->tracked_frequency > 0.0F) {
        predicted_lag =
            (float)context->sample_rate /
            ((float)context->decimation_factor *
             context->tracked_frequency);
    }

    for (uint32_t lag = INSTRUMENT_MIN_LAG;
         lag <= INSTRUMENT_MAX_LAG;
         ++lag) {
        float difference = 0.0F;

        for (uint32_t sample = UINT32_C(0);
             sample < INSTRUMENT_ANALYSIS_WINDOW;
             ++sample) {
            difference += instrument_absolute(
                instrument_tracker_sample(context, sample) -
                instrument_tracker_sample(context, sample + lag));
        }
        /* Prefer the first strong periodic match and gently resist octave
         * jumps away from the previously accepted period. */
        float score = difference *
            (1.0F + (float)lag * 0.0015F);
        if (predicted_lag > 0.0F) {
            score += instrument_absolute(
                (float)lag - predicted_lag) * energy * 0.0008F;
        }
        if (score < best_score) {
            best_score = score;
            best_difference = difference;
            best_lag = lag;
            best_lag_fraction = (float)lag;
        }
        /* AMDF also reaches deep minima at two and three periods. Selecting
         * the first strong local valley avoids reporting a clean A3 as A2
         * merely because the doubled integer lag happens to align exactly. */
        if (lag > INSTRUMENT_MIN_LAG + UINT32_C(1) &&
            previous_difference < two_back_difference &&
            previous_difference <= difference &&
            previous_difference < energy * 0.56F) {
            const float curvature =
                two_back_difference - 2.0F * previous_difference +
                difference;
            float correction = 0.0F;

            if (instrument_absolute(curvature) > 0.000001F) {
                correction = 0.5F *
                    (two_back_difference - difference) / curvature;
                correction = instrument_clamp(correction, -0.5F, 0.5F);
            }
            best_difference = previous_difference;
            best_lag = lag - UINT32_C(1);
            best_lag_fraction = (float)best_lag + correction;
            break;
        }
        two_back_difference = previous_difference;
        previous_difference = difference;
    }

    if (best_lag == UINT32_C(0)) {
        context->tracking_confidence = 0.0F;
        return;
    }
    const float confidence = instrument_clamp(
        1.0F - best_difference / (2.0F * energy + 0.000001F),
        0.0F,
        1.0F);
    const float candidate =
        (float)context->sample_rate /
        ((float)context->decimation_factor * best_lag_fraction);
    context->tracking_confidence = confidence;
    if (confidence >= 0.58F && candidate >= 65.0F && candidate <= 1400.0F) {
        if (context->tracked_frequency <= 0.0F) {
            context->tracked_frequency = candidate;
        } else {
            context->tracked_frequency +=
                (candidate - context->tracked_frequency) * 0.18F;
        }
    }
}

static void instrument_track_sample(
    instrument_context_t *context,
    float input)
{
    const float magnitude = instrument_absolute(input);
    const float envelope_coefficient =
        magnitude > context->input_envelope ? 0.04F : 0.00045F;
    context->input_envelope += envelope_coefficient *
        (magnitude - context->input_envelope);

    /* One-pole anti-alias filtering before the deliberately inexpensive 4:1
     * decimator. Guitar fundamentals remain well below its new Nyquist. */
    context->decimation_filter +=
        0.18F * (input - context->decimation_filter);
    ++context->decimation_count;
    if (context->decimation_count < context->decimation_factor) return;
    context->decimation_count = UINT32_C(0);
    context->tracker[context->tracker_write] =
        context->decimation_filter;
    context->tracker_write =
        (context->tracker_write + UINT32_C(1)) &
        INSTRUMENT_TRACKER_MASK;
    if (context->tracker_count < INSTRUMENT_TRACKER_SAMPLES) {
        ++context->tracker_count;
    }
    ++context->analysis_count;
    if (context->analysis_count >= INSTRUMENT_ANALYSIS_INTERVAL) {
        context->analysis_count = UINT32_C(0);
        instrument_analyze_pitch(context);
    }
}

static float instrument_voice_sample(
    instrument_context_t *context,
    instrument_channel_state_t *state,
    float increment)
{
    const float phase_a = state->phase_a;
    const float phase_b = state->phase_b;
    const float phase_c = state->phase_c;
    float raw;
    float tone_coefficient;

    switch (context->voice) {
    case INSTRUMENT_BOWED_ENSEMBLE:
        raw = instrument_saw(phase_a) * 0.48F +
            instrument_saw(phase_b) * 0.34F +
            instrument_sine(instrument_wrap(phase_a * 2.0F)) * 0.18F;
        tone_coefficient = 0.035F + context->character * 0.14F;
        state->phase_b = instrument_wrap(phase_b + increment * 1.006F);
        state->phase_c = instrument_wrap(phase_c + increment * 0.501F);
        break;
    case INSTRUMENT_CELLO:
        raw = instrument_sine(phase_a) * 0.55F +
            instrument_saw(phase_a) * 0.22F +
            instrument_sine(phase_c) * 0.33F;
        tone_coefficient = 0.025F + context->character * 0.09F;
        state->phase_b = instrument_wrap(phase_b + increment * 1.002F);
        state->phase_c = instrument_wrap(phase_c + increment * 0.5F);
        break;
    case INSTRUMENT_VIOLIN:
        raw = instrument_saw(phase_a) * 0.58F +
            instrument_sine(instrument_wrap(phase_a * 3.0F)) * 0.27F +
            instrument_saw(phase_b) * 0.15F;
        tone_coefficient = 0.08F + context->character * 0.24F;
        state->phase_b = instrument_wrap(phase_b + increment * 1.009F);
        state->phase_c = instrument_wrap(phase_c + increment * 2.0F);
        break;
    case INSTRUMENT_TONEWHEEL_ORGAN:
        raw = instrument_sine(phase_a) * 0.58F +
            instrument_sine(instrument_wrap(phase_a * 2.0F)) * 0.27F +
            instrument_sine(instrument_wrap(phase_a * 3.0F)) * 0.15F;
        tone_coefficient = 0.12F + context->character * 0.28F;
        state->phase_b = instrument_wrap(phase_b + increment * 2.0F);
        state->phase_c = instrument_wrap(phase_c + increment * 3.0F);
        break;
    case INSTRUMENT_CLARINET:
        raw = instrument_square(phase_a) * 0.52F +
            instrument_square(instrument_wrap(phase_a * 3.0F)) * 0.20F +
            instrument_sine(phase_b) * 0.28F;
        tone_coefficient = 0.025F + context->character * 0.16F;
        state->phase_b = instrument_wrap(phase_b + increment * 1.003F);
        state->phase_c = instrument_wrap(phase_c + increment * 3.0F);
        break;
    case INSTRUMENT_SYNTH_BRASS:
        raw = instrument_saw(phase_a) * 0.62F +
            instrument_square(phase_b) * 0.23F +
            instrument_sine(instrument_wrap(phase_a * 2.0F)) * 0.15F;
        tone_coefficient = 0.025F + context->character * 0.12F +
            context->input_envelope * 0.45F;
        state->phase_b = instrument_wrap(phase_b + increment * 0.997F);
        state->phase_c = instrument_wrap(phase_c + increment * 2.0F);
        break;
    case INSTRUMENT_SYNTH_BASS:
        raw = instrument_square(phase_c) * 0.48F +
            instrument_sine(phase_a) * 0.34F +
            instrument_saw(phase_a) * 0.18F;
        tone_coefficient = 0.018F + context->character * 0.10F;
        state->phase_b = instrument_wrap(phase_b + increment);
        state->phase_c = instrument_wrap(phase_c + increment * 0.5F);
        break;
    case INSTRUMENT_BELL_MARIMBA:
    default:
        raw = instrument_sine(phase_a) * 0.54F +
            instrument_sine(phase_b) * 0.30F +
            instrument_sine(phase_c) * 0.16F;
        tone_coefficient = 0.10F + context->character * 0.35F;
        state->phase_b = instrument_wrap(phase_b + increment * 2.73F);
        state->phase_c = instrument_wrap(phase_c + increment * 4.11F);
        break;
    }
    state->phase_a = instrument_wrap(phase_a + increment);
    tone_coefficient = instrument_clamp(tone_coefficient, 0.01F, 0.65F);
    state->tone += tone_coefficient * (raw - state->tone);
    return state->tone;
}

static void instrument_clear_state(instrument_context_t *context)
{
    context->input_envelope = 0.0F;
    context->synth_envelope = 0.0F;
    context->tracked_frequency = 0.0F;
    context->tracking_confidence = 0.0F;
    context->decimation_filter = 0.0F;
    context->tracker_write = UINT32_C(0);
    context->tracker_count = UINT32_C(0);
    context->decimation_count = UINT32_C(0);
    context->analysis_count = UINT32_C(0);
    for (uint32_t sample = UINT32_C(0);
         sample < INSTRUMENT_TRACKER_SAMPLES;
         ++sample) {
        context->tracker[sample] = 0.0F;
    }
    for (uint8_t channel = UINT8_C(0);
         channel < EFFECT_RUNTIME_MAX_CHANNELS;
         ++channel) {
        context->channels[channel].phase_a = 0.0F;
        context->channels[channel].phase_b = 0.17F;
        context->channels[channel].phase_c = 0.41F;
        context->channels[channel].tone = 0.0F;
        context->channels[channel].auxiliary = 0.0F;
    }
}

static uint16_t instrument_initialize_voice(
    void *opaque,
    uint32_t sample_rate,
    uint32_t maximum_block_frames,
    uint32_t voice)
{
    instrument_context_t *context = (instrument_context_t *)opaque;

    (void)maximum_block_frames;
    if (sample_rate < UINT32_C(32000) || sample_rate > UINT32_C(96000)) {
        return EFFECT_RUNTIME_INVALID_ARGUMENT;
    }
    context->sample_rate = sample_rate;
    context->decimation_factor = sample_rate > UINT32_C(64000)
        ? UINT32_C(8)
        : INSTRUMENT_DECIMATION;
    context->voice = voice;
    context->articulation = 0.55F;
    context->character = 0.55F;
    context->mix = 0.72F;
    context->sensitivity = 0.012F;
    instrument_clear_state(context);
    return EFFECT_RUNTIME_OK;
}

#define DEFINE_INSTRUMENT_INITIALIZER(name, voice_id) \
    static uint16_t name( \
        void *opaque, \
        uint32_t sample_rate, \
        uint32_t maximum_block_frames) \
    { \
        return instrument_initialize_voice( \
            opaque, sample_rate, maximum_block_frames, voice_id); \
    }

DEFINE_INSTRUMENT_INITIALIZER(
    bowed_ensemble_initialize,
    INSTRUMENT_BOWED_ENSEMBLE)
DEFINE_INSTRUMENT_INITIALIZER(cello_initialize, INSTRUMENT_CELLO)
DEFINE_INSTRUMENT_INITIALIZER(violin_initialize, INSTRUMENT_VIOLIN)
DEFINE_INSTRUMENT_INITIALIZER(organ_initialize, INSTRUMENT_TONEWHEEL_ORGAN)
DEFINE_INSTRUMENT_INITIALIZER(clarinet_initialize, INSTRUMENT_CLARINET)
DEFINE_INSTRUMENT_INITIALIZER(brass_initialize, INSTRUMENT_SYNTH_BRASS)
DEFINE_INSTRUMENT_INITIALIZER(bass_initialize, INSTRUMENT_SYNTH_BASS)
DEFINE_INSTRUMENT_INITIALIZER(bell_initialize, INSTRUMENT_BELL_MARIMBA)

static void instrument_reset(void *opaque)
{
    instrument_clear_state((instrument_context_t *)opaque);
}

static float instrument_attack_coefficient(
    const instrument_context_t *context)
{
    float milliseconds;

    switch (context->voice) {
    case INSTRUMENT_BOWED_ENSEMBLE:
    case INSTRUMENT_CELLO:
    case INSTRUMENT_VIOLIN:
        milliseconds = 18.0F + context->articulation * 180.0F;
        break;
    case INSTRUMENT_SYNTH_BRASS:
        milliseconds = 8.0F + context->articulation * 85.0F;
        break;
    case INSTRUMENT_BELL_MARIMBA:
        milliseconds = 2.0F + context->articulation * 8.0F;
        break;
    default:
        milliseconds = 3.0F + context->articulation * 35.0F;
        break;
    }
    return 1000.0F / (milliseconds * (float)context->sample_rate);
}

static uint16_t instrument_process(
    void *opaque,
    effect_audio_block_t *block)
{
    instrument_context_t *context = (instrument_context_t *)opaque;
    const float attack = instrument_attack_coefficient(context);
    const float release =
        1000.0F /
        ((90.0F + context->articulation * 900.0F) *
         (float)context->sample_rate);

    for (uint32_t frame = UINT32_C(0);
         frame < block->frame_count;
         ++frame) {
        const float detector_input = block->channels[0][frame];
        instrument_track_sample(context, detector_input);
        const float tracked =
            context->tracking_confidence >= 0.58F &&
            context->input_envelope >= context->sensitivity
                ? 1.0F
                : 0.0F;
        const float target = tracked * instrument_clamp(
            context->input_envelope * 5.5F,
            0.0F,
            1.0F);
        const float envelope_coefficient =
            target > context->synth_envelope ? attack : release;
        context->synth_envelope += envelope_coefficient *
            (target - context->synth_envelope);
        const float frequency = context->tracked_frequency > 0.0F
            ? context->tracked_frequency
            : 110.0F;
        const float increment = frequency / (float)context->sample_rate;

        for (uint8_t channel = UINT8_C(0);
             channel < block->channel_count;
             ++channel) {
            const float dry = block->channels[channel][frame];
            float wet = instrument_voice_sample(
                context,
                &context->channels[channel],
                increment);
            wet *= context->synth_envelope * 1.35F;
            wet = wet / (1.0F + instrument_absolute(wet));
            block->channels[channel][frame] =
                dry + (wet - dry) * context->mix;
        }
    }
    return EFFECT_RUNTIME_OK;
}

static uint16_t instrument_set_parameter(
    void *opaque,
    uint32_t parameter_id,
    float value)
{
    instrument_context_t *context = (instrument_context_t *)opaque;

    if (parameter_id == EFFECT_INSTRUMENT_PARAMETER_ARTICULATION) {
        context->articulation = value;
    } else if (parameter_id == EFFECT_INSTRUMENT_PARAMETER_CHARACTER) {
        context->character = value;
    } else if (parameter_id == EFFECT_INSTRUMENT_PARAMETER_MIX) {
        context->mix = value;
    } else if (parameter_id == EFFECT_INSTRUMENT_PARAMETER_SENSITIVITY) {
        context->sensitivity = value;
    } else {
        return EFFECT_RUNTIME_PARAMETER_NOT_FOUND;
    }
    return EFFECT_RUNTIME_OK;
}

static const effect_parameter_descriptor_t instrument_parameters[] = {
    {
        EFFECT_INSTRUMENT_PARAMETER_ARTICULATION,
        "Articulation",
        "ratio",
        0.0F,
        1.0F,
        0.55F,
    },
    {
        EFFECT_INSTRUMENT_PARAMETER_CHARACTER,
        "Character",
        "ratio",
        0.0F,
        1.0F,
        0.55F,
    },
    {
        EFFECT_INSTRUMENT_PARAMETER_MIX,
        "Instrument mix",
        "ratio",
        0.0F,
        1.0F,
        0.72F,
    },
    {
        EFFECT_INSTRUMENT_PARAMETER_SENSITIVITY,
        "Tracking sensitivity",
        "linear",
        0.001F,
        0.2F,
        0.012F,
    },
};

#define INSTRUMENT_DESCRIPTOR(symbol, effect_id, label, initializer) \
    const effect_descriptor_t symbol = { \
        .key = { EFFECT_VENDOR_OPEN, effect_id }, \
        .name = label, \
        .abi_version = EFFECT_RUNTIME_ABI_VERSION, \
        .parameter_count = (uint16_t)( \
            sizeof(instrument_parameters) / \
            sizeof(instrument_parameters[0])), \
        .parameters = instrument_parameters, \
        .context_size = sizeof(instrument_context_t), \
        .context_alignment = _Alignof(instrument_context_t), \
        .initialize = initializer, \
        .reset = instrument_reset, \
        .process = instrument_process, \
        .set_parameter = instrument_set_parameter, \
    }

INSTRUMENT_DESCRIPTOR(
    ncr2_effect_bowed_ensemble,
    EFFECT_OPEN_BOWED_ENSEMBLE_ID,
    "Instrument · Bowed Ensemble",
    bowed_ensemble_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_cello_voice,
    EFFECT_OPEN_CELLO_VOICE_ID,
    "Instrument · Cello",
    cello_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_violin_voice,
    EFFECT_OPEN_VIOLIN_VOICE_ID,
    "Instrument · Violin",
    violin_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_tonewheel_organ,
    EFFECT_OPEN_TONEWHEEL_ORGAN_ID,
    "Instrument · Tonewheel Organ",
    organ_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_clarinet_voice,
    EFFECT_OPEN_CLARINET_VOICE_ID,
    "Instrument · Clarinet",
    clarinet_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_synth_brass,
    EFFECT_OPEN_SYNTH_BRASS_ID,
    "Instrument · Synth Brass",
    brass_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_synth_bass,
    EFFECT_OPEN_SYNTH_BASS_ID,
    "Instrument · Synth Bass",
    bass_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_bell_marimba,
    EFFECT_OPEN_BELL_MARIMBA_ID,
    "Instrument · Bell / Marimba",
    bell_initialize);

static const effect_descriptor_t *const instrument_effects[] = {
    &ncr2_effect_bowed_ensemble,
    &ncr2_effect_cello_voice,
    &ncr2_effect_violin_voice,
    &ncr2_effect_tonewheel_organ,
    &ncr2_effect_clarinet_voice,
    &ncr2_effect_synth_brass,
    &ncr2_effect_synth_bass,
    &ncr2_effect_bell_marimba,
};

const effect_registry_t ncr2_instrument_effect_registry = {
    .effects = instrument_effects,
    .count = sizeof(instrument_effects) / sizeof(instrument_effects[0]),
};
