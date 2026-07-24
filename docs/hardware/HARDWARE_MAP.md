# NCR-2 hardware evidence map

This document separates confirmed board facts from inferences and unresolved
measurements. A value is not promoted to the board-support package until its
evidence reaches `confirmed`.

## Confirmed components and memories

| Item | Value | Evidence |
|---|---|---|
| MCU | `MIMXRT1051DVL6B` | PCB marking and valid RT1050 FCFB/IVT |
| External SDRAM | ESMT `M12L2561616A` | PCB marking |
| SDRAM capacity | 32 MiB, 16-bit | part datasheet geometry and stock DCD |
| SDRAM base | `0x80000000` | decoded SEMC DCD writes |
| Boot NOR | W25Q64-class, 8 MiB | device read and verified dump size |
| NOR XIP base | `0x60000000` | IVT pointers and RT1050 memory map |
| Recovery USB | HID `9527:c157` | live enumeration and descriptors |
| Normal USB | MIDI `9527:c177` | live enumeration |

## Confirmed stock peripheral use

These peripherals occur in the stock code and SDK driver strings. Execution
tracing distinguishes the active audio path from unused shared SDK code:

- SAI1 (active factory audio path)
- SAI2 and SAI3 (shared SDK code present; not executed by ENG3 bring-up)
- eDMA and DMAMUX
- ADC1 and ADC2
- ADC_ETC
- PIT and XBARA
- GPIO1 through GPIO5
- USB1
- FlexSPI
- SEMC

## Inputs and outputs requiring physical confirmation

| Function | Candidate/inference | Status | Required evidence |
|---|---|---|---|
| DFU footswitch | active-low GPIO1_IO21 on `GPIO_AD_B1_05`, guarded by active-high GPIO3_IO02 on `GPIO_SD_B1_02` | source-confirmed early-boot behavior; PCB continuity pending | continuity or controlled register observation |
| second footswitch | unknown | unresolved | continuity/register observation |
| step knob | ADC/ADC_ETC channel unknown | unresolved | controlled ADC trace |
| remaining knobs | ADC channels unknown | unresolved | controlled ADC trace |
| expression input | ADC channel/presence unknown | unresolved | schematic trace and ADC capture |
| mode/status LEDs | GPIO/PWM unknown | unresolved | continuity and safe current-path trace |
| bypass/mute | relay/switch/control unknown | unresolved | component ID and oscilloscope |

The failed binary monitor experiment is specifically evidence that
GPIO1_IO21 must not be treated as the runtime switch mapping without another
measurement.

The opt-in open board adapter intentionally uses this pair only for the
recovery condition sampled at startup. It configures both pads as inputs with
100 kOhm pull-ups and hysteresis, and contains no GPIO output writes. This
does not promote either pin to a runtime footswitch mapping.

## Audio interface

Offline execution of the verified factory Metal engine confirms:

- SAI1 at a nominal 48 kHz;
- four 32-bit words per frame;
- transmitter-owned MCLK/BCLK/frame sync;
- receiver synchronized to the transmitter;
- eDMA channel 0 for RX and channel 16 for TX;
- two 128-byte buffers per direction, or eight frames per ping/pong half.

The exact register, pin, clock, and TCD values are in
[FACTORY_AUDIO.md](FACTORY_AUDIO.md).

Remaining physical measurements:

1. Identify every converter/codec IC and strap resistor.
2. Trace the confirmed SAI1 pads to converter pins.
3. Capture MCLK, BCLK, frame sync, TX data, and RX data during stock
   operation to reconcile nominal and measured rates.
4. Determine the meaning and order of all four serial slots.
5. Observe power-on/off mute and bypass timing into an amplifier or dummy
   load.

## Debug and recovery measurements required

- Locate SWDIO, SWCLK, RESET, GND, and reference-voltage pads.
- Determine whether SWD access is fuse-disabled.
- Locate BOOT_MODE or recovery strap access, if present.
- Test the ROM serial downloader only after the strap/fuse state is known.
- Document a safe RT1051-reset method for in-circuit NOR programming.

## Source artifacts

- Machine-readable flash layout:
  `firmware/platform/ncr2/flash_layout.json`
- Parsed stock boot configuration:
  `firmware/platform/ncr2/boot/stock_boot_config.json`
- Full source architecture:
  `PROGRAMMABLE_PEDAL_PLAN.md`
- Opt-in source board wrapper:
  `firmware/platform/ncr2/board/ncr2_board.c`
