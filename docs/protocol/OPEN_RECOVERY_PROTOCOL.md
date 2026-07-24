# Open NCR-2 recovery protocol

Status: packet, range-validation, retry-cache, update transaction, metadata
journal, and trial/confirmation policy layers implemented and host-tested.
USB transport and FlexSPI operations are not yet connected on hardware.

## Transport

The intended recovery transport is one vendor-HID interface with 64-byte
input and output reports. This matches the physical shape already proven on
the pedal while using an independent, versioned packet format.

The open firmware must use an appropriately assigned development/community
USB VID/PID before distribution. NUX's VID/PID is not part of this protocol.

## Packet layout

Every packet is exactly 64 bytes, little-endian:

| Offset | Size | Field |
|---:|---:|---|
| `0x00` | 4 | magic `NXFX` (`0x5846584e`) |
| `0x04` | 1 | protocol version |
| `0x05` | 1 | command |
| `0x06` | 2 | flags |
| `0x08` | 4 | session nonce |
| `0x0c` | 4 | sequence |
| `0x10` | 4 | slot-relative offset |
| `0x14` | 2 | payload length, `0..32` |
| `0x16` | 2 | response status |
| `0x18` | 4 | payload CRC32 |
| `0x1c` | 4 | header CRC32 over bytes `0x00..0x1b` |
| `0x20` | 32 | payload |

The CRC is the reflected IEEE CRC-32 used by zlib.

## Commands

| Value | Command | Mutating |
|---:|---|---|
| 1 | `GET_INFO` | no |
| 2 | `BEGIN_IMAGE` | yes |
| 3 | `ERASE_SLOT` | yes |
| 4 | `WRITE_CHUNK` | yes |
| 5 | `READ_CHUNK` | no |
| 6 | `FINALIZE_IMAGE` | yes |
| 7 | `SET_PENDING` | yes |
| 8 | `REBOOT` | yes |
| 9 | `GET_LOG` | no |

`BEGIN_IMAGE` establishes a fresh nonzero session nonce and resets sequence
handling. Every subsequent session command must match the nonce and exact
expected sequence. Retries receive the prior response; skipped or reordered
commands are rejected.

The low bit of `flags` selects slot A (`0`) or B (`1`). All other flag bits
are currently invalid. `GET_INFO` uses zero flags. The target selected by
`BEGIN_IMAGE` must be different from both the confirmed and active slot.

`WRITE_CHUNK` carries `1..32` payload bytes. Its `offset` must equal the end
of the previous successful write, which makes the transfer contiguous and
prevents holes. `READ_CHUNK` uses `length` as the requested read size and
sends that many zero payload bytes so both request CRCs remain unambiguous.

`FINALIZE_IMAGE` re-reads the manifest and payload through the flash backend,
then independently validates:

- manifest magic, version, size, board, load address, and CRC32;
- DTCM initial stack alignment/range;
- Thumb reset vector within the loaded image; and
- SHA-256 of the complete stored payload.

`SET_PENDING` is rejected until all those checks pass. A byte received over
USB is never trusted merely because its transfer was acknowledged.

## Address safety

Host offsets are always relative to an explicitly selected application slot.
The parser resolves them only after checking:

```text
slot is A or B
length > 0
offset < slot_size
length <= slot_size - offset
```

The subtraction form prevents 32-bit wraparound. No command accepts an
absolute flash address. The resolver can produce addresses only inside:

- application A: `0x60400000–0x605fffff`
- application B: `0x60600000–0x607fffff`

It cannot address the FCFB, DCD, bootloader, metadata, or preserved factory
region.

## Update transaction

The intended transaction is:

1. `GET_INFO`
2. `BEGIN_IMAGE`
3. `ERASE_SLOT` for the inactive slot
4. ordered `WRITE_CHUNK` packets
5. optional `READ_CHUNK` verification
6. `FINALIZE_IMAGE` to validate manifest and complete-image SHA-256
7. `SET_PENDING`
8. `REBOOT`

The bootloader then records each trial before executing the pending slot.
The application must explicitly confirm itself after a healthy audio/control
startup. Exhausted trials roll back to the last confirmed slot.

Confirmation is not an application-side flash write. The bootloader publishes
the pending slot and exact metadata sequence in retained SRC GPR3–GPR6. Once
healthy, the application converts that handoff token to a confirmation token
and warm-resets. The bootloader consumes it, verifies both fields against the
current pending record, and appends the confirmed record itself. A watchdog
reset, torn token, stale sequence, or replay cannot confirm an image.

No metadata record is written until `FINALIZE_IMAGE` has independently
validated the complete inactive slot.

Metadata uses two 4 KiB sectors of 32-byte append-only records. A partially
programmed record has no valid CRC and is ignored. When the active sector is
full, the next state is programmed and verified in the other sector before
the old sector is erased. If power fails—or cleanup erase fails—both sectors
are scanned and the newer valid sequence wins.

Pending images receive exactly three recorded attempts. The trial counter is
durably appended before each jump. A fourth reset without confirmation
appends a rollback record and selects the last confirmed image. The
host-tested handoff layer can arm the opt-in RT1051 WDOG1 adapter for an
eight-second trial. Hardware wiring of these already-tested policies waits
on the RAM-resident FlexSPI backend and target watchdog gate.

## Host tooling

Build a validated slot stream:

```sh
python3 tools/open_image.py pack-slot \
  --application build/open/ncr2_app.bin \
  --version 0.1.0 \
  --build-number 1 \
  --output build/open/ncr2-app-0.1.0.slot
```

Offline inspection is safe now:

```sh
python3 tools/pedalctl.py inspect-slot \
  build/open/ncr2-app-0.1.0.slot
```

The `info` and `upload` commands are implemented against `hidraw`, including
exact retries, sequence checking, inactive-slot selection, finalization, and
pending activation. They must not be used until the matching open recovery
firmware and a non-NUX USB identity are running on the pedal.

## Status values

In addition to packet-format errors, the engine reports explicit failures for
backend I/O, invalid transaction phase, invalid image, active-slot selection,
premature activation, unsupported flags, and out-of-order writes. Failed
commands do not consume a sequence number, so the host can correct or retry
the same step.
