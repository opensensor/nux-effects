# Factory audio contract

This document records the audio interface executed by the factory ENG3
(Metal/Amp) image. It is the compatibility target for the first source-built
audio passthrough.

The values below were recovered by running the verified factory image in the
offline RT1051 model in `tools/emulate_factory_audio.py`, then checking the
executed instructions and register definitions against the pinned MCUX SDK.
The tool never communicates with the pedal and only patches its private
emulation copy.

## Important correction

The active factory audio path is **SAI1 + eDMA**. Earlier literal-pool scans
found SAI2 and SAI3 wrappers that are present in the shared SDK code, but ENG3
does not execute them during audio bring-up. This is why execution tracing is
the source of truth for the hardware implementation.

## Stream format

| Property | Factory value |
| --- | --- |
| Nominal sample rate | 48,000 Hz |
| SAI peripheral | SAI1 |
| Word size | 32 bits |
| Words per frame | 4 |
| Frame size | 16 bytes |
| Bit order | MSB first |
| Frame sync | active-low, one-bit early, 32 bits wide |
| Clock ownership | transmitter is asynchronous clock master |
| Receiver clocking | synchronous to transmitter |
| FIFO watermark | 16 words |
| Enabled data channel | channel 0 |
| Transfer engine | eDMA with DMAMUX |

The four words per frame are a real property of the configured serial stream,
not an assumption that it is ordinary two-slot stereo. Until live channel
semantics are measured, source firmware must preserve all four slots.

## Clock tree

The factory image programs the audio PLL with:

| Field | Value |
| --- | --- |
| `loopDivider` | 32 |
| `postDivider` | 1 |
| numerator | 7,800 |
| denominator | 10,000 |
| source | 24 MHz oscillator |

It selects PLL4/Audio PLL for SAI1, with SAI1 predivider 1 and postdivider 32.
The resulting SAI1 MCLK root is approximately 24.585 MHz for an exact 24 MHz
reference. With SAI `DIV=1`, the bit-clock divider is four, yielding
approximately 6.146 MHz. The image nevertheless uses a nominal sample-rate
constant of exactly 48,000 Hz. A source implementation should initially copy
the factory register values rather than “correcting” this small discrepancy.

Executed clock state:

```text
PLL_AUDIO       = 0x80102020
PLL_AUDIO_NUM   = 0x00001e78
PLL_AUDIO_DENOM = 0x00002710
CCM_CSCMR1      = ... SAI1_CLK_SEL=2
CCM_CS1CDR      = ... SAI1_CLK_PRED=0, SAI1_CLK_PODF=31
IOMUXC_GPR_GPR1 = ... SAI1_MCLK_DIR=1
```

## SAI1 registers

The final configured values captured by the model are:

```text
TCSR = 0x80050001    RCSR = 0x80050001
TCR1 = 0x00000010    RCR1 = 0x00000010
TCR2 = 0x07000001    RCR2 = 0x47000001
TCR3 = 0x00010000    RCR3 = 0x00010000
TCR4 = 0x00031f1b    RCR4 = 0x00031f1b
TCR5 = 0x1f1f1f00    RCR5 = 0x1f1f1f00
```

`0x00050000` in the captured CSR words comes from factory writes to clear
write-one-to-clear FIFO flags. A memory-only peripheral model retains those
bits; physical hardware does not. The operational enable bits are transmitter
or receiver enable plus FIFO-request DMA enable.

## Pins

The factory image configures these pins for SAI1 alternate function 3 with
software-input-on and pad value `0x10b0`:

| Signal | RT1051 pad | Mux register |
| --- | --- | --- |
| MCLK | GPIO_AD_B1_09 | `0x401f8120` |
| RX data 0 | GPIO_AD_B1_12 | `0x401f812c` |
| TX data 0 | GPIO_B1_01 | `0x401f8180` |
| TX BCLK | GPIO_B1_02 | `0x401f8184` |
| TX sync | GPIO_B1_03 | `0x401f8188` |

RX BCLK and RX sync are not routed to pads; the receiver is synchronized to
the transmitter internally. The relevant daisy selections are MCLK=1,
RX_DATA00=1, TX_BCLK=2, and TX_SYNC=2.

## DMA topology

The factory image uses eDMA channel 0 for SAI1 RX and channel 16 for SAI1 TX:

