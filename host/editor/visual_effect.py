"""Validated no-code DSP blocks and C generation for Open Effect Lab.

The browser edits a small declarative model. This module is the only place
that translates that model into an effect-runtime descriptor, so visual and
advanced-code effects use the same compiler, registry, preview, and export
path after generation.
"""

from __future__ import annotations

import math
from typing import Any

import codegen


MAX_BLOCKS = 10
DELAY_MAX_SAMPLES = 4096


BLOCKS: tuple[dict[str, Any], ...] = (
    {
        "kind": "gain",
        "name": "Gain",
        "category": "Level",
        "summary": "Boost or attenuate the signal.",
        "detail": "Use before distortion for more drive or last for output level.",
        "cost": "tiny",
        "controls": (
            {"key": "gain", "name": "Gain", "unit": "×", "minimum": 0.0,
             "maximum": 4.0, "step": 0.01, "default": 1.0},
        ),
    },
    {
        "kind": "lowpass",
        "name": "Low-pass tone",
        "category": "Tone",
        "summary": "Roll off fizz and upper harmonics.",
        "detail": "A smooth one-pole filter; lower cutoff sounds darker.",
        "cost": "tiny",
        "controls": (
            {"key": "cutoff", "name": "Cutoff", "unit": "Hz", "minimum": 80.0,
             "maximum": 18000.0, "step": 10.0, "default": 5000.0,
             "scale": "log"},
            {"key": "mix", "name": "Mix", "unit": "%", "minimum": 0.0,
             "maximum": 100.0, "step": 1.0, "default": 100.0},
        ),
    },
    {
        "kind": "highpass",
        "name": "High-pass tone",
        "category": "Tone",
        "summary": "Remove rumble, boom, and rectifier DC.",
        "detail": "A smooth one-pole filter; raise cutoff for a tighter sound.",
        "cost": "tiny",
        "controls": (
            {"key": "cutoff", "name": "Cutoff", "unit": "Hz", "minimum": 10.0,
             "maximum": 3000.0, "step": 5.0, "default": 90.0,
             "scale": "log"},
            {"key": "mix", "name": "Mix", "unit": "%", "minimum": 0.0,
             "maximum": 100.0, "step": 1.0, "default": 100.0},
        ),
    },
    {
        "kind": "soft_clip",
        "name": "Soft clip",
        "category": "Drive",
        "summary": "Rounded overdrive without an abrupt clipping edge.",
        "detail": "Drive creates harmonics, Level compensates loudness, Mix retains attack.",
        "cost": "small",
        "controls": (
            {"key": "drive", "name": "Drive", "unit": "×", "minimum": 1.0,
             "maximum": 40.0, "step": 0.1, "default": 6.0},
            {"key": "level", "name": "Level", "unit": "×", "minimum": 0.0,
             "maximum": 2.0, "step": 0.01, "default": 0.8},
            {"key": "mix", "name": "Mix", "unit": "%", "minimum": 0.0,
             "maximum": 100.0, "step": 1.0, "default": 100.0},
        ),
    },
    {
        "kind": "hard_clip",
        "name": "Hard clip",
        "category": "Drive",
        "summary": "Sharper distortion and fuzz-like flattening.",
        "detail": "Threshold sets how early the waveform is clipped.",
        "cost": "tiny",
        "controls": (
            {"key": "threshold", "name": "Threshold", "unit": "linear",
             "minimum": 0.03, "maximum": 1.0, "step": 0.005, "default": 0.35},
            {"key": "mix", "name": "Mix", "unit": "%", "minimum": 0.0,
             "maximum": 100.0, "step": 1.0, "default": 100.0},
        ),
    },
    {
        "kind": "gate",
        "name": "Noise gate",
        "category": "Dynamics",
        "summary": "Reduce low-level hiss between notes.",
        "detail": "Threshold is linear amplitude; Floor controls what remains when closed.",
        "cost": "tiny",
        "controls": (
            {"key": "threshold", "name": "Threshold", "unit": "linear",
             "minimum": 0.001, "maximum": 0.25, "step": 0.001,
             "default": 0.02, "scale": "log"},
            {"key": "floor", "name": "Closed level", "unit": "%",
             "minimum": 0.0, "maximum": 100.0, "step": 1.0, "default": 0.0},
        ),
    },
    {
        "kind": "tremolo",
        "name": "Tremolo",
        "category": "Motion",
        "summary": "Rhythmic volume motion from a lightweight triangle LFO.",
        "detail": "Rate controls speed and Depth controls the amount of movement.",
        "cost": "small",
        "controls": (
            {"key": "rate", "name": "Rate", "unit": "Hz", "minimum": 0.1,
             "maximum": 15.0, "step": 0.05, "default": 4.0},
            {"key": "depth", "name": "Depth", "unit": "%", "minimum": 0.0,
             "maximum": 100.0, "step": 1.0, "default": 65.0},
        ),
    },
    {
        "kind": "short_delay",
        "name": "Short delay",
        "category": "Time",
        "summary": "Slapback, doubling, and metallic short echoes.",
        "detail": "Bounded to 80 ms so its context fits the current effect arena.",
        "cost": "131 kB state",
        "limit": 1,
        "controls": (
            {"key": "time", "name": "Time", "unit": "ms", "minimum": 1.0,
             "maximum": 80.0, "step": 0.25, "default": 55.0},
            {"key": "feedback", "name": "Feedback", "unit": "%",
             "minimum": 0.0, "maximum": 90.0, "step": 1.0, "default": 25.0},
            {"key": "mix", "name": "Mix", "unit": "%", "minimum": 0.0,
             "maximum": 100.0, "step": 1.0, "default": 35.0},
        ),
    },
    {
        "kind": "swell",
        "name": "Envelope swell",
        "category": "Instrument voice",
        "summary": "Soften the pick attack into a bowed fade-in.",
        "detail": "Attack controls the bow-like rise; Release controls how long notes linger.",
        "cost": "small",
        "controls": (
            {"key": "attack", "name": "Attack", "unit": "ms", "minimum": 5.0,
             "maximum": 500.0, "step": 1.0, "default": 90.0,
             "scale": "log"},
            {"key": "release", "name": "Release", "unit": "ms", "minimum": 20.0,
             "maximum": 1500.0, "step": 5.0, "default": 500.0,
             "scale": "log"},
            {"key": "sensitivity", "name": "Sensitivity", "unit": "linear",
             "minimum": 0.001, "maximum": 0.2, "step": 0.001,
             "default": 0.008, "scale": "log"},
        ),
    },
    {
        "kind": "rectifier",
        "name": "Full-wave octave",
        "category": "Pitch / texture",
        "summary": "Fold negative half-cycles upward for an octave-like texture.",
        "detail": "Usually sounds best before a high-pass tone block.",
        "cost": "tiny",
        "controls": (
            {"key": "mix", "name": "Mix", "unit": "%", "minimum": 0.0,
             "maximum": 100.0, "step": 1.0, "default": 60.0},
        ),
    },
    {
        "kind": "limiter",
        "name": "Safety limiter",
        "category": "Level",
        "summary": "Place last to bound unexpected output peaks.",
        "detail": "This is a transparent hard ceiling, not a compressor model.",
        "cost": "tiny",
        "controls": (
            {"key": "ceiling", "name": "Ceiling", "unit": "linear",
             "minimum": 0.1, "maximum": 1.0, "step": 0.01, "default": 0.95},
        ),
    },
)

