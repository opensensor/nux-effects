#include "effects_instrument.h"

#include <stddef.h>
#include <stdint.h>

#define INSTRUMENT_TRACKER_SAMPLES UINT32_C(512)
#define INSTRUMENT_TRACKER_MASK \
    (INSTRUMENT_TRACKER_SAMPLES - UINT32_C(1))
#define INSTRUMENT_ANALYSIS_WINDOW UINT32_C(192)
#define INSTRUMENT_MIN_LAG UINT32_C(8)
#define INSTRUMENT_MAX_LAG UINT32_C(184)
#define INSTRUMENT_ANALYSIS_INTERVAL UINT32_C(24)
#define INSTRUMENT_DECIMATION UINT32_C(4)
#define INSTRUMENT_PARTIAL_COUNT UINT32_C(12)
#define INSTRUMENT_BODY_MODE_COUNT UINT32_C(3)
#define INSTRUMENT_MIDI_FIRST UINT32_C(36)
#define INSTRUMENT_MIDI_LAST UINT32_C(96)

enum instrument_voice {
    INSTRUMENT_STEEL_ACOUSTIC = 0,
    INSTRUMENT_NYLON_CLASSICAL = 1,
    INSTRUMENT_TWELVE_STRING = 2,
    INSTRUMENT_BANJO = 3,
    INSTRUMENT_SITAR = 4,
    INSTRUMENT_UPRIGHT_BASS = 5,
    INSTRUMENT_BOWED_CELLO = 6,
    INSTRUMENT_CLARINET = 7,
};

typedef struct instrument_target {
    float partials[INSTRUMENT_PARTIAL_COUNT];
    float pitch_ratio;
    float spectral_mix;
    float body_mix;
    float transient_level;
    float output_gain;
    float body_frequency[INSTRUMENT_BODY_MODE_COUNT];
    float body_radius[INSTRUMENT_BODY_MODE_COUNT];
    float body_gain[INSTRUMENT_BODY_MODE_COUNT];
    float attack_ms;
    float release_ms;
} instrument_target_t;

/* The target spectra are normalized harmonic envelopes, not EQ curves.  The
 * input is first decomposed into phase-aligned partials; these profiles then
 * define the newly reconstructed source before target body resonances. */
