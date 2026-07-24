# Pinned MCUXpresso SDK integration

This note records the exact vendor baseline chosen for the open NCR-2 board
support package. It is a reproducibility record, not authorization to flash
the current bootloader.

## Locked source

`firmware/sdk-lock.json` pins complete 40-character Git revisions for:

- MCUXpresso SDK core;
- NXP CMSIS 5;
- MCUXpresso USB middleware; and
- MCUXpresso SDK examples.

Fetch them without committing vendor source:

```sh
python3 tools/fetch_mcux_sdk.py
```

The resulting layout is:

```text
third_party/mcux-sdk-workspace/
├── core/
│   ├── CMSIS/
│   └── devices/MIMXRT1051/
├── middleware/usb/
└── examples/
```

The fetcher refuses to replace a non-empty destination and verifies every
checked-out `HEAD` against the lock.

## Baseline proven to build

The pinned `evkbimxrt1050/usb_device_hid_generic/bm` example was configured
and built successfully with the installed GNU Arm toolchain. Its generated
bare-metal image uses:

- EHCI controller 0 / USB OTG1;
- the 480 MHz USB PHY PLL;
- `usb_device_ehci.c`, `usb_device_dci.c`, and `usb_phy.c`;
- HID class, Chapter 9, and class-dispatch sources;
- interrupt IN endpoint 1 and OUT endpoint 2;
- two DMA-aligned receive buffers; and
- `USB_DeviceClassInit`, `USB_DeviceHidRecv`,
  `USB_DeviceHidSend`, and `USB_DeviceRun`.

That reference build linked about 18 KiB of text and 21 KiB of data before
removing its debug console, test features, and EVK-only board support.

## NCR-2 integration delta

The pedal contains `MIMXRT1051DVL6B`, so production source must use:

```text
CPU_MIMXRT1051DVL6B
core/devices/MIMXRT1051/
```

The EVK example targets RT1052. Its USB, clock, GPIO, cache, FlexSPI, SEMC,
SAI, and eDMA peripherals are shared, but RT1052-only LCDIF/CSI/PXP features
must not leak into the pedal build. The SDK includes a dedicated RT1051 CMSIS
device and startup definition, so there is no reason to compile the final
BSP as RT1052.

The compile-checked open recovery adapter changes the example from 8-byte
reports to the repository's 64-byte `NXFX` packets. It:

- refuses to start while its VID/PID are unassigned and refuses NUX's VID;
- disable ROOT2/compliance/debug-console features;
- dispatch each completed OUT report to `recovery_engine_process`;
- send the exact returned 64-byte response on endpoint 1;
- immediately re-arm endpoint 2 for the next request; and
- place both DMA buffers in an aligned, non-cacheable OCRAM section.

The adapter and the pinned vendor sources compile for the exact
`MIMXRT1051DVL6B` target with:

```sh
python3 tools/fetch_mcux_sdk.py

cmake -S firmware -B build/open-usb -G Ninja \
  -DCMAKE_MAKE_PROGRAM="$(command -v ninja)" \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/cmake/arm-none-eabi-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNCR2_BUILD_MCUX_USB_ADAPTER=ON \
  -DNCR2_MCUX_SDK_ROOT="$PWD/third_party/mcux-sdk-workspace"
cmake --build build/open-usb --target ncr2_mcux_usb_adapter
```

This is deliberately an object-library compile gate, not a flashable image.
The board-specific USB clock, PHY initialization, interrupt-vector wrapper,
MPU setup, and a flash backend must be completed before it is linked into
the recovery boot path. With the default compile definitions, the adapter's
VID and PID are zero and `ncr2_recovery_usb_start` returns
`NCR2_RECOVERY_USB_UNASSIGNED_ID`; it cannot enumerate accidentally.

## Clock, MPU, and memory requirements

The stock FCFB/DCD already initializes FlexSPI and the 32 MiB SEMC SDRAM
before the reset vector. The open boot path still needs an explicit,
source-controlled MPU/cache setup:

- XIP NOR at `0x60000000`: read-only, executable, cacheable while idle;
- SDRAM at `0x80000000`: executable/cacheable for the loaded application;
- USB DMA buffers: non-cacheable and aligned to the SDK requirement;
- peripheral space: device memory; and
- ITCM/DTCM/OCRAM: executable/data attributes appropriate to each section.

USB recovery itself does not require SDRAM. Keeping the recovery stack and
buffers in on-chip RAM allows recovery even if SEMC validation later fails.

## FlexSPI write constraint

The processor cannot fetch ordinary XIP instructions while the same FlexSPI
controller is executing an IP erase/program command. Therefore the eventual
backend must:

1. place the complete erase/program/status-poll call graph and constants in
   an ITCM `.ramfunc` section;
2. mask interrupts around each individual flash IP command;
3. invalidate the affected FlexSPI/AHB cache range afterward;
4. restore interrupts before sending the HID response; and
5. verify every programmed region through the normal read path.

The protocol never gives this backend an absolute host-provided address.
`recovery_resolve_range` has already reduced every operation to the inactive
application partition before the backend is called.

## Next hardware gates

Before any full-chip open image is approved:

1. assign a legitimate project/community USB identity;
2. enumerate the open HID stack from a RAM/debug build;
3. prove GET_INFO without any flash mutation;
4. identify and test the physical recovery footswitch from source;
5. erase/program/read back a sacrificial sector in an open application slot;
6. persist and scan boot-state records across interrupted writes;
7. validate pending-trial rollback with a watchdog; and
8. retain the external programmer recovery procedure throughout.
