# Factory-engine compatibility bridge

The transitional open image can retain the known-working Metal/overdrive
behavior without embedding factory firmware in this repository.
`ncr2_factory_bridge` is 100% original assembly linked as an ordinary open
application at `0x80000000`. It:

1. validates the preserved Metal engine vectors at XIP address `0x600c0000`;
2. copies the audited `0x1e000` bytes into ITCM address zero;
3. executes `DSB` and `ISB`; and
4. branches to the factory reset vector at `0x0000e4b5`.

The factory reset handler then installs VTOR and MSP itself, matching the
stock launcher's behavior. A vector mismatch enters a quiet `WFI` fault loop
before ITCM is modified.

The open hardware reset path runs the pinned RT1051 `SystemInit` before
loading this bridge, so ITCM use and the XIP instruction path no longer rely
solely on incidental ROM state. The bridge itself does not require D-cache.

## Stock-launcher compatibility preparation

The first hardware bridge reached the factory engine but produced only the
pedal's dry hardware bypass. A complete launcher trace then showed that the
stock launcher performed five operations before copying the selected engine:

1. install its seven-region MPU map and leave caches disabled;
2. configure eight board pin muxes and pad controls;
3. establish the 600 MHz ARM/AHB/IPG clock tree;
4. enable the DWT cycle counter; and
5. initialize the two boot-switch GPIOs as inputs.

`ncr2_factory_compat_bridge` reconstructs those operations in original C
source before the audited Metal copy-and-jump. Its pin list and pad values
match the verified dump exactly:

- GPIO1_IO21 and GPIO3_IO02: boot-switch inputs, `0x70b0`;
- GPIO2_IO07 and GPIO2_IO08: GPIO controls, `0xd0b0`;
- GPIO2_IO30, GPIO2_IO31, and GPIO2_IO11: GPIO controls, `0x70b0`; and
- GPIO1_IO26: GPIO control, `0xf0b0`.

The factory DCD already enables SYS PLL at `0x400d8030` before configuring
SEMC/SDRAM. The compatibility bridge therefore recreates the factory ARM PLL
and clock-root settings but intentionally does not restart SYS PLL while it
is executing from SDRAM.

The first compatibility diagnostic also exposed an independent handoff bug:
the bridge kept the factory XIP source in caller-saved register `r1` across
the trial-confirmation and compatibility-preparation calls. Those calls were
allowed to overwrite `r1`, so the subsequent ITCM loop copied from an
arbitrary address. The bridge now reloads `0x600c0000` immediately before
the copy. The post-link checker disassembles `Reset_Handler` and rejects a
bridge without that post-call reload.

`ncr2_factory_compat_diagnostic` adds a reversible, short pulse of the four
candidate indicator GPIOs before restoring their prior direction/output
state and launching Metal. It is intended only as an A/B Slot-B bring-up
image.

The bridge contains no NUX code, tables, strings, or binary fragments. A full
private test image is assembled locally from the user's verified dump, and
the guarded packer proves that the factory compatibility region is
byte-identical.

## Dependency audit and corrected metadata location

Ghidra has 10,000–14,000 decoded instructions and 3,000–4,000 reference
sources in each factory engine. The reusable
`ghidra/AuditFactoryEngineDependencies.java` audit found:

- no engine references into the replaced open-boot region;
- exactly seven references per engine into factory state at `0x6002d000`,
  `0x6002df00`, and `0x6002e000`; and
- no engine references into the proposed open metadata sector at
  `0x603f0000–0x603fffff`.

The factory functions do more than read those low sectors: they call their
own erase/program helpers. The original transitional layout would therefore
have allowed a factory engine to corrupt an open journal at
`0x20000–0x2ffff`. The corrected layout preserves
`0x20000–0x3effff` and places the open journal in the erased 64 KiB block at
`0x3f0000–0x3fffff`.

Run the dependency audit against the local Ghidra project with:

```sh
/path/to/analyzeHeadless \
  artifacts/device-firmware-analysis/ghproj ncr2 \
  -process dump1.bin -noanalysis \
  -scriptPath ghidra \
  -postScript AuditFactoryEngineDependencies.java
```

The script fails if any engine references the open boot region or proposed
metadata sector, or if the expected factory-state reference set changes.

## Build and inspect

```sh
cmake --build build/open --target ncr2_factory_bridge
python3 tools/check_factory_bridge.py \
  build/open/ncr2_factory_bridge.elf
```

With a pinned MCUX SDK workspace configured, build the hardware-compatibility
variants with:

```sh
cmake --build build/open-hardware-boot \
  --target ncr2_factory_compat_bridge \
           ncr2_factory_compat_diagnostic
python3 tools/check_factory_bridge.py \
  build/open-hardware-boot/ncr2_factory_compat_diagnostic.elf
python3 tools/open_image.py pack-slot \
  --application \
    build/open-hardware-boot/ncr2_factory_compat_diagnostic.bin \
  --version 0.1.2 \
  --build-number 3 \
  --output \
    build/open-hardware-boot/ncr2-factory-compat-diagnostic-0.1.2.slot
```

Use `ncr2_factory_bridge.bin` as the `--application` input to
`tools/open_image.py pack`. Generated full images remain private, ignored
artifacts.

## Safety status

This bridge closes the audible-behavior gap for a future first image; it does
not make the current bootloader hardware-approved. A useful first physical
image still requires an assigned open USB identity and successful read-only
recovery enumeration before any one-time programmer install. The generated
ID-guarded offline image must not be flashed.
