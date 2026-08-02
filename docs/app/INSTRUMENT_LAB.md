# Instrument Lab engine pack

Status: host-preview prototype. It is not part of the v0.24 hardware image.

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

## Signal model

This is expressive resynthesis, not a cosmetic EQ preset. A bounded 4:1
decimator (8:1 at 96 kHz) feeds a 512-sample AMDF pitch tracker. It searches
guitar periods from roughly 65 Hz to 1.4 kHz, selects the first strong match
to avoid octave-down errors, performs fractional-lag interpolation, and
smooths accepted pitch changes. The detected continuous frequency drives each
voice, so bends and vibrato survive instead of being forced onto an
equal-tempered note grid.

An envelope follower carries the player's dynamics into the synthesized
voice. Articulation controls the voice-specific attack and release;
Character changes oscillator balance and filtering; Instrument Mix blends
resynthesis with the original guitar; Tracking Sensitivity sets the input
gate. Oscillators use bounded saw, square, and continuous parabolic-sine
primitives, followed by voice-specific filtering and a soft output bound.

The implementation is allocation-free, uses about 2.3 KiB of effect context,
contains no mutable file-scope DSP state, and needs no `libm` call in the audio
path. The editor executes the firmware C directly. Current host measurements
put a Bowed Ensemble preview at approximately 2–3% of one 48 kHz block
deadline, but Cortex-M7 cycle measurement remains required before device
approval.

## Playing constraints

The first tracker is intentionally monophonic. Single notes, clean muting, and
neck-pickup tone provide the most stable result. Chords do not crash or produce
non-finite output, but the tracker will choose one dominant periodicity rather
than separating every note. Approximately 31 ms of pitch history is needed
before the first confident note, followed by the selected voice's articulation
attack.

High-quality polyphonic guitar-to-instrument conversion is a separate research
track: it requires multi-pitch estimation, onset association, and per-note
voice allocation. That work should not be hidden behind labels that imply
MIDI accuracy the pedal has not demonstrated.

## MIDI and device integration

The current prototype produces audio, not USB MIDI messages. Its continuous
frequency and confidence are the right inputs for a later converter:

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
