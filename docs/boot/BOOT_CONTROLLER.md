# Boot controller

The open boot policy is implemented in
`firmware/bootloader/src/boot_controller.c`. It is hardware-independent:
flash journal access, recovery-request sampling, and slot loading are
injected services, so the complete decision tree runs in host tests.

## Decision order

1. Consume and clear any retained trial-confirmation token.
2. Consume a physical or one-shot software recovery request.
3. If recovery was requested, ignore the confirmation for this boot and load
   metadata for reporting only. Do not
   increment a pending-image trial counter.
4. Accept a confirmation only when its slot and sequence match the exact
   pending journal record.
5. Load the newest valid two-sector journal record.
6. Roll back an expired pending image, or append its next trial record before
   attempting to boot it.
7. Validate, copy, and verify the selected slot.
8. If a pending slot is invalid, durably reject it before loading the
   confirmed slot.
9. If a confirmed slot is invalid, try the other slot once as an emergency
   fallback.
10. Enter recovery if the journal cannot be trusted or neither slot loads.

The slot callback performs the manifest, vector, payload SHA-256, SDRAM copy,
and copied-image SHA-256 checks. A handoff decision therefore means the
destination image has already passed both source and destination validation.

## Pending-image confirmation

SRC GPR3 through GPR6 (`0x400f8028–0x400f8037`) hold a four-word
token/inverse and sequence/inverse mailbox. Before handing off to a pending
application, the bootloader publishes the selected slot and the exact
journal sequence. A watchdog reset leaves that token in the `handoff` state,
which cannot confirm anything and is consumed on the next boot.

After the application has passed its SDRAM, audio-DMA, and control-health
gates, it can call:

```c
#include "ncr2_boot_request.h"

if (ncr2_boot_confirm_healthy() == 0) {
    /* Request the board's validated warm-reset path. */
}
```

This atomically changes the retained token from `handoff` to `confirmation`;
it does not write flash. On the next warm reset, the bootloader accepts it
only if both the slot and journal sequence still match the pending record.
The bootloader—not the application—then appends the durable confirmed state.
Stale, replayed, partial, and mismatched tokens are cleared without
confirming a slot.

`boot_handoff_prepare` exposes watchdog start as an injected service, so the
whole policy runs in host tests. The opt-in RT1051 adapter configures WDOG1
for an eight-second pending trial, continues in wait/stop, pauses under a
debugger, and does not assert the external watchdog output. The default
offline boot image deliberately supplies no watchdog callback.

## One-shot recovery mailbox

The mailbox uses two retained System Reset Controller registers:
`SRC_GPR8` at `0x400f803c` and `SRC_GPR9` at `0x400f8040`. The current
token is an eight-byte magic/inverse pair. Application code can arm it with:

```c
#include "ncr2_boot_request.h"

ncr2_boot_recovery_arm();
/* Request a warm system reset through the future board-support API. */
```

The bootloader consumes and clears valid and partial tokens. A partial write
cannot accidentally match the 64-bit pair, and a deliberate recovery entry
does not spend an A/B trial.

The pinned MCUXpresso SDK documents SRC GPR values as retained through the
reset process and permits arbitrary values in GPRs other than the two
reserved wake registers. The factory-engine zero-wear prototype has also
demonstrated SRC GPR retention through `NVIC_SystemReset` on this pedal. The
open bootloader still consumes the token only once and validates both words,
so unrelated reset state cannot become a recovery request accidentally.

## Hardware gate

The default bootloader uses a read-only XIP journal backend. Its erase and
program callbacks intentionally fail. This lets ordinary confirmed-slot
selection use the tested controller without making the image capable of
mutating NOR.

The default boot image remains disconnected from the FlexSPI, USB, and board
adapters. A separate opt-in hardware bootloader now connects all three to the
same controller and installs its vector table explicitly. It is read-only by
default; USB enumeration and physical NOR mutation require independent build
switches. The board adapter configures only the two recovered early-boot
input pads, USB1 clocks/PHY/IRQ, WDOG1, and warm reset.

Passing the post-link hardware checks proves composition, vectors, memory
placement, and protected-range policy only. Pending-trial persistence and
USB recovery become hardware-approved only after the documented target gates
pass. Until then, the default controller recovery decision stops in the
bootloader diagnostic loop.
