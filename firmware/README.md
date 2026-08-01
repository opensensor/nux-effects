# NCR-2 open firmware foundation

This directory contains the source-level replacement boot chain described in
the root architecture plan.

## Current status

The default binaries are **offline inspection artifacts only**. The separately
gated RT1051 hardware bootloader has passed physical recovery USB, bounded
FlexSPI erase/program/readback, durable A/B journal mutation, watchdog trial
handoff, application confirmation, and bidirectional slot switching on a Verb
Core Deluxe. Source audio, cache/MPU, remaining GPIO behavior, and whole-chip
Open Recover restore retain separate hardware gates.

Implemented:

- complete 8 MiB flash partition map;
- bootloader linked at the stock `0x60002000` vector address;
- application linked for a slot-independent SDRAM load at `0x80000000`;
- application manifest with header CRC32 and payload SHA-256;
- journaled A/B boot-state format and slot selection with fallback;
- power-loss-safe two-sector journal append/rotation layer;
- three-attempt pending boot, explicit confirmation, and rollback policy;
- a host-tested boot controller that journals trials before loading,
  durably rejects bad pending images, falls back once, and enters recovery
  on journal or image failure;
- a fixed, one-shot software-recovery mailbox in retained SRC GPR8/GPR9,
  plus an application-facing arm API;
- a one-word, one-shot factory-engine mailbox in retained SRC GPR10, with a
  unified eight-position/two-second selection gesture and four launch
  descriptors that pin stock main-loop/LED-updater hooks and zero-filled
  monitor caves;
- a replay- and torn-write-safe pending-trial mailbox in SRC GPR3–GPR6,
  binding application health confirmation to one slot and journal sequence;
- a host-tested handoff service that starts an injected trial watchdog and
  publishes the retained confirmation challenge;
- vector, stack, size, board, and load-address checks;
- range-confined 64-byte open recovery packet format;
- host-tested inactive-slot update transaction and retry behavior;
- `pedalctl.py` host packet/client implementation;
- a compile-checked RT1051 EHCI/HID adapter for 64-byte `NXFX` reports;
- an opt-in RT1051 board adapter for the recovered boot inputs, USB1
  clocks/PHY/IRQ, an eight-second WDOG1 trial, and warm reset;
- a host-tested NOR mutation policy and compile/link-checked RT1051
  FlexSPI adapter with an ITCM-only command call graph;
- a combined, deliberately nonbootable hardware link probe joining the board,
  USB, FlexSPI, recovery, and journal layers;
- an opt-in hardware bootloader with explicit VTOR installation, complete
  RT1051 vectors, the W25Q64 probe, the board/USB/FlexSPI adapters, the
  recovery engine, the boot journal, and the watchdog handoff;
- pinned RT1051 `SystemInit` execution before ITCM copy or hardware services;
- separate compile-time gates for USB enumeration and physical NOR mutation,
  both disabled by default;
- a post-link hardware checker for vectors, VTOR, protected flash size,
  capability markers, the complete USB stack, and USB DMA placement;
- an allocation-free effect registry and caller-sized processing-chain
  runtime with no fixed effect-count limit;
- stable program and bank descriptors with catalog validation, transactional
  inactive-chain preparation, and count-independent navigation;
- host-tested descriptor-driven Gain and Soft Clip reference effects;
- a reusable RAM-only program selector with configurable hold duration that
  operates on any runtime catalog size;
- a tested adapter from that NOR policy to the recovery engine and
  power-loss-safe boot journal;
- a guarded full-chip packer that starts from the verified factory dump;
- exact preservation checks for the stock boot header and factory region.
- an opt-in, copyright-neutral factory Metal bridge that validates the
  preserved vectors, copies the engine to ITCM, and reproduces the stock
  handoff without embedding factory bytes.
- a dynamic source launcher that reuses the compatibility preparation for
  Delay, Reverb, Modulation, or Metal, applies a transient main-loop
  knob-select/return monitor to the ITCM copy, maps positions 5–8 back to four
  open slots without a second hold duration, and never modifies the preserved
  factory NOR;
- an opt-in source application linked to the recovered factory ITCM slot ABI,
  with a strict post-link checker and paired OEM-DFU/Metal-restore packer;
- an opt-in factory-compatible SAI1/eDMA passthrough with DTCM ping-pong
  buffers, a weak source DSP hook, and offline register/ISR emulation.
- separately gated factory GPIO startup levels and the observed delayed
  GPIO1_IO26 transition, with offline pin-state emulation.

Not implemented:

