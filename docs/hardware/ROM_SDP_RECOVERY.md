# RT1051 ROM recovery over USB

This is the last-resort recovery path for an NCR-2 whose external W25Q64 boot
image is blank or corrupt. It was exercised on physical hardware on
2026-07-24 after a CH341A write lost contact.

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

For a persistent setup, add both identities to a udev rule:

```udev
SUBSYSTEM=="usb", ATTR{idVendor}=="1fc9", ATTR{idProduct}=="0130", MODE="0666"
SUBSYSTEM=="usb", ATTR{idVendor}=="15a2", ATTR{idProduct}=="0073", MODE="0666"
```

Reload the rules before reconnecting:

```bash
sudo udevadm control --reload-rules
sudo udevadm trigger
```

## Recovery commands

With the pedal visible as `1fc9:0130`, perform the read-only status query:

```bash
python3 tools/ncr2_rom_recover.py status
```

Load the flashloader into RAM:

```bash
python3 tools/ncr2_rom_recover.py load
```

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

## Image exercised on hardware

The open image restored in the 2026-07-24 session was:

```text
artifacts/open-hardware-v0.2.0/ncr2-open-hw-usb-0.2.0-full.bin
SHA-256 536c67827605a54ce9d5da3be075c91e5e0f3783cab9a7f0d74975cc1a465e0b
size    8388608 bytes
```

Generated flash images remain excluded from Git. Preserve each image
separately with its SHA-256.
