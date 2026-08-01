"""Preview the hardware application's fixed-point presets in the editor.

`firmware/hardware_app/src/main.c` implements its eight panel presets as
per-sample fixed-point code with file-scope state, not as
`effect_descriptor_t` effects. It cannot be registered directly, and it
cannot be compiled on the host either: the same file initializes SAI,
eDMA, GPIO, and the watchdog through NXP SDK headers.

Its DSP is, however, a self-contained region of integer C. This module
lifts exactly that region out of the file at build time and wraps each
preset in an ABI adapter, so the editor previews the algorithms the
firmware actually runs.

Nothing here modifies `main.c`. The extraction is strict: if the region
stops matching what it expects — a renamed function, a missing preset,
state that moved — it raises rather than quietly previewing something
that is no longer the firmware's code.

Two documented differences from the device build:

- section attributes are stripped, so the delay line lives in ordinary
  host memory instead of SDRAM; and
- the effect ramp starts complete, because a preview restarts the
  process on every render and would otherwise always begin mid-fade.
"""

from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Any


ROOT = Path(__file__).resolve().parents[2]
MAIN_SOURCE = ROOT / "firmware" / "hardware_app" / "src" / "main.c"
FACTORY_INCLUDE = ROOT / "firmware" / "factory_slot" / "include"

ADAPTER_SOURCE_NAME = "editor_hardware_app.c"
EFFECT_ID_BASE = 0x0B000001
VENDOR_OPEN = 0x4F50454E
SAMPLE_RATE_HZ = 48000
KNOB_ADC_MAX = 4095

PRESET_ENUM_NAMES = (
    "NCR2_EFFECT_SHINE_DRIVE",
    "NCR2_EFFECT_WALL_FUZZ",
    "NCR2_EFFECT_BREATHE_VIBE",
    "NCR2_EFFECT_ECHOES_TAPE",
    "NCR2_EFFECT_RAGE_DRIVE",
    "NCR2_EFFECT_COCKED_WAH",
    "NCR2_EFFECT_GUERRILLA_TREM",
    "NCR2_EFFECT_WHAMMY_FUZZ",
)

PRESET_NAMES = (
    "Shine Drive",
    "Wall Fuzz",
    "Breathe Vibe",
    "Echoes Tape",
    "Rage Drive",
    "Cocked Wah",
    "Guerrilla Trem",
    "Whammy Fuzz",
)

# The DSP region, in the order the generated unit needs them.
REQUIRED_FUNCTIONS = (
    "capture_frame",
    "clamp_knob",
    "clamp_symmetric",
    "phase_increment",
    "triangle_q15",
    "highpass_input",
    "lowpass_sample",
    "blend_samples_q15",
    "shape_drive",
    "reset_transient_effect_state",
    "initialize_effect_processor",
    "update_effect_selection",
    "make_effect_parameters",
    "process_selected_effect",
    "apply_effect_output",
    "ncr2_factory_audio_process_block",
)

DEFINE_PATTERN = re.compile(r"^[ \t]*#[ \t]*define[ \t]+(NCR2_\w+)", re.M)
GLOBAL_PATTERN = re.compile(
    r"^(?:static[ \t]+)?(?:volatile[ \t]+)?(?:static[ \t]+)?"
    r"(?:u?int(?:8|16|32|64)_t|size_t)[ \t]+"
    r"(g_\w+)\s*(?:\[[^\]]*\])?[^;]*;",
    re.M | re.S,
)
SECTION_ATTRIBUTE_PATTERN = re.compile(
    r"__attribute__\s*\(\(\s*section\s*\([^)]*\)\s*,?\s*"
    r"(aligned\s*\(\s*\d+\s*\))?\s*\)\)"
)
IDENTIFIER_PATTERN = re.compile(r"[A-Za-z_]\w*")

C_KEYWORDS = frozenset(
    {
        "sizeof", "int", "unsigned", "signed", "char", "short", "long",
        "float", "double", "void", "const", "volatile", "static",
        "struct", "union", "enum", "typedef", "return", "if", "else",
        "for", "while", "switch", "case", "break", "continue", "default",
        "do", "goto", "extern", "inline", "restrict", "_Bool",
        "UINT8_C", "UINT16_C", "UINT32_C", "UINT64_C", "INT8_C",
        "INT16_C", "INT32_C", "INT64_C", "INT32_MIN", "INT32_MAX",
        "uint8_t", "uint16_t", "uint32_t", "uint64_t", "int8_t",
        "int16_t", "int32_t", "int64_t", "size_t",
    }
)


class ExtractionError(RuntimeError):
    """Raised when main.c no longer matches the expected DSP region."""


@dataclass(frozen=True)
class Preset:
    index: int
    name: str
    effect_id: int
    identifier: str


