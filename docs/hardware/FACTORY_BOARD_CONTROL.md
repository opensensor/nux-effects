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

## Observed final state

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

## Output mute relay

The v0.4.9 relay hunt announced each GPIO2 candidate on the indicator before
pulsing it. Group 3 clicked audibly, identifying **GPIO2_IO24** as the relay
control. The pin idles low in the recovered factory startup levels and the
relay energises when it is driven high.

The v0.5.0 routing test then established what it switches, by generating a
440 Hz tone in the source-native transmit path and alternating the relay:

| GPIO2_IO24 | dry guitar | source-native tone |
|---|---|---|
| low, released | audible | audible |
| high, energised | cut | cut |

Energising the relay silences both signals, so it is an **output mute**, not
a bypass. It most likely pairs with the delayed GPIO1_IO26 transition as
pop suppression across audio startup. Open firmware should leave it released
during normal operation and energise it only across transitions.

An earlier hypothesis that this relay had to be engaged to leave bypass mode
was wrong, and is recorded here because it shaped two builds.

The remaining GPIO2 outputs, IO11, IO23, IO25, IO26 and IO27, produced no
audible or visible response.

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

## Proven source-native transmit path

The 440 Hz tone was generated by the open application, carried by the
recovered SAI1/eDMA configuration, and heard at the output jack. Both
preconditions were reported on the indicator first: two green flashes for a
successful SAI and eDMA init, then three yellow for transmit blocks actually
being consumed. This is the first confirmed end-to-end audio output from
source-native code on this hardware.

The capture direction remains unproven. The application ignored its input,
so nothing yet shows that samples reach `ncr2_factory_audio_process_block`.

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
