# Preserved NUX firmware and analysis artifacts

This directory permanently preserves the files used to reverse-engineer and
validate USB HID-DFU on the NUX NCR-2/Core Deluxe platform.

## Directory layout

### `oem-metal-core-deluxe-v1.26/`

Complete byte-for-byte copy of the temporary OEM collection:

- original NUX Windows firmware ZIP;
- original NUX macOS firmware ZIP;
- extracted Windows `NUX Device Updater.exe`;
- original macOS updater DMG;
- extracted macOS updater application;
- both copies of `COREDLX_V1.26(20240220).bin`; and
- x86-64 updater disassembly artifacts.

Official source URLs:

```text
https://nux.cherubtechnology.com/download/Firmware/Effects/Core/MetalCoreDeluxeMKII/NUX_MetalCoreDeluxeMKII_Firmware_V1.26_Windows.zip
https://nux.cherubtechnology.com/download/Firmware/Effects/Core/MetalCoreDeluxeMKII/NUX_MetalCoreDeluxeMKII_Firmware_V1.26_macOS.zip
```

Important OEM SHA-256 values:

```text
Windows ZIP
dee67bafc48d4b48eb344e749054d827240d6f291a0ead9b040ff36dc0ddeeaf

macOS ZIP
edc37fe493628720d7a95d12fe4055c140ca369616f0d0f412885e3f33ee24bf

COREDLX_V1.26(20240220).bin
15b7d751a6114e1f02a4560f9add6ea1ad6b544b53c7baae183a8cfa3146cdc4
```

### `ghidra-windows-updater/`

The analyzed Ghidra project for the official Windows updater and its full
decompilation/analysis log. This is the project that exposed the host-side
540-byte BINA record and 64-byte HID report framing.

### `device-firmware-analysis/`

The complete device-analysis scratchpad, including:

- isolated 64 KiB `RTX_DFU` image;
- original Ghidra projects for the full dump and DFU image;
- every Ghidra script and analysis log used during discovery; and
- a working copy of the original flash dump.

The isolated DFU image SHA-256 is:

```text
94b12cc1fd25bb84fdbc1defa6b2dbd397f6575675058431ac65f7e9264e84dd
```

### `session-logs/`

Full raw Thumb disassembly of the 64 KiB DFU image and the final targeted
Ghidra trace that identified:

- HID record reassembly at `0x45d8`;
- record decoding at `0x464e`;
- page programming at `0x47b4`; and
- the hardcoded flash base `0x60000`.

## Workspace files outside this directory

The repository root contains the primary operational artifacts:

- `dump1.bin`, `dump2.bin`, and `dump3.bin`: three identical 8 MiB reads;
- `nux-metal-mode.bin`: full dump with selector byte changed from 1 to 3;
- `eng3-slot1.bina`: successfully tested USB ENG3 substitution stream;
- `restore-stock-slots.bina`: reversible stock engine restoration stream;
- `tools/nux_dfu.py`: guarded Linux HID-DFU host utility;
- `DFU_PROTOCOL.md`: recovered protocol and live-validation notes;
- `PROGRAMMABLE_PEDAL_PLAN.md`: source-level open bootloader, A/B updater,
  board-support, audio, extensible DSP, and hardware-validation plan; and
- `ghidra/`: durable Ghidra analysis scripts.

The global `SHA256SUMS` file in the repository root covers every preserved
regular file except generated Python bytecode and the manifest itself.

## Hardware validation

On 2026-07-23, `eng3-slot1.bina` was sent to the pedal:

```text
pre-transfer query:   V1.2.4
reports accepted:     4,617 / 4,617
post-transfer query:  ENG3TEST
normal boot:          successful
audio result:         functioning ENG3 overdrive/amp pedal
```

## Current source passthrough candidate

The ignored local directory `build/factory-candidate/` contains the first
combined source audio/GPIO artifact. It is deliberately named
`NOT-FOR-FLASH` until the two remaining candidate control pins are identified
electrically:

```text
ncr2_factory_slot_app.elf
83efd0dedcd3940ff7dc63e03c6b27f9ab56fa348ebe93e2937d91c51fd47922

ncr2_factory_slot_app.bin
4026ba5538d0a4fdc067f8d1c293841e09b05bd1211ef4e274af5e4651b5b93b

package/source-passthrough-gpio-NOT-FOR-FLASH.bina
12ad4c51ee01c58f35a0e81a4feb0b3e7697e579c579bae6947b191ab8b89827

package/metal-restore.bina
bb7f1713268e6fcc7b656c572af9674c28a74ce1f37a48708ba2ad1511dfb868
```

The restore package is byte-identical to the live-tested
`eng3-slot1.bina`. Both BINA streams map only `0x60000..0x9ffff`; packaging
and report expansion were performed offline and nothing was sent to USB.