static const instrument_target_t instrument_targets[] = {
    {
        { 1.00F, 0.72F, 0.53F, 0.41F, 0.32F, 0.25F,
          0.20F, 0.16F, 0.13F, 0.10F, 0.08F, 0.065F },
        1.0F, 0.48F, 0.34F, 0.16F, 1.00F,
        { 108.0F, 205.0F, 342.0F },
        { 0.9982F, 0.9974F, 0.9968F },
        { 0.46F, 0.32F, 0.22F },
        2.5F, 180.0F,
    },
    {
        { 1.00F, 0.48F, 0.25F, 0.14F, 0.085F, 0.055F,
          0.038F, 0.028F, 0.021F, 0.016F, 0.012F, 0.009F },
        1.0F, 0.36F, 0.42F, 0.07F, 1.05F,
        { 96.0F, 188.0F, 315.0F },
        { 0.9985F, 0.9978F, 0.9970F },
        { 0.52F, 0.30F, 0.18F },
        4.5F, 220.0F,
    },
    {
        { 1.00F, 0.82F, 0.62F, 0.48F, 0.38F, 0.31F,
          0.25F, 0.20F, 0.17F, 0.14F, 0.115F, 0.095F },
        1.006F, 0.28F, 0.30F, 0.13F, 0.88F,
        { 122.0F, 247.0F, 515.0F },
        { 0.9980F, 0.9971F, 0.9964F },
        { 0.40F, 0.34F, 0.26F },
        2.0F, 200.0F,
    },
    {
        { 0.72F, 1.00F, 0.84F, 0.68F, 0.56F, 0.47F,
          0.40F, 0.34F, 0.29F, 0.25F, 0.21F, 0.18F },
        1.0F, 0.38F, 0.36F, 0.38F, 1.05F,
        { 255.0F, 515.0F, 930.0F },
        { 0.9968F, 0.9958F, 0.9948F },
        { 0.38F, 0.34F, 0.28F },
        1.5F, 95.0F,
    },
    {
        { 1.00F, 0.67F, 0.57F, 0.48F, 0.41F, 0.35F,
          0.30F, 0.26F, 0.23F, 0.20F, 0.175F, 0.15F },
        1.002F, 0.30F, 0.46F, 0.31F, 0.95F,
        { 178.0F, 635.0F, 1280.0F },
        { 0.9984F, 0.9964F, 0.9945F },
        { 0.42F, 0.34F, 0.24F },
        2.0F, 260.0F,
    },
    {
        { 1.00F, 0.52F, 0.28F, 0.16F, 0.095F, 0.060F,
          0.040F, 0.028F, 0.020F, 0.015F, 0.011F, 0.008F },
        0.5F, 0.04F, 0.56F, 0.11F, 0.72F,
        { 72.0F, 142.0F, 268.0F },
        { 0.9990F, 0.9983F, 0.9974F },
        { 0.50F, 0.31F, 0.19F },
        5.0F, 320.0F,
    },
    {
        { 1.00F, 0.66F, 0.48F, 0.37F, 0.29F, 0.23F,
          0.19F, 0.16F, 0.135F, 0.115F, 0.098F, 0.084F },
        0.5F, 0.02F, 0.52F, 0.24F, 0.84F,
        { 88.0F, 177.0F, 305.0F },
        { 0.9992F, 0.9986F, 0.9978F },
        { 0.46F, 0.34F, 0.20F },
        58.0F, 460.0F,
    },
    {
        { 1.00F, 0.035F, 0.53F, 0.025F, 0.31F, 0.018F,
          0.20F, 0.014F, 0.14F, 0.011F, 0.10F, 0.009F },
        1.0F, 0.10F, 0.50F, 0.045F, 0.85F,
        { 310.0F, 910.0F, 1510.0F },
        { 0.9980F, 0.9960F, 0.9938F },
        { 0.42F, 0.36F, 0.22F },
        13.0F, 210.0F,
    },
};

static const float instrument_guitar_profile[INSTRUMENT_PARTIAL_COUNT] = {
    1.00F, 0.76F, 0.58F, 0.45F, 0.36F, 0.29F,
    0.24F, 0.20F, 0.17F, 0.145F, 0.125F, 0.108F,
};

typedef struct instrument_voice_state {
    float analysis_phase[INSTRUMENT_PARTIAL_COUNT];
    float synthesis_phase[INSTRUMENT_PARTIAL_COUNT];
    float analysis_i[INSTRUMENT_PARTIAL_COUNT];
    float analysis_q[INSTRUMENT_PARTIAL_COUNT];
    float body_previous[INSTRUMENT_BODY_MODE_COUNT];
    float body_older[INSTRUMENT_BODY_MODE_COUNT];
    float body_coefficient[INSTRUMENT_BODY_MODE_COUNT];
    float body_radius_squared[INSTRUMENT_BODY_MODE_COUNT];
    float tone;
    float dc;
    float percussive_envelope;
} instrument_voice_state_t;