- a project USB VID/PID;
- physical validation of a complete 8 MiB restore through Open Recover;
- source-controlled MPU/data-cache policy for the full open application;
- source SEMC initialization;
- physical validation of the source audio path and analog mute/bypass
  sequencing;
- GPIO diagnostics.

## Build

```sh
cmake -S firmware -B build/open -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/cmake/arm-none-eabi-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release
cmake --build build/open
arm-none-eabi-size \
  build/open/ncr2_bootloader.elf \
  build/open/ncr2_app.elf
```

The future BSP is pinned to the official NXP MCUXpresso SDK commit recorded in
`SDK_REVISION`, with its required modules pinned in `sdk-lock.json`. Fetch the
exact source workspace with:

```sh
python3 tools/fetch_mcux_sdk.py
```

The current boot skeleton deliberately needs no vendor headers, which keeps
image-format and recovery logic host-testable.

The controller policy and the current no-write hardware boundary are
documented in
[BOOT_CONTROLLER.md](../docs/boot/BOOT_CONTROLLER.md).

The pinned RT1050 vendor HID example was also built successfully from this
workspace. The exact integration delta for the real RT1051 pedal is recorded
in [MCUX_SDK_INTEGRATION.md](../docs/hardware/MCUX_SDK_INTEGRATION.md).

The optional RT1051 adapter itself can be compile-checked after fetching the
pinned workspace:

```sh
cmake -S firmware -B build/open-usb -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/cmake/arm-none-eabi-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNCR2_BUILD_MCUX_USB_ADAPTER=ON \
  -DNCR2_MCUX_SDK_ROOT="$PWD/third_party/mcux-sdk-workspace"
cmake --build build/open-usb --target ncr2_mcux_usb_adapter
```

This target only produces objects. It defaults to an unassigned VID/PID and
the adapter refuses to start, so it is not an enumerating or flashable
recovery image.

The optional FlexSPI path has a separate compile and link probe:

```sh
cmake -S firmware -B build/open-flexspi -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/cmake/arm-none-eabi-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNCR2_BUILD_MCUX_FLEXSPI_ADAPTER=ON \
  -DNCR2_MCUX_SDK_ROOT="$PWD/third_party/mcux-sdk-workspace"
cmake --build build/open-flexspi \
  --target ncr2_mcux_flexspi_adapter ncr2_flexspi_link_probe
python3 tools/check_ramfunc.py \
  build/open-flexspi/ncr2_flexspi_link_probe.elf
```

The link probe is not a bootable firmware image. It exists to prove the
complete flash-busy call graph is resident in ITCM.

The board, USB, and FlexSPI paths can also be compiled and linked together
without a reset vector or boot entry:

```sh
cmake -S firmware -B build/open-hardware-probe -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/cmake/arm-none-eabi-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNCR2_BUILD_MCUX_BOARD_ADAPTER=ON \
  -DNCR2_BUILD_MCUX_USB_ADAPTER=ON \
  -DNCR2_BUILD_MCUX_FLEXSPI_ADAPTER=ON \
  -DNCR2_MCUX_SDK_ROOT="$PWD/third_party/mcux-sdk-workspace"
cmake --build build/open-hardware-probe \
  --target ncr2_hardware_link_probe
python3 tools/check_ramfunc.py \
  build/open-hardware-probe/ncr2_hardware_link_probe.elf
python3 tools/check_hardware_probe.py \
  build/open-hardware-probe/ncr2_hardware_link_probe.elf
```

The checker requires every board/USB/NOR integration symbol while rejecting
a reset handler or vector table. Passing it is a compile/link gate, not
permission to flash.

## Build the opt-in hardware bootloader

The bootable RT1051 integration target is excluded from the default build.
It requires all three MCUX adapters and remains read-only unless physical
write support is enabled separately:

```sh
cmake -S firmware -B build/open-hardware-boot -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/cmake/arm-none-eabi-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNCR2_BUILD_MCUX_BOARD_ADAPTER=ON \
  -DNCR2_BUILD_MCUX_USB_ADAPTER=ON \
  -DNCR2_BUILD_MCUX_FLEXSPI_ADAPTER=ON \
  -DNCR2_BUILD_HARDWARE_BOOTLOADER=ON \
  -DNCR2_MCUX_SDK_ROOT="$PWD/third_party/mcux-sdk-workspace"
cmake --build build/open-hardware-boot \
  --target ncr2_hardware_bootloader
python3 tools/check_hardware_bootloader.py \
  build/open-hardware-boot/ncr2_hardware_bootloader.elf
python3 tools/check_ramfunc.py \
  build/open-hardware-boot/ncr2_hardware_bootloader.elf
```

