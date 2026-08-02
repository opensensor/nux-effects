# Open Effect Lab

A local page for designing NCR-2 programs, writing new effects in C, and
hearing them before anything is built or flashed.

```sh
python3 host/editor/server.py --open
```

Requires a host C compiler (`cc`) and nothing else — no packages, no
network access, or device for previewing. Web DFU additionally requires a
WebHID-capable Chromium browser and a pedal already running Open Recover.

## What it does

- **Registry** — every effect in the compiled registry, with the names,
  units, ranges, and context sizes read back from its descriptor.
- **Program** — order effects into a chain and set parameters. Controls
  are bounded by the descriptor, so the page cannot ask for a value the
  firmware would reject.
- **Engine bank** — compose four open engine slots (pedal positions 5–8),
  each with eight independently previewable effect programs, and export a
  versioned bank definition as JSON.
- **Listening review** — mark every position Keep, Tune, or Replace, attach
  notes, step across all 32 effects from the keyboard, and retain the complete
  bank and review in browser storage. Version-2 bank JSON imports and exports
  the review alongside program settings.
- **Preview** — plays the chain's output through the *actual* firmware
  runtime compiled on this machine. The default input is a bundled six-second
  CC0 clean electric-guitar DI performance; synthetic plucks, sine, sweep,
  noise, an impulse, a file, and the microphone remain available. Optional
  level matching changes playback gain only, never the rendered samples or
  measurements. `Space` plays; `B` swaps between dry and processed.
- **Effect source** — a compilable ABI template to start from, compiler
  diagnostics with file and line, and a real-time rule scan.
- **Hardware app presets** — an opt-in toggle that lifts all 32 fixed-point
  presets across Open Amp Studio, Drive + Dynamics, Motion + Pitch, and
  Echo + Space out of `firmware/hardware_app/src/main.c` and previews them
  with Amount,
  Character, and Level as the raw ADC counts the firmware reads. That
  file is never modified. Previews are pinned to 48 kHz, one preset per
  chain, and cannot be exported — on the device they are a `switch`, not
  registry effects.
- **Export** — `effects_<name>.c`, its header, and a
  `programs_builtin.c`-style program block, ready to review and commit.
- **Web DFU** — connect a known Open Recover device through WebHID and
  install a reviewed `.slot` artifact to the inactive A/B application slot.

The charts show input and output waveforms in separate lanes, their
spectra, and the chain's response to a −1 → +1 ramp. The measurements
table reports the runtime's own validation status codes, non-finite
samples, peak and RMS, arena use, and host-relative block timing.

The interface uses a locally compiled Tailwind CSS 4 shell. The generated
stylesheet is committed so running the editor still needs no Node packages.
After changing utility classes, rebuild it with:

```sh
npm install
npm run build:editor-css
```

## Current boundary

It does not re-implement DSP in JavaScript, write to the repository, or yet
compile the exported 4×8 bank into a target artifact. That build step still
produces a reviewed `.slot` outside the browser; Web DFU installs that whole
application because the recovery protocol deliberately exposes no arbitrary
factory or engine-region writes. Host block timing is not the on-target CPU
budget gate.

## Layout

| Path | Role |
|---|---|
| `server.py` | loopback HTTP server and JSON/binary API |
| `builder.py` | compiles firmware + authored sources, runs the binary |
| `codegen.py` | generated configuration and exported firmware C |
| `hardware_app.py` | lifts the hardware application's fixed-point presets |
| `rt_rules.py` | heuristic real-time rule scan |
| `native/editor_host.c` | harness that renders through the real chain |
| `static/` | the page |

Compiled previews are cached under `build/effect-editor/`, keyed by the
exact sources and configuration; delete that directory to force a clean
rebuild.

Design notes and the safety model are in
[docs/app/EFFECT_EDITOR.md](../../docs/app/EFFECT_EDITOR.md).

## Safety

The server compiles and runs C that the page submits. Keep it on
loopback (it refuses other addresses), run it on a machine you trust,
and review authored sources as you would any code you are about to
execute.

Web DFU also checks the Open Recover product string, requires explicit
acknowledgement because development builds temporarily borrow NUX's USB ID,
chooses only the slot opposite the confirmed application, validates the slot
manifest and payload SHA-256, and implements no full-flash commands.
