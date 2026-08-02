# Instrument Lab engine pack

Status: active recorded-input prototype. It is not part of the v0.24 hardware
image and has not been approved for another device trial.

Instrument Lab adds a fifth, optional eight-program engine without removing
the existing Open Amp Studio, Drive + Dynamics, Motion + Pitch, or Echo +
Space definitions. In Open Effect Lab, choose **Instrument Lab**, choose an
open target slot, and select **Load for preview**. This changes only the
editor bank; the original engines remain in the catalog and can be restored or
loaded into other slots.

The eight initial voices are:

1. Bowed Ensemble
2. Cello
3. Violin
4. Tonewheel Organ
5. Clarinet
6. Synth Brass
7. Synth Bass
8. Bell / Marimba

### Rejected v0.25 hardware trial

The first device integration was rejected after live guitar testing through a
Marshall tube amplifier. Its oscillator voices did not resemble the browser
preview, real-guitar tracking was unstable and harsh, and the physical control
surface could not reproduce the editor's four parameters: Mix and Tracking
Sensitivity were fixed while only Articulation and Character reached panel
knobs. Clean synthetic-tone pitch tests were therefore insufficient evidence
for a playable hardware build. The pedal was restored to v0.24, and another
Instrument Lab image must not be approved without captured-pedal input tests,
on-target deadline measurements, and explicit mappings for every editor
control.

## Rebuilt signal model

This is expressive resynthesis, not a cosmetic EQ preset. A bounded 4:1
decimator, or 8:1 above 64 kHz, feeds a 512-sample tracker. The tracker uses a
YIN-style cumulative normalized difference function over a 192-sample analysis
window. It combines adaptive noise-floor gating, attack detection, octave
continuity, and confirmation of large jumps before changing the stable pitch.
The detected continuous frequency still carries bends and vibrato rather than
forcing every sample onto an equal-tempered note grid.

The resynthesizer no longer treats every instrument as a differently filtered
saw or square oscillator. Bowed Ensemble, Cello, Violin, and Clarinet use a
shared bounded waveguide with voice-specific excitation and damping. The
waveguide is driven primarily by shaped noise and the player's envelope; only
a small pickup-waveform component remains for articulation. Organ, Brass,
Bass, and Bell / Marimba use voice-specific additive partial models and
percussive envelopes. The organ has a drawbar-like sub/fifth spectrum rather
than reusing the brass harmonic series. Each voice has its own attack, release,
excitation, partial balance, output trim, and speaker-friendly filter.

The panel-facing controls are:

- **Transformation**: a meaningful near-dry-to-resynthesized sweep, defaulting
  to complete transformation;
- **Character**: voice-specific brightness, bow pressure, register, or strike;
- **Instrument Mix**: the explicit dry/resynthesized balance; and
- **Tracking Sensitivity**: the input gate relative to the learned noise floor.

The implementation is allocation-free, uses approximately 6.2 KiB of effect
context, contains no mutable file-scope DSP state, and needs no `libm` call in
the audio path. The editor executes this firmware C directly in offline and
live-input preview. Cortex-M7 cycle measurement remains required before device
approval.

## Acceptance evidence

The first trustworthy pedal recording exposed another failed assumption: the
previous defaults changed samples but retained enough dry guitar to make the
whole pack perceptually ineffective. Instrument Mix and Transformation now
default to fully wet, sensitivity is matched to recorded pedal input, and saved
slots using the exact old defaults are migrated automatically. The pickup no
longer dominates the string waveguide excitation.

The bundled raw DI fixture is not normalized and level matching is off for
Instrument Lab. Across all eight voices the current default RMS level is about
-1 dB to +1.2 dB relative to dry, with peaks below 0.35. Automated tests require
all eight renders to be finite and level-safe. They also subtract the best-fit
dry signal and require the resynthesized component to dominate every default,
limit pairwise voice correlation so different labels cannot hide the same
oscillator, and require both Transformation and Character to make material
audible changes.

Those host gates do not constitute hardware approval. Before another slot is
flashed, the rebuilt version still needs:

1. live review using the editor's audio-interface path with representative
   clean guitar level;
2. a captured pedal-ADC fixture to expose any input scaling difference;
3. on-target Cortex-M7 deadline and arena measurements; and
4. an explicit mapping for all four controls on the physical panel.

## Playing constraints

The first tracker is intentionally monophonic. Single notes, clean muting, and
neck-pickup tone provide the most stable result. Chords do not crash or produce
non-finite output, but the tracker will choose one dominant periodicity rather
than separating every note. Approximately 25–40 ms of pitch history is needed
before the first confident note, followed by the selected voice's attack.

High-quality polyphonic guitar-to-instrument conversion is a separate research
track: it requires multi-pitch estimation, onset association, and per-note
voice allocation. That work should not be hidden behind labels that imply
MIDI accuracy the pedal has not demonstrated.

## MIDI and device integration

The current prototype produces audio, not USB MIDI messages. It now exposes a
read-only note-state record containing the nearest MIDI note, residual pitch
bend, velocity, frequency, confidence, gate state, and onset count. That state
is the foundation for a later converter:

- quantize a stable onset to the nearest MIDI note;
- send pitch bend for the residual continuous offset;
- derive velocity from the attack envelope;
- send note-off only after confidence and envelope hysteresis agree; and
- keep audio resynthesis available when no USB host is connected.

Installing more than four open engines without discarding the larger catalog
depends on the normal-application control plane described in
[OPEN_LIVE_CONTROL_PROTOCOL.md](../protocol/OPEN_LIVE_CONTROL_PROTOCOL.md).
The application can carry many registered engine packs, while the four
physical open positions are assignments. WebHID changes those assignments or
selects a non-panel engine in RAM at an audio block boundary. Recovery DFU
continues to install code only to an inactive A/B application slot.
