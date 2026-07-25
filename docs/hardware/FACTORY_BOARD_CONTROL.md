# Factory board-control evidence

This document records the GPIO behavior executed by the verified factory
Metal/Amp image before its audio initializer enables SAI1. It separates
observed register behavior from inferred board semantics so source firmware
does not turn a plausible pin name into an unsafe hardware assumption.

## Method

`tools/emulate_factory_audio.py` models the RT1051 GPIO data set/clear/toggle
aliases and reports the final `DR` and `GDIR` values. Static Thumb
disassembly was then used to identify the routines responsible for each
output. The model starts from zeroed peripheral state and stops at ITCM
address `0x8110`, immediately after the factory SAI1/eDMA initializer.

Use `--trace-gpio` to include every GPIO write. Without that option, the
normal audio trace remains compact and only prints the final GPIO state.

## Observed state at the end of the SAI initializer

```text
GPIO1: DR=0x85003000 GDIR=0x85003000
GPIO2: DR=0x0400000c GDIR=0x0f80080c
GPIO3: DR=0x00000000 GDIR=0x00000000
GPIO4: DR=0x00000000 GDIR=0x00000000
GPIO5: DR=0x00000000 GDIR=0x00000000
```

The final output pins represented by those values are:

| Port/pin | Final level | Factory pad configuration | Observed behavior |
| --- | ---: | ---: | --- |
| GPIO1_IO24 | high | mux 5, pad `0x10b0` | paired with IO31 and changed by the pass/bypass state routine |
| GPIO1_IO26 | high | mux 5, pad `0xf0b0` | driven low, delayed, then driven high once during initialization |
| GPIO1_IO31 | high | mux 5, pad `0x10b0` | paired with IO24 and changed by the pass/bypass state routine |
| GPIO2_IO11 | low | mux 5, pad `0x70b0` | configured once and held low |
| GPIO2_IO23 | low | mux 5, pad `0x70b0` | part of a five-pin state/indicator bank |
| GPIO2_IO24 | low | mux 5, pad `0x70b0` | part of a five-pin state/indicator bank |
| GPIO2_IO25 | low | mux 5, pad `0x70b0` | part of a five-pin state/indicator bank |
| GPIO2_IO26 | high | mux 5, pad `0x70b0` | part of a five-pin state/indicator bank |
| GPIO2_IO27 | low | mux 5, pad `0x70b0` | part of a five-pin state/indicator bank |

That stop point was too early to describe the running analog-output state.
Continuing the same verified Metal image past `0x8110` until its first
runtime refresh of the GPIO2 bank reaches:

```text
GPIO1: DR=0x85003000 GDIR=0x85003000
GPIO2: DR=0x0a80000c GDIR=0x0f80080c
```

The post-SAI transition is exact:

| GPIO2 pin | end of SAI initializer | running state |
|---|---:|---:|
| IO11 | low | low |
| IO23 | low | high |
| IO24, complementary route leg | low | low |
| IO25 | low | high |
| IO26 | high | **low** |
| IO27 | low | high |

This materially changes the open-firmware output diagnosis. The v0.8.2
application held every unknown candidate high while it ran the fuzz, a state
the stock bank updater never produces. v0.8.3 reproduced the ordinary
post-SAI bank, but kept it fixed while the footswitch changed only the LED
and DSP flag. The deeper `0x5532` trace below shows that fixed bank is the
bypass member of a complementary route pair.

The offline trace is available with:

```sh
PYTHONPATH=/path/to/unicorn \
python3 tools/emulate_factory_audio.py dump1.bin \
  --continue-after-audio --trace-gpio \
  --instruction-limit 20000000
```

GPIO1_IO12/13 and GPIO2_IO02/03 are also configured as outputs and toggled
by the software serial-bus implementation. They are not part of the static
analog-output candidate set.

## Static call-path evidence

The relevant Metal ITCM functions are:

| Address | Evidence-backed role |
| --- | --- |
| `0x5438` | initializes the GPIO directions and initial levels |
| `0x5532` | refreshes the GPIO2_IO23–27 bank from runtime state |
| `0x56f0` | exercises GPIO1_IO31/24 with delays during startup |
| `0x7732` | updates GPIO1_IO31/24 from the running pass/bypass state |
| `0x795c` | drives GPIO1_IO26 low, delays, then drives it high |
| `0x9db4` | initializes the GPIO-backed software serial bus |

