# NUX Core Deluxe HID-DFU protocol

This documents the updater protocol recovered from:

- the NCR-2 `RTX_DFU` image in `dump1.bin`;
- NUX Device Updater 2.1.11.30; and
- the official Metal Core Deluxe MKII V1.26 firmware container.

No guessed commands are required.

## USB transport

- DFU entry: hold the relevant footswitch while applying pedal power.
- USB identity: `9527:c157`, product `NUX CORE DELUXE DFU`.
- Interface: vendor HID, no report ID on the wire.
- Host-to-device endpoint: `0x02`, 64-byte interrupt reports.
- Device-to-host endpoint: `0x81`, 64-byte interrupt reports.

The non-destructive version query is one report containing byte `0x56`
followed by 63 zero bytes. The response stores the eight-byte version field
at bytes 3 through 10. The unmodified pedal reports `V1.2.4`.

## Firmware records

An official container begins with `NUX DFU` and has `TEXT`, `BINA`, and
`EXTR` sections. Only `BINA` is streamed to the pedal.

`BINA` is a sequence of 540-byte records. Each record is divided into nine
60-byte pieces and sent as:

```text
01 <sequence 00..08> 00 00 <60 bytes>
```

The first record has operation 1 in header bytes 2–3 (big-endian). Its
512-byte payload contains:

```text
payload + 0x00  bitwise-inverted "COREDLX" (7 bytes)
payload + 0x08  bitwise-inverted 8-byte version
payload + 0x14  big-endian zero
payload + 0x18  big-endian image length
payload + 0x1c  big-endian zero
```

Every remaining record has operation 4. Its 28-byte header is followed by
one 512-byte flash page. Header bytes 12–15 contain the big-endian page
index, except the last page uses `0xffffffff` to finalize the update.

## Device-side behavior

The decisive functions in the 64 KiB DFU image loaded at address zero are:

- `0x45d8`: HID OUT callback and nine-report reassembly;
- `0x464e`: setup/page record parser;
- `0x47b4`: erase/program orchestration;
- `0x5468`: 64 KiB block erase;
- `0x5564`: page programming; and
- `0x582c`: final version-sector update.

The page writer always computes:

```text
flash address = 0x60000 + running_offset
```

The `0x60000` base is an immediate constant in the bootloader. The record's
page-index field is only tested for first-page `0` and final-page
`0xffffffff`; it cannot select a different flash base.

Consequences:

- stock USB DFU cannot change the engine selector at flash `0x20000`;
- it can safely update or substitute the engine slots beginning at
  `0x60000`; and
- finalization follows vendor behavior by erasing the 4 KiB sector at
  `0x5e000` and writing the eight-byte version field at its beginning.

## ENG3 test strategy

The factory selector remains `1`, so the launcher still loads slot 1.
`eng3-slot1.bina` preserves the original slot 0 (`0x60000–0x7ffff`) and
copies the factory ENG3/Metal image from `0xc0000–0xdffff` into selected
slot 1 (`0x80000–0x9ffff`).

This is equivalent to selecting ENG3 without modifying protected boot or
selector sectors. It programs only `0x60000–0x9ffff`, plus the vendor's
normal version-sector finalization.

`restore-stock-slots.bina` restores the factory slot 0 and slot 1 bytes
from the verified dump and restores the version string `V1.2.4`.

Known hashes:

```text
dump1.bin
  4263ef41c0745f6e8c00be13b52391b6b04a5f51779b12d0e191abf6888e7a14

eng3-slot1.bina
  bb7f1713268e6fcc7b656c572af9674c28a74ce1f37a48708ba2ad1511dfb868

restore-stock-slots.bina
  92a30e9878d237f4e8c7f91e4c17e0204dd90fde0620853f6176184885a527bb
```

## Source factory-slot transition

The recovered launcher ABI also supports an original source image linked at
ITCM address zero. `ncr2_factory_slot_app` is constrained to the launcher's
`0x1e000` copy size and has its own vector, stack, VTOR, and reset path.

`make-factory-slot` constructs two offline artifacts:

```bash
./tools/nux_dfu.py make-factory-slot \
  dump1.bin \
  build/factory-slot/ncr2_factory_slot_app.bin \
  build/factory-slot/source-slot-OFFLINE-ONLY.bina \
  build/factory-slot/metal-restore.bina
```

The source package preserves stock slot 0 and installs the open binary into
selected slot 1. The paired restore copies factory Metal slot 3 into selected
slot 1. With the verified dump and default `ENG3TEST` version, that restore is
byte-identical to the live-validated `eng3-slot1.bina`.

The current source application has no audio initialization or target-visible
diagnostic and is not approved for streaming. This command establishes
reproducible packaging and rollback; it does not turn an incomplete payload
into safe firmware.

## Host utility

Inspect and expand a stream without touching USB:

```bash
./tools/nux_dfu.py inspect eng3-slot1.bina
./tools/nux_dfu.py dry-run eng3-slot1.bina
```

Run the safe live version query:

```bash
./tools/nux_dfu.py query
```

The flash command requires the exact BINA SHA-256, an explicit execution
flag, and (optionally) the expected current device version:

```bash
./tools/nux_dfu.py stream eng3-slot1.bina \
  --execute \
  --confirm-sha256 bb7f1713268e6fcc7b656c572af9674c28a74ce1f37a48708ba2ad1511dfb868 \
  --expected-device-version V1.2.4
```

The restoration command uses the same safeguards:

```bash
./tools/nux_dfu.py stream restore-stock-slots.bina \
  --execute \
  --confirm-sha256 92a30e9878d237f4e8c7f91e4c17e0204dd90fde0620853f6176184885a527bb \
  --expected-device-version ENG3TEST
```

Do not remove pedal power or disconnect USB during a transfer. If a transfer
is interrupted, remain in DFU mode and rerun either the test or restore
stream. The verified full `dump1.bin` remains the programmer-level recovery
image.

## Live validation

On 2026-07-23, `eng3-slot1.bina` was streamed to the NCR-2 in DFU mode:

```text
pre-transfer version:  V1.2.4
reports accepted:      4,617 / 4,617
post-transfer version: ENG3TEST
```

The post-transfer `0x56` query proves that the final sentinel record ran and
the bootloader committed the custom eight-byte version marker. Normal-mode
ENG3/audio validation requires a physical power cycle without holding the
DFU footswitch.

The subsequent normal power-on test succeeded. The former NCR-2 reverb pedal
booted the copied ENG3 image and operated as an overdrive/amp pedal. This
confirms that engine slot substitution works without changing the protected
selector byte at flash `0x20000`.