BLOCK_BY_KIND = {block["kind"]: block for block in BLOCKS}

RECIPES: tuple[dict[str, Any], ...] = (
    {"id": "clean_boost", "name": "Clean boost", "summary": "More level with a gentle fizz trim",
     "blocks": (("gain", {"gain": 1.8}), ("lowpass", {"cutoff": 9500.0, "mix": 100.0}))},
    {"id": "warm_drive", "name": "Warm drive", "summary": "Rounded breakup with a darker finish",
     "blocks": (("soft_clip", {"drive": 7.5, "level": 0.75, "mix": 88.0}),
                ("lowpass", {"cutoff": 5200.0, "mix": 100.0}),
                ("limiter", {"ceiling": 0.95}))},
    {"id": "tight_fuzz", "name": "Tight fuzz", "summary": "Trim bass, clip hard, tame the edge",
     "blocks": (("highpass", {"cutoff": 120.0, "mix": 100.0}),
                ("gain", {"gain": 2.6}),
                ("hard_clip", {"threshold": 0.24, "mix": 100.0}),
                ("lowpass", {"cutoff": 4300.0, "mix": 100.0}))},
    {"id": "vintage_trem", "name": "Vintage trem", "summary": "Simple musical amplitude motion",
     "blocks": (("tremolo", {"rate": 4.2, "depth": 72.0}),)},
    {"id": "slapback", "name": "Slapback", "summary": "Fast single-repeat rockabilly echo",
     "blocks": (("short_delay", {"time": 68.0, "feedback": 16.0, "mix": 32.0}),)},
    {"id": "bowed_pad", "name": "Bowed string pad", "summary": "Pick-free swell, octave color and ensemble space",
     "blocks": (("swell", {"attack": 110.0, "release": 650.0,
                            "sensitivity": 0.006}),
                ("lowpass", {"cutoff": 4300.0, "mix": 100.0}),
                ("rectifier", {"mix": 35.0}),
                ("highpass", {"cutoff": 65.0, "mix": 100.0}),
                ("short_delay", {"time": 18.0, "feedback": 12.0, "mix": 28.0}),
                ("limiter", {"ceiling": 0.9}))},
    {"id": "organ_octave", "name": "Organ octave", "summary": "Sustained octave-rich keyboard color",
     "blocks": (("swell", {"attack": 35.0, "release": 900.0,
                            "sensitivity": 0.004}),
                ("rectifier", {"mix": 58.0}),
                ("highpass", {"cutoff": 75.0, "mix": 100.0}),
                ("lowpass", {"cutoff": 5200.0, "mix": 100.0}),
                ("soft_clip", {"drive": 2.2, "level": 0.72, "mix": 45.0}))},
    {"id": "sitar_resonator", "name": "Sitar resonator", "summary": "Bright pluck into a short sympathetic resonance",
     "blocks": (("highpass", {"cutoff": 150.0, "mix": 100.0}),
                ("soft_clip", {"drive": 4.0, "level": 0.8, "mix": 72.0}),
                ("short_delay", {"time": 13.0, "feedback": 68.0, "mix": 52.0}),
                ("lowpass", {"cutoff": 6800.0, "mix": 100.0}),
                ("limiter", {"ceiling": 0.9}))},
    {"id": "octave_grit", "name": "Octave grit", "summary": "Rectified octave color without runaway DC",
     "blocks": (("rectifier", {"mix": 65.0}),
                ("highpass", {"cutoff": 80.0, "mix": 100.0}),
                ("soft_clip", {"drive": 3.5, "level": 0.8, "mix": 65.0}))},
)


