# nux-effects

Open firmware research and development for the NUX Core Deluxe effects-pedal
platform, initially tested on the NCR-2 Reverb Core Deluxe.

The long-term target is a source-level programmable pedal with:

- a recoverable open bootloader;
- A/B application updates over USB;
- live Delay, Reverb, Modulation, and Drive modes;
- mode changes in RAM without flash wear;
- a board-support package for the i.MX RT1051 hardware; and
- reproducible host tools, firmware images, and tests.

The current architecture and phased bring-up plan are in
[PROGRAMMABLE_PEDAL_PLAN.md](PROGRAMMABLE_PEDAL_PLAN.md). The recovered stock
HID updater protocol is documented in [DFU_PROTOCOL.md](DFU_PROTOCOL.md).
The offline source boot chain and build instructions are in
[firmware/README.md](firmware/README.md).

## Current hardware facts

- NXP MIMXRT1051DVL6B Cortex-M7
- 8 MiB external FlexSPI NOR
- 32 MiB ESMT M12L2561616A SDRAM on SEMC
- SAI2/SAI3 with eDMA for the stock audio path
- USB MIDI in normal mode
- 64-byte vendor HID reports in recovery mode

## Artifact policy

This repository intentionally does **not** commit full flash dumps, packaged
NUX firmware, updater executables, generated replacement images, or Ghidra
project databases. Those files may be copyrighted, device-specific, large,
or unsafe to flash without verifying the exact target.

Local tools and tests can use a separately preserved, verified dump named
`dump1.bin`. Its expected SHA-256 is recorded in the documentation and
validation code. Generated flash images remain ignored by Git.

## Safety

Do not flash experimental firmware unless you have:

1. a verified full-chip backup;
2. a tested recovery procedure;
3. reviewed the exact write ranges; and
4. confirmed the target hardware revision.

The source-level replacement bootloader has not yet reached its hardware
flash gate. The currently documented USB tool targets the recovered factory
HID bootloader and requires explicit hashes and execution flags.

## Development status

The factory HID-DFU protocol has been recovered and live-tested. A factory
Metal/Amp engine was successfully substituted into the NCR-2's selected slot,
proving the shared Core Deluxe platform and the USB deployment path.

Development is now moving to an independent bootloader, board-support
package, deterministic audio engine, and original/open DSP implementations.
