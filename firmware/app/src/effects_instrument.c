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
#define INSTRUMENT_WAVEGUIDE_SAMPLES UINT32_C(1024)
#define INSTRUMENT_WAVEGUIDE_MASK \
    (INSTRUMENT_WAVEGUIDE_SAMPLES - UINT32_C(1))
#define INSTRUMENT_MIDI_FIRST UINT32_C(36)
#define INSTRUMENT_MIDI_LAST UINT32_C(96)

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

typedef struct instrument_voice_state {
    float phase[6];
    float tone;
    float body;
    float wave_tone;
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
    float waveguide[INSTRUMENT_WAVEGUIDE_SAMPLES];
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
    uint32_t waveguide_write;
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

static float instrument_waveguide(
    instrument_context_t *context,
    float normalized_input,
    float noise,
    float feedback,
    float damping)
{
    float delay = (float)context->sample_rate /
        instrument_clamp(context->tracked_frequency, 65.0F, 1450.0F);
    uint32_t whole;
    float fraction;
    uint32_t recent;
    uint32_t older;
    float delayed;
    float excitation;

    delay = instrument_clamp(
        delay, 16.0F, (float)(INSTRUMENT_WAVEGUIDE_SAMPLES - UINT32_C(2)));
    whole = (uint32_t)delay;
    fraction = delay - (float)whole;
    recent = (context->waveguide_write + INSTRUMENT_WAVEGUIDE_SAMPLES -
        whole) & INSTRUMENT_WAVEGUIDE_MASK;
    older = (recent + INSTRUMENT_WAVEGUIDE_SAMPLES - UINT32_C(1)) &
        INSTRUMENT_WAVEGUIDE_MASK;
    delayed = context->waveguide[recent] * (1.0F - fraction) +
        context->waveguide[older] * fraction;
    context->voice_state.wave_tone += damping *
        (delayed - context->voice_state.wave_tone);
    /* Excite the model from the player's envelope and articulation, but do
     * not feed enough of the pickup waveform through the delay line for the
     * result to collapse into a resonant guitar effect.  The earlier balance
     * measured as strongly dry-correlated on real DI even at 100% mix. */
    excitation = normalized_input * 0.018F +
        noise * (0.042F + context->character * 0.026F);
    context->waveguide[context->waveguide_write] = instrument_soft_bound(
        excitation + context->voice_state.wave_tone * feedback);
    context->waveguide_write =
        (context->waveguide_write + UINT32_C(1)) &
        INSTRUMENT_WAVEGUIDE_MASK;
    return context->voice_state.wave_tone;
}

static void instrument_advance_phases(
    instrument_context_t *context,
    float increment,
    const float ratios[6])
{
    for (uint32_t partial = UINT32_C(0);
         partial < UINT32_C(6);
         ++partial) {
        context->voice_state.phase[partial] = instrument_wrap(
            context->voice_state.phase[partial] +
            increment * ratios[partial]);
    }
}

static float instrument_voice_sample(
    instrument_context_t *context,
    float input,
    float frequency)
{
    instrument_voice_state_t *state = &context->voice_state;
    const float increment = frequency / (float)context->sample_rate;
    const float brightness = context->character;
    const float input_scale = context->input_envelope > 0.008F
        ? context->input_envelope
        : 0.008F;
    const float normalized_input = instrument_clamp(
        input / input_scale, -1.0F, 1.0F);
    const float noise = instrument_random(context);
    float ratios[6] = { 1.0F, 2.0F, 3.0F, 4.0F, 5.0F, 6.0F };
    float raw;
    float tone_coefficient;
    float output_gain;

    switch (context->voice) {
    case INSTRUMENT_BOWED_ENSEMBLE:
        ratios[1] = 1.006F;
        raw = instrument_waveguide(
            context, normalized_input, noise,
            0.988F + brightness * 0.008F,
            0.10F + brightness * 0.22F) * 1.55F +
            instrument_sine(state->phase[0]) * 0.20F +
            instrument_sine(state->phase[1]) * 0.14F;
        tone_coefficient = 0.10F + brightness * 0.24F;
        output_gain = 0.85F;
        break;
    case INSTRUMENT_CELLO:
        ratios[1] = 0.5F;
        ratios[2] = 2.0F;
        raw = instrument_waveguide(
            context, normalized_input, noise,
            0.991F + brightness * 0.005F,
            0.075F + brightness * 0.14F) * 1.65F +
            instrument_sine(state->phase[1]) * 0.20F +
            instrument_sine(state->phase[0]) * 0.15F;
        tone_coefficient = 0.075F + brightness * 0.17F;
        output_gain = 0.95F;
        break;
    case INSTRUMENT_VIOLIN:
        ratios[1] = 2.0F;
        ratios[2] = 3.0F;
        raw = instrument_waveguide(
            context, normalized_input, noise,
            0.984F + brightness * 0.010F,
            0.14F + brightness * 0.30F) * 1.45F +
            instrument_sine(state->phase[1]) * (0.12F + brightness * 0.15F) +
            instrument_sine(state->phase[2]) * brightness * 0.10F;
        tone_coefficient = 0.14F + brightness * 0.34F;
        output_gain = 1.00F;
        break;
    case INSTRUMENT_TONEWHEEL_ORGAN:
        /* Drawbar-like sub, fundamental, fifth, octave and upper drawbars.
         * The non-integer fifth separates this voice from the saw-like brass
         * spectrum instead of making both presets the same oscillator mix. */
        ratios[0] = 0.5F;
        ratios[1] = 1.0F;
        ratios[2] = 1.5F;
        ratios[3] = 2.0F;
        ratios[4] = 3.0F;
        ratios[5] = 4.0F;
        raw = instrument_sine(state->phase[0]) * 0.18F +
            instrument_sine(state->phase[1]) * 0.48F +
            instrument_sine(state->phase[2]) * (0.08F + brightness * 0.16F) +
            instrument_sine(state->phase[3]) * 0.13F +
            instrument_sine(state->phase[4]) * brightness * 0.12F +
            instrument_sine(state->phase[5]) * brightness * 0.08F;
        tone_coefficient = 0.22F + brightness * 0.38F;
        output_gain = 0.56F;
        break;
    case INSTRUMENT_CLARINET:
        ratios[1] = 3.0F;
        ratios[2] = 5.0F;
        ratios[3] = 7.0F;
        raw = instrument_waveguide(
            context, normalized_input, noise,
            0.972F + brightness * 0.016F,
            0.08F + brightness * 0.26F) * 0.85F +
            instrument_sine(state->phase[0]) * 0.48F +
            instrument_sine(state->phase[1]) * (0.10F + brightness * 0.20F) +
            instrument_sine(state->phase[2]) * brightness * 0.10F +
            noise * context->input_envelope * 0.16F;
        tone_coefficient = 0.10F + brightness * 0.28F;
        output_gain = 0.58F;
        break;
    case INSTRUMENT_SYNTH_BRASS:
        raw = instrument_sine(state->phase[0]) * 0.48F +
            instrument_sine(state->phase[1]) * (0.16F + brightness * 0.20F) +
            instrument_sine(state->phase[2]) * (0.08F + brightness * 0.16F) +
            instrument_sine(state->phase[3]) * brightness * 0.10F;
        tone_coefficient = 0.08F + brightness * 0.32F +
            context->input_envelope * 0.35F;
        output_gain = 0.62F;
        break;
    case INSTRUMENT_SYNTH_BASS:
        ratios[1] = 0.5F;
        ratios[2] = 2.0F;
        raw = instrument_sine(state->phase[1]) * 0.48F +
            instrument_sine(state->phase[0]) * 0.42F +
            instrument_sine(state->phase[2]) * brightness * 0.20F;
        tone_coefficient = 0.07F + brightness * 0.20F;
        output_gain = 0.50F;
        break;
    case INSTRUMENT_BELL_MARIMBA:
    default:
        ratios[1] = 2.73F;
        ratios[2] = 4.07F;
        ratios[3] = 5.43F;
        raw = instrument_sine(state->phase[0]) * 0.56F +
            instrument_sine(state->phase[1]) * (0.16F + brightness * 0.12F) +
            instrument_sine(state->phase[2]) * (0.10F + brightness * 0.10F) +
            instrument_sine(state->phase[3]) * brightness * 0.08F;
        raw *= state->percussive_envelope;
        tone_coefficient = 0.16F + brightness * 0.38F;
        output_gain = 2.10F;
        break;
    }

    instrument_advance_phases(context, increment, ratios);
    tone_coefficient = instrument_clamp(
        tone_coefficient, 0.05F, 0.62F);
    state->tone += tone_coefficient * (raw - state->tone);
    state->body += 0.0015F * (state->tone - state->body);
    return (state->tone - state->body * 0.22F) * output_gain;
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
    context->waveguide_write = UINT32_C(0);
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
    for (uint32_t sample = UINT32_C(0);
         sample < INSTRUMENT_WAVEGUIDE_SAMPLES;
         ++sample) {
        context->waveguide[sample] = 0.0F;
    }
    for (uint32_t partial = UINT32_C(0);
         partial < UINT32_C(6);
         ++partial) {
        context->voice_state.phase[partial] =
            (float)partial * 0.137F;
    }
    context->voice_state.tone = 0.0F;
    context->voice_state.body = 0.0F;
    context->voice_state.wave_tone = 0.0F;
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
        milliseconds = 12.0F + context->transformation * 95.0F;
        break;
    case INSTRUMENT_SYNTH_BRASS:
        milliseconds = 7.0F + context->transformation * 42.0F;
        break;
    case INSTRUMENT_BELL_MARIMBA:
        milliseconds = 2.0F;
        break;
    default:
        milliseconds = 4.0F + context->transformation * 18.0F;
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
    const float release = 1000.0F /
        ((120.0F + context->transformation * 680.0F) *
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
    if (effect_id < EFFECT_OPEN_BOWED_ENSEMBLE_ID ||
        effect_id > EFFECT_OPEN_BELL_MARIMBA_ID) {
        return EFFECT_RUNTIME_EFFECT_NOT_FOUND;
    }
    *state = ((const instrument_context_t *)instance->context)->note;
    return EFFECT_RUNTIME_OK;
}