| Direction | eDMA channel | DMAMUX source | Peripheral address |
| --- | ---: | ---: | --- |
| RX | 0 | 19 (`SAI1 RX`) | `0x403840a0` |
| TX | 16 | 20 (`SAI1 TX`) | `0x40384020` |

Each direction has two software TCDs linked in a ring, stored at
`0x2000bf40` through `0x2000bfbf`:

```text
RX TCD A: SAI1_RDR0 -> 0x2000bd40, next RX TCD B
RX TCD B: SAI1_RDR0 -> 0x2000bdc0, next RX TCD A
TX TCD A: 0x2000be40 -> SAI1_TDR0, next TX TCD B
TX TCD B: 0x2000bec0 -> SAI1_TDR0, next TX TCD A
```

Every TCD performs 32-bit transfers, 64 bytes per minor loop, and two minor
loops per major loop. A ping or pong half is therefore 128 bytes:

```text
128 bytes / (4 slots * 4 bytes) = 8 audio frames
8 frames / 48,000 Hz = 166.67 microseconds
```

The RX TCDs request a major-loop interrupt and enable scatter/gather. The TX
TCDs enable scatter/gather without a major-loop interrupt. This strongly
suggests that the RX completion callback owns the eight-frame DSP cadence and
fills the corresponding TX half.

The recovered TX DMAMUX value contains `CHCFG.TRIG`, although the RT1050
periodic-trigger feature is defined only for DMA channels 0 through 3 and TX
uses channel 16. The compatibility target preserves the executed value
exactly. Hardware diagnostics route channel 16 in normal peripheral-request
mode and may enable its major-loop interrupt temporarily so TX consumption is
measured independently of the RX callback.

## Converter register image

The same executed Metal path pulses GPIO1_IO26 low-to-high, then writes all
AK4619 registers from `0x00` through `0x14` in ascending order. Its source
table at DTCM address `0x20000000` is:

```text
37 ac 1c 03 22 22 30 30 30 30 00 55 00 00 18 18 18 18 04 05 0a
```

This is the source-firmware compatibility target. In particular, register
`0x12` is `0x04`, not the inferred `0x00` used by the first open codec
configuration. Reproducing only a datasheet-derived subset is insufficient
when diagnosing an otherwise live SAI/eDMA path.

## Reproduction

With a verified private dump and a carved factory Metal image:

```sh
PYTHONPATH=/path/to/unicorn \
python3 tools/emulate_factory_audio.py \
  dump1.bin \
  --engine /path/to/nux_metal_engine.bin
```

Successful execution stops at Metal ITCM address `0x8110`, immediately after
the factory initializer enables both SAI1 directions and their DMA requests.

The source implementation can be built and checked independently:

```sh
cmake -S firmware -B build/factory-audio -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/firmware/cmake/arm-none-eabi-toolchain.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNCR2_BUILD_FACTORY_SLOT_APP=ON \
  -DNCR2_FACTORY_SLOT_AUDIO_PASSTHROUGH=ON \
  -DNCR2_MCUX_SDK_ROOT="$PWD/third_party/mcux-sdk-workspace"
cmake --build build/factory-audio --target ncr2_factory_slot_app
python3 tools/check_factory_slot.py \
  build/factory-audio/ncr2_factory_slot_app.elf
PYTHONPATH=/path/to/unicorn \
python3 tools/emulate_source_audio.py \
  build/factory-audio/ncr2_factory_slot_app.elf
```

The source emulator validates the exact SAI1, PLL, pin, DMAMUX, and hardware
TCD state and executes one synthetic RX-completion interrupt through the
weak passthrough block hook. This is a strong digital-contract check, but it
does not model the external converter or analog switching.

## Remaining hardware gates

The digital audio loop is now implemented. That is not by itself enough to
declare an image safe to flash. Before hardware deployment, the source target
must also reproduce or conservatively preserve:

1. the analog mute/bypass/relay sequencing;
2. a bounded failure mode that leaves the analog path muted or bypassed;
3. a byte-identical known-good Metal restore container.

The current source buffers and TCDs are in DTCM, so data-cache coherency is
not involved. External IRQ0 and the eDMA acknowledgement/copy path are
validated by the post-link checker and source emulator.
The current GPIO evidence and the two remaining pin/polarity questions are
tracked in [FACTORY_BOARD_CONTROL.md](FACTORY_BOARD_CONTROL.md).
