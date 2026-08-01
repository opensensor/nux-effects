# Open NCR-2 recovery protocol

Status: protocol version 2 is implemented and host-tested. The USB transport,
open boot path, A/B metadata, and read-only `GET_INFO` path have run on the
pedal. Version 1 exposed a hardware defect: `ERASE_SLOT` erased an entire
2 MiB slot synchronously and an MCUX FlexSPI poll could wait forever.
Version 2 bounds every controller/busy poll and erases only the sectors
covered by the declared incoming image.

## Transport

The intended recovery transport is one vendor-HID interface with 64-byte
input and output reports. This matches the physical shape already proven on
the pedal while using an independent, versioned packet format.

Development hardware temporarily borrows `9527:c157` so existing host setup
continues to work. The product string `NUX Effects Open Recover`, packet
magic, protocol version, and `bcdDevice=0.05` distinguish it from the stock
NUX updater. A project-owned USB identity remains mandatory before
distribution.

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
| 10 | `BEGIN_FULL_FLASH` | yes |
| 11 | `ERASE_FULL_FLASH` | yes |
| 12 | `FINALIZE_FULL_FLASH` | yes |
| 13 | `READ_KNOBS` | no |

`BEGIN_IMAGE` establishes a fresh nonzero session nonce and resets sequence
handling. In protocol version 2 its `offset` field declares the exact
manifest-plus-payload byte count. The engine rejects sizes smaller than a
manifest plus two vectors or larger than one slot. Every subsequent session
command must match the nonce and exact expected sequence. Retries receive the
prior response; skipped or reordered commands are rejected.

The low bit of `flags` selects slot A (`0`) or B (`1`). In the XIP A/B
personality, every other bit is invalid. In the RAM personality, bit 15
(`0x8000`) identifies a whole-flash transaction and is accepted only when
`GET_INFO` advertises
`RECOVERY_CAPABILITY_FULL_FLASH_RAM`. `GET_INFO` uses zero flags. The target selected by
`BEGIN_IMAGE` must be different from both the confirmed and active slot.

`WRITE_CHUNK` carries `1..32` payload bytes. Its `offset` must equal the end
of the previous successful write, which makes the transfer contiguous and
prevents holes. Both write and read ranges must remain inside the byte count
declared by `BEGIN_IMAGE`. `READ_CHUNK` uses `length` as the requested read
size and sends that many zero payload bytes so both request CRCs remain
unambiguous.

### `READ_KNOBS`

Samples the four front-panel controls on ADC1 and returns one 32-byte
payload. It touches no flash and opens no session, so it uses zero flags,
zero session, and zero sequence exactly like `GET_INFO` and `GET_LOG`. It is
available in both the XIP bootloader and the RAM personality, including
inside a whole-flash session whose erase has already run.

The command exists because the stepped Type control's detent voltages have
never been measured. Firmware that quantises that channel into equal bins is
guessing, and a detent that lands near an assumed boundary — or two detents
that fall in one bin — silently refuses to change program.

| Offset | Size | Field |
|---:|---:|---|
| `0x00` | 4 | magic `KNOB` (`0x424f4e4b`) |
| `0x04` | 8 | `uint16` values: Amount, Character, Type, Level |
| `0x0c` | 2 | minimum Type reading across the burst |
| `0x0e` | 2 | maximum Type reading across the burst |
| `0x10` | 4 | ADC1 channel numbers, matching the value order |
| `0x14` | 1 | burst length used for the Type channel |
| `0x15` | 1 | `1` when every conversion succeeded |
| `0x16` | 1 | ADC resolution in bits |
| `0x17` | 1 | reserved |
| `0x18` | 4 | capture counter, incremented per command |
| `0x1c` | 4 | reserved |

The min/max pair bounds the electrical noise at a resting detent, which is
the number a movement threshold must clear so a stationary knob can never
trip it. The capture counter lets a host reject a stale HID input report left
in the queue by an earlier exchange.

The converter is left disabled until the first `READ_KNOBS`, so a recovery
session that never asks behaves exactly as it did before the command existed.
A failed conversion still returns a well-formed payload with `valid == 0`
rather than an error status the host has to interpret.

`READ_KNOBS` is deliberately exempt from the retry cache. That cache exists so
a resent mutating command is not applied twice; a live measurement is the
opposite, and every identical request must sample the converter again. A host
polling a stationary knob sends byte-identical packets, and a cached reply
would freeze the reading at the first capture.