The executed `0x5532` bank updater establishes that GPIO2_IO24 and
GPIO2_IO25 are a complementary audio-route pair. In the ordinary startup
state it clears IO24 and sets IO25. Changing the associated runtime effect
state clears IO25 and sets IO24 while leaving IO23 and IO27 asserted. The
stock routine never drives IO24 and IO25 high together; the early source
relay test did exactly that, so its click without an audio-route change was
not a valid bypass/effect transition.

The factory diagnostic string table contains `T.PASS`, `B.PASS`, `S.PASS`,
`CODEC`, and `OUTMUTE`, but it does not contain absolute pointers that prove
a one-to-one mapping from those labels to the pins above. GPIO1_IO31/24 are
strong pass/bypass candidates because the running state routine controls
them as a pair. GPIO1_IO26 and GPIO2_IO11 remain the two strongest
codec/mute candidates. Their exact meaning and polarity require a schematic,
PCB continuity check, or live voltage observation.

## Bicolor indicator contract

Further disassembly of the Metal engine resolves the paired GPIO1 outputs as
an active-low bicolor indicator:

| Optical state | GPIO1_IO24 | GPIO1_IO31 |
|---|---:|---:|
| factory color A | low | high |
| factory color B | high | low |
| factory off | high | high |
| both-low diagnostic | low | low |

Routine `0x56f0` selects one of the two single-low states and returns both
outputs high between its startup pulses. Routine `0x7732` updates the same
pair from the running pass/bypass state and contains a both-low path. This
proves that all four electrical combinations are valid factory behavior,
although static analysis alone cannot name color A versus color B or
determine whether both-low optically mixes the dies.

## Confirmed on hardware, 2026-07-24

The v0.4.8 blink-code sweep resolved the indicator on physical hardware. Each
candidate pin was pulsed index+1 times against its recovered idle level, and
the observed blink counts identify the pins unambiguously:

| GPIO | blink group | observed |
|---|---:|---|
| GPIO1_IO24 | 1 | green die |
| GPIO1_IO26 | 2 | nothing visible, not an indicator |
| GPIO1_IO31 | 3 | red die |

Both indicator pins idle high and light when driven low, confirming the
active-low reading. The GPIO1_IO24/GPIO1_IO31 pair recovered from routine
`0x56f0` is therefore the bicolor footswitch indicator, with green and red
dies rather than the red and yellow assumed from casual observation.

The v0.4.9 indicator demonstration then confirmed a third colour. Driving
both pins low lights both dies and mixes to yellow, so the part is a
three-lead bicolour LED rather than a two-terminal inverse-parallel device.
The complete indicator vocabulary available to the open firmware is:

| GPIO1_IO24 | GPIO1_IO31 | colour |
|---|---|---|
| low | high | green |
| high | low | red |
| low | low | yellow |
| high | high | off |

That is four distinct states from two pins, enough to signal bypass,
engaged, recovery and fault conditions without any additional hardware.

The open hardware effect application now reuses the bootloader-confirmed
active-low GPIO1_IO21 primary footswitch as a debounced runtime toggle.
It starts bypassed with the indicator off; each pressed edge toggles the DSP
fuzz and the complementary GPIO2_IO24/25 audio route together, with the red
die held on while the effect is active.

## Complementary audio-route relay

The v0.4.9 relay hunt announced each GPIO2 candidate on the indicator before
pulsing it. Group 3 clicked audibly, identifying **GPIO2_IO24** as the relay
control. That test changed one candidate at a time and therefore did not
reproduce the factory relay state.

The executed `0x5532` routine supplies the missing context: IO24 and IO25 are
always driven as complements. The normal startup state is IO24 low / IO25
high. The alternate effect state is IO24 high / IO25 low. The v0.5.0 and
v0.7.0 tests asserted IO24 without clearing IO25, leaving both high—a state
the factory firmware never uses. Their click or mute behavior therefore did
not identify the relay as a standalone output mute.

## Signal architecture

The dry guitar signal remained audible throughout the tone test, while the
application deliberately ignored its input and emitted only a generated
tone. The dry path therefore reaches the output jack by a route the
application does not control, which matches the usual reverb topology of an
always-connected analog dry path with the converter output mixed in as wet.

Two consequences for the open firmware:

- the pedal was never held in an analog bypass; the earlier "unchanged
  audio" results were entirely explained by the application not executing,
  see `artifacts/open-hardware-v0.4.7/README.md`; and
- silencing the dry signal is not possible from the DSP alone, so a fully
  wet or true-bypass behaviour would need whatever control governs the
  analog mix, which has not yet been located.

