# Extensible effect and program runtime

The open application is not modeled after the factory's four mutually
exclusive engine images. Those images proved the hardware can run several
effect families, but they are not an architectural limit.

## Vocabulary

- An **effect** is one reusable DSP unit such as a filter, compressor, drive,
  delay, chorus, pitch shifter, convolution block, or utility mixer.
- An **instance** is one effect plus its parameter and signal-history state.
- A **program** is a complete playable configuration: an ordered chain
  initially, and later a validated graph with splits, parallel paths, and
  mixers.
- A **bank** is a user-facing collection of programs.

Delay, Reverb, Modulation, and Drive will be useful early programs and test
categories. They are neither reserved slots nor the complete product.

## Effect ABI

`firmware/app/include/effect_runtime.h` defines a small source-level ABI:

- stable `(vendor_id, effect_id)` identity;
- ABI version and human-readable name;
- context size and alignment;
- initialize, reset, in-place process, and parameter callbacks; and
- explicit sample rate and maximum block size at initialization.

The registry is a caller-owned array of descriptor pointers. It has no
compiled-in effect-count limit. Duplicate keys, malformed descriptors, and
unsupported ABI versions are rejected before audio starts.

The current processor is an ordered in-place chain. Its instance array and
context arena are supplied by the application, so board builds choose their
own capacity and memory placement. Adding a graph planner later does not
change individual effect descriptors.

## Real-time rules

The runtime:

- performs no dynamic allocation;
- performs no flash access;
- performs no USB, logging, formatting, or locking;
- validates all nodes and memory before processing;
- calls effects in deterministic order; and
- stops a block on the first explicit callback error.

Large state such as reverb and delay memory can be allocated from an SDRAM
arena during program preparation. Hot state and kernels can use DTCM/ITCM.
Program activation must finish before the audio crossfade begins.

## Programs and navigation

`program_selector` accepts a runtime program count and current index. It is
not aware of effect types or banks. The current default binding is:

1. hold the assigned navigation switch continuously for 5 seconds;
2. advance to the next program;
3. latch so one hold produces exactly one change; and
4. require 50 ms of stable release before rearming.

Those timings and the resulting command are policy, not ABI. A future editor
or on-device mapping can bind tap, double-tap, hold, MIDI, or expression
events to next/previous program, bank selection, effect bypass, tap tempo, or
parameter control.

No program selection writes NOR. Preset persistence is a separate,
power-loss-safe transaction and never occurs in the audio path.

## Growth path

The first source effects should establish reusable test infrastructure, not
a closed list:

1. gain, mix, filters, and soft clipping;
2. delay-line primitives and modulation;
3. dynamics and drive models;
4. algorithmic reverb;
5. pitch/time effects and multi-band routing;
6. convolution or capture-based effects where CPU and SDRAM budgets permit;
7. community effects registered under their own vendor IDs.

Every program build will eventually report DTCM, SDRAM, CPU-cycle, and
latency budgets before it can become active.
