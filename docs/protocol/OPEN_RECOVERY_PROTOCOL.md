# Open NCR-2 recovery protocol

Status: packet and range-validation layer implemented; USB transport and flash
operations are not yet connected.

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

No metadata record is written until `FINALIZE_IMAGE` has independently
validated the complete inactive slot.

