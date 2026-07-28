# Known defect: FlexSPI IP page program corrupts alternating FIFO fills

Status: the second candidate and conservative small host transactions failed
physical validation. Open full-chip restore is fail-closed before erase.
Discovered 2026-07-24 on physical hardware, once the FlexSPI port-size fix
made slot writes reachable for the first time.

## Symptom

`ncr2_nor_program` reports `NCR2_NOR_VERIFY_ERROR` (status 7) in the
verify-data phase, with `backend_status` 0, meaning the backend page program
claimed success and the flash reported itself idle.

## Evidence

Writing 32 bytes of `0x00` to a freshly erased slot B, then reading the same
range twice:

```
blank : ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
read1 : 0000000000000000110000000000000000000000000000001100000000000000
read2 : 0000000000000000110000000000000000000000000000001100000000000000
```

Bytes 8 and 24 read `0x11` instead of `0x00`. Reads are stable across
repeats, so the flash genuinely holds those values.

An earlier attempt with real manifest data failed at exactly the same two
byte offsets:

```
intended: 4f50454e01000010 480d000000000080 000000004e435232 0800040010000000
readback: 4f50454e01000010 590d000000000080 000000004e435232 1800040010000000
```

`0x59 = 0x48 | 0x11` and `0x18 = 0x08 | 0x10`. In every case the readback
bits are a superset of the intended bits, which is consistent with bits that
were never driven to zero rather than with a faulty read.

## Analysis

Offsets 8 and 24 are the first byte of the second and fourth eight-byte
groups. `ram_transfer` fills the IP TX FIFO one watermark unit at a time:

- wait for `IPTXWE`;
- write `TFDR[0]` and `TFDR[1]`, that is eight bytes;
- write `IPTXWE` back to `INTR` to advance.

The loop then immediately re-reads `INTR`. If the flag has not yet been
re-evaluated by the controller, the next iteration writes `TFDR[0]` again
before the previous entry has been consumed, corrupting the first word of
alternating fills. The observed pattern, first byte of every other group,
matches that race.

## Impact

The open recovery protocol cannot correctly write NOR. Slot upload and
full-chip restore both complete their erase phase and then fail verification.
Every image installed on this pedal so far was written by NXP's RAM
flashloader, which does its own programming and has passed a full 8 MiB
SHA-256 readback every time, so no installed image is affected.

### Erase is unaffected

The defect is confined to the page-program path. Erase has completed
correctly on every physical attempt, including repeated full-chip erases and
the bounded 64 KiB erase used by `pedalctl handoff-to-rom`, which was
exercised on hardware on 2026-07-27. Erase leaves cells at `0xff`, so there is
no FIFO payload to corrupt.

That asymmetry is load-bearing rather than incidental: it is what lets the
open recovery personality reliably blank the boot header and hand control to
the immutable NXP ROM, which is currently the only verified way to install a
new bootloader. Treat erase through this backend as trusted and program
through it as unusable until the defect is fixed and physically revalidated.

## First attempted fix

1. Constrain each IP write transfer to a single watermark unit so the fill
   loop never runs more than once, at the cost of one IP command per eight
   bytes.
2. Poll `IPTXFSTS` fill level rather than relying on the `IPTXWE` flag to
   decide when the previous entry has been consumed.
3. Raise the TX watermark so a whole 32-byte host chunk fits one fill.

Option 1 was tested physically on 2026-07-25 and was insufficient. The first
real-manifest attempt left byte 24 at `0x15` instead of `0x05`. Reprogramming
the same data did not clear that bit. A new 32-byte all-zero block at the next
address reproduced the original signature:

```
0000000000000000110000000000000000000000000000001000000000000000
```

That rules out a worn flash cell and shows that the controller state persists
across the separate eight-byte commands.

## Failed second candidate

Option 3 is now implemented. The driver sets `IPTXFCR[TXWMRK]` to three, so
one watermark represents 32 bytes, clears any pending TX/RX watermark flags,
and limits each page-program command to that same 32-byte size.
`ram_transfer` therefore loads `TFDR[0..7]` once and never reuses a FIFO word
within the command.

Physical full-restore testing on 2026-07-26 disproved it. The very first
32-byte FCFB block read back as:

```
intended: 4643464200040156000000000003030001010000010400000002000000000000
readback: 4643464200040156110000000003030001010000010400001002000000000000
```

Bytes 8 and 24 retain the same stray bits as before.

## Failed conservative host mode

Four ordered eight-byte host `WRITE_CHUNK` transactions over that same
partially programmed range initially passed immediate device-side
verification. A continued eight-byte stream then failed at offset `0x218`:
an intended zero byte read back as `0x11`. Retrying the same eight-byte write
could not clear it. A four-byte retry also failed with the same retained bit.

Small host payloads therefore do not make this backend safe. The host now
requires `RECOVERY_CAPABILITY_VERIFIED_FULL_PROGRAM` before it begins a
full-chip restore. Current recovery firmware deliberately does not advertise
that capability, so `pedalctl restore-full` refuses before beginning a
session or erasing any flash. The NXP ROM flashloader remains the physically
verified full-image recovery path. Normal A/B upload is also unapproved until
the backend itself is corrected and validated on hardware.

## Reproducing

Requires an inactive slot, so it does not disturb a working image:

```python
c.begin(pedalctl.SLOT_B, 8192)
c.erase()
c._session_command("write-chunk", offset=0, payload=b"\x00" * 32)
c.read(0, 32)
```