With the default zero VID/PID, this target cannot enumerate. USB enumeration
additionally requires `NCR2_ENABLE_HARDWARE_USB_ENUMERATION=ON` and a
legitimately assigned non-NUX `NCR2_OPEN_USB_VID`/`NCR2_OPEN_USB_PID`; the
checker then uses `--expect-usb-stack`. Physical writes require the independent
`NCR2_HARDWARE_RECOVERY_WRITE_ENABLE=ON` switch and the checker then uses
`--write-enabled`. Enabling those switches proves link composition only. It
does not approve the resulting image for hardware.

## Decode the verified stock boot configuration

The private `dump1.bin` is not committed. When present locally:

```sh
python3 tools/open_image.py extract-boot \
  --dump dump1.bin \
  --json-output firmware/platform/ncr2/boot/stock_boot_config.json \
  --fragments build/open/stock_boot
```

The JSON register-write description is retained as hardware documentation.
Binary fragments remain ignored build artifacts.

## Build the factory Metal compatibility bridge

The bridge is excluded from the default build and contains no factory
firmware:

```sh
cmake --build build/open --target ncr2_factory_bridge
python3 tools/check_factory_bridge.py \
  build/open/ncr2_factory_bridge.elf
arm-none-eabi-size build/open/ncr2_factory_bridge.elf
```

The expected payload is 776 bytes. It executes from SDRAM, validates the
preserved Metal vectors at `0x600c0000`, copies `0x1e000` bytes to ITCM, and
branches to the audited factory reset vector. See
[FACTORY_BRIDGE.md](../docs/hardware/FACTORY_BRIDGE.md) for the cross-reference
audit and corrected metadata layout.

## Build the source factory-slot application

This transition target runs source-owned code through the still-recoverable
factory launcher. It is excluded from the default build. Its audio path is a
second explicit opt-in because analog mute/bypass control is not yet known:

```sh
cmake -S firmware -B build/factory-slot -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/cmake/arm-none-eabi-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNCR2_BUILD_FACTORY_SLOT_APP=ON \
  -DNCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH=ON \
  -DNCR2_FACTORY_SLOT_BOARD_CONTROLS=ON \
  -DNCR2_MCUX_SDK_ROOT="$PWD/third_party/mcux-sdk-workspace"
cmake --build build/factory-slot --target ncr2_factory_slot_app
python3 tools/check_factory_slot.py \
  build/factory-slot/ncr2_factory_slot_app.elf
PYTHONPATH=/path/to/unicorn \
python3 tools/emulate_source_audio.py \
  build/factory-slot/ncr2_factory_slot_app.elf
```

When the private verified dump is present, an offline BINA plus a paired
static-Metal restore can be produced with:

```sh
python3 tools/nux_dfu.py make-factory-slot \
  dump1.bin \
  build/factory-slot/ncr2_factory_slot_app.bin \
  build/factory-slot/source-slot-OFFLINE-ONLY.bina \
  build/factory-slot/metal-restore.bina
```

Do not stream the source package yet. It is a structurally valid digital
passthrough image that reproduces the observed factory GPIO startup sequence,
but it is not approved until the two candidate control pins' electrical
meaning and fail-safe polarity have been confirmed. See
[FACTORY_SLOT_APP.md](../docs/hardware/FACTORY_SLOT_APP.md).

## Build a guarded offline full image

```sh
python3 tools/open_image.py pack \
  --dump dump1.bin \
  --bootloader build/open/ncr2_bootloader.bin \
  --application build/open/ncr2_app.bin \
  --version 0.1.0 \
  --build-number 1 \
  --output build/open/ncr2-open-0.1.0-full.bin \
  --report build/open/ncr2-open-0.1.0-report.json

python3 tools/open_image.py inspect \
  build/open/ncr2-open-0.1.0-full.bin
```

Packing a full image does not approve it for hardware. The report must show:

- the first `0x2000` bytes preserved;
- factory state and content at `0x20000–0x3effff` preserved;
- application A valid;
- application B erased; and
- no changed partition outside bootloader, metadata, and application slots.

## Build and inspect an A/B slot image

The open updater transmits one contiguous manifest-plus-payload slot image:

```sh
python3 tools/open_image.py pack-slot \
  --application build/open/ncr2_app.bin \
  --version 0.1.0 \
  --build-number 1 \
  --output build/open/ncr2-app-0.1.0.slot

python3 tools/pedalctl.py inspect-slot \
  build/open/ncr2-app-0.1.0.slot
```

`pedalctl.py upload` has passed bidirectional A/B switching on the open
bootloader. Do not point it at the stock NUX recovery device: the protocols
are intentionally unrelated, and the borrowed development USB identity still
requires the explicit host flag.