Firmware that cannot sample the panel leaves capability bit `0x100` clear and
answers `BAD_COMMAND`, which is also what any build predating the command
returns. `BACKEND_ERROR` therefore always means a real converter fault.

`ERASE_SLOT` rounds the declared byte count up to the W25Q64's 4 KiB erase
unit and erases exactly that prefix of the inactive slot. A 5.8 KiB bridge
therefore erases two sectors, not 512. Blank sectors are read and skipped.
The RT1051 FlexSPI transfer, software-reset, and NOR-busy polls all have
finite limits; a controller fault returns a diagnostic backend error rather
than wedging USB forever.

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
2. `BEGIN_IMAGE` with the exact total slot-image length in `offset`
3. `ERASE_SLOT` for the inactive slot
4. ordered `WRITE_CHUNK` packets
5. optional `READ_CHUNK` verification
6. `FINALIZE_IMAGE` to validate manifest and complete-image SHA-256
7. `SET_PENDING`
8. `REBOOT`

The bootloader then records each trial before executing the pending slot.
The application must explicitly confirm itself after a healthy audio/control
startup. Exhausted trials roll back to the last confirmed slot.

`REBOOT` may reset the RT1051 after it accepts the HID OUT report but before
the final HID IN acknowledgement reaches Linux. After `SET_PENDING` (or a
verified full-flash finalization) has succeeded, `pedalctl` treats only an
operating-system device-disconnect error at `REBOOT` as an expected
unacknowledged reset and reports `reboot_acknowledged: false`. Errors or lost
acknowledgements at every earlier stage remain fatal.

Confirmation is not an application-side flash write. The bootloader publishes
the pending slot and exact metadata sequence in retained SRC GPR3–GPR6. Once
healthy, the application converts that handoff token to a confirmation token
and warm-resets. The bootloader consumes it, verifies both fields against the
current pending record, and appends the confirmed record itself. A watchdog
reset, torn token, stale sequence, or replay cannot confirm an image.

No metadata record is written until `FINALIZE_IMAGE` has independently
validated the complete inactive slot.

## RAM-resident whole-flash restore

Whole-flash restore is intentionally a separate personality, built as
`ncr2_ram_recovery`. The production development build embeds that checked
binary in the protected bootloader partition. On physical recovery entry,
the small XIP stub copies it to SDRAM and jumps to its vector table before
USB starts. It does not consume an application/effect slot. A post-link
checker rejects any load segment, required entry point, or direct branch
into the FlexSPI XIP window, and the bootloader checker verifies that the
embedded bytes equal the independently checked RAM binary. Only this image
calls `ncr2_flexspi_nor_init_full_flash()` and
`recovery_engine_enable_full_flash()`.

The RAM personality also loads the durable A/B boot journal and permits
ordinary bounded slot uploads. Full-flash mode widens the low-level address
policy, not the slot transaction policy: inactive-slot updates still protect
the confirmed and selected slots, validate the manifest, and append
`SET_PENDING` through the journal backend. A whole-flash transaction has no
`SET_PENDING` step because the supplied 8 MiB image replaces the journal
itself.

The XIP bootloader compiles the command numbers so it can return
`FULL_FLASH_DISABLED`, but it neither advertises the capability nor widens
its NOR write policy.

A destructive transaction is:

1. `GET_INFO`, requiring capability bit `0x20`;
2. `BEGIN_FULL_FLASH`, with:
   - flags exactly `0x8000`;
   - session field `0x45504957` (little-endian `WIPE`);
   - offset exactly `0x00800000`; and
   - payload exactly the expected 32-byte SHA-256;
3. 128 ordered `ERASE_FULL_FLASH` requests, each erasing and acknowledging
   one 64 KiB range; the response offset is the first byte not yet erased;
4. ordered `WRITE_CHUNK` reports covering all 8 MiB;
5. optional `READ_CHUNK` reports;
6. `FINALIZE_FULL_FLASH`, which re-hashes all 8 MiB; and
7. `REBOOT`.

There is no `SET_PENDING` step because the transaction replaces the journal,
bootloader, and both slots. `REBOOT` is rejected until the complete-chip hash
matches. Host tooling additionally requires the literal confirmation
`WIPE-ALL-8MIB`.

Capability bit `0x40` advertises progressive full-chip erase. The host
refuses the original monolithic implementation when that bit is absent.
Each 64 KiB response is a durable progress boundary, and exact-request retry
returns the cached response without erasing the range twice.