typedef struct instrument_context {
    uint32_t sample_rate;
    uint32_t voice;
    float transformation;
    float character;
    float mix;
    float sensitivity;
    float input_envelope;
    float slow_envelope;
    float noise_floor;
    float synth_envelope;
    float tracked_frequency;
    float candidate_frequency;
    float tracking_confidence;
    float decimation_filter_a;
    float decimation_filter_b;
    float tracker[INSTRUMENT_TRACKER_SAMPLES];
    uint32_t tracker_write;
    uint32_t tracker_count;
    uint32_t decimation_count;
    uint32_t decimation_factor;
    uint32_t analysis_count;
    uint32_t candidate_count;
    uint32_t lost_count;
    uint32_t onset_count;
    uint32_t handled_onset;
    uint32_t onset_refractory;
    uint32_t random_state;
    effect_instrument_note_state_t note;
    instrument_voice_state_t voice_state;
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

/* Continuous and bounded. The following low-pass and speaker conditioning
 * remove most of this inexpensive approximation's residual upper harmonics. */
static float instrument_sine(float phase)
{
    const float bipolar = phase * 2.0F - 1.0F;
    return 4.0F * bipolar * (1.0F - instrument_absolute(bipolar));
}

static float instrument_soft_bound(float value)
{
    return value / (1.0F + instrument_absolute(value));
}

static float instrument_random(instrument_context_t *context)
{
    context->random_state =
        context->random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return (float)(int32_t)(context->random_state >> 1) /
        1073741824.0F;
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

static void instrument_accept_pitch(
    instrument_context_t *context,
    float candidate,
    float confidence)
{
    if (context->tracked_frequency <= 0.0F) {
        context->tracked_frequency = candidate;
        context->candidate_count = UINT32_C(0);
        return;
    }

    {
        const float previous = context->tracked_frequency;
        float corrected = candidate;
        float ratio = candidate / previous;

        /* A harmonic-rich guitar can briefly make the first or second
         * harmonic look stronger than the fundamental. Keep octave
         * continuity unless a new onset repeatedly proves the jump. */
        if (ratio > 1.82F && ratio < 2.18F) {
            corrected = candidate * 0.5F;
        } else if (ratio > 0.46F && ratio < 0.55F) {
            corrected = candidate * 2.0F;
        }
        ratio = corrected / previous;
        if (ratio > 0.78F && ratio < 1.28F) {
            const float smoothing = confidence > 0.88F ? 0.28F : 0.16F;
            context->tracked_frequency +=
                (corrected - context->tracked_frequency) * smoothing;
            context->candidate_frequency = corrected;
            context->candidate_count = UINT32_C(0);
            return;
        }

        if (context->candidate_frequency > 0.0F &&
            instrument_absolute(
                corrected - context->candidate_frequency) <
                context->candidate_frequency * 0.035F) {
            ++context->candidate_count;
        } else {
            context->candidate_frequency = corrected;
            context->candidate_count = UINT32_C(1);
        }
        if (context->candidate_count >= UINT32_C(3)) {
            context->tracked_frequency = corrected;
            context->candidate_count = UINT32_C(0);
        }
    }
}

/* YIN-style cumulative normalized difference. Unlike the earlier raw AMDF,
 * the first usable valley represents the fundamental even when a picked
 * guitar note has a stronger second or third harmonic. */
static void instrument_analyze_pitch(instrument_context_t *context)
{
    float cumulative = 0.0F;
    float two_back = 1.0F;
    float previous = 1.0F;
    float best = 1.0F;
    float selected_value = 1.0F;
    float selected_lag = 0.0F;
    float mean_absolute = 0.0F;
    uint32_t best_lag = UINT32_C(0);

    if (context->tracker_count <
        INSTRUMENT_ANALYSIS_WINDOW + INSTRUMENT_MAX_LAG) {
        context->tracking_confidence = 0.0F;
        return;
    }
    for (uint32_t sample = UINT32_C(0);
         sample < INSTRUMENT_ANALYSIS_WINDOW;
         ++sample) {
        mean_absolute += instrument_absolute(
            instrument_tracker_sample(context, sample));
    }
    mean_absolute /= (float)INSTRUMENT_ANALYSIS_WINDOW;
    {
        const float gate = context->sensitivity > context->noise_floor * 3.5F
            ? context->sensitivity
            : context->noise_floor * 3.5F;
        if (mean_absolute < gate) {
            context->tracking_confidence = 0.0F;
            ++context->lost_count;
            return;
        }
    }

    for (uint32_t lag = UINT32_C(1);
         lag <= INSTRUMENT_MAX_LAG;
         ++lag) {
        float difference = 0.0F;

        for (uint32_t sample = UINT32_C(0);
             sample < INSTRUMENT_ANALYSIS_WINDOW;
             ++sample) {
            const float delta =
                instrument_tracker_sample(context, sample) -
                instrument_tracker_sample(context, sample + lag);
            difference += delta * delta;
        }
        cumulative += difference;
        {
            const float normalized = difference * (float)lag /
                (cumulative + 0.000000001F);

            if (lag >= INSTRUMENT_MIN_LAG && normalized < best) {
                best = normalized;
                best_lag = lag;
            }
            if (lag > INSTRUMENT_MIN_LAG + UINT32_C(1) &&
                previous < two_back && previous <= normalized &&
                previous < 0.22F) {
                const float curvature =
                    two_back - 2.0F * previous + normalized;
                float correction = 0.0F;

                if (instrument_absolute(curvature) > 0.000001F) {
                    correction = 0.5F *
                        (two_back - normalized) / curvature;
                    correction = instrument_clamp(
                        correction, -0.5F, 0.5F);
                }
                selected_lag = (float)(lag - UINT32_C(1)) + correction;
                selected_value = previous;
                break;
            }
            two_back = previous;
            previous = normalized;
        }
    }

    if (selected_lag <= 0.0F && best_lag != UINT32_C(0) && best < 0.34F) {
        selected_lag = (float)best_lag;
        selected_value = best;
    }
    if (selected_lag <= 0.0F) {
        context->tracking_confidence *= 0.75F;
        ++context->lost_count;
        return;
    }

    {
        const float candidate = (float)context->sample_rate /
            ((float)context->decimation_factor * selected_lag);
        const float confidence = instrument_clamp(
            1.0F - selected_value, 0.0F, 1.0F);

        context->tracking_confidence = confidence;
        context->lost_count = UINT32_C(0);
        if (confidence >= 0.66F &&
            candidate >= 65.0F && candidate <= 1450.0F) {
            instrument_accept_pitch(context, candidate, confidence);
        }
    }
}

static void instrument_track_sample(
    instrument_context_t *context,
    float input)
{
    const float magnitude = instrument_absolute(input);
    const float attack = magnitude > context->input_envelope
        ? 0.045F
        : 0.0012F;

    context->input_envelope +=
        attack * (magnitude - context->input_envelope);
    context->slow_envelope +=
        0.00035F * (magnitude - context->slow_envelope);
    if (context->onset_refractory > UINT32_C(0)) {
        --context->onset_refractory;
    }
    {
        const float gate = context->sensitivity > context->noise_floor * 4.0F
            ? context->sensitivity
            : context->noise_floor * 4.0F;
        if (context->onset_refractory == UINT32_C(0) &&
            context->input_envelope > gate * 1.5F &&
            context->input_envelope > context->slow_envelope * 1.65F) {
            ++context->onset_count;
            context->onset_refractory = context->sample_rate / UINT32_C(12);
            context->candidate_count = UINT32_C(0);
        }
        if (context->input_envelope < gate * 1.2F) {
            context->noise_floor +=
                0.00008F * (magnitude - context->noise_floor);
        }
    }

    context->decimation_filter_a +=
        0.16F * (input - context->decimation_filter_a);
    context->decimation_filter_b +=
        0.24F *
        (context->decimation_filter_a - context->decimation_filter_b);
    ++context->decimation_count;
    if (context->decimation_count < context->decimation_factor) return;
    context->decimation_count = UINT32_C(0);
    context->tracker[context->tracker_write] =
        context->decimation_filter_b;
    context->tracker_write =
        (context->tracker_write + UINT32_C(1)) & INSTRUMENT_TRACKER_MASK;
    if (context->tracker_count < INSTRUMENT_TRACKER_SAMPLES) {
        ++context->tracker_count;
    }
    ++context->analysis_count;
    if (context->analysis_count >= INSTRUMENT_ANALYSIS_INTERVAL) {
        context->analysis_count = UINT32_C(0);
        instrument_analyze_pitch(context);
    }
}

static float instrument_target_body(
    instrument_context_t *context,
    const instrument_target_t *target,
    float excitation)
{
    instrument_voice_state_t *state = &context->voice_state;
    float body = 0.0F;

    for (uint32_t mode = UINT32_C(0);
         mode < INSTRUMENT_BODY_MODE_COUNT;
         ++mode) {
        const float radius = target->body_radius[mode];
        const float next = excitation * (1.0F - radius) +
            state->body_coefficient[mode] * state->body_previous[mode] -
            state->body_radius_squared[mode] * state->body_older[mode];

        state->body_older[mode] = state->body_previous[mode];
        state->body_previous[mode] = instrument_soft_bound(next);
        body += state->body_previous[mode] * target->body_gain[mode];
    }
    return excitation * (1.0F - target->body_mix) +
        body * target->body_mix * 3.2F;
}

/* Pitch-synchronous harmonic transplantation.  Quadrature demodulators
 * estimate the complex amplitude of each guitar partial.  Reconstruction
 * divides out an approximate pickup spectrum and applies the target spectrum,
 * while a separately generated component can create missing harmonics or a
 * new register.  No sample of the dry waveform is mixed directly into wet. */
static float instrument_voice_sample(
    instrument_context_t *context,
    float input,
    float frequency)
{
    instrument_voice_state_t *state = &context->voice_state;
    const instrument_target_t *target = &instrument_targets[context->voice];
    const float base_increment = frequency / (float)context->sample_rate;
    const float input_scale = context->input_envelope > 0.008F
        ? context->input_envelope
        : 0.008F;
    const float normalized_input = instrument_clamp(
        input / input_scale, -1.0F, 1.0F);
    const float analyzer_coefficient = 0.0022F +
        context->transformation * 0.0018F;
    const float noise = instrument_random(context);
    float spectral = 0.0F;
    float synthesized = 0.0F;
    float partial_sum = 0.0F;

    for (uint32_t partial = UINT32_C(0);
         partial < INSTRUMENT_PARTIAL_COUNT;
         ++partial) {
        const float harmonic = (float)(partial + UINT32_C(1));
        const float analysis_frequency = frequency * harmonic;
        const float synthesis_frequency =
            frequency * target->pitch_ratio * harmonic;
        const float sine = instrument_sine(state->analysis_phase[partial]);
        const float cosine = -instrument_sine(instrument_wrap(
            state->analysis_phase[partial] + 0.25F));
        const float position = (float)partial /
            (float)(INSTRUMENT_PARTIAL_COUNT - UINT32_C(1));
        const float tilt = instrument_clamp(
            1.0F + (context->character - 0.5F) *
                1.8F * (position - 0.28F),
            0.35F,
            1.85F);
        const float target_weight = target->partials[partial] * tilt;

        if (analysis_frequency < (float)context->sample_rate * 0.46F) {
            const float reconstructed = 2.0F *
                (state->analysis_i[partial] * cosine +
                 state->analysis_q[partial] * sine);
            const float transfer = instrument_clamp(
                target_weight / instrument_guitar_profile[partial],
                0.08F,
                4.5F);

            state->analysis_i[partial] += analyzer_coefficient *
                (normalized_input * cosine - state->analysis_i[partial]);
            state->analysis_q[partial] += analyzer_coefficient *
                (normalized_input * sine - state->analysis_q[partial]);
            spectral += reconstructed * transfer;
        } else {
            state->analysis_i[partial] *= 0.996F;
            state->analysis_q[partial] *= 0.996F;
        }
        if (synthesis_frequency < (float)context->sample_rate * 0.46F) {
            synthesized += instrument_sine(
                state->synthesis_phase[partial]) * target_weight;
        }
        partial_sum += target_weight;
        state->analysis_phase[partial] = instrument_wrap(
            state->analysis_phase[partial] + base_increment * harmonic);
        state->synthesis_phase[partial] = instrument_wrap(
            state->synthesis_phase[partial] +
            base_increment * target->pitch_ratio * harmonic);
    }

    spectral *= 0.82F / (0.72F + partial_sum * 0.12F);
    synthesized *= 1.35F / (0.90F + partial_sum * 0.34F);
    {
        float raw = spectral * target->spectral_mix +
            synthesized * (1.0F - target->spectral_mix);

        raw += noise * state->percussive_envelope *
            target->transient_level;
        if (context->voice == INSTRUMENT_SITAR) {
            raw += instrument_soft_bound(spectral * 7.0F) *
                state->percussive_envelope * 0.16F;
        } else if (context->voice == INSTRUMENT_BOWED_CELLO) {
            raw += noise * (0.06F + context->character * 0.10F);
        } else if (context->voice == INSTRUMENT_CLARINET) {
            raw += noise * (0.018F + context->character * 0.022F);
        }
        raw = instrument_target_body(context, target, raw);
        state->tone += (0.12F + context->character * 0.34F) *
            (raw - state->tone);
        state->dc += 0.0012F * (state->tone - state->dc);
        return (state->tone - state->dc * 0.18F) * target->output_gain;
    }
}

static void instrument_update_note_state(
    instrument_context_t *context,
    uint8_t active)
{
    context->note.frequency_hz = context->tracked_frequency;
    context->note.confidence = context->tracking_confidence;
    context->note.onset_count = context->onset_count;
    context->note.active = active;
    if (active == UINT8_C(0) || context->tracked_frequency <= 0.0F) return;

    {
        float reference = 65.40639F;
        float best_difference = 1000000.0F;
        float best_reference = reference;
        uint32_t best_note = INSTRUMENT_MIDI_FIRST;

        for (uint32_t note = INSTRUMENT_MIDI_FIRST;
             note <= INSTRUMENT_MIDI_LAST;
             ++note) {
            const float difference = instrument_absolute(
                context->tracked_frequency - reference);
            if (difference < best_difference) {
                best_difference = difference;
                best_reference = reference;
                best_note = note;
            }
            reference *= 1.0594631F;
        }
        {
            const float semitones =
                (context->tracked_frequency / best_reference - 1.0F) *
                17.31234F;
            float bend = 8192.0F + semitones * 4096.0F;

            bend = instrument_clamp(bend, 0.0F, 16383.0F);
            context->note.midi_note = (uint8_t)best_note;
            context->note.pitch_bend = (uint16_t)bend;
        }
    }
}

static void instrument_clear_state(instrument_context_t *context)
{
    context->input_envelope = 0.0F;
    context->slow_envelope = 0.0F;
    context->noise_floor = 0.0002F;
    context->synth_envelope = 0.0F;
    context->tracked_frequency = 0.0F;
    context->candidate_frequency = 0.0F;
    context->tracking_confidence = 0.0F;
    context->decimation_filter_a = 0.0F;
    context->decimation_filter_b = 0.0F;
    context->tracker_write = UINT32_C(0);
    context->tracker_count = UINT32_C(0);
    context->decimation_count = UINT32_C(0);
    context->analysis_count = UINT32_C(0);
    context->candidate_count = UINT32_C(0);
    context->lost_count = UINT32_C(0);
    context->onset_count = UINT32_C(0);
    context->handled_onset = UINT32_C(0);
    context->onset_refractory = UINT32_C(0);
    context->random_state = UINT32_C(0x6d2b79f5);
    context->note.frequency_hz = 0.0F;
    context->note.confidence = 0.0F;
    context->note.onset_count = UINT32_C(0);
    context->note.pitch_bend = UINT16_C(8192);
    context->note.active = UINT8_C(0);
    context->note.midi_note = UINT8_C(0);
    context->note.velocity = UINT8_C(0);
    for (uint32_t sample = UINT32_C(0);
         sample < INSTRUMENT_TRACKER_SAMPLES;
         ++sample) {
        context->tracker[sample] = 0.0F;
    }
    for (uint32_t partial = UINT32_C(0);
         partial < INSTRUMENT_PARTIAL_COUNT;
         ++partial) {
        context->voice_state.analysis_phase[partial] = instrument_wrap(
            (float)partial * 0.117F);
        context->voice_state.synthesis_phase[partial] = instrument_wrap(
            (float)partial * 0.173F);
        context->voice_state.analysis_i[partial] = 0.0F;
        context->voice_state.analysis_q[partial] = 0.0F;
    }
    for (uint32_t mode = UINT32_C(0);
         mode < INSTRUMENT_BODY_MODE_COUNT;
         ++mode) {
        context->voice_state.body_previous[mode] = 0.0F;
        context->voice_state.body_older[mode] = 0.0F;
    }
    context->voice_state.tone = 0.0F;
    context->voice_state.dc = 0.0F;
    context->voice_state.percussive_envelope = 0.0F;
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
    if (voice >= sizeof(instrument_targets) / sizeof(instrument_targets[0])) {
        return EFFECT_RUNTIME_INVALID_ARGUMENT;
    }
    context->sample_rate = sample_rate;
    context->decimation_factor = sample_rate > UINT32_C(64000)
        ? UINT32_C(8)
        : INSTRUMENT_DECIMATION;
    context->voice = voice;
    context->transformation = 1.0F;
    context->character = 0.50F;
    context->mix = 1.0F;
    context->sensitivity = 0.006F;
    instrument_clear_state(context);
    for (uint32_t mode = UINT32_C(0);
         mode < INSTRUMENT_BODY_MODE_COUNT;
         ++mode) {
        const instrument_target_t *target = &instrument_targets[voice];
        const float radius = target->body_radius[mode];
        const float phase = target->body_frequency[mode] /
            (float)sample_rate;
        const float cosine = -instrument_sine(instrument_wrap(
            phase + 0.25F));

        context->voice_state.body_coefficient[mode] =
            2.0F * radius * cosine;
        context->voice_state.body_radius_squared[mode] = radius * radius;
    }
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
    steel_acoustic_initialize,
    INSTRUMENT_STEEL_ACOUSTIC)
DEFINE_INSTRUMENT_INITIALIZER(
    nylon_classical_initialize,
    INSTRUMENT_NYLON_CLASSICAL)
DEFINE_INSTRUMENT_INITIALIZER(
    twelve_string_initialize,
    INSTRUMENT_TWELVE_STRING)
DEFINE_INSTRUMENT_INITIALIZER(banjo_initialize, INSTRUMENT_BANJO)
DEFINE_INSTRUMENT_INITIALIZER(sitar_initialize, INSTRUMENT_SITAR)
DEFINE_INSTRUMENT_INITIALIZER(upright_bass_initialize, INSTRUMENT_UPRIGHT_BASS)
DEFINE_INSTRUMENT_INITIALIZER(bowed_cello_initialize, INSTRUMENT_BOWED_CELLO)
DEFINE_INSTRUMENT_INITIALIZER(clarinet_initialize, INSTRUMENT_CLARINET)

static void instrument_reset(void *opaque)
{
    instrument_clear_state((instrument_context_t *)opaque);
}

static float instrument_attack_coefficient(
    const instrument_context_t *context)
{
    const float milliseconds = 1.0F +
        instrument_targets[context->voice].attack_ms *
        context->transformation;

    return 1000.0F / (milliseconds * (float)context->sample_rate);
}

static uint16_t instrument_process(
    void *opaque,
    effect_audio_block_t *block)
{
    instrument_context_t *context = (instrument_context_t *)opaque;
    const float attack = instrument_attack_coefficient(context);
    const float release = 1000.0F /
        ((40.0F + context->transformation *
            instrument_targets[context->voice].release_ms) *
         (float)context->sample_rate);

    for (uint32_t frame = UINT32_C(0);
         frame < block->frame_count;
         ++frame) {
        const float detector_input = block->channels[0][frame];
        float wet;
        float effective_mix;
        float target;
        uint8_t tracked;

        instrument_track_sample(context, detector_input);
        tracked = (context->tracking_confidence >= 0.66F &&
            context->tracked_frequency > 0.0F &&
            context->lost_count < UINT32_C(5))
            ? UINT8_C(1)
            : UINT8_C(0);
        target = tracked != UINT8_C(0)
            ? instrument_clamp(
                context->input_envelope *
                    (0.95F + context->transformation * 0.85F),
                0.0F,
                0.48F)
            : 0.0F;
        context->synth_envelope +=
            (target > context->synth_envelope ? attack : release) *
            (target - context->synth_envelope);

        if (context->handled_onset != context->onset_count) {
            context->handled_onset = context->onset_count;
            context->voice_state.percussive_envelope = instrument_clamp(
                context->input_envelope * 8.0F, 0.18F, 1.0F);
            context->note.velocity = (uint8_t)instrument_clamp(
                context->input_envelope * 720.0F, 1.0F, 127.0F);
        }
        context->voice_state.percussive_envelope *=
            0.99955F - context->character * 0.00018F;
        wet = instrument_voice_sample(
            context,
            detector_input,
            context->tracked_frequency > 0.0F
                ? context->tracked_frequency
                : 110.0F);
        wet = instrument_soft_bound(wet * context->synth_envelope * 1.05F);
        effective_mix = context->mix *
            (0.06F + context->transformation * 0.94F);
        effective_mix = instrument_clamp(effective_mix, 0.0F, 1.0F);

        for (uint8_t channel = UINT8_C(0);
             channel < block->channel_count;
             ++channel) {
            const float dry = block->channels[channel][frame];
            block->channels[channel][frame] =
                dry + (wet - dry) * effective_mix;
        }
        instrument_update_note_state(
            context,
            (tracked != UINT8_C(0) && context->synth_envelope > 0.003F)
                ? UINT8_C(1)
                : UINT8_C(0));
    }
    return EFFECT_RUNTIME_OK;
}

static uint16_t instrument_set_parameter(
    void *opaque,
    uint32_t parameter_id,
    float value)
{
    instrument_context_t *context = (instrument_context_t *)opaque;

    if (parameter_id == EFFECT_INSTRUMENT_PARAMETER_TRANSFORMATION) {
        context->transformation = value;
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
        EFFECT_INSTRUMENT_PARAMETER_TRANSFORMATION,
        "Transformation",
        "ratio",
        0.0F,
        1.0F,
        1.0F,
    },
    {
        EFFECT_INSTRUMENT_PARAMETER_CHARACTER,
        "Character",
        "ratio",
        0.0F,
        1.0F,
        0.50F,
    },
    {
        EFFECT_INSTRUMENT_PARAMETER_MIX,
        "Instrument mix",
        "ratio",
        0.0F,
        1.0F,
        1.0F,
    },
    {
        EFFECT_INSTRUMENT_PARAMETER_SENSITIVITY,
        "Tracking sensitivity",
        "linear",
        0.001F,
        0.2F,
        0.006F,
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
    ncr2_effect_steel_acoustic,
    EFFECT_OPEN_STEEL_ACOUSTIC_ID,
    "Transform · Steel Acoustic",
    steel_acoustic_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_nylon_classical,
    EFFECT_OPEN_NYLON_CLASSICAL_ID,
    "Transform · Nylon Classical",
    nylon_classical_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_twelve_string,
    EFFECT_OPEN_TWELVE_STRING_ID,
    "Transform · Twelve String",
    twelve_string_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_banjo,
    EFFECT_OPEN_BANJO_ID,
    "Transform · Banjo",
    banjo_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_sitar,
    EFFECT_OPEN_SITAR_ID,
    "Transform · Sitar",
    sitar_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_upright_bass,
    EFFECT_OPEN_UPRIGHT_BASS_ID,
    "Transform · Upright Bass",
    upright_bass_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_bowed_cello,
    EFFECT_OPEN_BOWED_CELLO_ID,
    "Transform · Bowed Cello",
    bowed_cello_initialize);
INSTRUMENT_DESCRIPTOR(
    ncr2_effect_clarinet,
    EFFECT_OPEN_CLARINET_ID,
    "Transform · Clarinet",
    clarinet_initialize);

static const effect_descriptor_t *const instrument_effects[] = {
    &ncr2_effect_steel_acoustic,
    &ncr2_effect_nylon_classical,
    &ncr2_effect_twelve_string,
    &ncr2_effect_banjo,
    &ncr2_effect_sitar,
    &ncr2_effect_upright_bass,
    &ncr2_effect_bowed_cello,
    &ncr2_effect_clarinet,
};

const effect_registry_t ncr2_instrument_effect_registry = {
    .effects = instrument_effects,
    .count = sizeof(instrument_effects) / sizeof(instrument_effects[0]),
};

uint16_t ncr2_instrument_get_note_state(
    const effect_instance_t *instance,
    effect_instrument_note_state_t *state)
{
    uint32_t effect_id;

    if (instance == NULL || instance->descriptor == NULL ||
        instance->context == NULL || state == NULL ||
        instance->descriptor->key.vendor_id != EFFECT_VENDOR_OPEN) {
        return EFFECT_RUNTIME_INVALID_ARGUMENT;
    }
    effect_id = instance->descriptor->key.effect_id;
    if (effect_id < EFFECT_OPEN_STEEL_ACOUSTIC_ID ||
        effect_id > EFFECT_OPEN_CLARINET_ID) {
        return EFFECT_RUNTIME_EFFECT_NOT_FOUND;
    }
    *state = ((const instrument_context_t *)instance->context)->note;
    return EFFECT_RUNTIME_OK;
}
