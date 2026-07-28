# RT1051 ROM recovery over USB

This began as the last-resort recovery path for an NCR-2 whose external
W25Q64 boot image is blank or corrupt, exercised on physical hardware on
2026-07-24 after a CH341A write lost contact.

It is now also the **routine** path for installing any new bootloader. The
open recovery personality cannot program NOR safely — see
[NOR_PROGRAM_FIFO_DEFECT.md](NOR_PROGRAM_FIFO_DEFECT.md) — so `pedalctl
restore-full` fails closed and every bootloader change has to go through the
NXP flashloader. Expect to use this document deliberately, not only after an
accident.

The method does not depend on the factory updater or the OpenSensor recovery
bootloader. It uses the immutable i.MX RT1051 mask ROM to start a second-stage
flashloader in on-chip RAM, then restores the complete 8 MiB NOR over USB.

## Proven device sequence

1. A missing or invalid external image makes the RT1051 enumerate as:

   ```text
   1fc9:0130 NXP Semiconductors SE Blank RT Family
   ```

2. `sdphost` confirms Serial Download Protocol and reports HAB disabled.
3. `ivt_flashloader.bin` is written to and started from `0x20208200`. This
   changes RAM only.
4. The RAM flashloader re-enumerates as:

   ```text
   15a2:0073 Freescale Semiconductor, Inc. USB COMPOSITE DEVICE
   ```

5. `blhost` applies FlexSPI option `0xC0000007` at RAM address `0x20202000`
   using external memory ID 9.
6. The flashloader reports the attached NOR as:

   ```text
   Start Address = 0x60000000
   Total Size = 8 MB
   Page Size = 256 bytes
   Sector Size = 4 KB
   Block Size = 64 KB
   ```

7. `0x60000000..0x607fffff` is erased and a complete 8 MiB image is written.

The mask ROM cannot program external FlexSPI NOR directly. The RAM
flashloader is required; `uuu` alone is not the complete solution.

## Preserved tools

The exact assets used are under `tools/vendor/nxp-mcubootutility/`. Their
upstream commit, hashes, and license are recorded in
`tools/vendor/nxp-mcubootutility/PROVENANCE.md`.

The guarded wrapper is:

```bash
python3 tools/ncr2_rom_recover.py --help
```

It validates its vendor assets before contacting USB. A flash image must:

- be exactly 8 MiB;
- begin with `FCFB`;
- contain an i.MX RT IVT at offset `0x1000`;
- optionally match an operator-supplied SHA-256; and
- be accompanied by `--execute` before NOR can change.

## Permissions

USB nodes are recreated at each enumeration. For one-off recovery, grant
access after each stage using the actual node reported by the wrapper:

```bash
sudo chmod a+rw /dev/bus/usb/BBB/DDD
```

Do not power-cycle between the `load` and `flash` stages.

For a persistent setup, install the repository's narrowly scoped rule. It
covers both the raw USB node and the `hidraw` node created by NXP's host
tools, as well as the OpenSensor recovery personality:

```bash
sudo install -m 0644 \
  tools/udev/99-nux-effects-recovery.rules \
  /etc/udev/rules.d/99-nux-effects-recovery.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Reconnect once after installing it. Future transitions between
`9527:c157`, `1fc9:0130`, and `15a2:0073` will receive the permissions
automatically.

## Reaching the ROM deliberately

A pedal with a valid boot image boots it and never offers `1fc9:0130`. The
ROM only falls back to its serial downloader when the header at the start of
flash is blank, so entering it on purpose means erasing that header first.

From physical recovery (`9527:c157`), with the follow-up image already built:

```bash
python3 tools/pedalctl.py handoff-to-rom \
  /path/to/full-8MiB-image.bin \
  --expected-sha256 SHA256 \
  --confirm ERASE-BOOT-REGION
```

It validates the follow-up image before touching anything, so a wrong path or
hash cannot strand the pedal with nothing to install, then erases exactly one
64 KiB chunk and stops. The factory compatibility region, both application
slots, and the boot journal are left intact. Erase is the one part of the open
NOR backend that is trustworthy, which is what makes this safe.

Then power-cycle with **no footswitch held**. The pedal enumerates as
`1fc9:0130 SE Blank RT Family` — the "Blank" is the ROM confirming the header
is gone, and is expected here rather than alarming.

Between that erase and a completed ROM flash the pedal has no bootable image.
That window is normal and recoverable; the ROM is mask-programmed and cannot
itself be damaged by anything above.

## Recovery commands

With the pedal visible as `1fc9:0130`, perform the read-only status query:

```bash
python3 tools/ncr2_rom_recover.py status
```

Load the flashloader into RAM:

```bash
python3 tools/ncr2_rom_recover.py load
```

`load` accepts `--wait-seconds` for the `15a2:0073` re-enumeration if the
default is too short on a slow hub.

`flash` operates on the RAM flashloader, not on the ROM, so `load` must have
succeeded in the same power cycle. Running `flash` while only `1fc9:0130` is
present prints the intended plan, reports `RT1052 RAM flashloader 15a2:0073
is not connected`, exits nonzero, and mutates nothing. Note that piping the
command through `tail` or similar discards that exit status — check the
message, not just the shell result.

If necessary, grant permission to the new `15a2:0073` USB node. Dry-run the
image first:

```bash
python3 tools/ncr2_rom_recover.py flash \
  --image /path/to/full-8MiB-image.bin \
  --expected-sha256 SHA256
```

Execute with the default complete readback verification:

```bash
python3 tools/ncr2_rom_recover.py flash \
  --image /path/to/full-8MiB-image.bin \
  --expected-sha256 SHA256 \
  --execute
```

To deliberately omit readback:

```bash
python3 tools/ncr2_rom_recover.py flash \
  --image /path/to/full-8MiB-image.bin \
  --expected-sha256 SHA256 \
  --execute \
  --no-verify
```

During erase the flashloader can remain synchronously busy and stop answering
USB for several minutes. Do not start a second `blhost`, interrupt the first
process, remove USB, or remove power. After a successful write, remove power
and boot normally.

## Images exercised on hardware

The open image restored in the 2026-07-24 session was:

```text
artifacts/open-hardware-v0.2.0/ncr2-open-hw-usb-0.2.0-full.bin
SHA-256 536c67827605a54ce9d5da3be075c91e5e0f3783cab9a7f0d74975cc1a465e0b
size    8388608 bytes
```

On 2026-07-27 the complete deliberate cycle was exercised end to end —
`handoff-to-rom` from physical recovery, cold start into `1fc9:0130`, `load`,
then `flash --execute --verify`:

```text
artifacts/open-hardware-v0.12.0/ncr2-open-psycho-rage-0.12.0-full.bin
SHA-256 9cefcf84af9a8c8b23c35a11a9bec7f8993bd5723eea81a0c31e4cc56cd765a8
size    8388608 bytes
```

The full 8 MiB readback matched. That image is the first to carry
`RECOVERY_COMMAND_READ_KNOBS`; its application slot is byte-identical to
v0.11.1, so the bootloader was the only functional change.

Generated flash images remain excluded from Git. Preserve each image
separately with its SHA-256.
