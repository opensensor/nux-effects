# NCR-2 open firmware foundation

This directory contains the source-level replacement boot chain described in
the root architecture plan.

## Current status

The current binaries are **offline inspection artifacts only**. They have not
passed the physical recovery, USB, GPIO, clock, or audio gates and must not be
flashed to a pedal.

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
- vector, stack, size, board, and load-address checks;
- range-confined 64-byte open recovery packet format;
- host-tested inactive-slot update transaction and retry behavior;
- `pedalctl.py` host packet/client implementation;
- a compile-checked RT1051 EHCI/HID adapter for 64-byte `NXFX` reports;
- an opt-in RT1051 board adapter for the recovered boot inputs, USB1
  clocks/PHY/IRQ, and warm reset;
- a host-tested NOR mutation policy and compile/link-checked RT1051
  FlexSPI adapter with an ITCM-only command call graph;
- a combined, deliberately nonbootable hardware link probe joining the board,
  USB, FlexSPI, recovery, and journal layers;
- a tested adapter from that NOR policy to the recovery engine and
  power-loss-safe boot journal;
- a guarded full-chip packer that starts from the verified factory dump;
- exact preservation checks for the stock boot header and factory region.

Not implemented:

- physical validation of the recovered footswitch input wrapper;
- a project USB VID/PID;
- physical validation of FlexSPI erase/program on a sacrificial sector;
- hardware-backed journal mutation and USB recovery entry from
  `bootloader_main`;
- watchdog confirmation and rollback;
- cache/MPU setup;
- source SEMC initialization;
- GPIO diagnostics or audio.

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

The resulting full image is still not approved for hardware. The report must
show:

- the first `0x2000` bytes preserved;
- `0x30000–0x3fffff` preserved;
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

`pedalctl.py upload` exists for protocol simulation and future hardware use,
but there is no approved open bootloader on the pedal yet. Do not point it at
the stock NUX recovery device: the protocols are intentionally unrelated.
