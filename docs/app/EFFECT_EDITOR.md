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

## Visual effect design

The primary authoring path is a no-code signal chain. A user can start from a
musical recipe, add validated DSP blocks from the categorized palette, reorder
them, and adjust controls that are bounded to firmware-safe ranges. The server
translates that declarative design into an `effect_descriptor_t`; from that
point onward it goes through the same compiler, real-time source checks,
registry, native preview, parameter override, and export path as handwritten C.

Every block control becomes a runtime parameter. Current building blocks cover
level, filtering, drive, gating, envelope swell, tremolo, bounded short delay,
octave texture, and output limiting. The instrument-voice recipes demonstrate
how the same pieces can make bowed-string, organ-octave, and sitar-resonator
starting points. Generated C remains available in a collapsed Advanced drawer,
but understanding or editing it is not required.

The **Additional engine packs** shelf is separate from the four-slot default
bank. Its first pack, [Instrument Lab](INSTRUMENT_LAB.md), supplies eight
pitch-tracked audio-resynthesis voices. Loading it into an editor slot is a
reversible bank assignment and does not remove the original catalog. Device
installation awaits the normal-application engine-assignment control plane;
the page labels the action as preview-only.

The primary listening signal is not synthesized. The page bundles a
six-second, 48 kHz mono excerpt from the CC0 GuitarJam dataset: a clean
electric guitar recorded directly into an audio interface. It preserves real
pick attacks, fret transitions, pickup harmonics, and playing dynamics. The
older physical-model pluck and chord remain explicitly labeled diagnostic
signals, while file capture, six-second raw microphone capture, and live audio
input allow testing a particular guitar and playing style. On Linux, the input
picker automatically prefers **Pedal USB — direct** when the normal-application
firmware is connected. The local server identifies the pedal through ALSA and
captures its exact 48 kHz/24-bit mono stream, so Chromium microphone permission
and device enumeration are not involved. A user can still choose the system
default or any browser audio device explicitly. Browser automatic gain
control, echo cancellation, and noise suppression are requested off for those
fallback inputs, and captured or uploaded audio is not normalized. The Input
trim is therefore an explicit part of the audition. Physical measurement of
the bench pedal found played-guitar peak/RMS about 17 dB below the bundled DI,
so direct pedal capture applies a visible, fixed +18 dB calibration before the
user's Input trim. This puts live input in the amplitude range used to author
the effects without changing browser-interface or file playback levels.
The server also overrides ALSA's unsuitable 125 ms default period with a
256-frame period and 1,024-frame buffer; this is essential to prevent bursty
capture from masquerading as reverb or heavy output delay.

The explicit **Record input** transport captures 6, 15, 30, or 60 seconds and
then selects that take as the test signal automatically. Recorded audition
bypasses the live scheduler, making a longer loop a practical fallback and a
clean way to distinguish input/DSP quality from live transport behavior.

Level-matched auditioning applies a bounded, peak-safe gain only in the Web
Audio playback mixer. It is off by default, and loading Instrument Lab turns
it off explicitly. The output waveform, spectrum, transfer curve, peak, RMS,
and exported parameter settings always describe the unmodified firmware
render. This keeps dry/wet comparisons honest without concealing an engine
whose device level needs correction.

## Reviewing the 4×8 bank

Each open position carries a review record: Unreviewed, Keep, Tune, or
Replace, plus free-form listening notes. Previous/next controls and keyboard
shortcuts make a complete 32-effect pass practical. The page persists the
bank, parameters, authored sources, reviews, and current position in local
browser storage. Exported `ncr2-open-engine-bank` version 3 JSON contains the
same data plus editable visual designs and their generated-source ownership.
Version 1 and 2 bank files remain importable with empty reviews or visual
designs as appropriate.

Tailwind CSS 4 provides the local interface utility layer, compiled into a
committed static stylesheet so the editor remains network-independent at
runtime. The existing focused CSS still owns charts, DSP parameter controls,
and semantic review colors.

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

The **Play live input** control provides continuous guitar monitoring through
the native firmware C. For the direct pedal source, the local server reads
bounded 1024-frame ALSA chunks, converts packed PCM24 to float32, processes
them in one persistent native process, and returns them for timestamped Web
Audio playback with an 80 ms scheduling cushion. Browser microphone inputs use
the same bounded 1024-frame chunk size. An experimental AudioWorklet ring was
rejected after physical testing: its refill/clock-correction behavior colored
otherwise-clean dry capture.
The process retains tracker, delay, envelope, and oscillator state between
chunks. This is deliberately the same compiled effect chain as offline
preview, not a JavaScript approximation. Moving an effect control replaces the
native session after a short debounce. **Hear dry / Hear processed** also works
during live input while the DSP continues advancing, making transport tone and
effect tone directly comparable without restarting the session.

This localhost round trip is an audition path, not a stage monitor: it has more
latency than the pedal and may drop old chunks rather than build an ever-growing
queue. Use headphones or mute the direct monitor path to prevent acoustic
feedback. Closing the page or pressing **Stop live input** terminates the native
process and releases the audio device.

The intended direct connection is guitar → pedal input and pedal USB-C →
computer. The separately gated normal hardware application now has a UAC1
capture path that presents the pedal's dry 48 kHz/24-bit mono input as
**NCR-2 Open Pedal Audio**; see
[USB_AUDIO.md](../hardware/USB_AUDIO.md). The v0.26.0 bench build has completed
physical high-speed enumeration, sustained ALSA capture, direct editor
recording, and persistent native-DSP transport. Audible played-guitar tone and
latency remain listening checks, and its temporary bench identity must be
replaced by a project-owned USB identity before a release.

Open Recover itself cannot provide live playing: recovery mode deliberately
does not initialize the analog audio engine, and its HID interface carries
commands rather than USB Audio. Letting the page adjust the running pedal DSP
also requires the separate normal-application HID control plane. The proposed
block-boundary mailbox, commands, and safety rules are specified in
[OPEN_LIVE_CONTROL_PROTOCOL.md](../protocol/OPEN_LIVE_CONTROL_PROTOCOL.md).

## Previewing the hardware application's presets

`firmware/hardware_app/src/main.c` implements 32 panel presets, arranged as
four open engines of eight effects, as
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
each preset in an ABI adapter. The registry therefore contains the exact
Open Amp Studio, Drive + Dynamics, Motion + Pitch, and Echo + Space defaults,
with Amount, Character, and Level exposed as the raw 0–4095 ADC counts the
firmware reads.

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
- the preview server never opens a USB device, writes to the repository, or
  silently produces a flashable image.

The separate WebHID updater is intentionally narrower than the host recovery
tool. It accepts only manifest-bearing open `.slot` images, writes only the
inactive A/B application slot, and has no full-flash command implementation.
It checks the Open Recover product name, requires acknowledgement of the
borrowed development USB identity, and relies on the bootloader to verify the
complete stored image again before making it pending.

Run it on a machine you trust, and treat authored effect sources the way
you would treat any code you are about to execute.
