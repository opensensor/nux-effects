# Open programmable firmware plan for the NUX Core Deluxe platform

Status: architecture plan based on the verified NCR-2 dump and live hardware
tests on 2026-07-23.

The immediate pedal configuration is the known-good static ENG3 image:

- USB DFU image: `eng3-slot1.bina`
- BINA SHA-256:
  `bb7f1713268e6fcc7b656c572af9674c28a74ce1f37a48708ba2ad1511dfb868`
- bootloader version marker after transfer: `ENG3TEST`
- normal-boot result previously verified: working amp/overdrive engine

The zero-wear binary shim experiment is paused. It proved that USB deployment
and factory-engine substitution work, but it did not reliably identify the
runtime footswitch input. The long-term design should be source firmware with
an explicit board-support package, not more hooks into proprietary binaries.

## 1. Target outcome

The finished pedal should:

1. Cold-boot reliably into a low-latency audio application.
2. Provide Delay, Reverb, Modulation, and Drive/Amp modes from source.
3. Change modes live, from RAM, without rebooting or erasing NOR.
4. Use a five-second footswitch hold to advance through the four modes.
5. Expose all controls through a descriptor-driven parameter system so new
   effects can be added without rewriting the control layer.
6. Accept recoverable firmware updates over USB.
7. Keep a known-good application slot and roll back after a failed update.
8. Preserve the original dump and, during bring-up, retain a factory-engine
   compatibility path.
9. Never require a socket programmer after the one-time open-bootloader
   installation, except as the final recovery mechanism.

## 2. Confirmed target facts

These are evidence-backed requirements, not design assumptions:

| Area | Confirmed fact |
|---|---|
| MCU | NXP `MIMXRT1051DVL6B`, Cortex-M7 |
| Boot flash | 8 MiB W25Q64-class FlexSPI NOR, XIP base `0x60000000` |
| External RAM | ESMT `M12L2561616A`, 32 MiB, 16-bit SDRAM |
| SDRAM mapping | SEMC at `0x80000000`; working timing exists in the stock DCD |
| Audio peripherals | SAI2/SAI3 and eDMA are present in the stock firmware |
| Controls | ADC1/ADC2, ADC_ETC, PIT/XBARA, and GPIO are used |
| USB normal mode | `9527:c177`, USB MIDI |
| USB recovery mode | `9527:c157`, vendor HID, 64-byte IN/OUT reports |
| Recovery entry | Hold the relevant footswitch while applying pedal power |
| Factory engines | Delay `0x60000`, Reverb `0x80000`, Mod `0xa0000`, Metal `0xc0000` |
| Factory engine size | `0x20000` bytes per slot; launcher copies `0x1e000` to ITCM |
| Stock update limit | Existing HID DFU writes from flash offset `0x60000` upward |
| Full recovery | Verified 8 MiB dump SHA-256 begins `4263ef41...` |

