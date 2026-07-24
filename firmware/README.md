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
- slot A validation with slot B fallback;
- vector, stack, size, board, and load-address checks;
- a guarded full-chip packer that starts from the verified factory dump;
- exact preservation checks for the stock boot header and factory region.

Not implemented:

- footswitch recovery input;
- USB recovery transport;
- FlexSPI erase/program routines;
- A/B trial-boot metadata;
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
`SDK_REVISION`. The current boot skeleton deliberately needs no vendor headers,
which keeps image-format and recovery logic host-testable.

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

