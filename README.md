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
The host page for designing programs, authoring effects in C, and previewing
them through the firmware's own runtime is documented in
[docs/app/EFFECT_EDITOR.md](docs/app/EFFECT_EDITOR.md).
The copyright-neutral diagnostic path that chain-loads the preserved factory
Metal engine is documented in
[docs/hardware/FACTORY_BRIDGE.md](docs/hardware/FACTORY_BRIDGE.md).
The source-built ITCM application target that matches the recovered factory
launcher ABI is documented in
[docs/hardware/FACTORY_SLOT_APP.md](docs/hardware/FACTORY_SLOT_APP.md).
The executed factory SAI/eDMA format and buffer topology are documented in
[docs/hardware/FACTORY_AUDIO.md](docs/hardware/FACTORY_AUDIO.md).
The recovered GPIO startup levels and remaining analog-control ambiguity are
documented in
[docs/hardware/FACTORY_BOARD_CONTROL.md](docs/hardware/FACTORY_BOARD_CONTROL.md).
Recovery through the immutable i.MX RT1051 ROM downloader is documented in
[docs/hardware/ROM_SDP_RECOVERY.md](docs/hardware/ROM_SDP_RECOVERY.md). It is
both the last resort and, while the open NOR programmer stays fail-closed,
the routine way to install a bootloader.
The guarded SDRAM personality that can replace all 8 MiB through the open
HID protocol is documented in
[docs/hardware/RAM_FULL_FLASH_RECOVERY.md](docs/hardware/RAM_FULL_FLASH_RECOVERY.md);
its whole-chip write is currently refused before erase because of
[docs/hardware/NOR_PROGRAM_FIFO_DEFECT.md](docs/hardware/NOR_PROGRAM_FIFO_DEFECT.md).
The exact Linux `sdphost`, `blhost`, and RT1052 RAM flashloader assets used on
physical NCR-2 hardware are preserved with a guarded host wrapper.

## Current hardware facts

- NXP MIMXRT1051DVL6B Cortex-M7
- 8 MiB external FlexSPI NOR
- 32 MiB ESMT M12L2561616A SDRAM on SEMC
- SAI1 with eDMA for the executed stock audio path
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

The Apache-2.0 NXP host utilities and RAM flashloader used for last-resort ROM
recovery are a deliberate exception: their exact binaries, hashes, upstream
commit, and license are preserved under `tools/vendor/nxp-mcubootutility/`.

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

A separate 17 KiB SDRAM-resident recovery target now supports an explicitly
armed whole-chip erase/write/hash transaction. The development bootloader can
embed that checked binary and copies it to SDRAM before recovery USB starts,
so it consumes no effect slot and remains alive after erasing itself. The
legacy XIP recovery path never advertises the capability and retains its
slot/metadata-only mutation policy. The embedded bytes and RAM residency are
both post-link checked.

The hardware target is separately gated for USB enumeration and NOR writes.
It defaults to unassigned USB IDs and a read-only recovery backend. A
post-link checker verifies the vector table, VTOR initialization, protected
flash limit, capability marker, complete USB stack, DMA-buffer placement and
alignment, and ITCM-only flash-busy call graph. These are offline structural
gates, not evidence that the image is safe to flash. Physical read-only USB
bring-up, sacrificial-sector NOR testing, and watchdog rollback validation
remain outstanding.

The hardware reset path now also calls the pinned RT1051 `SystemInit` before
copying ITCM routines or entering C services. This source-controls FPU access,
watchdog/SysTick cleanup, and instruction-cache enable instead of relying on
unknown power-on state. A full application MPU/data-cache policy remains a
later audio-BSP gate.

An additional opt-in transition target now links the open application directly
for the factory engine-slot ABI: vectors at ITCM zero, DTCM stack, pinned
`SystemInit`, and a hard `0x1e000` copy limit. The recovered OEM updater packer
can wrap it in a validated BINA stream and emits a byte-identical Metal restore
alongside it. An additional opt-in source path now reproduces the executed
factory SAI1/eDMA contract, including the PLL, pins, four-slot framing,
ping-pong TCD rings, IRQ0 handler, and a weak passthrough/DSP hook. It passes
offline reset/register emulation and a synthetic DMA-block copy. A separately
gated board adapter now also reproduces the observed factory GPIO output
levels and delayed GPIO1_IO26 transition. This remains an offline packaging
milestone, not a flash candidate, until GPIO1_IO26 and GPIO2_IO11 are
electrically identified and their fail-safe polarities confirmed.

The application now has an allocation-free extensible effect registry,
caller-sized processing chains, validated program/bank catalogs, and a
generic program selector. Its default five-second navigation gesture
operates on runtime catalog sizes and has no four-effect assumption.
The first source catalog contains six host-tested starter programs assembled
from Gain and Soft Clip descriptors; it is test scaffolding pending audio
hardware bring-up.

A host design page under `host/editor/` now composes those descriptors into
programs, accepts new effects written against the ABI, and previews them by
compiling and running the application's own `effect_runtime` and
`program_runtime` sources locally. It can also lift the hardware
application's eight fixed-point panel presets out of
`firmware/hardware_app/src/main.c` at build time and preview those, without
modifying that file; the extraction fails loudly if the DSP region moves. It reports the runtime's real validation
status codes, non-finite output, arena use, and host-relative block timing,
and exports firmware-shaped C that CI compiles against the include tree. It
holds no second DSP implementation, opens no USB device, and produces no
flashable image; on-target CPU budget and audible hardware tests remain
device-side gates.

For first-image continuity, an opt-in 776-byte original-source bridge now
validates and loads the preserved factory Metal engine from the compatibility
region. Dependency analysis also caught and corrected a flash-layout
collision: every factory engine uses state sectors at `0x2d000/0x2e000`, so
the open journal now lives at the erased, reference-audited `0x3f0000`
sector. The bridge and a full private overlay pass offline validation, but no
generated image is approved for hardware.