def presets() -> tuple[Preset, ...]:
    return tuple(
        Preset(
            index=index,
            name=name,
            effect_id=EFFECT_ID_BASE + index,
            identifier=re.sub(r"[^a-z0-9]+", "_", name.lower()),
        )
        for index, name in enumerate(PRESET_NAMES)
    )


def preset_keys() -> frozenset[tuple[int, int]]:
    return frozenset(
        (VENDOR_OPEN, preset.effect_id) for preset in presets()
    )


def available() -> bool:
    return MAIN_SOURCE.is_file()


def _block_at(text: str, start: int) -> str:
    """Return the brace-balanced block that begins at or after `start`."""
    opening = text.find("{", start)
    if opening < 0:
        raise ExtractionError("expected a block after position")
    depth = 0
    index = opening
    while index < len(text):
        character = text[index]
        if character == "{":
            depth += 1
        elif character == "}":
            depth -= 1
            if depth == 0:
                end = text.find(";", index)
                end = end + 1 if 0 <= end <= index + 64 else index + 1
                return text[start:end]
        index += 1
    raise ExtractionError("unbalanced braces in extracted block")


def _extract_function(text: str, name: str) -> str:
    pattern = re.compile(
        r"^(?:static[ \t]+)?[A-Za-z_][\w ]*?[\w*][ \t\n]*"
        + re.escape(name)
        + r"[ \t\n]*\([^;{]*\)[ \t\n]*\{",
        re.M,
    )
    match = pattern.search(text)
    if match is None:
        raise ExtractionError(
            f"{name}() is no longer where the extractor expects it"
        )
    return _block_at(text, match.start())


def _extract_defines(text: str) -> tuple[list[str], set[str]]:
    """Return whole `#define NCR2_*` lines, continuations included."""
    lines = text.splitlines()
    collected: list[str] = []
    names: set[str] = set()
    index = 0
    while index < len(lines):
        match = DEFINE_PATTERN.match(lines[index])
        if match is not None:
            names.add(match.group(1))
            block = [lines[index]]
            while block[-1].rstrip().endswith("\\") and index + 1 < len(lines):
                index += 1
                block.append(lines[index])
            collected.extend(block)
        index += 1
    return collected, names


def _extract_globals(
    text: str,
    known: set[str],
    used_in: str,
) -> list[str]:
    """Take the integer file-scope state the extracted DSP refers to.

    Only names the extracted functions actually use are kept, so board
    and codec state stays behind and the generated unit compiles without
    unused-variable noise. A declaration whose initializer reaches
    outside the extracted defines is skipped for the same reason; if the
    DSP genuinely needed it, the unit fails to compile and the caller
    hears about it rather than getting silence.
    """
    collected: list[str] = []
    for match in GLOBAL_PATTERN.finditer(text):
        declaration = match.group(0)
        name = match.group(1)
        if not re.search(rf"\b{re.escape(name)}\b", used_in):
            continue
        initializer = declaration.split("=", 1)[1] if "=" in declaration else ""
        unknown = [
            identifier
            for identifier in IDENTIFIER_PATTERN.findall(initializer)
            if identifier not in known and identifier not in C_KEYWORDS
        ]
        if unknown:
            continue
        collected.append(SECTION_ATTRIBUTE_PATTERN.sub("", declaration))
    return collected


def _extract_types(text: str) -> list[str]:
    blocks: list[str] = []

    enum_start = text.find("enum")
    while enum_start >= 0:
        block = _block_at(text, enum_start)
        if PRESET_ENUM_NAMES[0] in block:
            blocks.append(block)
            break
        enum_start = text.find("enum", enum_start + 4)
    else:
        raise ExtractionError("the preset enumeration was not found")

    typedef_start = text.find("typedef struct ncr2_effect_parameters")
    if typedef_start < 0:
        raise ExtractionError("ncr2_effect_parameters_t was not found")
    blocks.append(_block_at(text, typedef_start))
    return blocks


def extract_dsp(source: Path | None = None) -> str:
    """Lift the fixed-point DSP region out of the hardware application."""
    path = Path(source) if source is not None else MAIN_SOURCE
    if not path.is_file():
        raise ExtractionError(f"{path} is not available")
    text = path.read_text()

    for name in PRESET_ENUM_NAMES:
        if name not in text:
            raise ExtractionError(f"preset {name} is missing from main.c")

    defines, known = _extract_defines(text)
    functions = [
        SECTION_ATTRIBUTE_PATTERN.sub("", _extract_function(text, name))
        for name in REQUIRED_FUNCTIONS
    ]
    bodies = "\n".join(functions)

    parts = [
        "/* Extracted from firmware/hardware_app/src/main.c. */",
        "",
        *defines,
        "",
        *_extract_types(text),
        "",
        *_extract_globals(text, known, bodies),
        "",
    ]
    for function in functions:
        parts.append(function)
        parts.append("")
    return "\n".join(parts)