def describe() -> dict[str, Any]:
    """Return JSON-ready palette and recipes, with tuples normalized."""
    blocks = []
    for definition in BLOCKS:
        entry = dict(definition)
        entry["controls"] = [dict(control) for control in definition["controls"]]
        blocks.append(entry)
    recipes = []
    for recipe in RECIPES:
        entry = {key: value for key, value in recipe.items() if key != "blocks"}
        entry["blocks"] = [
            {"kind": kind, "values": dict(values)}
            for kind, values in recipe["blocks"]
        ]
        recipes.append(entry)
    return {
        "blocks": blocks,
        "recipes": recipes,
        "limits": {"maximum_blocks": MAX_BLOCKS, "short_delay_blocks": 1},
    }


def _number(value: Any, label: str) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{label} must be a number") from error
    if not math.isfinite(number):
        raise ValueError(f"{label} must be finite")
    return number


def validate_spec(raw: Any) -> dict[str, Any]:
    if not isinstance(raw, dict):
        raise ValueError("visual effect must be an object")
    name = str(raw.get("name") or "").strip()
    if not name or len(name) > 48:
        raise ValueError("effect names are 1 to 48 characters")
    try:
        effect_id = int(raw.get("effect_id"))
    except (TypeError, ValueError) as error:
        raise ValueError("effect id must be an integer") from error
    if effect_id < 0x1000 or effect_id > 0xFFFFFFFF:
        raise ValueError("visual effect id must be between 0x1000 and 0xffffffff")
    raw_blocks = raw.get("blocks")
    if not isinstance(raw_blocks, list) or not raw_blocks:
        raise ValueError("add at least one visual DSP block")
    if len(raw_blocks) > MAX_BLOCKS:
        raise ValueError(f"visual effects are limited to {MAX_BLOCKS} blocks")

    counts: dict[str, int] = {}
    blocks = []
    for index, raw_block in enumerate(raw_blocks):
        if not isinstance(raw_block, dict):
            raise ValueError(f"block {index + 1} must be an object")
        kind = str(raw_block.get("kind") or "")
        definition = BLOCK_BY_KIND.get(kind)
        if definition is None:
            raise ValueError(f"block {index + 1} has unknown type {kind!r}")
        counts[kind] = counts.get(kind, 0) + 1
        if counts[kind] > int(definition.get("limit", MAX_BLOCKS)):
            raise ValueError(f"{definition['name']} may appear only once")
        raw_values = raw_block.get("values") or {}
        if not isinstance(raw_values, dict):
            raise ValueError(f"block {index + 1} values must be an object")
        values: dict[str, float] = {}
        for control in definition["controls"]:
            key = control["key"]
            value = _number(raw_values.get(key, control["default"]),
                            f"block {index + 1} {control['name']}")
            if value < control["minimum"] or value > control["maximum"]:
                raise ValueError(
                    f"block {index + 1} {control['name']} is outside "
                    f"{control['minimum']}..{control['maximum']}"
                )
            values[key] = value
        blocks.append({"kind": kind, "values": values})
    return {"name": name, "effect_id": effect_id, "blocks": blocks}


