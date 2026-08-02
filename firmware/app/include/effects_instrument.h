#ifndef NCR2_EFFECTS_INSTRUMENT_H
#define NCR2_EFFECTS_INSTRUMENT_H

#include "effect_runtime.h"

/* Phase-aligned spectral transformations. They retain expressive pitch bends
 * rather than forcing detected notes onto an equal-tempered MIDI grid. */
#define EFFECT_OPEN_STEEL_ACOUSTIC_ID UINT32_C(0x00000100)
#define EFFECT_OPEN_NYLON_CLASSICAL_ID UINT32_C(0x00000101)
#define EFFECT_OPEN_TWELVE_STRING_ID UINT32_C(0x00000102)
#define EFFECT_OPEN_BANJO_ID UINT32_C(0x00000103)
#define EFFECT_OPEN_SITAR_ID UINT32_C(0x00000104)
#define EFFECT_OPEN_UPRIGHT_BASS_ID UINT32_C(0x00000105)
#define EFFECT_OPEN_BOWED_CELLO_ID UINT32_C(0x00000106)
#define EFFECT_OPEN_CLARINET_ID UINT32_C(0x00000107)

/* Source compatibility for experimental banks exported before v0.27. */
#define EFFECT_OPEN_BOWED_ENSEMBLE_ID EFFECT_OPEN_STEEL_ACOUSTIC_ID
#define EFFECT_OPEN_CELLO_VOICE_ID EFFECT_OPEN_NYLON_CLASSICAL_ID
#define EFFECT_OPEN_VIOLIN_VOICE_ID EFFECT_OPEN_TWELVE_STRING_ID
#define EFFECT_OPEN_TONEWHEEL_ORGAN_ID EFFECT_OPEN_BANJO_ID
#define EFFECT_OPEN_CLARINET_VOICE_ID EFFECT_OPEN_SITAR_ID
#define EFFECT_OPEN_SYNTH_BRASS_ID EFFECT_OPEN_UPRIGHT_BASS_ID
#define EFFECT_OPEN_SYNTH_BASS_ID EFFECT_OPEN_BOWED_CELLO_ID
#define EFFECT_OPEN_BELL_MARIMBA_ID EFFECT_OPEN_CLARINET_ID

#define EFFECT_INSTRUMENT_PARAMETER_TRANSFORMATION UINT32_C(1)
/* Source compatibility for experimental banks exported before v0.25. */
#define EFFECT_INSTRUMENT_PARAMETER_ARTICULATION \
    EFFECT_INSTRUMENT_PARAMETER_TRANSFORMATION
#define EFFECT_INSTRUMENT_PARAMETER_CHARACTER UINT32_C(2)
#define EFFECT_INSTRUMENT_PARAMETER_MIX UINT32_C(3)
#define EFFECT_INSTRUMENT_PARAMETER_SENSITIVITY UINT32_C(4)

typedef struct effect_instrument_note_state {
    float frequency_hz;
    float confidence;
    uint32_t onset_count;
    uint16_t pitch_bend;
    uint8_t active;
    uint8_t midi_note;
    uint8_t velocity;
} effect_instrument_note_state_t;

extern const effect_descriptor_t ncr2_effect_steel_acoustic;
extern const effect_descriptor_t ncr2_effect_nylon_classical;
extern const effect_descriptor_t ncr2_effect_twelve_string;
extern const effect_descriptor_t ncr2_effect_banjo;
extern const effect_descriptor_t ncr2_effect_sitar;
extern const effect_descriptor_t ncr2_effect_upright_bass;
extern const effect_descriptor_t ncr2_effect_bowed_cello;
extern const effect_descriptor_t ncr2_effect_clarinet;
extern const effect_registry_t ncr2_instrument_effect_registry;

#define ncr2_effect_bowed_ensemble ncr2_effect_steel_acoustic
#define ncr2_effect_cello_voice ncr2_effect_nylon_classical
#define ncr2_effect_violin_voice ncr2_effect_twelve_string
#define ncr2_effect_tonewheel_organ ncr2_effect_banjo
#define ncr2_effect_clarinet_voice ncr2_effect_sitar
#define ncr2_effect_synth_brass ncr2_effect_upright_bass
#define ncr2_effect_synth_bass ncr2_effect_bowed_cello
#define ncr2_effect_bell_marimba ncr2_effect_clarinet

/* Read-only state for a later USB-MIDI/control-plane adapter. Audio effects
 * remain fully usable when no host consumes this information. */
uint16_t ncr2_instrument_get_note_state(
    const effect_instance_t *instance,
    effect_instrument_note_state_t *state);

#endif
