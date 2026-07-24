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
- the bare-metal OSA implementation used by the device controller;
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
- disables ROOT2/compliance/debug-console features;
- dispatches each completed OUT report to `recovery_engine_process`;
- sends the exact returned 64-byte response on endpoint 1;
- immediately re-arms endpoint 2 for the next request; and
- places the USB DMA objects in aligned DTCM while compiling the vendor stack
  with `DATA_SECTION_IS_CACHEABLE=0`.

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
With the default compile definitions, the adapter's VID and PID are zero and
`ncr2_recovery_usb_start` returns
`NCR2_RECOVERY_USB_UNASSIGNED_ID`; it cannot enumerate accidentally.

## Minimal board wrapper

The opt-in board adapter now provides only the hardware needed for an early
recovery proof:

- samples active-low GPIO1_IO21 on `GPIO_AD_B1_05` while requiring
  GPIO3_IO02 on `GPIO_SD_B1_02` to remain high;
- configures those pads as inputs with 100 kOhm pull-ups and hysteresis;
- enables the USB1 480 MHz PHY PLL and controller clock;
- initializes EHCI PHY0 with the trims used by the pinned reference;
- enables `USB_OTG1_IRQn` at priority 3 and dispatches it to the open USB
  adapter; and
- configures WDOG1 for an eight-second pending-image trial, without interrupt
  or external timeout assertion and with pause-under-debug behavior; and
- requests warm reset with `NVIC_SystemReset`.

It contains no LED, bypass, mute, audio, or other GPIO output writes. The
GPIO pair is source-confirmed for the stock early-boot condition but still
requires PCB continuity or controlled target observation before a full-chip
image can pass the hardware gate.

The open bootloader vector table now contains the USB OTG1 vector at RT1051
external IRQ 113. The handler remains a weak default unless the opt-in board
adapter is explicitly linked.

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
The RT1050 Cortex-M7 exposes its tightly coupled memories to system DMA
masters through the AHBS interface, as documented in
[NXP AN12077](https://www.nxp.com/docs/en/application-note/AN12077.pdf), and
NXP's pinned RT1050 generic-HID example uses this same DTCM placement. OCRAM
remains an option if later multi-master measurements justify moving the
buffers.

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

The current source implements this as two layers:

- `ncr2_nor.c` is the hardware-independent policy layer. It permits mutation
  only in boot metadata or application A/B, requires 4 KiB-aligned erases,
  splits writes at 256-byte page boundaries, rejects attempted zero-to-one
  programming, and verifies every mutation through readback.
- `ncr2_flexspi_nor.c` is an opt-in MCUX adapter for the exact RT1051. It
  installs private single-pad W25Q commands in LUT sequences 11–15, refuses
  any JEDEC ID other than `EF 40 17`, disables interrupts for the complete
  mutation, waits for WIP to clear, resets the AHB buffers, and invalidates
  Cortex-M7 caches before verification.

The low-level functions and the exact MCUX blocking call graph are linked
into ITCM by `ncr2_bootloader.ld`. Startup copies that section from its flash
load image before C initialization. The linker asserts the MCUX entry points
are in ITCM; the post-link checker also rejects any direct or indirect branch
from `.ramfunc` back into XIP.

Compile and link-check this path without producing a hardware-approved image:

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

This remains an offline gate. The default `bootloader_main` uses the
host-tested controller with a deliberately read-only XIP journal backend.
The opt-in hardware bootloader described below wires the MCUX FlexSPI adapter
to the controller and recovery engine, but also defaults to read-only. No
erase/program command has been issued to the physical pedal.

## Combined hardware integration probe

Enable all three adapters to compile and link the complete board, USB,
recovery engine, boot journal, trial-confirmation/watchdog handoff, NOR
policy, and FlexSPI dependency graph:

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

This ELF intentionally has no vector table or reset handler, and its entry is
the recovery-input initializer rather than a reset path. The checker rejects
any accidental boot structure while requiring all integration symbols. It
proves source completeness only; it is not a flashable image.

## Opt-in bootable integration

`ncr2_hardware_bootloader` is an `EXCLUDE_FROM_ALL` target that joins the
same integration graph to the real reset/vector path. Startup installs
`g_boot_vectors` into `SCB->VTOR`, executes `DSB`/`ISB`, copies `.ramfunc`,
and then enters the shared boot controller. The hardware services provide:

- exact early-recovery GPIO sampling;
- a W25Q64 JEDEC probe;
- read-only or explicitly write-enabled recovery storage and journal
  callbacks;
- pending-trial WDOG1 handoff and warm reset; and
- optional USB1 HID recovery entry.

Build the default read-only, non-enumerating form with:

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

Enumeration is a separate gate:
`NCR2_ENABLE_HARDWARE_USB_ENUMERATION=ON` requires nonzero, assigned
`NCR2_OPEN_USB_VID` and `NCR2_OPEN_USB_PID` values and rejects NUX's VID. A
linked enumerating image is checked with `--expect-usb-stack`, which
additionally requires the EHCI/DCI/HID symbols and validates every known USB
DMA object against the DTCM bounds and required alignment.

Physical recovery writes are another independent gate,
`NCR2_HARDWARE_RECOVERY_WRITE_ENABLE=ON`, and add the
`--write-enabled` checker expectation. Neither option is permission to
flash. The full read-only and write-enabled graphs have only been built and
inspected offline.

## Next hardware gates

Before any full-chip open image is approved:

1. assign a legitimate project/community USB identity;
2. verify the two early-recovery input pads on target;
3. enumerate the open HID stack from a RAM/debug build;
4. prove GET_INFO without any flash mutation;
5. erase/program/read back a sacrificial sector in an open application slot;
6. persist and scan boot-state records across interrupted writes;
7. validate pending-trial rollback with a watchdog; and
8. retain the external programmer recovery procedure throughout.