ADAPTER_PROLOGUE = '''/*
 * Generated by host/editor. The DSP below is the hardware application's
 * own fixed-point code; only the ABI wrapper is new.
 */

#include <stddef.h>
#include <stdint.h>

#include "factory_audio.h"
#include "effect_runtime.h"

'''

ADAPTER_BRIDGE = '''
/* ---- editor adapter ------------------------------------------- */

#define HARDWARE_APP_MAX_FRAMES 512U

typedef struct hardware_app_context {
    uint32_t preset;
    uint32_t amount;
    uint32_t character;
    uint32_t output;
} hardware_app_context_t;

#define HARDWARE_APP_PARAMETER_AMOUNT UINT32_C(1)
#define HARDWARE_APP_PARAMETER_CHARACTER UINT32_C(2)
#define HARDWARE_APP_PARAMETER_OUTPUT UINT32_C(3)

static uint32_t g_hardware_app_adapter_instances;
static int32_t g_hardware_app_adapter_input[
    HARDWARE_APP_MAX_FRAMES * NCR2_FACTORY_AUDIO_SLOTS];
static int32_t g_hardware_app_adapter_output[
    HARDWARE_APP_MAX_FRAMES * NCR2_FACTORY_AUDIO_SLOTS];

static uint16_t hardware_app_prepare(
    void *opaque,
    uint32_t sample_rate,
    uint32_t maximum_block_frames,
    uint32_t preset)
{
    hardware_app_context_t *context =
        (hardware_app_context_t *)opaque;

    /* Every modulation rate in these presets is derived from the
       device sample rate, so a preview at another rate would be a
       different effect. */
    if (sample_rate !=
        (uint32_t)NCR2_FACTORY_AUDIO_SAMPLE_RATE_HZ) {
        return UINT16_C(1);
    }
    if (maximum_block_frames > HARDWARE_APP_MAX_FRAMES) {
        return UINT16_C(1);
    }
    /* The presets keep their state at file scope, exactly as the pedal
       does; two instances in one chain would share it. */
    if (g_hardware_app_adapter_instances != UINT32_C(0)) {
        return UINT16_C(1);
    }
    ++g_hardware_app_adapter_instances;

    context->preset = preset;
    context->amount = UINT32_C(2048);
    context->character = UINT32_C(2048);
    context->output = UINT32_C(4095);

    g_hardware_app_effect_index = preset;
    g_hardware_app_enable_effect = UINT32_C(1);
    g_hardware_app_emit_tone = UINT32_C(0);
    initialize_effect_processor();
    /* A render restarts the process, so hold the enable ramp complete
       instead of fading in on every preview. */
    g_effect_ramp = NCR2_PARAMETER_Q15_ONE;
    return EFFECT_RUNTIME_OK;
}

static void hardware_app_reset(void *opaque)
{
    (void)opaque;
    reset_transient_effect_state();
}

static uint16_t hardware_app_process(
    void *opaque,
    effect_audio_block_t *block)
{
    hardware_app_context_t *context =
        (hardware_app_context_t *)opaque;
    const float input_scale = (float)NCR2_SAFE_OUTPUT_PEAK;
    const float output_scale = (float)NCR2_DAC_OUTPUT_PEAK;

    if (block->frame_count > HARDWARE_APP_MAX_FRAMES) {
        return EFFECT_RUNTIME_INVALID_ARGUMENT;
    }

    g_hardware_app_knob_amount = context->amount;
    g_hardware_app_knob_character = context->character;
    g_hardware_app_knob_output = context->output;
    g_hardware_app_effect_index = context->preset;

    for (uint32_t frame = UINT32_C(0);
         frame < block->frame_count;
         ++frame) {
        float mixed = 0.0F;
        int64_t fixed;

        for (uint8_t channel = UINT8_C(0);
             channel < block->channel_count;
             ++channel) {
            mixed += block->channels[channel][frame];
        }
        mixed /= (float)block->channel_count;
        fixed = (int64_t)(mixed * input_scale);
        if (fixed > (int64_t)NCR2_SAFE_OUTPUT_PEAK) {
            fixed = (int64_t)NCR2_SAFE_OUTPUT_PEAK;
        } else if (fixed < -(int64_t)NCR2_SAFE_OUTPUT_PEAK) {
            fixed = -(int64_t)NCR2_SAFE_OUTPUT_PEAK;
        }
        for (size_t slot = 0U;
             slot < (size_t)NCR2_FACTORY_AUDIO_SLOTS;
             ++slot) {
            g_hardware_app_adapter_input
                [(size_t)frame * NCR2_FACTORY_AUDIO_SLOTS + slot] =
                    (int32_t)fixed;
        }
    }

    ncr2_factory_audio_process_block(
        g_hardware_app_adapter_input,
        g_hardware_app_adapter_output,
        (size_t)block->frame_count);

    for (uint32_t frame = UINT32_C(0);
         frame < block->frame_count;
         ++frame) {
        const float value =
            (float)g_hardware_app_adapter_output
                [(size_t)frame * NCR2_FACTORY_AUDIO_SLOTS] /
            output_scale;

        for (uint8_t channel = UINT8_C(0);
             channel < block->channel_count;
             ++channel) {
            block->channels[channel][frame] = value;
        }
    }
    return EFFECT_RUNTIME_OK;
}

static uint16_t hardware_app_set_parameter(
    void *opaque,
    uint32_t parameter_id,
    float value)
{
    hardware_app_context_t *context =
        (hardware_app_context_t *)opaque;
    const uint32_t counts = (uint32_t)(value + 0.5F);

    if (parameter_id == HARDWARE_APP_PARAMETER_AMOUNT) {
        context->amount = counts;
    } else if (parameter_id == HARDWARE_APP_PARAMETER_CHARACTER) {
        context->character = counts;
    } else if (parameter_id == HARDWARE_APP_PARAMETER_OUTPUT) {
        context->output = counts;
    } else {
        return EFFECT_RUNTIME_PARAMETER_NOT_FOUND;
    }
    return EFFECT_RUNTIME_OK;
}

static const effect_parameter_descriptor_t
hardware_app_parameters[] = {
    {
        HARDWARE_APP_PARAMETER_AMOUNT,
        "Amount",
        "adc",
        0.0F,
        4095.0F,
        2048.0F,
    },
    {
        HARDWARE_APP_PARAMETER_CHARACTER,
        "Character",
        "adc",
        0.0F,
        4095.0F,
        2048.0F,
    },
    {
        HARDWARE_APP_PARAMETER_OUTPUT,
        "Level",
        "adc",
        0.0F,
        4095.0F,
        4095.0F,
    },
};

#define HARDWARE_APP_PRESET(identifier, preset_index, effect_id, label) \\
static uint16_t identifier##_initialize(                                \\
    void *opaque,                                                       \\
    uint32_t sample_rate,                                               \\
    uint32_t maximum_block_frames)                                      \\
{                                                                       \\
    return hardware_app_prepare(                                        \\
        opaque, sample_rate, maximum_block_frames, preset_index);       \\
}                                                                       \\
                                                                        \\
const effect_descriptor_t ncr2_effect_hw_##identifier = {               \\
    .key = { EFFECT_VENDOR_OPEN, effect_id },                           \\
    .name = label,                                                      \\
    .abi_version = EFFECT_RUNTIME_ABI_VERSION,                          \\
    .parameter_count =                                                  \\
        sizeof(hardware_app_parameters) /                               \\
            sizeof(hardware_app_parameters[0]),                         \\
    .parameters = hardware_app_parameters,                              \\
    .context_size = sizeof(hardware_app_context_t),                     \\
    .context_alignment = _Alignof(hardware_app_context_t),              \\
    .initialize = identifier##_initialize,                              \\
    .reset = hardware_app_reset,                                        \\
    .process = hardware_app_process,                                    \\
    .set_parameter = hardware_app_set_parameter,                        \\
}

'''


def adapter_source(source: Path | None = None) -> str:
    """Return the complete adapter translation unit."""
    lines = [ADAPTER_PROLOGUE, extract_dsp(source), ADAPTER_BRIDGE]
    for preset in presets():
        lines.append(
            f"HARDWARE_APP_PRESET({preset.identifier}, "
            f"UINT32_C({preset.index}), "
            f"UINT32_C(0x{preset.effect_id:08X}), "
            f'"{preset.name}");'
        )
    lines.append("")
    return "\n".join(lines)


def descriptor_symbols() -> list[str]:
    return [f"ncr2_effect_hw_{preset.identifier}" for preset in presets()]


def describe() -> dict[str, Any]:
    """Session metadata for the page."""
    return {
        "available": available(),
        "source": str(MAIN_SOURCE.relative_to(ROOT)),
        "sample_rate": SAMPLE_RATE_HZ,
        "knob_maximum": KNOB_ADC_MAX,
        "presets": [
            {
                "index": preset.index,
                "name": preset.name,
                "effect_id": preset.effect_id,
                "vendor_id": VENDOR_OPEN,
            }
            for preset in presets()
        ],
    }