NXP's current SDK provides RT1050 examples for
[SAI/eDMA ping-pong audio](https://mcuxpresso.nxp.com/mcuxsdk/latest/html/examples/driver_examples/sai/edma_ping_pong_buffer_half_interrupt/readme.html),
[SEMC SDRAM initialization](https://mcuxpresso.nxp.com/mcuxsdk/latest/html/examples/driver_examples/semc/sdram/readme.html),
and
[generic USB HID](https://mcuxpresso.nxp.com/mcuxsdk/latest/html/examples/usb_examples/usb_device_hid_generic/readme.html).
The RT1050 Boot ROM also has a USB/UART serial downloader capable of loading a
flashloader into internal RAM, although using it on this board depends on the
board's boot-mode access and fuse state. See NXP's
[RT1050 manufacturing guide](https://mcuxpresso.nxp.com/mcuxsdk/latest/html/middleware/mcu_bootloader/docs/iMXRT1050_Manufacturing_User_Guide/topics/overview.html).

## 3. Architecture decision

Use two source projects with a small, stable interface:

```text
ROM -> open recovery bootloader -> selected application slot
                                  |
                                  +-> unified audio application
                                      +-- platform/BSP
                                      +-- audio engine
                                      +-- control engine
                                      +-- four effect graphs
                                      +-- USB MIDI/editor
```

The bootloader owns validation, recovery USB, updates, and rollback. The
application owns all real-time audio and user interaction. The application
does not erase firmware sectors during normal operation.

The four modes are not separate boot images. They are four DSP graphs in one
application. Changing mode is a parameter/state transition:

```text
footswitch hold -> choose next graph -> crossfade -> update LEDs
```

There is no reset and no flash operation in this path.

## 4. Transitional 8 MiB flash map

The first open release should preserve the factory content while using the
currently empty upper half of the NOR:

| Flash offset | Size | Purpose |
|---:|---:|---|
| `0x000000–0x01ffff` | 128 KiB | Open FCFB/DCD/IVT and recovery bootloader |
| `0x020000–0x02ffff` | 64 KiB | Redundant boot metadata and update journal |
| `0x030000–0x3fffff` | 3.8125 MiB | Preserved factory compatibility/archive region |
| `0x400000–0x5fffff` | 2 MiB | Open application slot A |
| `0x600000–0x7fffff` | 2 MiB | Open application slot B |

The one-time programmer image should start from the verified factory dump,
restore all four original engine slots, then overlay:

- the open bootloader;
- initial boot metadata; and
- the first open application in slot A.

This preserves the original coefficient, identity, and preset regions during
bring-up. A bootloader menu can retain a factory-compatibility loader as a
diagnostic fallback. Once the open firmware is mature, a later layout may
reclaim the factory region, but that is not necessary for the first usable
release.

### Application-slot format

Each 2 MiB slot begins with one erased-sector-aligned manifest:

```c
struct pedal_image_manifest {
    uint32_t magic;
    uint16_t format_version;
    uint16_t header_size;
    uint32_t image_size;
    uint32_t vector_offset;
    uint32_t entry_point;
    uint32_t board_id;
    uint32_t semantic_version;
    uint32_t build_number;
    uint8_t  image_sha256[32];
    uint8_t  reserved[...];
};
```

The manifest should fit in the first 4 KiB. The image follows at
`slot_base + 0x1000`. A CRC can detect accidental header corruption quickly;
SHA-256 validates the complete image. Signed releases can be added later
without changing the base format by versioning the manifest.

## 5. Open recovery bootloader

### Responsibilities

The bootloader should:

1. Use an open FCFB and the known-good SEMC DCD settings.
2. Sample the confirmed recovery input early, before application startup.
3. Validate both application manifests and hashes.
4. Select confirmed slot, pending slot, rollback slot, or recovery mode.
5. Start a watchdog-backed trial boot for a newly written image.
6. Mark an image confirmed only after the application reports a healthy boot.
7. Enter USB recovery after repeated failed boots.
8. Refuse every erase/write request outside application slots and boot
   metadata.
9. Keep all FlexSPI erase/program code and required constants in ITCM while
   NOR is busy.
10. Provide an explicit software-reset request from the application into
    recovery mode.

The bootloader should not update itself in version 1. Bootloader replacement
remains a programmer-only operation until the application and recovery path
have extensive power-loss testing.

### Boot metadata

Use two independent 4 KiB sectors with append-only records:

```c
struct boot_record {
    uint32_t magic;
    uint32_t sequence;
    uint8_t  confirmed_slot;
    uint8_t  pending_slot;
    uint8_t  trial_count;
    uint8_t  flags;
    uint32_t record_crc32;
};
```

The newest valid sequence wins. Never modify a record in place. When a sector
fills, compact into the other sector, verify it, and only then erase the old
one. This keeps an interrupted metadata write recoverable.

### USB update protocol

Retain 64-byte vendor-HID reports for simple cross-platform access, but define
an open versioned protocol rather than extending unknown vendor behavior:

- `GET_INFO`
- `BEGIN_IMAGE(slot, size, hash)`
- `ERASE_SLOT`
- `WRITE_CHUNK(offset, sequence, data, crc)`
- `READ_CHUNK` for verification/recovery
- `FINALIZE_IMAGE`
- `SET_PENDING`
- `REBOOT`
- `GET_LOG`

Every mutating command must include:

- protocol magic and version;
- session nonce;
- monotonically increasing sequence;
- explicit slot-relative offset;
- payload CRC;
- a range check against the selected application slot.

The host must be able to resume an interrupted transfer and re-read the
written image before making it pending. The bootloader must never infer a
write address from unchecked host data.

For a distributable open project, do not ship with NUX's USB VID/PID. Use an
appropriately assigned community/development VID/PID. The personal prototype
can use a temporary development identity while enumeration is being tested.

### Ultimate recovery

Recovery order:

1. Physical footswitch -> open HID recovery.
2. Automatic rollback to the other valid application.
3. NXP ROM serial downloader, if boot-mode access is confirmed.
4. In-circuit NOR programmer with the RT1051 held in reset.

The verified full dump remains immutable and offline.

## 6. Unified pedal application

### Execution model

Start bare-metal. The audio path is small enough that an RTOS is not required,
and avoiding one simplifies latency and fault analysis.

- eDMA moves SAI receive/transmit data in ping-pong buffers.
- DMA ISR acknowledges the transfer and publishes one ready block.
- A highest-priority audio worker processes exactly one block.
- USB, MIDI, controls, LEDs, preset work, and logging run cooperatively at
  lower priority.
- No allocation, formatting, flash access, locks, or USB work occurs in the
  audio path.
- DWT cycle counters record worst-case DSP time and deadline margin.

FreeRTOS can be reconsidered after audio and recovery are stable, but it
should not be a milestone dependency.

### Memory placement

| Memory | Intended use |
|---|---|
| ITCM | reset/startup, IRQs, DMA callbacks, hot DSP kernels |
| DTCM/OCRAM | stacks, audio ping-pong buffers, control state |
| External SDRAM | delay lines, reverb networks, large tables, captures |
| FlexSPI NOR | bootloader, A/B applications, immutable factory fallback |

The linker must produce a map file and fail the build on ITCM/DTCM overflow.
External SDRAM receives a destructive startup test in diagnostics mode, not on
every normal boot.

### Board-support package

All reverse-engineered board details belong in one explicit BSP:

```text
platform/ncr2/
  board_clock.c
  board_pins.c
  board_sdram.c
  board_audio.c
  board_controls.c
  board_leds.c
  board_bypass.c
  board_usb.c
  board_manifest.h
```

No effect implementation may reference raw GPIO, SAI, ADC, or CCM registers.
This is important because the failed shim showed that an inferred GPIO
assignment is not enough; each physical control must be verified and recorded.

### Audio HAL

The audio HAL exposes a board-independent contract:

```c
typedef struct {
    float left[AUDIO_BLOCK_FRAMES];
    float right[AUDIO_BLOCK_FRAMES];
} audio_block_t;

void audio_init(const audio_config_t *config);
bool audio_take_input(audio_block_t *block);
void audio_submit_output(const audio_block_t *block);
```

The converter wire format stays inside the HAL. DSP uses normalized float32
initially. Later optimization can use ARM CMSIS-DSP or fixed-point only where
profiling justifies it.

The actual sample rate, bit depth, SAI master/slave roles, MCLK/BCLK/LRCLK
ratios, slot width, and DMA block length must be measured or recovered before
this interface is frozen.

## 7. Four-mode DSP design

One top-level engine owns four effect graphs:

```c
enum effect_mode {
    EFFECT_DELAY,
    EFFECT_REVERB,
    EFFECT_MODULATION,
    EFFECT_DRIVE,
};
```

All graph storage is preallocated. A mode switch:

1. stops feeding new input into the old graph;
2. initializes or restores the target graph state;
3. crossfades old and new outputs for 10–30 ms;
4. retires the old graph after the fade;
5. updates control mappings and LEDs.

No firmware sector is touched.

Initial source algorithms:

- Delay: stereo delay, filtering, feedback, optional tap modulation.
- Reverb: algorithmic FDN/Schroeder-style network using external SDRAM.
- Modulation: chorus/flanger/phaser family with shared LFO infrastructure.
- Drive: oversampled waveshaper, tone stack, level control, optional
  cabinet-style EQ.

These should be original implementations or use clearly compatible
open-source components. Reverse engineering can establish hardware
interfaces and behavior, but distributable source should not copy NUX's
proprietary DSP code or coefficient tables.

### Parameter descriptors

Each mode publishes data rather than hard-coded knob logic:

```c
struct parameter_descriptor {
    const char *name;
    float minimum;
    float maximum;
    float default_value;
    enum parameter_curve curve;
    enum smoothing_policy smoothing;
};
```

The control layer maps physical knobs and MIDI CC to these descriptors. This
lets the same UI drive different modes and makes a desktop editor possible.

### Footswitch behavior

Proposed version-1 interaction:

- normal press: effect/bypass behavior, after the actual switch topology is
  mapped;
- hold for five seconds: advance
  `Delay -> Reverb -> Modulation -> Drive -> Delay`;
- release after the hold: commit the live mode change;
- visible LED pattern: announce the destination before changing.

The selected mode lives in RAM. Optional persistence occurs only on an
explicit "save" action, never on every mode change.

## 8. Presets without excessive flash wear

Ordinary control changes and mode changes are volatile.

Persistent presets use a small log-structured store in a dedicated region:

- append immutable records with generation and CRC;
- never rewrite a record in place;
- keep two erase blocks so compaction is transactional;
- erase only when the log is full;
- save only after an explicit gesture or editor command;
- rate-limit saves and expose remaining/used records in diagnostics.

Preset storage should remain separate from A/B application slots. The
transitional map can reserve part of the metadata/factory gap after the
factory compatibility path is proven. Until then, presets can be volatile or
host-managed.

## 9. Unknowns that must be closed on hardware

These are blocking facts for a safe source rewrite:

1. Exact ADC/DAC or codec part numbers and their strap pins.
2. Full audio clock tree and whether the RT1051 or converter is clock master.
3. SAI2/SAI3 pin mux, direction, frame format, sample rate, and word width.
4. DMA request/channel mapping and stock block length.
5. Physical mapping and polarity of every footswitch, knob, expression input,
   LED, relay, mute, and bypass control.
6. Whether bypass is analog relay, electronic switch, codec mute, or a
   combination.
7. Safe startup/shutdown ordering needed to prevent speaker pops.
8. Accessible SWD, UART, boot-mode, and reset test pads.
9. Fuse/security state relevant to ROM serial downloader and debug.
10. SDRAM timing conversion from the working DCD into reviewed source.

The fastest reliable method is a combination of:

- high-resolution PCB photos and continuity tracing;
- stock-firmware register/pin-mux extraction in Ghidra;
- logic-analyzer capture of MCLK/BCLK/LRCLK/data;
- oscilloscope observation of mute/bypass at boot;
- SWD observation if the pads and fuse state permit it.

No guessed GPIO should reach the output-switching or power-control path.

## 10. Implementation phases and acceptance gates

### Phase 0 — preserve and characterize

Work:

- verify all preservation hashes and keep an offline copy;
- identify converter and analog switching parts;
- produce a pin/peripheral spreadsheet with evidence for each signal;
- capture audio clocks and control polarities;
- locate SWD/reset/boot pads.

Gate:

- every safety-critical and audio signal has a confirmed MCU pad and polarity;
- logic-analyzer traces document the stock audio bus.

### Phase 1 — reproducible toolchain and image packer

Work:

- pin an NXP MCUXpresso SDK revision;
- build with `arm-none-eabi-gcc` and CMake/Ninja;
- produce FCFB, DCD, IVT, linker map, ELF, raw image, and full-chip overlay;
- create a host validator that rejects overlaps and bad hashes.

Gate:

- two clean builds are byte-identical;
- the packer proves that only intended full-chip ranges differ from
  `dump1.bin`.

### Phase 2 — open bootloader on a test image

Work:

- boot, LED heartbeat, watchdog, USB HID recovery;
- slot manifests and SHA-256;
- A/B pending/confirm/rollback;
- flash programming from ITCM;
- power-loss injection during every update stage.

Gate:

- 100 consecutive cold boots;
- unplugging power at arbitrary update points always leaves recovery or one
  bootable slot;
- bootloader refuses writes to its own range and factory range.

### Phase 3 — SDRAM and board diagnostics

Work:

- source-level SEMC initialization matching the stock DCD;
- walking-bit, address-line, burst, and soak tests over 32 MiB;
- control/LED/footswitch/relay diagnostic mode.

Gate:

- repeated full SDRAM tests at temperature with zero errors;
- physical control mapping confirmed in a human-readable diagnostic report.

### Phase 4 — clean audio passthrough

Work:

- SAI2/SAI3 clocks and eDMA;
- format conversion;
- mute-safe startup and shutdown;
- bypass/passthrough only;
- cycle, overrun, and underrun telemetry.

Gate:

- stable stereo passthrough for at least one hour;
- zero DMA underruns;
- measured latency, level, noise, and channel polarity documented;
- no startup/shutdown pop at normal amplifier gain.

### Phase 5 — controls and live four-mode shell

Work:

- filtered ADC scanning;
- debounced footswitch state machines;
- five-second hold mode cycling;
- LED mode indication;
- four pass-through graph placeholders with crossfade.

Gate:

- all four modes cycle repeatedly without reset;
- no flash changes occur during a 10,000-cycle automated switch test;
- no click larger than the agreed threshold at graph transitions.

### Phase 6 — DSP effects

Implement and test in this order:

1. Delay
2. Modulation
3. Drive
4. Reverb

Reverb is last because it exercises the largest state and most complicated
stability/tail behavior.

Gate for each effect:

- host-side impulse/golden tests;
- no NaN/Inf output under randomized controls;
- bounded feedback and output;
- worst-case DSP time below 70% of one audio-block deadline;
- audible hardware test with output limiter enabled.

### Phase 7 — USB MIDI, editor, and presets

Work:

- MIDI CC/program handling;
- versioned parameter schema;
- host CLI/editor;
- explicit preset saves and log-structured persistence;
- diagnostic capture and firmware update UI.

Gate:

- malformed USB/MIDI packets cannot block audio;
- preset power-loss tests retain the last valid generation;
- host and firmware reject incompatible schemas cleanly.

### Phase 8 — release hardening

Work:

- long-duration soak and watchdog fault injection;
- A/B update from Linux, macOS, and Windows;
- license/SBOM review;
- reproducible release bundle and recovery instructions;
- measured CPU/RAM/latency/noise report.

Gate:

- a new user can restore, update, roll back, and recover using only the
  documented tools.

## 11. Proposed source tree

```text
firmware/
  CMakeLists.txt
  cmake/
    arm-none-eabi-toolchain.cmake
  bootloader/
    src/
    linker/
    tests/
  app/
    src/
    linker/
  platform/
    common/
    ncr2/
  audio/
    engine/
    format/
    tests/
  controls/
  dsp/
    delay/
    reverb/
    modulation/
    drive/
    common/
  protocols/
    boot_hid/
    midi/
  storage/
host/
  pedalctl/
  editor/
tools/
  pack_image.py
  make_factory_overlay.py
  verify_layout.py
docs/
  hardware/
  protocol/
  bringup/
tests/
  host/
  hardware/
third_party/
```

Use C17 for platform/boot code. C++ may be introduced for DSP only if it adds
clear value and all allocation/exceptions/RTTI behavior is controlled. Keep
the C ABI between bootloader, platform, and application.

## 12. Test strategy

### Host tests

- image/manifest parser fuzzing;
- boot-selection state-machine exhaustive tests;
- power-loss model tests for metadata and presets;
- DSP impulse, sweep, randomized-parameter, NaN, and clipping tests;
- parameter-schema compatibility tests;
- deterministic-build hash check.

### Target tests

- FlexRAM layout and stack-watermark checks;
- SDRAM memory tests;
- SAI loopback and DMA stress;
- GPIO/ADC diagnostic report;
- watchdog and deliberate hard-fault recovery;
- flash range-protection tests;
- USB malformed-packet tests.

### Hardware-in-loop tests

- automated relay/footswitch actuation where practical;
- analog loopback into an audio interface;
- latency, gain, frequency response, THD+N, and noise measurement;
- repeated power cuts during update and preset commit;
- long-duration four-mode switching with flash contents hashed before/after.

## 13. Immediate next work package

Do not replace the bootloader yet. The next implementation session should:

1. Create the source repository skeleton and pin the NXP SDK revision.
2. Generate a machine-readable flash map and full-image overlap validator.
3. Extract the stock FCFB/DCD into reviewed source structures.
4. Build a bootable "recovery heartbeat" image for offline inspection.
5. Map the PCB's debug and audio pins before that image is programmed.
6. Prepare a one-time full-chip overlay that:
   - starts from the verified factory dump;
   - restores the original four factory slots;
   - installs the open bootloader;
   - installs application A;
   - leaves application B erased;
   - emits a complete range diff and SHA-256 manifest.

The first physical open-firmware test should prove only boot, watchdog,
recovery USB, and rollback. Audio comes after recovery is independently
reliable.

