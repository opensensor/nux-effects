#ifndef NCR2_EFFECTS_BASIC_H
#define NCR2_EFFECTS_BASIC_H

#include "effect_runtime.h"

#define EFFECT_OPEN_BASIC_GAIN_ID UINT32_C(1)
#define EFFECT_OPEN_BASIC_SOFT_CLIP_ID UINT32_C(2)

#define EFFECT_GAIN_PARAMETER_GAIN UINT32_C(1)

#define EFFECT_SOFT_CLIP_PARAMETER_DRIVE UINT32_C(1)
#define EFFECT_SOFT_CLIP_PARAMETER_LEVEL UINT32_C(2)
#define EFFECT_SOFT_CLIP_PARAMETER_MIX UINT32_C(3)

extern const effect_descriptor_t ncr2_effect_basic_gain;
extern const effect_descriptor_t ncr2_effect_basic_soft_clip;
extern const effect_registry_t ncr2_basic_effect_registry;

#endif
