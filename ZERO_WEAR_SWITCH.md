# Zero-wear four-engine switching

This is a static prototype for selecting all four factory engines without
erasing NOR flash at runtime.

Nothing in `zero_wear/` writes a device. `tools/build_zero_wear.py` only
creates local binaries and a guarded BINA artifact for later review.

## Design

The stock selector remains `1`, so the launcher always copies slot 1 to
ITCM. Slot 1 is replaced by a minimal picker:

1. Read and clear a one-shot request from `SRC_GPR9` (`0x400f8040`).
2. Use engine 1 (reverb) when no valid request exists.
3. Record the selected engine in `SRC_GPR7`; clear the hold counter in
   `SRC_GPR8`.
4. Branch to an XIP stub in the picker's flash source.
5. The XIP stub calls the stock launcher `memcpy` at `0x600029f8` to copy
   the chosen 0x1e000-byte engine to ITCM, then calls the stock handoff at
   `0x60003ff8`.

The XIP transition is required: the launcher copy routine is a plain
`memcpy`, so calling it from the ITCM picker would overwrite the picker's
return address before it returned.

All four GPT1 vectors are patched to the same monitor injected at ITCM
`0x1df00`. That 256-byte window is zero-filled in every factory engine.
Keeping the interrupt code in ITCM avoids assuming that each engine's
post-startup MPU configuration permits FlexSPI instruction fetch. Their
stock GPT1 ISRs are byte-for-byte identical, and their GPT1 timer
configuration uses:

- clock: IPG (`kCLOCK_IpgClk`);
- prescaler: 24;
- compare: 100,000;
- stock ISR: clear output-compare flag 1 and return.

The factory starts GPT1 for polling but leaves `GPT1->IR` at zero and does
not enable `GPT1_IRQn` in the NVIC. Each patched engine redirects the first
four bytes of its identical `GPT_StartTimer` wrapper into a second monitor
stub at ITCM `0x1dfa0`. That stub preserves the original timer-start
operation, enables output-compare interrupt 1, and enables NVIC IRQ 100.

At the observed 132 MHz IPG clock this interrupt runs at 55 Hz. The monitor
samples the active-low `GPIO1_IO21` footswitch and requires 275 consecutive
pressed samples (5 seconds). It advances the engine modulo four and stages
the request in `SRC_GPR9`. The system reset occurs when the switch is
released. Waiting for release is important because the untouched launcher
uses this same switch for DFU entry; it must see the switch high after the
warm reset. Starting from the factory default, the user-visible cycle is:

`Reverb -> Modulation -> Metal -> Delay -> Reverb`

The picker consumes the request after reset.

The monitor cave also has a startup-specific guard. For each exact factory
engine, the builder pins the reset vector, Keil `__main` entry, and last
nonzero byte. The audited startup path sets VTOR/MSP, initializes the FPU,
then enters the runtime; it has no scatter-load pass that can overwrite the
zero-filled ITCM tail at `0x1df00`.

NXP's MCUXpresso SRC driver documents that SRC GPR values are held through
the reset process. It reserves GPR1/GPR2 by convention for wake
entry/argument and says the other GPRs can hold arbitrary values. This
prototype uses GPR7-GPR9; the stock 8 MiB image has no literal reference to
the SRC block or these register addresses.

Source:
<https://github.com/nxp-mcuxpresso/mcux-sdk/blob/main/drivers/src/fsl_src.h>

## Flash layout

| Offset | Content |
|---:|---|
| `0x60000` | factory delay, GPT1 vector/code cave patched |
| `0x80000` | picker + XIP runtime |
| `0xa0000` | factory modulation, GPT1 vector/code cave patched |
| `0xc0000` | factory metal, GPT1 vector/code cave patched |
| `0xe0000` | relocated factory reverb, GPT1 vector/code cave patched |

The generated DFU image covers `0x60000..0xfffff`. It cannot affect the
FCFB, launcher, selector, or DFU bootloader below `0x60000`.

## Build

```sh
./tools/build_zero_wear.py
python3 -m unittest discover -s tests -v
```

The builder pins the known stock dump SHA-256, refuses to reuse an existing
output directory, validates that every engine differs only in its GPT1
vector, four-byte GPT start hook, and verified zero-filled monitor cave,
round-trips the BINA parser, and records artifact hashes.

The build produces three reviewable stages:

| Artifact | Patched engines | Purpose |
|---|---|---|
| `picker-only.bina` | none | Prove picker, relocation, XIP copy, and normal reverb boot |
| `retention-test.bina` | reverb only | Hold and release once: retained request should reboot into stock modulation |
| `zero-wear-switch.bina` | all four | Full four-engine cycle |

## Hardware test gates

The prototype is deliberately not marked ready to flash. Before a live
write:

1. Review both generated disassemblies.
2. Confirm the picker and XIP addresses against the exact stock dump.
3. Deploy `picker-only.bina`, which defaults to relocated reverb.
4. Confirm cold boot and DFU recovery.
5. Deploy `retention-test.bina`; hold for about 5 seconds and release. It
   should warm-reset from reverb into unmodified modulation. A power cycle
   returns to reverb.
6. Only then deploy `zero-wear-switch.bina`.

The DFU footswitch check executes in the untouched launcher before slot 1,
so USB recovery remains available even if the picker or engine patch fails.