## Converter and the state of the audio path

The converter is an **AKM AK4619VN**, a four-channel ADC plus four-channel
DAC codec in a 32-pin QFN at `U3`. Its channel count corroborates the
recovered SAI configuration exactly: four 32-bit TDM slots per frame.

Control is over I2C at 7-bit address `0b001000C`, where the low bit is set
by the `CAD` pin, so `0x10` or `0x11`. Register `0x00` powers the
converters, and **its reset default is `0x00`: `PMAD1`, `PMAD2`, `PMDA1`,
`PMDA2` and `RSTN` are all zero, leaving every ADC and DAC powered down and
held in reset.** The datasheet requires `PDN` low during supply ramp, then
high, then at least 10 ms before any register access.

The open firmware had never performed any I2C transaction, so the codec was
never configured. That single fact explains both directions of the audio
path being dead.

### Confirmed control bus

| property | value |
|---|---|
| SCL | `GPIO_AD_B0_12` / GPIO1_IO12 |
| SDA | `GPIO_AD_B0_13` / GPIO1_IO13 |
| pad function | LPI2C4 alternates, driven bit-banged |
| 7-bit address | `0x10`, so the `CAD` pin is tied low |

Established on hardware by the strict v0.7.6 scan, which required the
AK4619's nonzero reset signature (`0x04 = 0x22`) and found candidate 3.
The continued factory trace independently executes its software serial bus
on GPIO1_IO12/13, corroborating that result.

The earlier v0.6.3 claim that GPIO_AD_B1_00/01 was confirmed was a false
positive and is retracted. The original probe accepted weak acknowledge and
zero-read behavior from a stuck or undriven candidate. Requiring released
lines to return high plus the nonzero reset signature removed that
ambiguity.

Note that `SD_B1_04/05` were briefly excluded from the scan as FlexSPI pads.
They are the `FLEXSPI_A_SS1_B` and `FLEXSPI_A_DQS` alternates, which a single
quad NOR does not use, so only `SD_B1_10/11` genuinely need excluding.

An earlier revision of this document claimed the transmit path was proven
end to end, on the strength of a generated 440 Hz tone being audible during
the v0.5.0 routing test. That claim was wrong and is retracted. With the
DACs powered down by default, the tone cannot have reached the output; what
was audible was the analog dry path alone, which the mute relay cut. RX DMA
and the processing callback are confirmed by the input meter, but the old
processed-block counter did not observe TX channel 16 and therefore was not
proof that a transmit buffer had been consumed.

The v0.7.1 full-scale tone result is also inconclusive. That image restored
the *startup* GPIO levels after configuring the codec, including driving
GPIO1_IO26 low again. The next diagnostic preserved IO26 high and counted
actual TX channel-16 major-loop interrupts. Hardware reported that both RX
and TX progressed, but the codec power check still failed after the other
GPIOs were restored. This disproved the assumption that IO26 alone was the
codec PDN control.

The v0.7.3 diagnostic therefore starts from the all-high candidate state in
which the codec answers, verifies register `0x00 = 0x37`, and then lowers
only the four GPIO2 candidates whose recovered levels differ from that
state. It reads the power register after each transition, re-releases and
reconfigures the codec after finding the PDN, and preserves the discovered
pin high when restoring the remaining outputs.

Note that three earlier images drove GPIO1_IO24 and GPIO1_IO31 correctly and
produced no light whatsoever. That was not a wrong pin hypothesis: the
application never executed, because the boot configuration block declared a
4 MiB FlexSPI window on an 8 MiB part and both application slots live above
that line. See `artifacts/open-hardware-v0.4.7/README.md`.

## Source-firmware consequence

The separately gated source adapter now reproduces the observed levels and
delay. It remains disabled by default, and the first source hardware test
must not guess the unresolved electrical semantics. The reviewed test should:

1. verify that the source adapter places the candidate outputs in the exact
   factory startup levels before enabling SAI;
2. verify the factory 100 ms low-to-high sequence on GPIO1_IO26;
3. leave the GPIO2_IO23–27 bank at a deterministic factory-valid pattern;
4. provide an explicit fail-safe path that returns the audio output to the
   electrically confirmed mute/bypass level; and
5. keep the live-tested Metal restore BINA beside the test artifact.

The source emulator confirms both the final levels and the delayed IO26
transition. Confirming the meaning and safe polarity of GPIO1_IO26 and
GPIO2_IO11 on the physical board is now the principal gate to an intentional
source-image flash.