def _field(index: int, key: str) -> str:
    return f"block_{index}_{key}"


def _parameter_macro(macro: str, index: int, key: str) -> str:
    return f"EFFECT_{macro}_PARAMETER_{index + 1}_{key.upper()}"


def _scaled(kind: str, key: str, field: str) -> str:
    if key in {"mix", "depth", "feedback", "floor"}:
        return f"({field} * 0.01F)"
    return field


def generate_source(raw: Any) -> dict[str, Any]:
    spec = validate_spec(raw)
    name = spec["name"]
    effect_id = spec["effect_id"]
    blocks = spec["blocks"]
    ident = codegen.slug(name)
    macro = ident.upper()
    symbol = f"ncr2_effect_{ident}"
    file_name = f"effects_{ident}.c"
    definitions = [BLOCK_BY_KIND[block["kind"]] for block in blocks]
    has_delay = any(block["kind"] == "short_delay" for block in blocks)
    needs_abs = any(block["kind"] in {"soft_clip", "gate", "rectifier", "swell"}
                    for block in blocks)

    parameters: list[tuple[int, int, dict[str, Any], str]] = []
    parameter_id = 1
    for index, definition in enumerate(definitions):
        for control in definition["controls"]:
            parameters.append((parameter_id, index, control,
                               _parameter_macro(macro, index, control["key"])))
            parameter_id += 1

    lines = [
        '#include "effect_runtime.h"',
        "",
        "/* Generated by Open Effect Lab's visual designer. */",
        f"#define EFFECT_OPEN_{macro}_ID UINT32_C(0x{effect_id:08X})",
    ]
    for number, _index, _control, parameter_macro in parameters:
        lines.append(f"#define {parameter_macro} UINT32_C({number})")
    if has_delay:
        lines.extend([
            f"#define {macro}_DELAY_SAMPLES UINT32_C({DELAY_MAX_SAMPLES})",
        ])
    lines.extend(["", f"typedef struct {ident}_context {{", "    uint32_t sample_rate;"])
    for index, definition in enumerate(definitions):
        for control in definition["controls"]:
            lines.append(f"    float {_field(index, control['key'])};")
        kind = definition["kind"]
        if kind == "lowpass":
            lines.extend([
                f"    float block_{index}_coefficient;",
                f"    float block_{index}_state[EFFECT_RUNTIME_MAX_CHANNELS];",
            ])
        elif kind == "highpass":
            lines.extend([
                f"    float block_{index}_coefficient;",
                f"    float block_{index}_previous_input[EFFECT_RUNTIME_MAX_CHANNELS];",
                f"    float block_{index}_previous_output[EFFECT_RUNTIME_MAX_CHANNELS];",
            ])
        elif kind == "tremolo":
            lines.extend([
                f"    float block_{index}_increment;",
                f"    float block_{index}_phase[EFFECT_RUNTIME_MAX_CHANNELS];",
            ])
        elif kind == "short_delay":
            lines.extend([
                f"    uint32_t block_{index}_frames;",
                f"    uint32_t block_{index}_write[EFFECT_RUNTIME_MAX_CHANNELS];",
                f"    float block_{index}_buffer[EFFECT_RUNTIME_MAX_CHANNELS]",
                f"        [{macro}_DELAY_SAMPLES];",
            ])
        elif kind == "swell":
            lines.extend([
                f"    float block_{index}_attack_coefficient;",
                f"    float block_{index}_release_coefficient;",
                f"    float block_{index}_envelope[EFFECT_RUNTIME_MAX_CHANNELS];",
                f"    float block_{index}_gain[EFFECT_RUNTIME_MAX_CHANNELS];",
            ])
    lines.extend([f"}} {ident}_context_t;", ""])

    if needs_abs:
        lines.extend([
            "static float visual_absolute(float value)",
            "{",
            "    return value < 0.0F ? -value : value;",
            "}",
            "",
        ])

    lines.extend([
        f"static void {ident}_clear_state({ident}_context_t *context)",
        "{",
    ])
    state_written = False
    for index, definition in enumerate(definitions):
        kind = definition["kind"]
        if kind in {"lowpass", "tremolo"}:
            member = "state" if kind == "lowpass" else "phase"
            lines.extend([
                "    for (uint8_t channel = UINT8_C(0);",
                "         channel < EFFECT_RUNTIME_MAX_CHANNELS; ++channel) {",
                f"        context->block_{index}_{member}[channel] = 0.0F;",
                "    }",
            ])
            state_written = True
        elif kind == "highpass":
            lines.extend([
                "    for (uint8_t channel = UINT8_C(0);",
                "         channel < EFFECT_RUNTIME_MAX_CHANNELS; ++channel) {",
                f"        context->block_{index}_previous_input[channel] = 0.0F;",
                f"        context->block_{index}_previous_output[channel] = 0.0F;",
                "    }",
            ])
            state_written = True
        elif kind == "short_delay":
            lines.extend([
                "    for (uint8_t channel = UINT8_C(0);",
                "         channel < EFFECT_RUNTIME_MAX_CHANNELS; ++channel) {",
                f"        context->block_{index}_write[channel] = UINT32_C(0);",
                "        for (uint32_t sample = UINT32_C(0);",
                f"             sample < {macro}_DELAY_SAMPLES; ++sample) {{",
                f"            context->block_{index}_buffer[channel][sample] = 0.0F;",
                "        }",
                "    }",
            ])
            state_written = True
        elif kind == "swell":
            lines.extend([
                "    for (uint8_t channel = UINT8_C(0);",
                "         channel < EFFECT_RUNTIME_MAX_CHANNELS; ++channel) {",
                f"        context->block_{index}_envelope[channel] = 0.0F;",
                f"        context->block_{index}_gain[channel] = 0.0F;",
                "    }",
            ])
            state_written = True
    if not state_written:
        lines.append("    (void)context;")
    lines.extend(["}", ""])

    lines.extend([
        f"static uint16_t {ident}_initialize(",
        "    void *opaque,",
        "    uint32_t sample_rate,",
        "    uint32_t maximum_block_frames)",
        "{",
        f"    {ident}_context_t *context = ({ident}_context_t *)opaque;",
        "",
        "    (void)maximum_block_frames;",
        "    context->sample_rate = sample_rate;",
    ])
    for index, (definition, block) in enumerate(zip(definitions, blocks)):
        for control in definition["controls"]:
            lines.append(
                f"    context->{_field(index, control['key'])} = "
                f"{codegen.format_float(block['values'][control['key']])};"
            )
        kind = definition["kind"]
        if kind == "lowpass":
            lines.append(
                f"    context->block_{index}_coefficient = "
                f"context->block_{index}_cutoff / "
                f"(context->block_{index}_cutoff + 0.15915494F * (float)sample_rate);"
            )
        elif kind == "highpass":
            lines.append(
                f"    context->block_{index}_coefficient = (float)sample_rate / "
                f"((float)sample_rate + 6.28318531F * context->block_{index}_cutoff);"
            )
        elif kind == "tremolo":
            lines.append(
                f"    context->block_{index}_increment = "
                f"context->block_{index}_rate / (float)sample_rate;"
            )
        elif kind == "short_delay":
            lines.extend([
                f"    context->block_{index}_frames = (uint32_t)(",
                f"        context->block_{index}_time * (float)sample_rate / 1000.0F);",
                f"    if (context->block_{index}_frames >= {macro}_DELAY_SAMPLES) {{",
                f"        context->block_{index}_frames = {macro}_DELAY_SAMPLES - UINT32_C(1);",
                "    }",
                f"    if (context->block_{index}_frames == UINT32_C(0)) {{",
                f"        context->block_{index}_frames = UINT32_C(1);",
                "    }",
            ])
        elif kind == "swell":
            lines.extend([
                f"    context->block_{index}_attack_coefficient = 1000.0F /",
                f"        (context->block_{index}_attack * (float)sample_rate);",
                f"    context->block_{index}_release_coefficient = 1000.0F /",
                f"        (context->block_{index}_release * (float)sample_rate);",
            ])
    lines.extend([
        f"    {ident}_clear_state(context);",
        "    return EFFECT_RUNTIME_OK;",
        "}",
        "",
        f"static void {ident}_reset(void *opaque)",
        "{",
        f"    {ident}_clear_state(({ident}_context_t *)opaque);",
        "}",
        "",
        f"static uint16_t {ident}_process(",
        "    void *opaque,",
        "    effect_audio_block_t *block)",
        "{",
        f"    {ident}_context_t *context = ({ident}_context_t *)opaque;",
        "",
        "    for (uint8_t channel = UINT8_C(0);",
        "         channel < block->channel_count; ++channel) {",
        "        for (uint32_t frame = UINT32_C(0);",
        "             frame < block->frame_count; ++frame) {",
        "            float sample = block->channels[channel][frame];",
    ])
    for index, definition in enumerate(definitions):
        kind = definition["kind"]
        if kind == "gain":
            lines.append(f"            sample *= context->block_{index}_gain;")
        elif kind == "lowpass":
            lines.extend([
                f"            const float block_{index}_dry = sample;",
                f"            context->block_{index}_state[channel] +=",
                f"                context->block_{index}_coefficient *",
                f"                (sample - context->block_{index}_state[channel]);",
                f"            sample = block_{index}_dry +",
                f"                (context->block_{index}_state[channel] - block_{index}_dry) *",
                f"                {_scaled(kind, 'mix', f'context->block_{index}_mix')};",
            ])
        elif kind == "highpass":
            lines.extend([
                f"            const float block_{index}_dry = sample;",
                f"            const float block_{index}_wet =",
                f"                context->block_{index}_coefficient *",
                f"                (context->block_{index}_previous_output[channel] +",
                f"                 sample - context->block_{index}_previous_input[channel]);",
                f"            context->block_{index}_previous_input[channel] = sample;",
                f"            context->block_{index}_previous_output[channel] = block_{index}_wet;",
                f"            sample = block_{index}_dry +",
                f"                (block_{index}_wet - block_{index}_dry) *",
                f"                {_scaled(kind, 'mix', f'context->block_{index}_mix')};",
            ])
        elif kind == "soft_clip":
            lines.extend([
                f"            const float block_{index}_dry = sample;",
                f"            const float block_{index}_driven =",
                f"                sample * context->block_{index}_drive;",
                f"            const float block_{index}_wet =",
                f"                (block_{index}_driven /",
                f"                 (1.0F + visual_absolute(block_{index}_driven))) *",
                f"                context->block_{index}_level;",
                f"            sample = block_{index}_dry +",
                f"                (block_{index}_wet - block_{index}_dry) *",
                f"                {_scaled(kind, 'mix', f'context->block_{index}_mix')};",
            ])
        elif kind == "hard_clip":
            lines.extend([
                f"            const float block_{index}_dry = sample;",
                f"            float block_{index}_wet = sample;",
                f"            if (block_{index}_wet > context->block_{index}_threshold) {{",
                f"                block_{index}_wet = context->block_{index}_threshold;",
                f"            }} else if (block_{index}_wet < -context->block_{index}_threshold) {{",
                f"                block_{index}_wet = -context->block_{index}_threshold;",
                "            }",
                f"            sample = block_{index}_dry +",
                f"                (block_{index}_wet - block_{index}_dry) *",
                f"                {_scaled(kind, 'mix', f'context->block_{index}_mix')};",
            ])
        elif kind == "gate":
            lines.extend([
                f"            if (visual_absolute(sample) < context->block_{index}_threshold) {{",
                f"                sample *= {_scaled(kind, 'floor', f'context->block_{index}_floor')};",
                "            }",
            ])
        elif kind == "tremolo":
            lines.extend([
                f"            float block_{index}_phase = context->block_{index}_phase[channel];",
                f"            const float block_{index}_triangle =",
                f"                block_{index}_phase < 0.5F",
                f"                    ? block_{index}_phase * 4.0F - 1.0F",
                f"                    : 3.0F - block_{index}_phase * 4.0F;",
                f"            const float block_{index}_depth =",
                f"                {_scaled(kind, 'depth', f'context->block_{index}_depth')};",
                f"            sample *= 1.0F - block_{index}_depth +",
                f"                block_{index}_depth * (0.5F + 0.5F * block_{index}_triangle);",
                f"            block_{index}_phase += context->block_{index}_increment;",
                f"            if (block_{index}_phase >= 1.0F) block_{index}_phase -= 1.0F;",
                f"            context->block_{index}_phase[channel] = block_{index}_phase;",
            ])
        elif kind == "short_delay":
            lines.extend([
                f"            const uint32_t block_{index}_write =",
                f"                context->block_{index}_write[channel];",
                f"            const uint32_t block_{index}_read =",
                f"                (block_{index}_write + {macro}_DELAY_SAMPLES -",
                f"                 context->block_{index}_frames) % {macro}_DELAY_SAMPLES;",
                f"            const float block_{index}_delayed =",
                f"                context->block_{index}_buffer[channel][block_{index}_read];",
                f"            context->block_{index}_buffer[channel][block_{index}_write] =",
                f"                sample + block_{index}_delayed *",
                f"                {_scaled(kind, 'feedback', f'context->block_{index}_feedback')};",
                f"            sample += (block_{index}_delayed - sample) *",
                f"                {_scaled(kind, 'mix', f'context->block_{index}_mix')};",
                f"            context->block_{index}_write[channel] =",
                f"                (block_{index}_write + UINT32_C(1)) % {macro}_DELAY_SAMPLES;",
            ])
        elif kind == "swell":
            lines.extend([
                f"            const float block_{index}_magnitude = visual_absolute(sample);",
                f"            const float block_{index}_envelope_coefficient =",
                f"                block_{index}_magnitude > context->block_{index}_envelope[channel]",
                f"                    ? 0.08F",
                f"                    : context->block_{index}_release_coefficient;",
                f"            context->block_{index}_envelope[channel] +=",
                f"                block_{index}_envelope_coefficient *",
                f"                (block_{index}_magnitude - context->block_{index}_envelope[channel]);",
                f"            const float block_{index}_target =",
                f"                context->block_{index}_envelope[channel] >=",
                f"                    context->block_{index}_sensitivity ? 1.0F : 0.0F;",
                f"            const float block_{index}_gain_coefficient =",
                f"                block_{index}_target > context->block_{index}_gain[channel]",
                f"                    ? context->block_{index}_attack_coefficient",
                f"                    : context->block_{index}_release_coefficient;",
                f"            context->block_{index}_gain[channel] +=",
                f"                block_{index}_gain_coefficient *",
                f"                (block_{index}_target - context->block_{index}_gain[channel]);",
                f"            sample *= context->block_{index}_gain[channel];",
            ])
        elif kind == "rectifier":
            lines.extend([
                f"            const float block_{index}_dry = sample;",
                f"            const float block_{index}_wet = visual_absolute(sample);",
                f"            sample = block_{index}_dry +",
                f"                (block_{index}_wet - block_{index}_dry) *",
                f"                {_scaled(kind, 'mix', f'context->block_{index}_mix')};",
            ])
        elif kind == "limiter":
            lines.extend([
                f"            if (sample > context->block_{index}_ceiling) {{",
                f"                sample = context->block_{index}_ceiling;",
                f"            }} else if (sample < -context->block_{index}_ceiling) {{",
                f"                sample = -context->block_{index}_ceiling;",
                "            }",
            ])
    lines.extend([
        "            block->channels[channel][frame] = sample;",
        "        }",
        "    }",
        "    return EFFECT_RUNTIME_OK;",
        "}",
        "",
        f"static uint16_t {ident}_set_parameter(",
        "    void *opaque,",
        "    uint32_t parameter_id,",
        "    float value)",
        "{",
        f"    {ident}_context_t *context = ({ident}_context_t *)opaque;",
        "",
    ])
    for sequence, (_number_id, index, control, parameter_macro) in enumerate(parameters):
        prefix = "if" if sequence == 0 else "else if"
        lines.extend([
            f"    {prefix} (parameter_id == {parameter_macro}) {{",
            f"        context->{_field(index, control['key'])} = value;",
        ])
        kind = definitions[index]["kind"]
        key = control["key"]
        if kind == "lowpass" and key == "cutoff":
            lines.extend([
                f"        context->block_{index}_coefficient = value /",
                f"            (value + 0.15915494F * (float)context->sample_rate);",
            ])
        elif kind == "highpass" and key == "cutoff":
            lines.extend([
                f"        context->block_{index}_coefficient =",
                f"            (float)context->sample_rate /",
                f"            ((float)context->sample_rate + 6.28318531F * value);",
            ])
        elif kind == "tremolo" and key == "rate":
            lines.append(
                f"        context->block_{index}_increment = value / "
                f"(float)context->sample_rate;"
            )
        elif kind == "short_delay" and key == "time":
            lines.extend([
                f"        context->block_{index}_frames = (uint32_t)(",
                "            value * (float)context->sample_rate / 1000.0F);",
                f"        if (context->block_{index}_frames >= {macro}_DELAY_SAMPLES) {{",
                f"            context->block_{index}_frames =",
                f"                {macro}_DELAY_SAMPLES - UINT32_C(1);",
                "        }",
                f"        if (context->block_{index}_frames == UINT32_C(0)) {{",
                f"            context->block_{index}_frames = UINT32_C(1);",
                "        }",
            ])
        elif kind == "swell" and key == "attack":
            lines.extend([
                f"        context->block_{index}_attack_coefficient = 1000.0F /",
                f"            (value * (float)context->sample_rate);",
            ])
        elif kind == "swell" and key == "release":
            lines.extend([
                f"        context->block_{index}_release_coefficient = 1000.0F /",
                f"            (value * (float)context->sample_rate);",
            ])
        lines.append("    }")
    lines.extend([
        "    else {",
        "        return EFFECT_RUNTIME_PARAMETER_NOT_FOUND;",
        "    }",
        "    return EFFECT_RUNTIME_OK;",
        "}",
        "",
        f"static const effect_parameter_descriptor_t {ident}_parameters[] = {{",
    ])
    for number, index, control, parameter_macro in parameters:
        default = blocks[index]["values"][control["key"]]
        lines.extend([
            "    {",
            f"        {parameter_macro},",
            f'        "{index + 1} · {codegen.escape_c_string(control["name"])}",',
            f'        "{codegen.escape_c_string(control["unit"])}",',
            f"        {codegen.format_float(control['minimum'])},",
            f"        {codegen.format_float(control['maximum'])},",
            f"        {codegen.format_float(default)},",
            "    },",
        ])
    lines.extend([
        "};",
        "",
        f"const effect_descriptor_t {symbol} = {{",
        "    .key = {",
        "        EFFECT_VENDOR_OPEN,",
        f"        EFFECT_OPEN_{macro}_ID,",
        "    },",
        f'    .name = "{codegen.escape_c_string(name)}",',
        "    .abi_version = EFFECT_RUNTIME_ABI_VERSION,",
        "    .parameter_count = (uint16_t)(",
        f"        sizeof({ident}_parameters) / sizeof({ident}_parameters[0])),",
        f"    .parameters = {ident}_parameters,",
        f"    .context_size = sizeof({ident}_context_t),",
        f"    .context_alignment = _Alignof({ident}_context_t),",
        f"    .initialize = {ident}_initialize,",
        f"    .reset = {ident}_reset,",
        f"    .process = {ident}_process,",
        f"    .set_parameter = {ident}_set_parameter,",
        "};",
        "",
    ])
    return {
        "name": name,
        "effect_id": effect_id,
        "file_name": file_name,
        "symbol": symbol,
        "text": "\n".join(lines),
        "spec": spec,
    }
