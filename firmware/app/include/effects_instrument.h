#ifndef NCR2_EFFECTS_INSTRUMENT_H
#define NCR2_EFFECTS_INSTRUMENT_H

#include "effect_runtime.h"

/* Audio-to-instrument voices. They retain expressive pitch bends rather than
 * forcing detected notes onto an equal-tempered MIDI grid. */
#define EFFECT_OPEN_BOWED_ENSEMBLE_ID UINT32_C(0x00000100)
#define EFFECT_OPEN_CELLO_VOICE_ID UINT32_C(0x00000101)
#define EFFECT_OPEN_VIOLIN_VOICE_ID UINT32_C(0x00000102)
#define EFFECT_OPEN_TONEWHEEL_ORGAN_ID UINT32_C(0x00000103)
#define EFFECT_OPEN_CLARINET_VOICE_ID UINT32_C(0x00000104)
#define EFFECT_OPEN_SYNTH_BRASS_ID UINT32_C(0x00000105)
#define EFFECT_OPEN_SYNTH_BASS_ID UINT32_C(0x00000106)
#define EFFECT_OPEN_BELL_MARIMBA_ID UINT32_C(0x00000107)

#define EFFECT_INSTRUMENT_PARAMETER_ARTICULATION UINT32_C(1)
#define EFFECT_INSTRUMENT_PARAMETER_CHARACTER UINT32_C(2)
#define EFFECT_INSTRUMENT_PARAMETER_MIX UINT32_C(3)
#define EFFECT_INSTRUMENT_PARAMETER_SENSITIVITY UINT32_C(4)

extern const effect_descriptor_t ncr2_effect_bowed_ensemble;
extern const effect_descriptor_t ncr2_effect_cello_voice;
extern const effect_descriptor_t ncr2_effect_violin_voice;
extern const effect_descriptor_t ncr2_effect_tonewheel_organ;
extern const effect_descriptor_t ncr2_effect_clarinet_voice;
extern const effect_descriptor_t ncr2_effect_synth_brass;
extern const effect_descriptor_t ncr2_effect_synth_bass;
extern const effect_descriptor_t ncr2_effect_bell_marimba;
extern const effect_registry_t ncr2_instrument_effect_registry;

#endif
