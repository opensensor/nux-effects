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
