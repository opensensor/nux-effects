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
| DFU/runtime footswitch | active-low GPIO1_IO21 on `GPIO_AD_B1_05`, guarded at boot by active-high GPIO3_IO02 on `GPIO_SD_B1_02` | physically confirmed by open-bank short press and two-second runtime hold | PCB continuity remains useful but is no longer required for the firmware mapping |
| second footswitch | unknown | unresolved | continuity/register observation |
| Decay knob | ADC1 channel 5 (`GPIO_AD_B1_00`) | source-confirmed from executed Reverb ADC_ETC chain and parameter path | live range check |
| Tweak knob | ADC1 channel 8 (`GPIO_AD_B1_03`) | source-confirmed from executed Reverb ADC_ETC chain and parameter path | live range check |
| Type/step knob | ADC1 channel 9 (`GPIO_AD_B1_04`) | source-confirmed; stock code quantizes this channel into eight modes. Detent voltages **not yet measured** | `pedalctl calibrate-selector` |
| Level knob | ADC1 channel 11 (`GPIO_AD_B1_06`) | source-confirmed from executed Reverb ADC_ETC chain and parameter path | live range check |
| expression input | ADC channel/presence unknown | unresolved | schematic trace and ADC capture |
| mode/status LEDs | GPIO/PWM unknown | unresolved | continuity and safe current-path trace |
| bypass/mute | relay/switch/control unknown | unresolved | component ID and oscilloscope |

The earlier binary monitor sampled GPIO1's `DR` output latch and therefore
could not observe a runtime input. The physically working open-bank gestures
establish GPIO1_IO21 as the runtime switch; runtime code reads `PSR` at
`0x401b8008`. The board adapter configures the pad as an input with a 100 kOhm
pull-up and hysteresis and contains no footswitch GPIO output writes.

## Audio interface

Offline execution of the verified factory Metal engine confirms:

- SAI1 at a nominal 48 kHz;
- four 32-bit words per frame;
- transmitter-owned MCLK/BCLK/frame sync;
- receiver synchronized to the transmitter;
- eDMA channel 0 for RX and channel 16 for TX;
- two 128-byte buffers per direction, or eight frames per ping/pong half.

Static disassembly plus peripheral-write emulation of the original Reverb
engine resolves its four-control ADC scan. ADC1 is configured for 12-bit
conversion with 32-sample hardware averaging. ADC_ETC trigger 0 runs a
back-to-back chain over channels 5, 8, 9 and 11. The result-consumer maps
those values to Decay, Tweak, the eight-position Type selector and Level,
respectively. The related Metal image adds channel 12 as a fifth shared-board
control, but the Reverb image neither configures nor consumes that segment.

The open multi-effect hardware application preserves those inputs but gives
them a conventional pedal mapping:

| Panel label | Open control |
|---|---|
| Decay | Amount (effect-specific intensity/rate/delay) |
| Tweak | Character |
| Type | eight-position effect selector |
| Level | effect output level |

The source audio application also gives the footswitch two gestures:

- a short press, completed on release, toggles the open effect and analog
  bypass; and
- a two-second hold launches one preserved factory engine. Type positions
  1–2 select Delay, 3–4 select Reverb, 5–6 select Modulation, and 7–8 select
  Metal. The indicator flashes that factory bank number before warm reset.

The launch request occupies only retained SRC GPR10 and is consumed before
audio initialization. Before replacing ITCM, the launcher validates the
chosen engine's exact stack/reset, its main-loop call and stock LED-updater
entry, and its zero-filled 256-byte tail cave. It patches only the ITCM RAM
copy: one proven main-loop call is redirected through a monitor that samples
the footswitch and then tail-calls the original stock LED updater. No factory
interrupt vector is changed. The preserved factory NOR remains byte-identical.
In a factory engine, choose the destination with the same Type-detent pairs,
hold for two to five seconds, and release. Hold for at least five seconds and
release to warm-reset into the open bank.
The factory Type knob continues to select that stock engine's own modes.
Holding while applying power remains the independent Open Recover gesture.

### Type selector: measured 2026-07-27

Measured on physical hardware over `RECOVERY_COMMAND_READ_KNOBS`, stepping
every detent while polling ADC1 channel 9. Knob position 1 is the topmost
printed label.

| Knob position | ADC | gap to next |
|---:|---:|---:|
| 1 | 3581 | 510 |
| 2 | 3071 | 511 |
| 3 | 2560 | 511 |
| 4 | 2049 | 511 |
| 5 | 1538 | 510 |
| 6 | 1028 | 512 |
| 7 | 516 | 514 |
| 8 | 2 | — |

Two facts that no previous build accounted for:

- **The ladder descends with position.** Position 1 is the *highest* voltage
  and position 8 is very nearly zero, so effect order assigned by ascending
  ADC runs backwards relative to the printed labels.
- **It spans 2..3581, not 0..4095.** The eight detents are evenly spaced at
  ~511 counts, but the top of the ladder stops roughly 500 counts short of
  full scale.

Resting noise is 1 to 5 counts across the whole sweep, so the shipped
160-count latch radius is more than thirty times larger than anything the
hardware actually produces.

### Two defects the measurement proves

Applying the shipped quantiser `effect = (adc * 8) / 4096` to the measured
detents:

| Knob position | ADC | bin | effect selected |
|---:|---:|---:|---|
| 1 | 3581 | 6 | Guerrilla Trem |
| 2 | 3071 | 5 | Cocked Wah |
| 3 | 2560 | 5 | Cocked Wah |
| 4 | 2049 | 4 | Rage Drive |
| 5 | 1538 | 3 | Echoes Tape |
| 6 | 1028 | 2 | Breathe Vibe |
| 7 | 516 | 1 | Wall Fuzz |
| 8 | 2 | 0 | Shine Drive |

1. **Positions 2 and 3 are the same effect.** Both land in bin 5, so one
   detent of the eight is a duplicate and moving between them changes
   nothing at all — which is indistinguishable from a broken selector.
2. **Whammy Fuzz can never be selected.** Bin 7 requires an ADC value of at
   least 3584 and the highest detent measures 3581, missing the last bin by
   **three counts**.

The 160-count latch radius, long suspected, is *not* implicated: every
adjacent gap is about 511 counts, comfortably clear of it. That suspicion
came from reasoning about an unmeasured ladder, and the measurement retired
it.

The correct algorithm is nearest-detent matching against this measured table
rather than equal bins, which tolerates both the offset span and the
descending order. Recalibrate with:

```sh
python3 tools/pedalctl.py calibrate-selector
```

Algorithm changes use a roughly 11 ms effect-to-clean-to-effect crossfade;
selector noise therefore falls back to clean converter audio and can never
mute the wet route.

The artist-inspired build groups four spacious, sustaining psychedelic
presets followed by four tight or experimental heavy presets. From low to
high ADC value they are Shine Drive, Wall Fuzz, Breathe Vibe, Echoes Tape,
Rage Drive, Cocked Wah, Guerrilla Trem and Whammy Fuzz. These names describe
the intended playing response rather than copies of a proprietary circuit.
All eight retain the `+/-0x10000000` internal DSP ceiling. The final DAC
stage permits `+/-0x20000000`, because the Level control now reaches +6 dB
instead of stopping at unity.

The first source build averaged all four AK4619 input slots and mapped the
full Level travel to 0..1x. On this mono pedal that made the open bank 12–18
dB quieter than a stock engine: inactive TDM slots diluted the input, and a
noon Level setting imposed another 6 dB cut. The runtime now selects the
strongest ADC slot per frame and maps Level to 0..2x, putting unity at the
physical midpoint while retaining a bounded +6 dB trim range.

The two physically preferred modulation slots, Breathe Vibe and Guerrilla
Trem, remain unchanged. Shine Drive, Wall Fuzz, Rage Drive and Whammy Fuzz
retain their high internal gain but use a parallel clean attack and a lower
wet return so they no longer arrive roughly an order of magnitude louder
than those reference slots. Cocked Wah receives compensating gain before
its parallel clean blend.

| Effect | Decay / Amount | Tweak / Character |
|---|---|---|
| Shine Drive | gain, 1x to 24x | warm/soft to bright/firm |
| Wall Fuzz | sustain gain, 4x to 96x | dark/rounded to open/firm |
| Breathe Vibe | modulation rate, 0.35 to 5 Hz | shallow throb to deep swirl |
| Echoes Tape | delay time, 120 to 625 ms | repeat level, feedback and tape age |
| Rage Drive | gain, 2x to 40x | thick to tight/biting |
| Cocked Wah | resonant center frequency | resonance and filter drive |
| Guerrilla Trem | rate, 1 to 16 Hz | smooth half-depth pulse to hard chop |
| Whammy Fuzz | dry-to-octave-up blend | clean pitch voice to driven fuzz |

### Measured branch levels

Measured 2026-07-27 by rendering every branch through the native preview
harness (`host/editor/hardware_app.py`) with a decaying three-note chord, at
both a soft and a hard pick. Figures are rms relative to the same branch's own
dry input:

| Preset | soft pick | hard pick |
|---|---:|---:|
| Shine Drive | +3.8 dB | +0.4 dB |
| Wall Fuzz | +8.7 dB | +1.9 dB |
| Breathe Vibe | −3.9 dB | −3.9 dB |
| Echoes Tape | −2.5 dB | −2.5 dB |
| Rage Drive | +6.4 dB | +1.5 dB |
| Cocked Wah | −0.2 dB | −0.4 dB |
| Guerrilla Trem | −3.3 dB | −3.3 dB |
| Whammy Fuzz | +4.3 dB | +1.5 dB |

**All eight branches produce continuous audio inside a roughly 12 dB window,
and none collapses.** This matters for interpreting the field report that
most Type positions sounded dead or faint: the two positions reported as good
(Breathe Vibe and Guerrilla Trem) are in fact the two *quietest* branches, so
loudness does not explain which positions were liked.

The harness sets the effect index directly and never runs the selector, so
these figures isolate the DSP from the control path. The DSP is healthy;
anything the player hears as a dead position therefore originates in the
selector, not in the algorithms. Reading the dry-blend coefficient of a branch
alone is misleading here — Shine Drive, Wall Fuzz and Rage Drive each pass
only about 0.4x dry, but their wet return makes up the difference.

The Whammy voice uses two delay-line read heads 180 degrees apart with
crossfaded windows. It avoids the full-wave rectifier responsible for the
old octave fuzz's harsh upper-register breakup. Tape feedback is bounded
below unity and its dual taps are progressively filtered. The 128 KiB delay
line occupies the linker-reserved, non-loadable SDRAM audio-buffer region,
which the application clears before making the effect route available.

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
