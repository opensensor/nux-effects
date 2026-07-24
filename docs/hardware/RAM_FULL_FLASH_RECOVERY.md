# RAM-resident full-flash recovery

`ncr2_ram_recovery` is the deliberately destructive RAM personality behind
the normal open recovery gesture. Its purpose is to let a known-good full
8 MiB image replace every byte of external NOR while the updater itself
remains alive. The checked binary is embedded in the protected bootloader
partition and copied to SDRAM before USB enumeration; it does not consume an
effect slot.

## Why it is a separate image

The normal bootloader executes most code directly from FlexSPI NOR. It cannot
erase its own FCFB, vectors, code, descriptors, or constants and keep running.
Merely marking the erase primitive as `.ramfunc` is insufficient: the USB
interrupt handler, recovery state, packet parser, CRC/SHA code, descriptor
tables, and every called routine also need to survive.

The full recovery image therefore links as follows:

| Content | Address |
|---|---:|
| vector table and all code/constant data | SDRAM `0x80000000` |
| initialized data, USB DMA state, BSS, stack | DTCM `0x20000000` |
| flash being replaced | FlexSPI XIP `0x60000000–0x607fffff` |

It preserves the open bootloader's clock and SEMC setup, explicitly installs
its own VTOR, initializes a full-range NOR policy, and enumerates the same
open HID protocol. `tools/check_ram_recovery.py` fails the build if any
loadable segment or required symbol is outside SDRAM/DTCM, if any direct
branch targets XIP, or if the vector table is invalid.

## Build

```sh
cmake -S firmware -B build/ram-recovery -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/cmake/arm-none-eabi-toolchain.cmake" \
  -DNCR2_MCUX_SDK_ROOT=/path/to/pinned/sdk \
  -DNCR2_BUILD_MCUX_USB_ADAPTER=ON \
  -DNCR2_BUILD_MCUX_FLEXSPI_ADAPTER=ON \
  -DNCR2_BUILD_MCUX_BOARD_ADAPTER=ON \
  -DNCR2_BUILD_HARDWARE_BOOTLOADER=ON \
  -DNCR2_BUILD_RAM_RECOVERY=ON \
  -DNCR2_EMBED_RAM_RECOVERY=ON \
  -DNCR2_HARDWARE_RECOVERY_WRITE_ENABLE=ON \
  -DNCR2_ENABLE_HARDWARE_USB_ENUMERATION=ON \
  -DNCR2_OPEN_USB_VID=0x9527 \
  -DNCR2_OPEN_USB_PID=0xc157 \
  -DNCR2_ALLOW_BORROWED_NUX_DFU_ID=ON

cmake --build build/ram-recovery --target ncr2_hardware_bootloader
python3 tools/check_hardware_bootloader.py \
  build/ram-recovery/ncr2_hardware_bootloader.elf \
  --write-enabled \
  --expect-embedded-ram-recovery \
  --ram-recovery-bin build/ram-recovery/ncr2_ram_recovery.bin
```

Install the resulting bootloader as part of a guarded full-chip image once.
Thereafter, entering physical recovery launches the embedded RAM personality.
Confirm capability `0x20` before issuing `restore-full`. A legacy XIP
recovery personality reports only `0x1f` and rejects the destructive begin
command.

## Failure model

The whole chip is erased before address zero is rewritten. Until
`FINALIZE_FULL_FLASH` succeeds:

- do not remove pedal power;
- do not disconnect USB;
- do not reboot;
- do not run a second host client.

An interruption after erase is expected to fall into the immutable NXP ROM
serial downloader or require the board's boot-mode strap. Restore it with
`tools/ncr2_rom_recover.py` and the pinned NXP RAM flashloader documented in
`ROM_SDP_RECOVERY.md`.

Normal application releases should continue to use A/B updates. Full restore
exists for bootloader development and recovery while the open boot chain is
still being stabilized.
