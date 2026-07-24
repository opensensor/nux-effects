# nux-effects

Open firmware research and development for the NUX Core Deluxe effects-pedal
platform, initially tested on the NCR-2 Reverb Core Deluxe.

The long-term target is a source-level programmable pedal with:

- a recoverable open bootloader;
- A/B application updates over USB;
- an extensible registry of source effects and caller-sized DSP chains;
- RAM-resident programs and banks with flash-free live changes;
- a board-support package for the i.MX RT1051 hardware; and
- reproducible host tools, firmware images, and tests.

The current architecture and phased bring-up plan are in
[PROGRAMMABLE_PEDAL_PLAN.md](PROGRAMMABLE_PEDAL_PLAN.md). The recovered stock
HID updater protocol is documented in [DFU_PROTOCOL.md](DFU_PROTOCOL.md).
The offline source boot chain and build instructions are in
[firmware/README.md](firmware/README.md).
The independent open recovery wire format is documented in
[docs/protocol/OPEN_RECOVERY_PROTOCOL.md](docs/protocol/OPEN_RECOVERY_PROTOCOL.md).
The allocation-free application effect ABI and program model are documented
in [docs/app/EFFECT_RUNTIME.md](docs/app/EFFECT_RUNTIME.md).
The copyright-neutral diagnostic path that chain-loads the preserved factory
Metal engine is documented in
[docs/hardware/FACTORY_BRIDGE.md](docs/hardware/FACTORY_BRIDGE.md).

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

The source-level replacement bootloader now has an opt-in, structurally
bootable RT1051 target, but it has not passed the physical hardware flash
gate. It is read-only by default, cannot enumerate with unassigned USB IDs,
and is not an approved image for the pedal. The currently documented USB
tool targets the recovered factory HID bootloader and requires explicit
hashes and execution flags.

## Development status

The factory HID-DFU protocol has been recovered and live-tested. A factory
Metal/Amp engine was successfully substituted into the NCR-2's selected slot,
proving the shared Core Deluxe platform and the USB deployment path.

Development is now moving to an independent bootloader, board-support
package, deterministic audio engine, and original/open DSP implementations.
The guarded A/B recovery transaction and matching host client are implemented
under host tests. The complete 64-byte USB HID stack, minimal RT1051 board
wrapper, boot journal, watchdog handoff, and RAM-resident FlexSPI backend now
link into an opt-in hardware bootloader. Its startup installs the open vector
table explicitly, including USB OTG1, before C initialization.

The hardware target is separately gated for USB enumeration and NOR writes.
It defaults to unassigned USB IDs and a read-only recovery backend. A
post-link checker verifies the vector table, VTOR initialization, protected
flash limit, capability marker, complete USB stack, DMA-buffer placement and
alignment, and ITCM-only flash-busy call graph. These are offline structural
gates, not evidence that the image is safe to flash. Physical read-only USB
bring-up, sacrificial-sector NOR testing, and watchdog rollback validation
remain outstanding.
The application now has an allocation-free extensible effect registry,
caller-sized processing chains, validated program/bank catalogs, and a
generic program selector. Its default five-second navigation gesture
operates on runtime catalog sizes and has no four-effect assumption.
The first source catalog contains six host-tested starter programs assembled
from Gain and Soft Clip descriptors; it is test scaffolding pending audio
hardware bring-up.

For first-image continuity, an opt-in 776-byte original-source bridge now
validates and loads the preserved factory Metal engine from the compatibility
region. Dependency analysis also caught and corrected a flash-layout
collision: every factory engine uses state sectors at `0x2d000/0x2e000`, so
the open journal now lives at the erased, reference-audited `0x3f0000`
sector. The bridge and a full private overlay pass offline validation, but no
generated image is approved for hardware.
