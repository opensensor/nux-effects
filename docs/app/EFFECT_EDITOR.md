# Host effect design and preview page

`host/editor` is a local page for composing programs from the effect
registry, writing new effects in C, and hearing the result before any
image is built or flashed. It is Phase 7 host tooling from
[PROGRAMMABLE_PEDAL_PLAN.md](../../PROGRAMMABLE_PEDAL_PLAN.md), not part
of any firmware image.

    python3 host/editor/server.py --open

## What the preview actually runs

Every previewed sample is produced by the firmware's own sources. A
session is compiled from:

- `firmware/app/src/effect_runtime.c`;
- `firmware/app/src/program_runtime.c`;
- `firmware/app/src/effects_basic.c`;
- the effect sources authored in the page; and
- a generated configuration unit that binds them into an
  `effect_registry_t`, a `program_descriptor_t`, a catalog, and a bank.

`host/editor/native/editor_host.c` links those, runs
`program_prepare` and `effect_chain_process` over the submitted audio,
and returns the processed samples with a JSON report. There is no second
DSP implementation to keep in sync: a preview that sounds wrong is a
program or effect that is wrong.

The browser owns only what the device does not: test-signal generation,
playback, and drawing.

## What it measures

The report is the runtime's own answer, not the page's opinion:

- `effect_registry_validate`, `program_catalog_validate`,
  `program_library_validate`, `effect_chain_initialize`, and
  `program_prepare` status codes, named from the firmware headers;
- per-parameter `effect_chain_set_parameter` results, so an out-of-range
  control reports `EFFECT_RUNTIME_PARAMETER_OUT_OF_RANGE` exactly as it
  would on the device;
- non-finite output samples, peak, and RMS change;
- context arena bytes consumed by the chain; and
- worst-case and mean block time.

Block time is **host-relative**. It is useful for spotting an effect
that is orders of magnitude too slow, and it is not the Cortex-M7 budget
gate; that gate is measured on the device with the audio path running.

An effect source is also scanned for the real-time rules in
[EFFECT_RUNTIME.md](EFFECT_RUNTIME.md) — allocation, formatting, file
access, locking, blocking calls, host headers, mutable file-scope state,
and double-precision maths. The scan is a heuristic that catches common
mistakes early. A clean scan is not a proof of real-time safety.

## Live controls without recompiling

The generated configuration comes in two shapes:

- **render**: the program lists its nodes with no parameter values, and
  the page applies values through `effect_chain_set_parameter`. Moving a
  control reuses the cached binary, so previews stay responsive.
- **validate**: the program descriptor carries its parameter values and
  is checked by `program_catalog_validate` and `program_prepare`, the
  path the device takes.

Both are the same code with the same validation; a host test asserts
they render identical samples for the same settings.

## Previewing the hardware application's presets

`firmware/hardware_app/src/main.c` implements its eight panel presets as
per-sample fixed-point code with file-scope state. They are not
`effect_descriptor_t` effects, and that file cannot be compiled on the
host at all: it also initializes SAI, eDMA, GPIO, and the watchdog
through NXP SDK headers.

Its DSP is nonetheless a self-contained region of integer C. With
**Hardware app presets** enabled, `host/editor/hardware_app.py` lifts
exactly that region out of the file at build time — the `NCR2_*`
constants, the preset enumeration, the parameter struct, the DSP state
the extracted functions actually use, and the chain from
`capture_frame` through `ncr2_factory_audio_process_block` — and wraps
each preset in an ABI adapter. Shine Drive, Wall Fuzz, Breathe Vibe,
Echoes Tape, Rage Drive, Cocked Wah, Guerrilla Trem, and Whammy Fuzz
then appear in the registry with Amount, Character, and Level exposed as
the raw 0–4095 ADC counts the firmware reads.

`main.c` is never modified. The extraction is strict and fails rather
than drifting: a renamed DSP function or a missing preset raises, and
host tests cover both cases. Anything it cannot account for surfaces as
a compile error in the page.

Four properties follow from the code being fixed-point firmware DSP
rather than a registry effect:

- previews are pinned to 48 kHz, because every modulation rate is
  derived from the device sample rate; another rate is refused;
- only one preset can be in a chain, because they share file-scope state
  exactly as the pedal does;
- the signal is summed to mono and fed through all four factory slots,
  matching `capture_frame` and the block writer; and
- these presets **cannot be exported** as an effect chain — a program
  using them is refused with that explanation, because on the device
  they are a `switch` in `main.c`, not registry entries.

Two documented differences from the device build: section attributes are
stripped, so the delay line lives in ordinary host memory instead of
SDRAM, and the effect ramp starts complete, because a preview restarts
the process on every render and would otherwise always begin mid-fade.

## Export

Export emits review-ready text and writes nothing:

- `firmware/app/src/effects_<name>.c` — the authored source as written;
- `firmware/app/include/effects_<name>.h` — identity and parameter
  macros derived from the compiled descriptor; and
- `firmware/app/src/programs_<name>.c` — a `programs_builtin.c`-style
  block for the designed program.

Shipped effects are exported with the macro names `effects_basic.h`
actually defines rather than names inferred from a descriptor string.
A host test compiles the exported files against the firmware include
tree so an export that cannot build fails in CI rather than in review.

Adopting a program still means adding its identity to
`programs_builtin.h`, listing it in a catalog and bank, and registering
any new effect — deliberately manual steps, because they change what the
pedal ships.

## Safety

The server compiles and executes C submitted by the page. That is the
feature, and it is also the risk:

- it binds loopback addresses only and refuses anything else;
- it rejects cross-origin requests and mismatched `Host` headers;
- previews run with CPU, address-space, and file-size limits and a wall
  clock timeout; and
- it never opens a USB device, writes to the repository, or produces a
  flashable image.

Run it on a machine you trust, and treat authored effect sources the way
you would treat any code you are about to execute.
