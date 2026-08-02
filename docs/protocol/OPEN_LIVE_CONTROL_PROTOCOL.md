# Open application live-control protocol

Status: design target; not implemented by the current hardware application.

## Purpose and audio path

Live control does not need to stream audio over USB. The instrument remains
connected to the pedal's analog input and the amplifier or audio interface to
its analog output. The running application owns SAI/eDMA and processes that
audio with device latency; a browser WebHID connection carries only engine,
effect, and parameter changes.

Open Recover is intentionally the wrong transport. Recovery owns flash update
and rollback while application audio is stopped. The normal application must
enumerate a separate project-owned USB identity and expose a versioned vendor
HID interface without entering recovery or resetting the processor.

```text
guitar -> ADC -> active DSP program -> DAC -> amplifier
                         ^
                         |
browser editor -> WebHID control mailbox
```

## First useful command set

The control interface should retain the recovery protocol's fixed-size,
checksummed packets and replay-safe sequence numbers, but use its own magic and
device identity. The first version needs only:

| Command | Direction | Meaning |
|---|---|---|
| `GET_CAPABILITIES` | device → host | Protocol version, four open slots, eight positions, parameter limits, and build identity |
| `GET_STATE` | device → host | Active engine/effect, bypass state, physical knob values, and current parameter values |
| `SELECT_ENGINE` | host → device | Select one of already-installed open engines 5–8 |
| `SELECT_EFFECT` | host → device | Select one of the active engine's eight effects |
| `SET_PARAMETER` | host → device | Set one validated parameter on the active program |
| `SET_BYPASS` | host → device | Match the footswitch's normal bypass operation |
| `GET_METERS` | device → host | Optional peak, clipping, and DSP deadline counters |

Selecting an installed engine/effect should use the same click-free ramp and
state reset as the physical Type selector. USB does not replace the knobs or
footswitch: `GET_STATE` reports later physical changes, and a physical move
wins unless a future explicit control-lock mode is added.

## Real-time boundary

The USB callback must never call DSP code, allocate, log, wait, or mutate an
active effect context. It validates packet shape and writes a small single-
producer command mailbox. At an audio block boundary the application:

1. validates the requested engine/effect/parameter against the installed
   catalog;
2. prepares inactive state when a program changes;
3. applies bounded parameters or begins the existing crossfade; and
4. publishes the resulting status for the next HID input report.

This permits live changes while keeping USB timing out of the audio interrupt.
Parameter smoothing belongs to each effect for controls whose discontinuity
would click.

## Installing authored effects

Version 1 live control selects and tunes effects that are already compiled into
the application slot. It does **not** upload arbitrary native C while audio is
running. The visual editor generates reviewed source and an application image;
the existing bounded Web DFU flow installs that image to the inactive A/B slot,
then reboots it as a trial.

A future separately signed data-driven DSP format could support hot-installed
effect records. That requires an instruction budget, bounded state declaration,
format validation, and atomic inactive-bank commit before it is safe. Native
code loading and flash erase from the live audio path are explicitly out of
scope.

## Browser-side live preview

Continuous host-only preview is a separate feature. It should compile the same
generated DSP source to WebAssembly and run it in an `AudioWorklet` fed by a
guitar audio interface. That gives useful drafting before a pedal build, while
WebHID controls the authoritative device rendition after installation. A
record-then-render preview remains valuable because it executes the native
firmware runtime exactly and can produce deterministic measurements.
