# Boot controller

The open boot policy is implemented in
`firmware/bootloader/src/boot_controller.c`. It is hardware-independent:
flash journal access, recovery-request sampling, and slot loading are
injected services, so the complete decision tree runs in host tests.

## Decision order

1. Consume a physical or one-shot software recovery request.
2. If recovery was requested, load metadata for reporting only. Do not
   increment a pending-image trial counter.
3. Load the newest valid two-sector journal record.
4. Roll back an expired pending image, or append its next trial record before
   attempting to boot it.
5. Validate, copy, and verify the selected slot.
6. If a pending slot is invalid, durably reject it before loading the
   confirmed slot.
7. If a confirmed slot is invalid, try the other slot once as an emergency
   fallback.
8. Enter recovery if the journal cannot be trusted or neither slot loads.

The slot callback performs the manifest, vector, payload SHA-256, SDRAM copy,
and copied-image SHA-256 checks. A handoff decision therefore means the
destination image has already passed both source and destination validation.

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

The compile-checked FlexSPI, USB, and board adapters remain disconnected from
the default boot image. The board adapter configures only the two recovered
early-boot input pads, USB1 clocks/PHY/IRQ, and warm reset. The combined
nonbootable link probe proves all three adapters resolve together, but
pending-trial persistence and USB recovery become hardware-approved only
after the documented target gates pass. Until then, a controller recovery
decision stops in the bootloader diagnostic loop.