This mode is deliberately not power-loss tolerant after the full erase. If
power or USB is lost before a complete verified rewrite, the chip may be
blank or incomplete and the immutable RT1051 ROM downloader is the recovery
path. This is factory recovery, not the normal update mechanism.

Metadata uses two 4 KiB sectors of 32-byte append-only records. A partially
programmed record has no valid CRC and is ignored. When the active sector is
full, the next state is programmed and verified in the other sector before
the old sector is erased. If power fails—or cleanup erase fails—both sectors
are scanned and the newer valid sequence wins.

Pending images receive exactly three recorded attempts. The trial counter is
durably appended before each jump. A fourth reset without confirmation
appends a rollback record and selects the last confirmed image. The
host-tested handoff layer can arm the opt-in RT1051 WDOG1 adapter for an
eight-second trial. Physical A-to-B and B-to-A tests on 2026-08-01 exercised
the RAM-resident journal backend, retained confirmation handoff, warm reset,
and durable commit on the Verb Core Deluxe.

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

Read the front panel without touching flash, and measure the stepped Type
ladder one detent at a time:

```sh
python3 tools/pedalctl.py knobs --watch
python3 tools/pedalctl.py calibrate-selector
```

Hand control to the immutable NXP ROM so a new bootloader can be installed:

```sh
python3 tools/pedalctl.py handoff-to-rom IMAGE \
  --expected-sha256 SHA256 --confirm ERASE-BOOT-REGION
```

This validates the follow-up image before mutating anything, then erases
exactly the first 64 KiB — the boot header the ROM checks — and stops. Erase
and bounded A/B programming are physically validated, but Open Recover still
refuses whole-chip programming. Everything above the header, including both
slots and the factory compatibility region, is untouched. See
[ROM_SDP_RECOVERY.md](../hardware/ROM_SDP_RECOVERY.md) for the cold start and
flash that follow.

`calibrate-selector` prompts through all eight positions, reports the
adjacent gaps against the worst resting noise it saw, warns when two detents
are indistinguishable, and prints a ready-to-paste firmware table. Both
commands refuse to run against firmware that does not advertise capability
bit `0x100`.

The `info` and `upload` commands are implemented against `hidraw`, including
exact retries, sequence checking, inactive-slot selection, finalization, and
pending activation. The host and bootloader must use the same protocol
version; the version-2 host intentionally refuses the installed version-1
recovery image.

After booting the embedded RAM recovery personality with the normal physical
recovery gesture, a complete image can be restored with:

```sh
python3 tools/pedalctl.py restore-full \
  --confirm WIPE-ALL-8MIB \
  --expected-sha256 <64-hex-digit-sha256> \
  artifacts/private/full-image.bin
```

When exactly one `9527:c157` recovery interface is present, the host finds
its current `/dev/hidrawN` node automatically. Supplying `--device` remains
available for multi-device setups. Auto-discovery does not weaken protocol
identification: `GET_INFO` must decode as version 2 and advertise RAM
full-flash, progressive-erase, and physically verified full-program
capabilities before the first mutating command is sent. An original NUX
updater cannot pass that gate.

The input must be exactly 8 MiB and contain plausible `FCFB` and IVT headers.
The full-restore command retains a conservative ten-minute per-command
timeout, but progressive erase keeps every individual request bounded to one
64 KiB range and prints aggregate progress every 512 KiB. In principle, the
single command begins the guarded transaction, progressively erases all NOR,
writes all 8 MiB with immediate device-side verification, asks RAM recovery
to hash the complete stored image, and reboots only after that digest
matches.

Current recovery firmware intentionally does not advertise
`RECOVERY_CAPABILITY_VERIFIED_FULL_PROGRAM`: physical testing found corrupt
writes with 32-, 8-, and 4-byte payloads. Consequently `restore-full` refuses
before `BEGIN_FULL_FLASH` or any erase. The capability may only be advertised
after a replacement NOR programming backend passes a complete physical
write/readback test. Until then, use `tools/ncr2_rom_recover.py` and the NXP
ROM flashloader for whole-chip recovery.

## Status values

In addition to packet-format errors, the engine reports explicit failures for
backend I/O, invalid transaction phase, invalid image, active-slot selection,
premature activation, unsupported flags, disabled full-flash recovery, and
out-of-order writes. Failed
commands do not consume a sequence number, so the host can correct or retry
the same step.
