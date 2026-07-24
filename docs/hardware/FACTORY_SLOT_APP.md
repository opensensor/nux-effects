# Source application on the factory launcher

`ncr2_factory_slot_app` is an opt-in transition target for bringing up
source-owned firmware without replacing the recovered factory bootloader.
It does not contain factory code or data.

This is a development bridge, not the final architecture. The final design
remains the open A/B bootloader plus SDRAM-loaded applications described in
`PROGRAMMABLE_PEDAL_PLAN.md`.

## Recovered launch ABI

The factory launcher:

1. selects one `0x20000`-byte engine slot;
2. copies its first `0x1e000` bytes to ITCM at `0x00000000`;
3. loads the stack/reset values from the copied vector table; and
4. transfers control to the Thumb reset handler.

The source target therefore:

- links its vector table at ITCM address zero;
- uses a DTCM stack top of `0x20020000`;
- installs `VTOR = 0`;
- calls the pinned RT1051 `SystemInit`;
- initializes `.data` and `.bss`; and
- enforces the `0x1e000` load budget in the linker and post-link checker.

The default payload remains a hardware-neutral application/runtime skeleton.
An additional, separately gated build reproduces the executed factory
SAI1/eDMA contract and runs a four-slot, 32-bit passthrough with two 128-byte
DTCM buffer halves. The source audio path has passed register-level emulation
and an emulated DMA-block copy, but it is still marked **offline only**:
the board's analog mute/bypass sequencing has not yet been recovered.

## Build and verify

Fetch the pinned SDK workspace first, then build the explicit target:

```sh
python3 tools/fetch_mcux_sdk.py

cmake -S firmware -B build/factory-slot -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/cmake/arm-none-eabi-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNCR2_BUILD_FACTORY_SLOT_APP=ON \
  -DNCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH=ON \
  -DNCR2_MCUX_SDK_ROOT="$PWD/third_party/mcux-sdk-workspace"

cmake --build build/factory-slot --target ncr2_factory_slot_app
python3 tools/check_factory_slot.py \
  build/factory-slot/ncr2_factory_slot_app.elf

PYTHONPATH=/path/to/unicorn \
python3 tools/emulate_source_audio.py \
  build/factory-slot/ncr2_factory_slot_app.elf
```

Omit `NCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH=ON` to produce the deliberately
hardware-inert default. The post-link checker validates the complete vector
table, including external IRQ0 targeting the shared eDMA channel 0/16
handler. The source emulator executes reset and initialization, checks the
factory SAI1, PLL, pin, DMAMUX, and TCD values, injects one RX block, invokes
the source ISR, and verifies the unchanged TX block. It does not access USB.

## Build an offline OEM-DFU package

The packer starts from the verified private dump because the factory HID
protocol always writes contiguously from flash offset `0x60000`. With the
NCR-2 selector still targeting slot 1, it preserves stock slot 0 and places
the source image in slot 1. It also emits a paired restore package that copies
the audited factory Metal engine from slot 3 back into slot 1:

```sh
python3 tools/nux_dfu.py make-factory-slot \
  dump1.bin \
  build/factory-slot/ncr2_factory_slot_app.bin \
  build/factory-slot/source-slot-OFFLINE-ONLY.bina \
  build/factory-slot/metal-restore.bina

python3 tools/nux_dfu.py dry-run \
  build/factory-slot/source-slot-OFFLINE-ONLY.bina
```

The command requires the known dump SHA-256 by default, validates the
application vectors and copy size, validates both generated BINA streams, and
refuses to overwrite existing artifacts.

For the verified local dump, the generated Metal restore is byte-identical to
the previously live-tested `eng3-slot1.bina`. The source package is still not
approved for streaming: digital passthrough is implemented, but the analog
mute/bypass state and live slot semantics remain unverified.

## Gates before first source-slot hardware test

1. ~~Recover and source-control exact audio clocks, pads, SAI framing, eDMA
   channels, and ping-pong buffer geometry.~~
2. Identify the converter/codec and prove mute/bypass sequencing.
3. ~~Implement a bounded passthrough path with DTCM DMA buffers and explicit
   cache policy.~~ The first image keeps DMA state in uncached DTCM.
4. Add a safe target-visible heartbeat that cannot drive an unknown output.
5. Review the generated write range and exact BINA hash.
6. Keep the byte-identical Metal restore package available before testing.

The factory recovery entry is below the `0x60000` write base and is not
modified by this package, but that fact does not make an unreviewed
application safe.
