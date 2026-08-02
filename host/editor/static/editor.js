import { OpenRecoveryDfu } from "/static/open_recovery_dfu.mjs";

/*
 * Open Effect Lab — design and preview NCR-2 effects.
 *
 * The browser owns test signals, playback, and drawing. Every sample of
 * processed audio comes back from the host server, which compiles and
 * runs the firmware's own effect runtime. No DSP is re-implemented here.
 */

const RENDER_SECONDS = 2.0;
const RAMP_FRAMES = 2048;
const DEBOUNCE_MS = 140;
const FFT_SIZE = 4096;
const STRING_FREQUENCIES = [82.41, 110.0, 146.83, 196.0, 246.94, 329.63];
const WORKSPACE_STORAGE_KEY = "ncr2-open-effect-lab-workspace-v2";
const AUDIO_INPUT_STORAGE_KEY = "ncr2-open-effect-lab-audio-input-v2";
const REVIEW_STATUSES = new Set(["unreviewed", "keep", "tune", "replace"]);
const OPEN_VENDOR_ID = 0x4f50454e;
const HARDWARE_PRESET_VENDOR_ID = OPEN_VENDOR_ID;
const HARDWARE_PRESET_FIRST_ID = 0x0b000001;
const HARDWARE_PRESET_COUNT = 32;
const PEDAL_AUDIO_PRODUCT_NAME = "NCR-2 Open Pedal Audio";
const PEDAL_CAPTURE_GAIN_DB = 18;

const OPEN_ENGINE_LAYOUT = [
  {
    name: "Open Amp Studio",
    effects: [
      "Glass Clean", "Tweed Bloom", "Class A Chime", "Brit Stack",
      "Brown Lead", "Cali Recto", "Bass Forge", "Acoustic IR",
    ],
  },
  {
    name: "Drive + Dynamics",
    effects: [
      "Shine Drive", "Wall Fuzz", "Rage Drive", "Cocked Wah",
      "Studio Comp", "Octave Fuzz", "String Ensemble", "Noise Gate",
    ],
  },
  {
    name: "Motion + Pitch",
    effects: [
      "Breathe Vibe", "Guerrilla Trem", "Dimension Chorus", "Jet Flanger",
      "Phase Orbit", "Rotary Cab", "Auto Wah", "Whammy Fuzz",
    ],
  },
  {
    name: "Echo + Space",
    effects: [
      "Echoes Tape", "Digital Delay", "Analog Delay", "Reverse Delay",
      "Hall Reverb", "Plate Reverb", "Shimmer Space", "Spring Tank",
    ],
  },
];

const OPTIONAL_ENGINE_PACKS = [
  {
    id: "instrument-lab",
    name: "Instrument Lab",
    summary:
      "Phase-aligned harmonic transplantation into string bodies and a clarinet bore; the dry waveform is not part of the wet output.",
    effects: [
      [0x100, "Steel Acoustic"],
      [0x101, "Nylon Classical"],
      [0x102, "Twelve String"],
      [0x103, "Banjo"],
      [0x104, "Sitar"],
      [0x105, "Upright Bass"],
      [0x106, "Bowed Cello"],
      [0x107, "Clarinet"],
    ],
  },
];

const LEGACY_INSTRUMENT_NAMES = [
  "Bowed Ensemble", "Cello", "Violin", "Tonewheel Organ",
  "Clarinet", "Synth Brass", "Synth Bass", "Bell / Marimba",
];

function emptyProgram(engine, position) {
  return {
    name: OPEN_ENGINE_LAYOUT[engine].effects[position],
    programId: ((engine + 5) << 8) | (position + 1),
    nodes: [],
    factoryPresetIndex: engine * 8 + position,
    review: { status: "unreviewed", notes: "" },
  };
}

const openEngines = OPEN_ENGINE_LAYOUT.map((layout, engine) => ({
  slot: engine + 5,
  name: layout.name,
  effects: layout.effects.map((_, position) =>
    emptyProgram(engine, position)),
}));

const state = {
  session: null,
  sources: [],
  activeSource: 0,
  visualCatalog: null,
  visualEffects: [],
  activeVisualEffect: 0,
  catalog: [],
  openEngines,
  activeOpenEngine: 0,
  activeEffectPosition: 0,
  program: openEngines[0].effects[0],
  sampleRate: 48000,
  blockFrames: 64,
  channels: 2,
  signal: "guitar",
  inputLevelDb: 0,
  input: null,
  inputDigest: null,
  output: null,
  ramp: null,
  rampDigest: null,
  rampOutput: null,
  report: null,
  build: null,
  monitor: "wet",
  levelMatch: false,
  playing: false,
  userAudio: null,
  guitarBytes: null,
  guitarAudio: null,
  audioInputDeviceId: "auto",
  pedalAudio: null,
  includeHardwareApp: false,
  dfu: null,
  dfuInfo: null,
};

const dom = {};
let renderTimer = null;
let renderToken = 0;

/* ------------------------------------------------------------------ */
/* Utilities                                                           */
/* ------------------------------------------------------------------ */

const query = (id) => document.getElementById(id);

function formatHex(value) {
  return `0x${(value >>> 0).toString(16).padStart(8, "0").toUpperCase()}`;
}

function parseNumber(text, fallback) {
  const trimmed = String(text ?? "").trim();
  const value = trimmed.startsWith("0x") || trimmed.startsWith("0X")
    ? Number.parseInt(trimmed, 16)
    : Number.parseInt(trimmed, 10);
  return Number.isFinite(value) && value >= 0 ? value : fallback;
}

function inputLevelText() {
  const trim = `${state.inputLevelDb >= 0 ? "+" : "−"}` +
    `${Math.abs(state.inputLevelDb)} dB`;
  return state.audioInputDeviceId === "pedal-alsa"
    ? `${trim} · pedal +${PEDAL_CAPTURE_GAIN_DB} dB calibrated`
    : trim;
}

function updateInputLevelControl() {
  const control = query("input-level");
  const label = query("input-level-value");
  if (control) control.value = String(state.inputLevelDb);
  if (label) label.textContent = inputLevelText();
}

/** Descriptor bounds arrive as float32; show them without the noise. */
function formatParameter(value) {
  if (Number.isInteger(value)) return String(value);
  return String(Number(Number(value).toPrecision(4)));
}

function decibels(value) {
  if (!(value > 0)) return -Infinity;
  return 20 * Math.log10(value);
}

function formatDb(value, digits = 1) {
  if (!Number.isFinite(value)) return "−∞ dB";
  const sign = value >= 0 ? "+" : "−";
  return `${sign}${Math.abs(value).toFixed(digits)} dB`;
}

async function digestOf(bytes) {
  const hash = await crypto.subtle.digest("SHA-256", bytes);
  return Array.from(new Uint8Array(hash))
    .map((byte) => byte.toString(16).padStart(2, "0"))
    .join("");
}

function setStatus(text, level = "idle") {
  dom.statusText.textContent = text;
  dom.statusDot.dataset.state = level;
}

function downloadJson(name, value) {
  const blob = new Blob([`${JSON.stringify(value, null, 2)}\n`], {
    type: "application/json",
  });
  const link = document.createElement("a");
  link.href = URL.createObjectURL(blob);
  link.download = name;
  link.click();
  URL.revokeObjectURL(link.href);
}

function normalizeReview(review) {
  const status = REVIEW_STATUSES.has(review?.status)
    ? review.status
    : "unreviewed";
  return {
    status,
    notes: typeof review?.notes === "string" ? review.notes : "",
  };
}

function hardwarePresetNode(node) {
  return node.vendorId === HARDWARE_PRESET_VENDOR_ID &&
    node.effectId >= HARDWARE_PRESET_FIRST_ID &&
    node.effectId < HARDWARE_PRESET_FIRST_ID + HARDWARE_PRESET_COUNT;
}

function engineBankDocument() {
  return {
    schema: "ncr2-open-engine-bank",
    version: 3,
    workspace: {
      active_engine: state.activeOpenEngine,
      active_effect: state.activeEffectPosition,
      signal: state.signal,
      input_level_db: state.inputLevelDb,
      level_match: state.levelMatch,
    },
    engine_slots: state.openEngines.map((engine) => ({
      slot: engine.slot,
      name: engine.name,
      effects: engine.effects.map((program, position) => ({
        position: position + 1,
        name: program.name,
        program_id: program.programId,
        review: normalizeReview(program.review),
        nodes: program.nodes.map((node) => ({
          vendor_id: node.vendorId,
          effect_id: node.effectId,
          parameters: Object.entries(node.values).map(([id, value]) => ({
            parameter_id: Number(id),
            value,
          })),
        })),
      })),
    })),
    sources: state.sources,
    visual_effects: state.visualEffects,
    active_visual_effect: state.activeVisualEffect,
  };
}

function persistWorkspace() {
  try {
    localStorage.setItem(
      WORKSPACE_STORAGE_KEY,
      JSON.stringify(engineBankDocument()),
    );
    localStorage.setItem(AUDIO_INPUT_STORAGE_KEY, state.audioInputDeviceId);
  } catch (error) {
    /* Private browsing and storage quotas must not disable the editor. */
  }
}

function documentNode(node) {
  const vendorId = Number(node?.vendor_id);
  const effectId = Number(node?.effect_id);
  if (!Number.isInteger(vendorId) || vendorId < 0 || vendorId > 0xffffffff ||
      !Number.isInteger(effectId) || effectId < 0 || effectId > 0xffffffff) {
    throw new Error("bank contains an invalid effect identity");
  }
  const values = {};
  for (const parameter of node.parameters ?? []) {
    const id = Number(parameter?.parameter_id);
    const value = Number(parameter?.value);
    if (!Number.isInteger(id) || id < 0 || !Number.isFinite(value)) {
      throw new Error("bank contains an invalid parameter value");
    }
    values[id] = value;
  }
  /* Migrate only the exact, shipped pre-audition defaults.  Any control the
     user actually moved is preserved, while saved Instrument Lab slots stop
     concealing the resynth behind the former 60%-effective wet blend. */
  if (vendorId === OPEN_VENDOR_ID &&
      effectId >= 0x100 && effectId <= 0x107 &&
      Math.abs(values[1] - 0.72) < 1e-6 &&
      Math.abs(values[2] - 0.50) < 1e-6 &&
      Math.abs(values[3] - 0.82) < 1e-6 &&
      Math.abs(values[4] - 0.010) < 1e-6) {
    values[1] = 1.0;
    values[3] = 1.0;
    values[4] = 0.006;
  }
  return { vendorId, effectId, values };
}

function applyEngineBankDocument(document) {
  if (document?.schema !== "ncr2-open-engine-bank" ||
      ![1, 2, 3].includes(Number(document.version))) {
    throw new Error("not a supported NCR-2 open engine bank");
  }
  if (!Array.isArray(document.engine_slots) ||
      document.engine_slots.length !== 4) {
    throw new Error("an open engine bank must contain slots 5–8");
  }

  const imported = OPEN_ENGINE_LAYOUT.map((layout, engineIndex) => {
    const slotNumber = engineIndex + 5;
    const slot = document.engine_slots.find(
      (candidate) => Number(candidate?.slot) === slotNumber,
    );
    if (!slot || !Array.isArray(slot.effects) || slot.effects.length !== 8) {
      throw new Error(`open engine slot ${slotNumber} must contain 8 effects`);
    }
    return {
      slot: slotNumber,
      name: typeof slot.name === "string" ? slot.name : layout.name,
      effects: Array.from({ length: 8 }, (_, position) => {
        const effect = slot.effects.find(
          (candidate) => Number(candidate?.position) === position + 1,
        );
        if (!effect) {
          throw new Error(
            `open engine slot ${slotNumber} is missing position ${position + 1}`,
          );
        }
        const programId = Number(effect.program_id);
        if (!Number.isInteger(programId) ||
            programId < 0 || programId > 0xffffffff) {
          throw new Error("bank contains an invalid program id");
        }
        return {
          name: typeof effect.name === "string"
            ? effect.name
            : layout.effects[position],
          programId,
          nodes: (effect.nodes ?? []).map(documentNode),
          factoryPresetIndex: engineIndex * 8 + position,
          review: normalizeReview(effect.review),
        };
      }),
    };
  });

  /* The spectral-transformation prototype deliberately reuses the
     experimental 0x100–0x107 identities. Rename an untouched saved copy of
     the old pack, but leave user-authored or individually renamed programs
     alone. */
  imported.forEach((engine) => {
    const untouchedLegacyPack = engine.effects.every((program, position) =>
      program.name === LEGACY_INSTRUMENT_NAMES[position] &&
      program.nodes.length === 1 &&
      program.nodes[0].vendorId === OPEN_VENDOR_ID &&
      program.nodes[0].effectId === 0x100 + position,
    );
    if (!untouchedLegacyPack) return;
    engine.name = "Instrument Lab";
    engine.effects.forEach((program, position) => {
      program.name = OPTIONAL_ENGINE_PACKS[0].effects[position][1];
    });
  });

  state.openEngines = imported;
  state.sources = Array.isArray(document.sources)
    ? document.sources.map((source) => {
      if (typeof source?.name !== "string" ||
          typeof source?.text !== "string") {
        throw new Error("bank contains an invalid authored source");
      }
      return {
        name: source.name,
        text: source.text,
        ...(typeof source.visualKey === "string"
          ? { visualKey: source.visualKey }
          : {}),
      };
    })
    : [];
  state.visualEffects = Array.isArray(document.visual_effects)
    ? document.visual_effects.map(normalizeVisualSpec)
    : [];
  state.activeVisualEffect = Math.max(
    0,
    Math.min(
      Math.max(0, state.visualEffects.length - 1),
      Number(document.active_visual_effect) || 0,
    ),
  );
  const workspace = document.workspace ?? {};
  state.activeOpenEngine = Math.max(
    0,
    Math.min(3, Number(workspace.active_engine) || 0),
  );
  state.activeEffectPosition = Math.max(
    0,
    Math.min(7, Number(workspace.active_effect) || 0),
  );
  state.signal = [
    "guitar", "pluck", "chord", "harmonic", "sine", "sweep", "noise",
    "impulse",
  ].includes(workspace.signal) ? workspace.signal : "guitar";
  state.inputLevelDb = Number.isFinite(Number(workspace.input_level_db))
    ? Math.max(-30, Math.min(18, Number(workspace.input_level_db)))
    : 0;
  state.levelMatch = workspace.level_match === true;
  state.program = state.openEngines[state.activeOpenEngine]
    .effects[state.activeEffectPosition];
  state.includeHardwareApp = state.openEngines.some((engine) =>
    engine.effects.some((program) => program.nodes.some(hardwarePresetNode)),
  );
}

function restoreWorkspace() {
  try {
    state.audioInputDeviceId =
      localStorage.getItem(AUDIO_INPUT_STORAGE_KEY) || "auto";
    const saved = localStorage.getItem(WORKSPACE_STORAGE_KEY);
    if (!saved) return false;
    applyEngineBankDocument(JSON.parse(saved));
    return true;
  } catch (error) {
    try {
      localStorage.removeItem(WORKSPACE_STORAGE_KEY);
    } catch (storageError) {
      /* Storage can be entirely unavailable in hardened browser profiles. */
    }
    return false;
  }
}

/* ------------------------------------------------------------------ */
/* Server                                                              */
/* ------------------------------------------------------------------ */

async function getJson(path) {
  const response = await fetch(path);
  if (!response.ok) throw new Error(`${path}: ${response.status}`);
  return response.json();
}

async function postJson(path, body) {
  const response = await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const payload = await response.json();
  if (!response.ok) throw new Error(payload.error || response.statusText);
  return payload;
}

function sessionRequest(extra = {}) {
  return {
    sources: state.sources.map((source) => ({
      name: source.name,
      text: source.text,
    })),
    program: {
      name: state.program.name,
      program_id: state.program.programId,
      nodes: state.program.nodes.map((node) => ({
        vendor_id: node.vendorId,
        effect_id: node.effectId,
        parameters: Object.entries(node.values).map(([id, value]) => ({
          parameter_id: Number(id),
          value,
        })),
      })),
    },
    sample_rate: state.sampleRate,
    block_frames: state.blockFrames,
    channels: state.channels,
    include_hardware_app: state.includeHardwareApp,
    ...extra,
  };
}

async function postFrame(path, request, payload) {
  const header = new TextEncoder().encode(JSON.stringify(request));
  const body = new Uint8Array(4 + header.length + (payload?.byteLength ?? 0));
  new DataView(body.buffer).setUint32(0, header.length, true);
  body.set(header, 4);
  if (payload) body.set(new Uint8Array(payload), 4 + header.length);

  const response = await fetch(path, {
    method: "POST",
    headers: { "Content-Type": "application/octet-stream" },
    body,
  });
  if (!response.ok) {
    const detail = await response.json().catch(() => ({}));
    throw new Error(detail.error || `render failed: ${response.status}`);
  }
  const buffer = await response.arrayBuffer();
  const length = new DataView(buffer).getUint32(0, true);
  const meta = JSON.parse(
    new TextDecoder().decode(new Uint8Array(buffer, 4, length)),
  );
  const audioBytes = buffer.byteLength - 4 - length;
  const audio = audioBytes > 0
    ? new Float32Array(buffer.slice(4 + length))
    : new Float32Array(0);
  return { meta, audio };
}

async function postRender(request, payload) {
  return postFrame("/api/render", request, payload);
}

/** Render through the host, sending the payload only when it is new. */
async function renderSignal(samples, digest, extra = {}) {
  const request = sessionRequest({
    input_sha256: digest,
    overrides: state.program.nodes.flatMap((node, index) =>
      Object.entries(node.values).map(([id, value]) => [
        index,
        Number(id),
        value,
      ]),
    ),
    ...extra,
  });
  let result = await postRender(request, null);
  if (result.meta.status === "unknown-input") {
    result = await postRender(request, samples.buffer);
  }
  return result;
}

/* ------------------------------------------------------------------ */
/* Test signals                                                        */
/* ------------------------------------------------------------------ */

function pluckedString(output, offset, frequency, seconds, seed) {
  const rate = state.sampleRate;
  const period = Math.max(2, Math.round(rate / frequency));
  const buffer = new Float32Array(period);
  let random = seed >>> 0;
  for (let index = 0; index < period; index += 1) {
    random = (random * 1664525 + 1013904223) >>> 0;
    buffer[index] = (random / 0xffffffff) * 2 - 1;
  }
  const total = Math.min(Math.floor(seconds * rate), output.length - offset);
  let cursor = 0;
  let previous = 0;
  for (let index = 0; index < total; index += 1) {
    const current = buffer[cursor];
    const filtered = 0.5 * (current + previous) * 0.996;
    buffer[cursor] = filtered;
    previous = current;
    cursor = (cursor + 1) % period;
    output[offset + index] += filtered;
  }
}

function generateSignal(kind) {
  const rate = state.sampleRate;
  const frames = Math.floor(RENDER_SECONDS * rate);
  const mono = new Float32Array(frames);

  if (kind === "pluck") {
    pluckedString(mono, 0, 110.0, RENDER_SECONDS, 0x1234567);
  } else if (kind === "chord") {
    STRING_FREQUENCIES.forEach((frequency, index) => {
      const offset = Math.floor(index * 0.018 * rate);
      pluckedString(
        mono,
        offset,
        frequency,
        RENDER_SECONDS - offset / rate,
        0x9e3779b9 + index * 2654435761,
      );
    });
  } else if (kind === "harmonic") {
    const notes = [110.0, 146.83, 196.0, 220.0];
    const noteFrames = Math.floor(frames / notes.length);
    notes.forEach((frequency, note) => {
      const start = note * noteFrames;
      const end = note === notes.length - 1 ? frames : start + noteFrames;
      for (let index = start; index < end; index += 1) {
        const local = index - start;
        const remaining = end - index;
        const envelope = Math.min(1, local / (0.008 * rate)) *
          Math.min(1, remaining / (0.045 * rate)) *
          Math.exp((-1.3 * local) / noteFrames);
        for (let partial = 1; partial <= 12; partial += 1) {
          const profile = 1 / Math.pow(partial, 0.72);
          mono[index] += envelope * profile * Math.sin(
            (2 * Math.PI * frequency * partial * local) / rate,
          );
        }
      }
    });
  } else if (kind === "sine") {
    for (let index = 0; index < frames; index += 1) {
      mono[index] = Math.sin((2 * Math.PI * 220 * index) / rate);
    }
  } else if (kind === "sweep") {
    const start = 40;
    const end = 8000;
    const duration = frames / rate;
    let phase = 0;
    for (let index = 0; index < frames; index += 1) {
      const time = index / rate;
      const frequency = start * Math.pow(end / start, time / duration);
      phase += (2 * Math.PI * frequency) / rate;
      const fade = Math.min(1, (frames - index) / (0.05 * rate));
      mono[index] = Math.sin(phase) * fade;
    }
  } else if (kind === "noise") {
    let random = 0xc0ffee;
    for (let index = 0; index < frames; index += 1) {
      random = (random * 1664525 + 1013904223) >>> 0;
      const envelope = Math.exp((-3 * index) / (0.5 * rate));
      mono[index] = ((random / 0xffffffff) * 2 - 1) * envelope;
    }
  } else if (kind === "impulse") {
    mono[Math.floor(0.01 * rate)] = 1.0;
  }
  return mono;
}

function normalize(mono) {
  let peak = 0;
  for (let index = 0; index < mono.length; index += 1) {
    peak = Math.max(peak, Math.abs(mono[index]));
  }
  if (peak > 0) {
    const scale = 1 / peak;
    for (let index = 0; index < mono.length; index += 1) {
      mono[index] *= scale;
    }
  }
  return mono;
}

function interleave(mono, channels, gain) {
  const output = new Float32Array(mono.length * channels);
  for (let frame = 0; frame < mono.length; frame += 1) {
    const value = mono[frame] * gain;
    for (let channel = 0; channel < channels; channel += 1) {
      output[frame * channels + channel] = value;
    }
  }
  return output;
}

function buildRamp() {
  const mono = new Float32Array(RAMP_FRAMES);
  for (let index = 0; index < RAMP_FRAMES; index += 1) {
    mono[index] = (index / (RAMP_FRAMES - 1)) * 2 - 1;
  }
  return interleave(mono, state.channels, 1.0);
}

async function prepareInput() {
  const captured = state.signal === "file" || state.signal === "mic";
  let mono;
  if (state.signal === "guitar" && state.guitarAudio !== null) {
    mono = state.guitarAudio;
  } else if (captured && state.userAudio !== null) {
    mono = state.userAudio;
  } else {
    mono = normalize(generateSignal(
      state.signal === "guitar" || captured ? "pluck" : state.signal,
    ));
  }
  const gain = Math.pow(10, state.inputLevelDb / 20);
  state.input = interleave(mono, state.channels, gain);
  state.inputDigest = await digestOf(state.input.buffer);
  state.ramp = buildRamp();
  state.rampDigest = await digestOf(state.ramp.buffer);
}

async function decodeUserAudio(arrayBuffer, normalizePeak = false) {
  const context = new OfflineAudioContext(1, 1, state.sampleRate);
  const decoded = await context.decodeAudioData(arrayBuffer);
  const frames = Math.min(decoded.length, state.sampleRate * 6);
  const mono = new Float32Array(frames);
  for (let channel = 0; channel < decoded.numberOfChannels; channel += 1) {
    const data = decoded.getChannelData(channel);
    for (let index = 0; index < frames; index += 1) {
      mono[index] += data[index] / decoded.numberOfChannels;
    }
  }
  return normalizePeak ? normalize(mono) : mono;
}

async function loadBundledGuitar() {
  if (state.guitarBytes === null) {
    const response = await fetch("/static/audio/clean-guitar-di.wav");
    if (!response.ok) {
      throw new Error(`clean guitar recording unavailable: ${response.status}`);
    }
    state.guitarBytes = await response.arrayBuffer();
  }
  state.guitarAudio = await decodeUserAudio(state.guitarBytes.slice(0));
}

async function captureMicrophone(seconds = 6.0) {
  if (state.audioInputDeviceId === "pedal-alsa") {
    const result = await postFrame(
      "/api/pedal/record",
      { seconds, gain_db: PEDAL_CAPTURE_GAIN_DB },
      null,
    );
    if (result.meta.status !== "ok") {
      throw new Error(result.meta.error || "direct pedal recording stopped");
    }
    return result.audio;
  }
  const stream = await acquireAudioInput();
  const context = new AudioContext({ sampleRate: state.sampleRate });
  const source = context.createMediaStreamSource(stream);
  const processorFrames = 4096;
  const capture = context.createScriptProcessor(processorFrames, 1, 1);
  const chunks = [];
  let captured = 0;
  const target = Math.floor(seconds * context.sampleRate);

  return new Promise((resolve) => {
    capture.onaudioprocess = (event) => {
      const input = event.inputBuffer.getChannelData(0);
      chunks.push(new Float32Array(input));
      captured += input.length;
      if (captured >= target) {
        capture.onaudioprocess = null;
        source.disconnect();
        capture.disconnect();
        stream.getTracks().forEach((track) => track.stop());
        context.close();
        const mono = new Float32Array(target);
        let offset = 0;
        for (const chunk of chunks) {
          const remaining = target - offset;
          if (remaining <= 0) break;
          mono.set(chunk.subarray(0, remaining), offset);
          offset += Math.min(chunk.length, remaining);
        }
        resolve(mono);
      }
    };
    source.connect(capture);
    capture.connect(context.destination);
  });
}

/* ------------------------------------------------------------------ */
/* Stateful native live input                                         */
/* ------------------------------------------------------------------ */

const livePreview = {
  active: false,
  starting: false,
  restarting: false,
  processing: false,
  token: null,
  generation: 0,
  stream: null,
  context: null,
  source: null,
  processor: null,
  silent: null,
  queue: [],
  outputNodes: new Set(),
  nextStart: 0,
  dropped: 0,
  deviceLabel: "",
  inputMode: "browser",
};
let liveRestartTimer = null;

function audioInputConstraints(deviceId = state.audioInputDeviceId) {
  return {
    autoGainControl: false,
    echoCancellation: false,
    noiseSuppression: false,
    channelCount: 1,
    sampleRate: state.sampleRate,
    ...(!["auto", "default", "pedal-alsa", ""].includes(deviceId)
      ? { deviceId: { exact: deviceId } }
      : {}),
  };
}

function selectedAudioInputLabel() {
  return dom.audioInputDevice?.selectedOptions?.[0]?.textContent ||
    "system default audio input";
}

async function refreshAudioInputDevices(preferredId = state.audioInputDeviceId) {
  if (!dom.audioInputDevice) return;
  const devices = navigator.mediaDevices?.enumerateDevices
    ? (await navigator.mediaDevices.enumerateDevices())
      .filter((device) => device.kind === "audioinput" &&
        !["default", "communications"].includes(device.deviceId))
    : [];
  const autoOption = document.createElement("option");
  autoOption.value = "auto";
  autoOption.textContent = "Auto-detect pedal / system default";
  const defaultOption = document.createElement("option");
  defaultOption.value = "default";
  defaultOption.textContent = "System default audio input";
  const options = [autoOption];
  if (state.pedalAudio?.available) {
    const pedalOption = document.createElement("option");
    pedalOption.value = "pedal-alsa";
    pedalOption.textContent = "Pedal USB — direct (recommended)";
    options.push(pedalOption);
  }
  options.push(defaultOption);
  dom.audioInputDevice.replaceChildren(...options);
  devices.forEach((device, index) => {
    const option = document.createElement("option");
    option.value = device.deviceId;
    option.textContent = device.label || `Audio input ${index + 1}`;
    dom.audioInputDevice.append(option);
  });

  let selected = preferredId;
  const specialInputs = ["auto", "default", "pedal-alsa"];
  if (selected === "pedal-alsa" && !state.pedalAudio?.available) {
    selected = "auto";
  } else if (!specialInputs.includes(selected) &&
      !devices.some((device) => device.deviceId === selected)) {
    selected = "auto";
  }
  if (selected === "auto") {
    if (state.pedalAudio?.available) {
      selected = "pedal-alsa";
    } else {
      const pedal = devices.find((device) =>
        device.label.toLowerCase().includes(
          PEDAL_AUDIO_PRODUCT_NAME.toLowerCase(),
        ));
      if (pedal) selected = pedal.deviceId;
    }
  }
  state.audioInputDeviceId = selected;
  dom.audioInputDevice.value = selected;
  updateInputLevelControl();
  persistWorkspace();
}

async function acquireAudioInput() {
  if (state.audioInputDeviceId === "pedal-alsa") {
    throw new Error("direct pedal audio must be captured by the local server");
  }
  if (!navigator.mediaDevices?.getUserMedia) {
    throw new Error("this browser does not expose audio capture");
  }
  let stream = await navigator.mediaDevices.getUserMedia({
    audio: audioInputConstraints(),
  });
  const acquiredId = stream.getAudioTracks()[0]?.getSettings?.().deviceId || "";
  if (state.audioInputDeviceId === "auto") {
    await refreshAudioInputDevices();
    if (!["auto", "default", ""].includes(state.audioInputDeviceId) &&
        state.audioInputDeviceId !== acquiredId) {
      stream.getTracks().forEach((track) => track.stop());
      stream = await navigator.mediaDevices.getUserMedia({
        audio: audioInputConstraints(state.audioInputDeviceId),
      });
    }
  } else {
    await refreshAudioInputDevices(state.audioInputDeviceId);
  }
  return stream;
}

function currentOverrides() {
  return state.program.nodes.flatMap((node, index) =>
    Object.entries(node.values).map(([id, value]) => [
      index,
      Number(id),
      value,
    ]));
}

function updateLivePreviewState(message = "") {
  if (!dom.liveInput) return;
  dom.liveInput.textContent = livePreview.active
    ? "Stop live input"
    : livePreview.starting
      ? "Starting…"
      : "Play live input";
  dom.liveInput.disabled = livePreview.starting;
  if (dom.liveInputState) {
    dom.liveInputState.textContent = message || (livePreview.active
      ? `Native DSP is live${livePreview.dropped
        ? ` · ${livePreview.dropped} output buffer refill(s)`
        : ""}${livePreview.deviceLabel
        ? ` · ${livePreview.deviceLabel}`
        : ""}`
      : "Use headphones or a muted monitor path to prevent feedback.");
  }
}

async function startNativeLiveSession() {
  const sampleRate = livePreview.context?.sampleRate ?? state.sampleRate;
  const payload = await postJson("/api/live/start", sessionRequest({
    sample_rate: sampleRate,
    overrides: currentOverrides(),
    input_mode: livePreview.inputMode,
  }));
  livePreview.token = payload.token;
  livePreview.generation += 1;
  livePreview.queue = [];
  return payload;
}

function scheduleLiveAudio(interleaved) {
  const context = livePreview.context;
  if (!context || !livePreview.active) return;
  const frames = Math.floor(interleaved.length / state.channels);
  if (frames === 0) return;
  const buffer = context.createBuffer(
    state.channels,
    frames,
    context.sampleRate,
  );
  for (let channel = 0; channel < state.channels; channel += 1) {
    const output = buffer.getChannelData(channel);
    for (let frame = 0; frame < frames; frame += 1) {
      output[frame] = interleaved[frame * state.channels + channel];
    }
  }
  const node = context.createBufferSource();
  node.buffer = buffer;
  node.connect(context.destination);
  const lead = livePreview.inputMode === "pedal-alsa" ? 0.08 : 0.035;
  const earliest = context.currentTime + lead;
  if (livePreview.nextStart < context.currentTime - 0.05) {
    livePreview.nextStart = earliest;
  }
  const startsAt = Math.max(earliest, livePreview.nextStart);
  node.start(startsAt);
  livePreview.nextStart = startsAt + buffer.duration;
  livePreview.outputNodes.add(node);
  node.addEventListener("ended", () => {
    livePreview.outputNodes.delete(node);
    node.disconnect();
  }, { once: true });
}

async function pumpLivePreview() {
  if (!livePreview.active || livePreview.restarting ||
      livePreview.processing || livePreview.queue.length === 0) return;
  livePreview.processing = true;
  const item = livePreview.queue.shift();
  try {
    const result = await postFrame(
      "/api/live/chunk",
      { token: livePreview.token },
      item.audio.buffer,
    );
    if (result.meta.status !== "ok") {
      throw new Error(result.meta.error || "live DSP stopped");
    }
    if (livePreview.active && item.generation === livePreview.generation) {
      scheduleLiveAudio(state.monitor === "dry" ? item.audio : result.audio);
    }
  } catch (error) {
    setStatus(`live input stopped: ${error.message}`, "error");
    await stopLivePreview(true);
    updateLivePreviewState(`Stopped: ${error.message}`);
  } finally {
    livePreview.processing = false;
    if (livePreview.active) pumpLivePreview();
  }
}

async function pumpPedalPreview() {
  if (!livePreview.active || livePreview.inputMode !== "pedal-alsa" ||
      livePreview.restarting || livePreview.processing) return;
  livePreview.processing = true;
  const generation = livePreview.generation;
  try {
    const result = await postFrame(
      "/api/live/pedal-chunk",
      {
        token: livePreview.token,
        frames: 1024,
        channels: state.channels,
        gain_db: state.inputLevelDb + PEDAL_CAPTURE_GAIN_DB,
        monitor: state.monitor,
      },
      null,
    );
    if (result.meta.status !== "ok") {
      throw new Error(result.meta.error || "direct pedal DSP stopped");
    }
    if (livePreview.active && generation === livePreview.generation) {
      scheduleLiveAudio(result.audio);
    }
  } catch (error) {
    setStatus(`live input stopped: ${error.message}`, "error");
    await stopLivePreview(true);
    updateLivePreviewState(`Stopped: ${error.message}`);
  } finally {
    livePreview.processing = false;
    if (livePreview.active && livePreview.inputMode === "pedal-alsa") {
      pumpPedalPreview();
    }
  }
}

function enqueueLiveInput(mono) {
  const gain = Math.pow(10, state.inputLevelDb / 20);
  const audio = interleave(mono, state.channels, gain);
  while (livePreview.queue.length >= 3) {
    livePreview.queue.shift();
    livePreview.dropped += 1;
  }
  livePreview.queue.push({
    audio,
    generation: livePreview.generation,
  });
  updateLivePreviewState();
  pumpLivePreview();
}

async function startLivePreview() {
  if (livePreview.active || livePreview.starting) return;
  livePreview.starting = true;
  const directPedal = state.audioInputDeviceId === "pedal-alsa";
  livePreview.inputMode = directPedal ? "pedal-alsa" : "browser";
  updateLivePreviewState(directPedal
    ? "Opening the pedal directly; no browser microphone permission is needed…"
    : "Requesting the raw instrument input…");
  try {
    if (directPedal) {
      livePreview.deviceLabel = state.pedalAudio?.name ||
        PEDAL_AUDIO_PRODUCT_NAME;
    } else {
      livePreview.stream = await acquireAudioInput();
      livePreview.deviceLabel =
        livePreview.stream.getAudioTracks()[0]?.label ||
        selectedAudioInputLabel();
    }
    livePreview.context = new AudioContext({ sampleRate: state.sampleRate });
    await livePreview.context.resume();
    await startNativeLiveSession();
    livePreview.active = true;
    livePreview.nextStart = livePreview.context.currentTime +
      (directPedal ? 0.1 : 0.05);
    livePreview.dropped = 0;
    dom.ab.disabled = false;
    if (directPedal) {
      setStatus("pedal USB is running through the native firmware DSP", "ok");
      updateLivePreviewState(
        "Direct pedal capture is live; browser microphone access is bypassed.",
      );
      pumpPedalPreview();
      return;
    }
    livePreview.source = livePreview.context.createMediaStreamSource(
      livePreview.stream,
    );
    livePreview.processor = livePreview.context.createScriptProcessor(
      1024,
      1,
      1,
    );
    livePreview.silent = livePreview.context.createGain();
    livePreview.silent.gain.value = 0;
    livePreview.processor.onaudioprocess = (event) => {
      if (!livePreview.active) return;
      enqueueLiveInput(new Float32Array(
        event.inputBuffer.getChannelData(0),
      ));
    };
    livePreview.source.connect(livePreview.processor);
    livePreview.processor.connect(livePreview.silent);
    livePreview.silent.connect(livePreview.context.destination);
    setStatus("live guitar is running through the native firmware DSP", "ok");
    updateLivePreviewState("Move a control; the native session reloads it after 450 ms.");
  } catch (error) {
    await stopLivePreview(true);
    throw error;
  } finally {
    livePreview.starting = false;
    updateLivePreviewState();
  }
}

async function restartLivePreview() {
  if (!livePreview.active || livePreview.restarting) return;
  livePreview.restarting = true;
  updateLivePreviewState("Applying controls to a fresh native DSP session…");
  try {
    await startNativeLiveSession();
    livePreview.nextStart = livePreview.context.currentTime + 0.04;
    setStatus("live controls applied", "ok");
  } catch (error) {
    setStatus(`could not update live DSP: ${error.message}`, "error");
    await stopLivePreview(true);
  } finally {
    livePreview.restarting = false;
    updateLivePreviewState();
    if (livePreview.inputMode === "pedal-alsa") pumpPedalPreview();
    else pumpLivePreview();
  }
}

function scheduleLiveRestart() {
  window.clearTimeout(liveRestartTimer);
  liveRestartTimer = window.setTimeout(restartLivePreview, 450);
}

async function stopLivePreview(notifyServer = true) {
  window.clearTimeout(liveRestartTimer);
  const token = livePreview.token;
  livePreview.active = false;
  livePreview.generation += 1;
  livePreview.queue = [];
  if (livePreview.processor) {
    livePreview.processor.onaudioprocess = null;
    livePreview.processor.disconnect();
  }
  if (livePreview.source) livePreview.source.disconnect();
  if (livePreview.silent) livePreview.silent.disconnect();
  livePreview.outputNodes.forEach((node) => {
    try { node.stop(); } catch (error) { /* already stopped */ }
    node.disconnect();
  });
  livePreview.outputNodes.clear();
  livePreview.stream?.getTracks().forEach((track) => track.stop());
  if (livePreview.context) {
    await livePreview.context.close().catch(() => {});
  }
  if (notifyServer && token) {
    await postJson("/api/live/stop", { token }).catch(() => {});
  }
  livePreview.token = null;
  livePreview.stream = null;
  livePreview.context = null;
  livePreview.source = null;
  livePreview.processor = null;
  livePreview.silent = null;
  livePreview.deviceLabel = "";
  livePreview.inputMode = "browser";
  livePreview.processing = false;
  livePreview.restarting = false;
  dom.ab.disabled = state.output === null;
  updateLivePreviewState();
}

/* ------------------------------------------------------------------ */
/* Playback                                                            */
/* ------------------------------------------------------------------ */

const playback = {
  context: null,
  nodes: [],
  gains: {},
  startedAt: 0,
  offset: 0,
};

function ensureContext() {
  if (playback.context === null) {
    playback.context = new AudioContext({ sampleRate: state.sampleRate });
  }
  return playback.context;
}

function toAudioBuffer(context, interleaved) {
  const frames = Math.floor(interleaved.length / state.channels);
  const buffer = context.createBuffer(
    state.channels,
    Math.max(frames, 1),
    state.sampleRate,
  );
  for (let channel = 0; channel < state.channels; channel += 1) {
    const data = buffer.getChannelData(channel);
    for (let frame = 0; frame < frames; frame += 1) {
      data[frame] = interleaved[frame * state.channels + channel];
    }
  }
  return buffer;
}

function stopNodes() {
  playback.nodes.forEach((node) => {
    try {
      node.stop();
    } catch (error) {
      /* already stopped */
    }
    node.disconnect();
  });
  playback.nodes = [];
}

function auditionWetGain() {
  if (!state.levelMatch || !state.report) return 1;
  const dry = Number(state.report.rms_input);
  const wet = Number(state.report.rms_output);
  const peak = Number(state.report.peak_output);
  if (!(dry > 0) || !(wet > 0)) return 1;
  const rmsGain = dry / wet;
  const peakSafeGain = peak > 0 ? 0.98 / peak : 1;
  return Math.max(0.001, Math.min(7.943, rmsGain, peakSafeGain));
}

function updateLevelMatchNote() {
  if (!dom.levelMatchNote) return;
  if (!state.levelMatch) {
    dom.levelMatchNote.textContent = "Off; playback uses the raw DSP level.";
    return;
  }
  const gain = auditionWetGain();
  dom.levelMatchNote.textContent = state.report
    ? `Playback trim ${formatDb(decibels(gain))}; DSP and measurements stay raw.`
    : "Playback only; measurements remain raw.";
}

function startPlayback(offset = 0) {
  if (state.input === null || state.output === null) return;
  const context = ensureContext();
  stopNodes();

  const buffers = {
    dry: toAudioBuffer(context, state.input),
    wet: toAudioBuffer(context, state.output),
  };
  playback.gains = {};
  Object.entries(buffers).forEach(([name, buffer]) => {
    const source = context.createBufferSource();
    const gain = context.createGain();
    source.buffer = buffer;
    source.loop = true;
    gain.gain.value = state.monitor === name
      ? (name === "wet" ? auditionWetGain() : 1)
      : 0;
    source.connect(gain).connect(context.destination);
    source.start(0, offset % buffer.duration);
    playback.nodes.push(source);
    playback.gains[name] = gain;
  });
  playback.startedAt = context.currentTime;
  playback.offset = offset;
  state.playing = true;
  dom.play.textContent = "Stop";
}

function stopPlayback() {
  stopNodes();
  state.playing = false;
  dom.play.textContent = "Play";
}

function currentOffset() {
  if (!state.playing || playback.context === null) return 0;
  const duration = state.input.length / state.channels / state.sampleRate;
  const elapsed = playback.context.currentTime - playback.startedAt;
  return (playback.offset + elapsed) % duration;
}

function applyMonitor() {
  Object.entries(playback.gains).forEach(([name, gain]) => {
    gain.gain.value = state.monitor === name
      ? (name === "wet" ? auditionWetGain() : 1)
      : 0;
  });
  dom.ab.textContent =
    state.monitor === "wet" ? "Hear dry" : "Hear processed";
  updateLevelMatchNote();
}

/* ------------------------------------------------------------------ */
/* Charts                                                              */
/* ------------------------------------------------------------------ */

function chartContext(canvas) {
  const ratio = window.devicePixelRatio || 1;
  const width = canvas.clientWidth;
  /*
   * Assigning canvas.height also rewrites the HTML height attribute. Reading
   * that attribute again on a Retina display multiplied the backing height
   * on every render (144 -> 288 -> 576...) and eventually exhausted the
   * widget. Capture the intended CSS height once and never derive it from the
   * mutable backing-store dimensions again.
   */
  const height = Number(
    canvas.dataset.cssHeight ?? canvas.getAttribute("height") ?? 150,
  );
  canvas.dataset.cssHeight = String(height);
  canvas.style.height = `${height}px`;
  if (canvas.width !== width * ratio || canvas.height !== height * ratio) {
    canvas.width = Math.max(1, Math.floor(width * ratio));
    canvas.height = Math.max(1, Math.floor(height * ratio));
  }
  const context = canvas.getContext("2d");
  context.setTransform(ratio, 0, 0, ratio, 0, 0);
  context.clearRect(0, 0, width, height);
  const styles = getComputedStyle(document.body);
  return {
    context,
    width,
    height,
    ink: styles.getPropertyValue("--text-muted").trim(),
    line: styles.getPropertyValue("--line").trim(),
    seriesIn: styles.getPropertyValue("--series-in").trim(),
    seriesOut: styles.getPropertyValue("--series-out").trim(),
  };
}

function drawGrid(view, rows, labeller) {
  const { context, width, height, line, ink } = view;
  context.strokeStyle = line;
  context.fillStyle = ink;
  context.lineWidth = 1;
  context.font = "10px ui-monospace, monospace";
  context.textBaseline = "middle";
  rows.forEach((fraction) => {
    const y = Math.round(fraction * height) + 0.5;
    context.beginPath();
    context.moveTo(0, y);
    context.lineTo(width, y);
    context.stroke();
    if (labeller) {
      const label = labeller(fraction);
      if (label) context.fillText(label, 4, y - 6);
    }
  });
}

function envelope(samples, channels, columns) {
  const frames = Math.floor(samples.length / channels);
  const perColumn = Math.max(1, Math.floor(frames / columns));
  const minimum = new Float32Array(columns);
  const maximum = new Float32Array(columns);
  for (let column = 0; column < columns; column += 1) {
    let low = 0;
    let high = 0;
    const start = column * perColumn;
    for (let index = 0; index < perColumn; index += 1) {
      const frame = start + index;
      if (frame >= frames) break;
      const value = samples[frame * channels];
      if (value < low) low = value;
      if (value > high) high = value;
    }
    minimum[column] = low;
    maximum[column] = high;
  }
  return { minimum, maximum };
}

/*
 * Input and output share one amplitude scale but get their own lane. An
 * overlay hides whichever signal is quieter inside the louder one, which
 * is exactly the comparison a drive effect is being judged on.
 */
function drawWaveform(canvas) {
  const view = chartContext(canvas);
  const { context, width, height } = view;
  if (state.input === null) return;

  const lanes = [
    { data: state.input, color: view.seriesIn, label: "input" },
    { data: state.output, color: view.seriesOut, label: "output" },
  ];
  const laneHeight = height / lanes.length;
  const scale = laneHeight / 2 / 1.15;

  context.font = "10px ui-monospace, monospace";
  lanes.forEach(({ data, color, label }, index) => {
    const middle = index * laneHeight + laneHeight / 2;

    context.strokeStyle = view.line;
    context.lineWidth = 1;
    context.beginPath();
    context.moveTo(0, Math.round(middle) + 0.5);
    context.lineTo(width, Math.round(middle) + 0.5);
    context.stroke();

    context.setLineDash([3, 3]);
    [1, -1].forEach((level) => {
      const y = Math.round(middle - level * scale) + 0.5;
      context.beginPath();
      context.moveTo(0, y);
      context.lineTo(width, y);
      context.stroke();
    });
    context.setLineDash([]);

    if (data && data.length > 0) {
      const { minimum, maximum } = envelope(
        data,
        state.channels,
        Math.floor(width),
      );
      context.strokeStyle = color;
      context.lineWidth = 1.5;
      context.beginPath();
      for (let column = 0; column < minimum.length; column += 1) {
        const x = column + 0.5;
        context.moveTo(x, middle - Math.max(maximum[column], 0.002) * scale);
        context.lineTo(x, middle - Math.min(minimum[column], -0.002) * scale);
      }
      context.stroke();
    }

    context.fillStyle = color;
    context.fillText(label, 6, index * laneHeight + 12);
    context.fillStyle = view.ink;
    context.fillText("±1", width - 20, middle - scale + 9);
  });

  if (lanes.length > 1) {
    context.strokeStyle = view.line;
    context.lineWidth = 1;
    context.beginPath();
    context.moveTo(0, Math.round(laneHeight) + 0.5);
    context.lineTo(width, Math.round(laneHeight) + 0.5);
    context.stroke();
  }
}

/* Spectra are recomputed only when the samples change, not on resize. */
const spectrumCache = new WeakMap();

function spectrumOf(samples, channels) {
  const cached = spectrumCache.get(samples);
  if (cached) return cached;
  const bins = fftMagnitudes(samples, channels);
  spectrumCache.set(samples, bins);
  return bins;
}

function fftMagnitudes(samples, channels) {
  const size = FFT_SIZE;
  const real = new Float64Array(size);
  const imaginary = new Float64Array(size);
  const frames = Math.floor(samples.length / channels);
  const available = Math.min(size, frames);
  for (let index = 0; index < available; index += 1) {
    const window =
      0.5 - 0.5 * Math.cos((2 * Math.PI * index) / (size - 1));
    real[index] = samples[index * channels] * window;
  }

  for (let index = 1, position = 0; index < size; index += 1) {
    let bit = size >> 1;
    for (; position & bit; bit >>= 1) position ^= bit;
    position ^= bit;
    if (index < position) {
      [real[index], real[position]] = [real[position], real[index]];
      [imaginary[index], imaginary[position]] =
        [imaginary[position], imaginary[index]];
    }
  }

  for (let length = 2; length <= size; length <<= 1) {
    const angle = (-2 * Math.PI) / length;
    const stepReal = Math.cos(angle);
    const stepImaginary = Math.sin(angle);
    for (let start = 0; start < size; start += length) {
      let factorReal = 1;
      let factorImaginary = 0;
      for (let offset = 0; offset < length / 2; offset += 1) {
        const a = start + offset;
        const b = a + length / 2;
        const productReal =
          real[b] * factorReal - imaginary[b] * factorImaginary;
        const productImaginary =
          real[b] * factorImaginary + imaginary[b] * factorReal;
        real[b] = real[a] - productReal;
        imaginary[b] = imaginary[a] - productImaginary;
        real[a] += productReal;
        imaginary[a] += productImaginary;
        const nextReal = factorReal * stepReal - factorImaginary * stepImaginary;
        factorImaginary =
          factorReal * stepImaginary + factorImaginary * stepReal;
        factorReal = nextReal;
      }
    }
  }

  const bins = new Float32Array(size / 2);
  for (let index = 0; index < bins.length; index += 1) {
    const magnitude = Math.hypot(real[index], imaginary[index]) / (size / 4);
    bins[index] = decibels(magnitude);
  }
  return bins;
}

function drawSpectrum(canvas) {
  const view = chartContext(canvas);
  const { context, width, height } = view;
  if (state.input === null) return;

  const top = 6;
  const bottom = -90;
  const nyquist = state.sampleRate / 2;
  const minimumHz = 20;
  const positionOf = (hertz) =>
    (Math.log10(Math.max(hertz, minimumHz) / minimumHz) /
      Math.log10(nyquist / minimumHz)) *
    width;

  context.strokeStyle = view.line;
  context.fillStyle = view.ink;
  context.font = "10px ui-monospace, monospace";
  context.lineWidth = 1;
  [-72, -48, -24, 0].forEach((level) => {
    const y = Math.round(((top - level) / (top - bottom)) * height) + 0.5;
    context.beginPath();
    context.moveTo(0, y);
    context.lineTo(width, y);
    context.stroke();
    context.fillText(`${level}`, 4, y < 14 ? y + 8 : y - 6);
  });
  [100, 1000, 10000].forEach((hertz) => {
    if (hertz >= nyquist) return;
    const x = Math.round(positionOf(hertz)) + 0.5;
    context.beginPath();
    context.moveTo(x, 0);
    context.lineTo(x, height);
    context.stroke();
    context.fillText(
      hertz >= 1000 ? `${hertz / 1000}k` : `${hertz}`,
      x + 3,
      height - 8,
    );
  });

  const series = [
    { data: state.input, color: view.seriesIn, alpha: 0.6 },
    { data: state.output, color: view.seriesOut, alpha: 1 },
  ];
  series.forEach(({ data, color, alpha }) => {
    if (!data || data.length === 0) return;
    const bins = spectrumOf(data, state.channels);
    context.globalAlpha = alpha;
    context.strokeStyle = color;
    context.lineWidth = 2;
    context.beginPath();
    let started = false;
    for (let index = 1; index < bins.length; index += 1) {
      const hertz = (index * state.sampleRate) / FFT_SIZE;
      if (hertz < minimumHz) continue;
      const x = positionOf(hertz);
      const clamped = Math.max(bottom, Math.min(top, bins[index]));
      const y = ((top - clamped) / (top - bottom)) * height;
      if (!started) {
        context.moveTo(x, y);
        started = true;
      } else {
        context.lineTo(x, y);
      }
    }
    context.stroke();
    context.globalAlpha = 1;
  });
}

function drawTransfer(canvas) {
  const view = chartContext(canvas);
  const { context, width, height } = view;
  const span = 1.25;
  const toX = (value) => ((value + span) / (2 * span)) * width;
  const toY = (value) => height - ((value + span) / (2 * span)) * height;

  context.strokeStyle = view.line;
  context.lineWidth = 1;
  [-1, 0, 1].forEach((level) => {
    const x = Math.round(toX(level)) + 0.5;
    const y = Math.round(toY(level)) + 0.5;
    context.beginPath();
    context.moveTo(x, 0);
    context.lineTo(x, height);
    context.moveTo(0, y);
    context.lineTo(width, y);
    context.stroke();
  });

  context.setLineDash([4, 4]);
  context.beginPath();
  context.moveTo(toX(-1), toY(-1));
  context.lineTo(toX(1), toY(1));
  context.stroke();
  context.setLineDash([]);

  if (state.rampOutput && state.rampOutput.length > 0) {
    context.strokeStyle = view.seriesOut;
    context.lineWidth = 2;
    context.beginPath();
    for (let frame = 0; frame < RAMP_FRAMES; frame += 1) {
      const input = (frame / (RAMP_FRAMES - 1)) * 2 - 1;
      const output = state.rampOutput[frame * state.channels];
      const x = toX(input);
      const y = toY(Math.max(-span, Math.min(span, output)));
      if (frame === 0) context.moveTo(x, y);
      else context.lineTo(x, y);
    }
    context.stroke();
  }

  context.fillStyle = view.ink;
  context.font = "10px ui-monospace, monospace";
  context.fillText("out +1", 4, 12);
  context.fillText("in +1", width - 34, toY(0) - 6);
  context.fillText("unity", toX(0.42), toY(0.72));
}

function drawAll() {
  drawWaveform(dom.waveform);
  drawSpectrum(dom.spectrum);
  drawTransfer(dom.transfer);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

function statusName(kind, value) {
  const table =
    kind === "program"
      ? state.session?.program_status_names
      : state.session?.effect_status_names;
  return table?.[String(value)] ?? `status ${value}`;
}

function metricRow(label, value, level) {
  const row = document.createElement("tr");
  const name = document.createElement("td");
  const reading = document.createElement("td");
  name.textContent = label;
  reading.textContent = value;
  if (level) reading.dataset.state = level;
  row.append(name, reading);
  return row;
}

function waveformReplacement(input, output) {
  if (!input || !output || input.length !== output.length || input.length < 2) {
    return null;
  }
  const skip = Math.min(
    input.length - 1,
    Math.floor(state.sampleRate * state.channels * 0.5),
  );
  let inputEnergy = 0;
  let outputEnergy = 0;
  let cross = 0;
  for (let index = skip; index < input.length; index += 1) {
    inputEnergy += input[index] * input[index];
    outputEnergy += output[index] * output[index];
    cross += input[index] * output[index];
  }
  if (inputEnergy <= 0 || outputEnergy <= 0) return null;
  const dryScale = cross / inputEnergy;
  let residualEnergy = 0;
  for (let index = skip; index < input.length; index += 1) {
    const residual = output[index] - dryScale * input[index];
    residualEnergy += residual * residual;
  }
  return {
    correlation: cross / Math.sqrt(inputEnergy * outputEnergy),
    residual: Math.sqrt(residualEnergy / outputEnergy),
  };
}

function updateMetrics(report) {
  const body = dom.metrics.querySelector("tbody");
  body.replaceChildren();
  if (!report) {
    body.append(metricRow("Render", "not run yet"));
    return;
  }

  const failures = [
    ["registry", "effect", report.registry_status],
    ["catalog", "program", report.catalog_status],
    ["library", "program", report.library_status],
    ["chain", "effect", report.chain_status],
    ["prepare", "program", report.prepare_status],
    ["parameter", "effect", report.parameter_status],
    ["process", "effect", report.process_status],
  ].filter(([, , value]) => Number(value) !== 0);

  if (failures.length === 0) {
    body.append(metricRow("Runtime validation", "all stages OK", "good"));
  } else {
    failures.forEach(([stage, kind, value]) => {
      body.append(
        metricRow(`${stage} status`, statusName(kind, value), "critical"),
      );
    });
  }

  const deadline = Number(report.deadline_ns) || 1;
  const worst = Number(report.worst_block_ns) || 0;
  const load = (worst / deadline) * 100;
  const peak = Number(report.peak_output) || 0;
  const nonFinite = Number(report.nonfinite_samples) || 0;
  const change =
    decibels(Number(report.rms_output)) - decibels(Number(report.rms_input));

  body.append(
    metricRow(
      "Peak output",
      `${peak.toFixed(3)} (${formatDb(decibels(peak))})`,
      peak > 1.0 ? "warning" : undefined,
    ),
  );
  body.append(metricRow("RMS change", formatDb(change)));
  const replacement = waveformReplacement(state.input, state.output);
  if (replacement !== null) {
    body.append(metricRow(
      "Waveform replacement",
      `${(replacement.residual * 100).toFixed(1)}% non-dry residual · ` +
        `${(replacement.correlation * 100).toFixed(1)}% correlation`,
    ));
  }
  body.append(
    metricRow(
      "Non-finite samples",
      String(nonFinite),
      nonFinite > 0 ? "critical" : "good",
    ),
  );
  body.append(
    metricRow(
      "Worst block (host)",
      `${(worst / 1000).toFixed(1)} µs of ${(deadline / 1000).toFixed(
        1,
      )} µs · ${load.toFixed(1)}%`,
      load > 70 ? "warning" : undefined,
    ),
  );
  body.append(
    metricRow(
      "Context arena",
      `${report.arena_used} of ${report.arena_size} bytes`,
    ),
  );
  body.append(
    metricRow(
      "Chain",
      `${report.instance_count} effect(s) · ${report.blocks} blocks`,
    ),
  );
}

function showDiagnostics(build) {
  const list = dom.diagnostics;
  list.replaceChildren();
  const diagnostics = build?.diagnostics ?? [];
  if (diagnostics.length === 0) {
    const item = document.createElement("li");
    item.className = "empty";
    item.textContent = build?.ok
      ? "Compiled cleanly."
      : build?.log || "No compiler output yet.";
    list.append(item);
    return;
  }
  diagnostics.forEach((diagnostic) => {
    const item = document.createElement("li");
    item.dataset.level = diagnostic.level;
    const where = document.createElement("span");
    where.className = "where";
    where.textContent = `${diagnostic.file}:${diagnostic.line}` +
      (diagnostic.column ? `:${diagnostic.column}` : "");
    item.append(where, document.createTextNode(diagnostic.message));
    list.append(item);
  });
}

function showLint(findings) {
  const list = dom.lint;
  list.replaceChildren();
  if (!findings || findings.length === 0) {
    const item = document.createElement("li");
    item.className = "empty";
    item.textContent = "Nothing flagged by the source scan.";
    list.append(item);
    return;
  }
  findings.forEach((finding) => {
    const item = document.createElement("li");
    item.dataset.level = "advisory";
    const where = document.createElement("span");
    where.className = "where";
    where.textContent = `${finding.file}:${finding.line} · ${finding.rule}`;
    item.append(where, document.createTextNode(finding.message));
    list.append(item);
  });
}

function scheduleRender() {
  if (livePreview.active) {
    scheduleLiveRestart();
    return;
  }
  window.clearTimeout(renderTimer);
  renderTimer = window.setTimeout(() => {
    runRender().catch((error) => setStatus(String(error.message), "error"));
  }, DEBOUNCE_MS);
}

async function runRender() {
  if (!state.session?.compiler_available) {
    setStatus("no host C compiler; previews unavailable", "error");
    return;
  }
  const token = ++renderToken;
  setStatus("compiling and rendering…", "busy");
  if (state.input === null) await prepareInput();

  const main = await renderSignal(state.input, state.inputDigest);
  if (token !== renderToken) return;

  state.build = main.meta.build;
  showDiagnostics(main.meta.build);
  showLint(main.meta.lint);

  if (main.meta.status === "build-failed") {
    state.output = null;
    setStatus("compile failed — see compiler output", "error");
    updateMetrics(null);
    drawAll();
    return;
  }
  if (main.meta.error) {
    setStatus(main.meta.error, "error");
    return;
  }

  /* The harness emits no audio when the runtime refuses the program;
     showing the input as if it were output would hide that. */
  state.output = main.audio.length > 0 ? main.audio : null;
  state.report = main.meta.report;

  const probe = await renderSignal(state.ramp, state.rampDigest);
  if (token !== renderToken) return;
  state.rampOutput = probe.audio.length > 0 ? probe.audio : state.ramp;

  updateMetrics(state.report);
  updateLevelMatchNote();
  drawAll();
  dom.ab.disabled = state.output === null;
  if (state.output === null) stopPlayback();
  else if (state.playing) startPlayback(currentOffset());

  const warnings = (state.build?.diagnostics ?? []).filter(
    (entry) => entry.level === "warning",
  ).length;
  const failed = [
    state.report.chain_status,
    state.report.prepare_status,
    state.report.process_status,
    state.report.parameter_status,
  ].some((value) => Number(value) !== 0);
  setStatus(
    failed
      ? "runtime rejected the program — see measurements"
      : `rendered through ${state.report.instance_count} effect(s)` +
        (warnings ? ` · ${warnings} compiler warning(s)` : ""),
    failed ? "error" : "ok",
  );
}

async function refreshCatalog() {
  const payload = await postJson("/api/build", sessionRequest());
  state.build = payload.build;
  showDiagnostics(payload.build);
  showLint(payload.lint);
  if (payload.catalog) {
    state.catalog = payload.catalog;
    /* An effect can leave the registry when its source is removed or its
       identity edited; a chain must never hold a node the registry has
       no descriptor for. */
    const kept = state.program.nodes.filter((node) => effectFor(node));
    const dropped = state.program.nodes.length - kept.length;
    state.program.nodes = kept;
    renderRegistry();
    renderChain();
    if (dropped > 0) {
      setStatus(
        `${dropped} chain node(s) dropped: no longer in the registry`,
        "busy",
      );
    }
  }
  if (payload.static_only?.length) {
    setStatus(
      `${payload.static_only[0].symbol} is static and cannot be ` +
        "registered; drop the static keyword",
      "error",
    );
  }
  return payload;
}

/* ------------------------------------------------------------------ */
/* Views                                                               */
/* ------------------------------------------------------------------ */

function effectFor(node) {
  return state.catalog.find(
    (effect) =>
      effect.vendor_id === node.vendorId && effect.effect_id === node.effectId,
  );
}

function renderRegistry() {
  dom.registry.replaceChildren();
  state.catalog.forEach((effect) => {
    const card = document.createElement("article");
    card.className = "effect-card shadow-sm transition-shadow hover:shadow-md";

    const header = document.createElement("header");
    const title = document.createElement("h3");
    title.textContent = effect.name;
    header.append(title);
    if (effect.authored || effect.hardware_app) {
      const tag = document.createElement("span");
      tag.className = "tag";
      tag.dataset.kind = effect.hardware_app ? "hardware" : "authored";
      tag.textContent = effect.hardware_app ? "hardware app" : "authored";
      header.append(tag);
    }

    const list = document.createElement("dl");
    [
      ["key", `${formatHex(effect.vendor_id)}:${formatHex(effect.effect_id)}`],
      ["params", String(effect.parameters.length)],
      ["context", `${effect.context_size} B / ${effect.context_alignment}`],
    ].forEach(([term, value]) => {
      const dt = document.createElement("dt");
      const dd = document.createElement("dd");
      dt.textContent = term;
      dd.textContent = value;
      list.append(dt, dd);
    });

    const actions = document.createElement("div");
    actions.className = "effect-actions";
    const add = document.createElement("button");
    add.type = "button";
    add.className = "primary small";
    add.textContent = "Add";
    add.addEventListener("click", () => addNode(effect));
    actions.append(add);

    card.append(header, list, actions);
    dom.registry.append(card);
  });
}

function addNode(effect) {
  if (effect.hardware_app) {
    /* These presets keep their state at file scope, exactly as the
       pedal does, so a chain can only run one of them. */
    const existing = state.program.nodes.find((node) => {
      const other = effectFor(node);
      return other?.hardware_app;
    });
    if (existing) {
      setStatus(
        "the hardware app presets share one set of file-scope state; " +
          "remove the other preset first",
        "error",
      );
      return;
    }
    const required = state.session?.hardware_app?.sample_rate;
    if (required && state.sampleRate !== required) {
      dom.sampleRate.value = String(required);
      changeFormat().catch(() => {});
      setStatus(
        `switched to ${required} Hz: these presets derive every ` +
          "modulation rate from the device sample rate",
        "busy",
      );
    }
  }

  const values = {};
  effect.parameters.forEach((parameter) => {
    values[parameter.parameter_id] = parameter.default_value;
  });
  state.program.nodes.push({
    vendorId: effect.vendor_id,
    effectId: effect.effect_id,
    values,
  });
  renderChain();
  renderEngineBank();
  persistWorkspace();
  scheduleRender();
}

function renderChain() {
  dom.chain.replaceChildren();
  dom.chainEmpty.hidden = state.program.nodes.length > 0;

  state.program.nodes.forEach((node, index) => {
    const effect = effectFor(node);
    const card = document.createElement("article");
    card.className = "node shadow-sm";

    const header = document.createElement("header");
    const title = document.createElement("h3");
    title.textContent = effect ? effect.name : "unknown effect";
    const position = document.createElement("span");
    position.className = "node-index";
    position.textContent = `#${index + 1}`;
    const left = document.createElement("div");
    left.append(position, document.createTextNode(" "), title);
    left.style.display = "flex";
    left.style.gap = "8px";
    left.style.alignItems = "baseline";

    const actions = document.createElement("div");
    actions.className = "node-actions";
    [
      ["↑", () => moveNode(index, -1), index === 0],
      ["↓", () => moveNode(index, 1), index === state.program.nodes.length - 1],
      ["✕", () => removeNode(index), false],
    ].forEach(([label, handler, disabled]) => {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = label;
      button.disabled = disabled;
      button.addEventListener("click", handler);
      actions.append(button);
    });
    header.append(left, actions);
    card.append(header);

    (effect?.parameters ?? []).forEach((parameter) => {
      card.append(parameterControl(node, parameter));
    });
    dom.chain.append(card);
  });
}

function renderEngineBank() {
  document.querySelectorAll("[data-open-engine]").forEach((button) => {
    const index = Number(button.dataset.openEngine);
    const active = index ===
      state.activeOpenEngine;
    button.dataset.active = String(active);
    button.setAttribute("aria-pressed", String(active));
    const name = button.querySelector("span");
    if (name) name.textContent = state.openEngines[index].name;
  });
  dom.effectPositions.replaceChildren();
  state.openEngines[state.activeOpenEngine].effects.forEach(
    (program, position) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className =
        "effect-position transition duration-150 hover:-translate-y-0.5 " +
        "hover:shadow-md";
      button.dataset.active = String(position === state.activeEffectPosition);
      button.dataset.review = normalizeReview(program.review).status;
      button.setAttribute(
        "aria-pressed",
        String(position === state.activeEffectPosition),
      );
      const number = document.createElement("b");
      number.textContent = String(position + 1);
      const name = document.createElement("span");
      name.textContent = program.name;
      const count = document.createElement("em");
      const review = normalizeReview(program.review).status;
      count.textContent = `${review === "unreviewed" ? "unreviewed" : review}` +
        ` · ${program.nodes.length} node${program.nodes.length === 1 ? "" : "s"}`;
      button.append(number, name, count);
      button.addEventListener("click", () => selectOpenProgram(
        state.activeOpenEngine,
        position,
      ));
      dom.effectPositions.append(button);
    },
  );
  renderReview();
}

function renderEnginePackLibrary() {
  const pack = OPTIONAL_ENGINE_PACKS.find(
    (candidate) => candidate.id === dom.enginePack.value,
  ) ?? OPTIONAL_ENGINE_PACKS[0];
  dom.enginePackDetail.textContent =
    `${pack.summary} ${pack.effects.map(([, name]) => name).join(" · ")}`;
}

function loadEnginePack() {
  const pack = OPTIONAL_ENGINE_PACKS.find(
    (candidate) => candidate.id === dom.enginePack.value,
  );
  const engineIndex = Number(dom.enginePackTarget.value);
  if (!pack || !Number.isInteger(engineIndex) ||
      engineIndex < 0 || engineIndex >= state.openEngines.length) {
    setStatus("choose a valid engine pack and open slot", "error");
    return;
  }
  const resolved = pack.effects.map(([effectId, name]) => {
    const effect = state.catalog.find((candidate) =>
      candidate.vendor_id === state.session.vendor_open &&
      candidate.effect_id === effectId);
    if (!effect) {
      throw new Error(`${name} is unavailable in this editor build`);
    }
    return { effect, name };
  });
  const engine = state.openEngines[engineIndex];
  engine.name = pack.name;
  engine.effects = resolved.map(({ effect, name }, position) => ({
    name,
    programId: ((engine.slot & 0xff) << 8) | (position + 1),
    nodes: [{
      vendorId: effect.vendor_id,
      effectId: effect.effect_id,
      values: Object.fromEntries(effect.parameters.map((parameter) => [
        parameter.parameter_id,
        parameter.default_value,
      ])),
    }],
    factoryPresetIndex: engineIndex * 8 + position,
    review: { status: "unreviewed", notes: "" },
  }));
  state.activeOpenEngine = engineIndex;
  state.activeEffectPosition = 0;
  state.program = engine.effects[0];
  /* Instrument Lab must be judged at its raw device gain. The first
     hardware trial sounded deceptively controlled only because browser
     auditioning attenuated its much hotter output. */
  state.levelMatch = false;
  dom.levelMatch.checked = false;
  updateLevelMatchNote();
  query("program-name").value = state.program.name;
  query("program-id").value = formatHex(state.program.programId);
  renderEngineBank();
  renderChain();
  persistWorkspace();
  scheduleRender();
  setStatus(
    `${pack.name} loaded into editor slot ${engine.slot}; existing catalog retained`,
    "ok",
  );
}

function reviewInventory() {
  const counts = { unreviewed: 0, keep: 0, tune: 0, replace: 0 };
  state.openEngines.forEach((engine) => {
    engine.effects.forEach((program) => {
      counts[normalizeReview(program.review).status] += 1;
    });
  });
  return counts;
}

function renderReview() {
  const engine = state.openEngines[state.activeOpenEngine];
  const program = engine.effects[state.activeEffectPosition];
  const review = normalizeReview(program.review);
  program.review = review;
  dom.reviewLocation.textContent =
    `Slot ${engine.slot} · position ${state.activeEffectPosition + 1} · ` +
    program.name;
  dom.reviewNotes.value = review.notes;
  document.querySelectorAll("[data-review-status]").forEach((button) => {
    const active = button.dataset.reviewStatus === review.status;
    button.dataset.active = String(active);
    button.setAttribute("aria-pressed", String(active));
  });
  const counts = reviewInventory();
  dom.reviewSummary.textContent =
    `${counts.keep} keep · ${counts.tune} tune · ` +
    `${counts.replace} replace · ${counts.unreviewed} left`;
}

function setReviewStatus(status) {
  if (!REVIEW_STATUSES.has(status)) return;
  state.program.review = normalizeReview(state.program.review);
  state.program.review.status = status;
  persistWorkspace();
  renderEngineBank();
}

function stepOpenProgram(delta) {
  const total = OPEN_ENGINE_LAYOUT.length * 8;
  const current = state.activeOpenEngine * 8 + state.activeEffectPosition;
  const next = (current + delta + total) % total;
  selectOpenProgram(Math.floor(next / 8), next % 8);
}

function selectOpenProgram(engine, position) {
  state.activeOpenEngine = engine;
  state.activeEffectPosition = position;
  state.program = state.openEngines[engine].effects[position];
  query("program-name").value = state.program.name;
  query("program-id").value = formatHex(state.program.programId);
  renderEngineBank();
  renderChain();
  persistWorkspace();
  scheduleRender();
}

function seedHardwarePreset(program) {
  if (program.nodes.length !== 0) return;
  program.nodes.push({
    vendorId: HARDWARE_PRESET_VENDOR_ID,
    effectId: HARDWARE_PRESET_FIRST_ID + program.factoryPresetIndex,
    values: { 1: 2048, 2: 2048, 3: 4095 },
  });
}

function removeHardwarePresets() {
  state.openEngines.forEach((engine) => {
    engine.effects.forEach((program) => {
      program.nodes = program.nodes.filter((node) => !(
        hardwarePresetNode(node)
      ));
    });
  });
}

function loadFirmwareDefaults() {
  if (!state.session?.hardware_app?.available) {
    setStatus("hardware app presets are unavailable in this checkout", "error");
    return;
  }
  state.openEngines.forEach((engine) => {
    engine.effects.forEach(seedHardwarePreset);
  });
  state.includeHardwareApp = true;
  dom.hardwareApp.checked = true;
  renderEngineBank();
  renderChain();
  persistWorkspace();
  setStatus("loading all 32 firmware defaults…", "busy");
  compileSources();
}

function exportEngineBank() {
  downloadJson("ncr2-open-engine-bank.json", engineBankDocument());
  setStatus("exported four engines, reviews, notes, and control settings", "ok");
}

async function importEngineBank(file) {
  const document = JSON.parse(await file.text());
  applyEngineBankDocument(document);
  persistWorkspace();
  state.activeSource = 0;
  dom.hardwareApp.checked = state.includeHardwareApp;
  dom.signal.value = state.signal;
  dom.levelMatch.checked = state.levelMatch;
  updateInputLevelControl();
  query("program-name").value = state.program.name;
  query("program-id").value = formatHex(state.program.programId);
  renderEngineBank();
  renderChain();
  renderSourceList();
  renderVisualDesigner();
  await prepareInput();
  await compileSources();
  setStatus(`imported ${file.name}`, "ok");
}

function updateDfuReady() {
  dom.dfuInstall.disabled = !(
    state.dfu &&
    state.dfuInfo &&
    dom.dfuFile.files?.length === 1 &&
    dom.dfuAck.checked
  );
}

async function connectDfu() {
  if (!dom.dfuAck.checked) {
    throw new Error("confirm that the pedal is running Open Recover first");
  }
  dom.webhidState.textContent = "Connecting…";
  const dfu = await OpenRecoveryDfu.request();
  const info = await dfu.getInfo();
  state.dfu = dfu;
  state.dfuInfo = info;
  const slotName = info.confirmedSlot === 0 ? "A" :
    info.confirmedSlot === 1 ? "B" : "invalid";
  dom.webhidState.textContent = `Open Recover · confirmed ${slotName}`;
  dom.dfuDetail.textContent =
    `Device reports ${Math.round(info.slotSize / 1024)} KiB A/B slots. ` +
    "The installer will select the inactive slot automatically.";
  updateDfuReady();
}

async function installDfu() {
  const file = dom.dfuFile.files?.[0];
  if (!file || !state.dfu || !state.dfuInfo || !dom.dfuAck.checked) {
    throw new Error("connect Open Recover and choose a slot image first");
  }
  dom.dfuInstall.disabled = true;
  const bytes = new Uint8Array(await file.arrayBuffer());
  const target = await state.dfu.install(bytes, state.dfuInfo, (
    phase,
    complete,
    total,
  ) => {
    dom.dfuProgress.value = total > 0 ? complete / total : 0;
    dom.dfuDetail.textContent = phase === "write"
      ? `Writing ${Math.round(complete / 1024)} / ${Math.round(total / 1024)} KiB…`
      : `${phase[0].toUpperCase()}${phase.slice(1)}…`;
  });
  dom.dfuProgress.value = 1;
  dom.webhidState.textContent = "Rebooting into trial image";
  dom.dfuDetail.textContent =
    `Installed to application slot ${target === 0 ? "A" : "B"}; ` +
    "the bootloader will roll back if the trial does not confirm.";
  state.dfu.close();
  state.dfu = null;
  state.dfuInfo = null;
}

function parameterControl(node, parameter) {
  const wrapper = document.createElement("div");
  wrapper.className = "parameter";

  /*
   * A visual design keeps its effect identity while its block schema changes.
   * During the registry refresh an already-loaded node can therefore see a
   * newly added parameter before buildVisualEffect replaces the node values.
   * Materialize the descriptor default instead of formatting `undefined`.
   */
  const stored = Number(node.values[parameter.parameter_id]);
  node.values[parameter.parameter_id] = Number.isFinite(stored)
    ? stored
    : parameter.default_value;

  const label = document.createElement("span");
  label.className = "parameter-label";
  label.textContent = parameter.name;

  const value = document.createElement("span");
  value.className = "parameter-value";

  const slider = document.createElement("input");
  slider.type = "range";
  slider.min = String(parameter.minimum);
  slider.max = String(parameter.maximum);
  slider.step = String((parameter.maximum - parameter.minimum) / 500 || 0.001);
  slider.value = String(node.values[parameter.parameter_id]);
  slider.setAttribute("aria-label", parameter.name);

  const show = () => {
    const current = node.values[parameter.parameter_id];
    value.textContent = `${Number(current).toFixed(3)} ${parameter.unit}`;
  };
  show();

  slider.addEventListener("input", () => {
    node.values[parameter.parameter_id] = Number(slider.value);
    show();
    persistWorkspace();
    scheduleRender();
  });

  const range = document.createElement("span");
  range.className = "parameter-range";
  const low = document.createElement("span");
  const high = document.createElement("span");
  low.textContent = formatParameter(parameter.minimum);
  high.textContent = formatParameter(parameter.maximum);
  range.append(low, high);

  wrapper.append(label, value, slider, range);
  return wrapper;
}

function moveNode(index, direction) {
  const target = index + direction;
  const nodes = state.program.nodes;
  if (target < 0 || target >= nodes.length) return;
  [nodes[index], nodes[target]] = [nodes[target], nodes[index]];
  renderChain();
  persistWorkspace();
  scheduleRender();
}

function removeNode(index) {
  state.program.nodes.splice(index, 1);
  renderChain();
  renderEngineBank();
  persistWorkspace();
  scheduleRender();
}

/* ------------------------------------------------------------------ */
/* Visual effect designer                                              */
/* ------------------------------------------------------------------ */

function visualKey() {
  if (typeof crypto.randomUUID === "function") return crypto.randomUUID();
  return `visual-${Date.now()}-${Math.random().toString(16).slice(2)}`;
}

function normalizeVisualSpec(raw) {
  const effectId = Number(raw?.effect_id ?? raw?.effectId);
  const blocks = Array.isArray(raw?.blocks) ? raw.blocks : [];
  return {
    editor_key: typeof raw?.editor_key === "string"
      ? raw.editor_key
      : visualKey(),
    name: typeof raw?.name === "string" && raw.name.trim()
      ? raw.name.trim().slice(0, 48)
      : "My Visual Effect",
    effect_id: Number.isInteger(effectId) ? effectId : 0x2001,
    blocks: blocks.map((block) => ({
      kind: String(block?.kind ?? ""),
      values: Object.fromEntries(
        Object.entries(block?.values ?? {})
          .filter(([, value]) => Number.isFinite(Number(value)))
          .map(([key, value]) => [key, Number(value)]),
      ),
    })),
  };
}

function visualDefinition(kind) {
  return state.visualCatalog?.blocks.find((block) => block.kind === kind);
}

function nextVisualEffectId() {
  const used = new Set([
    ...state.catalog.map((effect) => effect.effect_id),
    ...state.visualEffects.map((effect) => effect.effect_id),
  ]);
  let effectId = 0x2001;
  while (used.has(effectId)) effectId += 1;
  return effectId;
}

function makeVisualEffect() {
  return {
    editor_key: visualKey(),
    name: `Visual Effect ${state.visualEffects.length + 1}`,
    effect_id: nextVisualEffectId(),
    blocks: [],
  };
}

function currentVisualEffect() {
  if (state.visualEffects.length === 0) {
    state.visualEffects.push(makeVisualEffect());
    state.activeVisualEffect = 0;
  }
  return state.visualEffects[state.activeVisualEffect];
}

function visualBlockValues(definition, overrides = {}) {
  return Object.fromEntries(definition.controls.map((control) => [
    control.key,
    Number(overrides[control.key] ?? control.default),
  ]));
}

function addVisualBlock(kind) {
  const spec = currentVisualEffect();
  const definition = visualDefinition(kind);
  if (!definition) return;
  const maximum = state.visualCatalog?.limits?.maximum_blocks ?? 10;
  if (spec.blocks.length >= maximum) {
    setStatus(`visual effects are limited to ${maximum} blocks`, "error");
    return;
  }
  const count = spec.blocks.filter((block) => block.kind === kind).length;
  if (count >= Number(definition.limit ?? maximum)) {
    setStatus(`${definition.name} may appear only once`, "error");
    return;
  }
  spec.blocks.push({
    kind,
    values: visualBlockValues(definition),
  });
  persistWorkspace();
  renderVisualDesigner();
}

function useVisualRecipe(recipe) {
  const spec = currentVisualEffect();
  spec.name = recipe.name;
  spec.blocks = recipe.blocks.map((block) => {
    const definition = visualDefinition(block.kind);
    return {
      kind: block.kind,
      values: visualBlockValues(definition, block.values),
    };
  });
  persistWorkspace();
  renderVisualDesigner();
  setStatus(`${recipe.name} loaded — shape it, then build + audition`, "ok");
}

function moveVisualBlock(index, direction) {
  const blocks = currentVisualEffect().blocks;
  const target = index + direction;
  if (target < 0 || target >= blocks.length) return;
  [blocks[index], blocks[target]] = [blocks[target], blocks[index]];
  persistWorkspace();
  renderVisualDesigner();
}

function removeVisualBlock(index) {
  currentVisualEffect().blocks.splice(index, 1);
  persistWorkspace();
  renderVisualDesigner();
}

function visualSliderValue(control, value) {
  if (control.scale !== "log") return value;
  const low = Math.log(control.minimum);
  const high = Math.log(control.maximum);
  return Math.round(1000 * (Math.log(value) - low) / (high - low));
}

function visualControlValue(control, sliderValue) {
  if (control.scale !== "log") return Number(sliderValue);
  const low = Math.log(control.minimum);
  const high = Math.log(control.maximum);
  const raw = Math.exp(low + (high - low) * Number(sliderValue) / 1000);
  return Math.round(raw / control.step) * control.step;
}

function visualValueText(control, value) {
  const digits = control.step < 0.01 ? 3 : control.step < 1 ? 2 : 0;
  return `${Number(value).toFixed(digits)} ${control.unit}`;
}

function renderVisualPalette() {
  dom.visualPalette.replaceChildren();
  const spec = currentVisualEffect();
  const maximum = state.visualCatalog?.limits?.maximum_blocks ?? 10;
  const categories = new Map();
  (state.visualCatalog?.blocks ?? []).forEach((definition) => {
    if (!categories.has(definition.category)) {
      categories.set(definition.category, []);
    }
    categories.get(definition.category).push(definition);
  });
  categories.forEach((definitions, category) => {
    const section = document.createElement("section");
    section.className = "palette-category";
    const title = document.createElement("h4");
    title.textContent = category;
    const options = document.createElement("div");
    options.className = "palette-options";
    definitions.forEach((definition) => {
      const button = document.createElement("button");
      button.type = "button";
      button.className = "palette-block";
      const used = spec.blocks.filter(
        (block) => block.kind === definition.kind,
      ).length;
      button.disabled = spec.blocks.length >= maximum ||
        used >= Number(definition.limit ?? maximum);
      button.title = definition.detail;
      const name = document.createElement("b");
      name.textContent = definition.name;
      const summary = document.createElement("span");
      summary.textContent = definition.summary;
      button.append(name, summary);
      button.addEventListener("click", () => addVisualBlock(definition.kind));
      options.append(button);
    });
    section.append(title, options);
    dom.visualPalette.append(section);
  });
}

function renderVisualRecipes() {
  dom.visualRecipes.replaceChildren();
  (state.visualCatalog?.recipes ?? []).forEach((recipe) => {
    const button = document.createElement("button");
    button.type = "button";
    button.className = "recipe-card";
    const name = document.createElement("b");
    name.textContent = recipe.name;
    const summary = document.createElement("span");
    summary.textContent = recipe.summary;
    button.append(name, summary);
    button.addEventListener("click", () => useVisualRecipe(recipe));
    dom.visualRecipes.append(button);
  });
}

function renderVisualControl(block, definition, control) {
  const wrapper = document.createElement("div");
  wrapper.className = "visual-control";
  const label = document.createElement("label");
  label.textContent = control.name;
  const output = document.createElement("output");
  const slider = document.createElement("input");
  slider.type = "range";
  slider.setAttribute("aria-label", `${definition.name}: ${control.name}`);
  if (control.scale === "log") {
    slider.min = "0";
    slider.max = "1000";
    slider.step = "1";
  } else {
    slider.min = String(control.minimum);
    slider.max = String(control.maximum);
    slider.step = String(control.step);
  }
  const value = Number(block.values[control.key] ?? control.default);
  block.values[control.key] = value;
  slider.value = String(visualSliderValue(control, value));
  output.value = visualValueText(control, value);
  slider.addEventListener("input", () => {
    const next = visualControlValue(control, slider.value);
    block.values[control.key] = next;
    output.value = visualValueText(control, next);
    persistWorkspace();
  });
  wrapper.append(label, output, slider);
  return wrapper;
}

function renderVisualDesigner() {
  if (!state.visualCatalog) return;
  const spec = currentVisualEffect();
  dom.visualSelect.replaceChildren();
  state.visualEffects.forEach((effect, index) => {
    const option = document.createElement("option");
    option.value = String(index);
    option.textContent = effect.name;
    dom.visualSelect.append(option);
  });
  dom.visualSelect.value = String(state.activeVisualEffect);
  dom.visualName.value = spec.name;
  dom.visualEffectId.value = formatHex(spec.effect_id);

  const maximum = state.visualCatalog.limits.maximum_blocks;
  dom.visualBudget.textContent = `${spec.blocks.length} / ${maximum} blocks`;
  const costs = spec.blocks.map(
    (block) => visualDefinition(block.kind)?.cost,
  ).filter(Boolean);
  dom.visualCost.textContent = costs.length
    ? `Estimated pieces: ${costs.join(" · ")}`
    : "Add a block or choose a recipe.";
  dom.visualEmpty.hidden = spec.blocks.length > 0;
  dom.visualChain.replaceChildren();

  spec.blocks.forEach((block, index) => {
    const definition = visualDefinition(block.kind);
    if (!definition) return;
    const card = document.createElement("article");
    card.className = "visual-block";
    const header = document.createElement("div");
    header.className = "visual-block-head";
    const title = document.createElement("div");
    title.className = "visual-block-title";
    const name = document.createElement("b");
    name.textContent = `${index + 1} · ${definition.name}`;
    const detail = document.createElement("span");
    detail.textContent = definition.detail;
    title.append(name, detail);
    const actions = document.createElement("div");
    actions.className = "visual-block-actions";
    [
      ["↑", () => moveVisualBlock(index, -1), index === 0],
      ["↓", () => moveVisualBlock(index, 1), index === spec.blocks.length - 1],
      ["✕", () => removeVisualBlock(index), false],
    ].forEach(([text, handler, disabled]) => {
      const button = document.createElement("button");
      button.type = "button";
      button.textContent = text;
      button.disabled = disabled;
      button.addEventListener("click", handler);
      actions.append(button);
    });
    header.append(title, actions);
    const controls = document.createElement("div");
    controls.className = "visual-controls";
    definition.controls.forEach((control) => {
      controls.append(renderVisualControl(block, definition, control));
    });
    card.append(header, controls);
    dom.visualChain.append(card);
  });
  renderVisualRecipes();
  renderVisualPalette();
}

async function buildVisualEffect() {
  const spec = currentVisualEffect();
  if (state.sources.length > 0 && !dom.source.disabled) {
    state.sources[state.activeSource].text = dom.source.value;
  }
  setStatus(`generating ${spec.name}…`, "busy");
  const generated = await postJson("/api/visual-source", {
    name: spec.name,
    effect_id: spec.effect_id,
    blocks: spec.blocks,
  });
  const owned = state.sources.findIndex(
    (source) => source.visualKey === spec.editor_key,
  );
  const collision = state.sources.findIndex(
    (source, index) => source.name === generated.file_name && index !== owned,
  );
  if (collision >= 0) {
    throw new Error("another authored effect uses that filename; rename this design");
  }
  const source = {
    name: generated.file_name,
    text: generated.text,
    visualKey: spec.editor_key,
  };
  if (owned >= 0) state.sources[owned] = source;
  else state.sources.push(source);
  state.activeSource = owned >= 0 ? owned : state.sources.length - 1;
  renderSourceList();
  persistWorkspace();

  const payload = await refreshCatalog();
  if (!payload.build.ok) {
    setStatus("generated source did not compile — see advanced output", "error");
    return;
  }
  const effect = state.catalog.find((candidate) =>
    candidate.vendor_id === state.session.vendor_open &&
    candidate.effect_id === generated.effect_id);
  if (!effect) throw new Error("compiled visual effect is missing from registry");
  const values = Object.fromEntries(effect.parameters.map((parameter) => [
    parameter.parameter_id,
    parameter.default_value,
  ]));
  state.program.name = generated.name;
  state.program.nodes = [{
    vendorId: effect.vendor_id,
    effectId: effect.effect_id,
    values,
  }];
  query("program-name").value = state.program.name;
  renderChain();
  renderEngineBank();
  persistWorkspace();
  setStatus(`${generated.name} compiled and loaded into this program`, "ok");
  scheduleRender();
  query("preview-title").scrollIntoView({ behavior: "smooth", block: "start" });
}

function renderSourceList() {
  dom.sourceSelect.replaceChildren();
  state.sources.forEach((source, index) => {
    const option = document.createElement("option");
    option.value = String(index);
    option.textContent = source.name;
    dom.sourceSelect.append(option);
  });
  const empty = state.sources.length === 0;
  dom.sourceSelect.disabled = empty;
  dom.deleteSource.disabled = empty;
  dom.source.disabled = empty;
  if (empty) {
    dom.source.value =
      "// No authored effect yet.\n" +
      "// Press “New effect” to start one from the ABI template.\n";
    return;
  }
  dom.sourceSelect.value = String(state.activeSource);
  dom.source.value = state.sources[state.activeSource].text;
}

async function newEffect() {
  const name = window.prompt("Effect name", "My Effect");
  if (!name) return;
  const used = state.catalog.map((effect) => effect.effect_id);
  let effectId = 0x1001;
  while (used.includes(effectId)) effectId += 1;

  const template = await getJson(
    `/api/template?name=${encodeURIComponent(name)}&effect_id=${effectId}`,
  );
  state.sources.push({ name: template.file_name, text: template.text });
  state.activeSource = state.sources.length - 1;
  persistWorkspace();
  renderSourceList();
  setStatus(`created ${template.file_name}`, "ok");
  await compileSources();
}

async function compileSources() {
  if (state.sources.length > 0) {
    state.sources[state.activeSource].text = dom.source.value;
  }
  persistWorkspace();
  setStatus("compiling…", "busy");
  try {
    const payload = await refreshCatalog();
    if (payload.build.ok) {
      setStatus("compiled", "ok");
      scheduleRender();
    } else {
      setStatus("compile failed — see compiler output", "error");
    }
  } catch (error) {
    setStatus(String(error.message), "error");
  }
}

async function runVerify() {
  setStatus("validating program descriptor…", "busy");
  try {
    const payload = await postJson("/api/verify", sessionRequest());
    showDiagnostics(payload.build);
    showLint(payload.lint);
    if (!payload.build.ok) {
      setStatus("compile failed — see compiler output", "error");
      return;
    }
    const report = payload.verify?.report ?? {};
    updateMetrics(report);
    const failures = [
      ["catalog", "program", report.catalog_status],
      ["library", "program", report.library_status],
      ["chain", "effect", report.chain_status],
      ["prepare", "program", report.prepare_status],
    ].filter(([, , value]) => Number(value) !== 0);
    setStatus(
      failures.length === 0
        ? "program descriptor validates on the firmware path"
        : `${failures[0][0]}: ${statusName(failures[0][1], failures[0][2])}`,
      failures.length === 0 ? "ok" : "error",
    );
  } catch (error) {
    setStatus(String(error.message), "error");
  }
}

async function exportSources() {
  try {
    const payload = await postJson("/api/export", {
      ...sessionRequest(),
      catalog: state.catalog,
    });
    dom.exportFiles.replaceChildren();
    payload.files.forEach((file) => {
      const block = document.createElement("div");
      block.className = "export-file";
      const header = document.createElement("header");
      const title = document.createElement("h3");
      title.textContent = file.path;
      const copy = document.createElement("button");
      copy.type = "button";
      copy.className = "ghost small";
      copy.textContent = "Copy";
      copy.addEventListener("click", (event) => {
        event.preventDefault();
        navigator.clipboard.writeText(file.text);
        copy.textContent = "Copied";
        window.setTimeout(() => (copy.textContent = "Copy"), 1200);
      });
      header.append(title, copy);
      const pre = document.createElement("pre");
      pre.textContent = file.text;
      block.append(header, pre);
      dom.exportFiles.append(block);
    });
    dom.exportDialog.showModal();
  } catch (error) {
    setStatus(String(error.message), "error");
  }
}

/* ------------------------------------------------------------------ */
/* Wiring                                                              */
/* ------------------------------------------------------------------ */

function fillSelect(select, values, selected) {
  select.replaceChildren();
  values.forEach((value) => {
    const option = document.createElement("option");
    option.value = String(value);
    option.textContent = String(value);
    option.selected = value === selected;
    select.append(option);
  });
}

async function changeFormat() {
  state.sampleRate = Number(dom.sampleRate.value);
  state.blockFrames = Number(dom.blockFrames.value);
  state.channels = Number(dom.channels.value);
  if (playback.context) {
    stopPlayback();
    await playback.context.close();
    playback.context = null;
  }
  if (state.guitarBytes !== null) await loadBundledGuitar();
  await prepareInput();
  scheduleRender();
}

async function changeSignal() {
  state.signal = dom.signal.value;
  persistWorkspace();
  if (state.signal === "file") {
    dom.fileInput.click();
    return;
  }
  if (state.signal === "mic") {
    if (state.userAudio === null) {
      setStatus("press Record input to capture a take", "ok");
      return;
    }
  }
  await prepareInput();
  scheduleRender();
}

async function recordInput() {
  const seconds = Number(dom.recordSeconds.value);
  if (![6, 15, 30, 60].includes(seconds)) return;
  if (livePreview.active) await stopLivePreview();
  dom.recordInput.disabled = true;
  dom.recordInput.textContent = "Recording…";
  setStatus(`recording ${seconds} s from the raw instrument input…`, "busy");
  try {
    state.userAudio = await captureMicrophone(seconds);
    state.signal = "mic";
    dom.signal.value = "mic";
    persistWorkspace();
    await prepareInput();
    scheduleRender();
    setStatus(
      `captured ${seconds} s from ${selectedAudioInputLabel()}`,
      "ok",
    );
  } catch (error) {
    setStatus(`audio input unavailable: ${error.message}`, "error");
  } finally {
    dom.recordInput.disabled = false;
    dom.recordInput.textContent = "Record input";
  }
}

function bind() {
  dom.statusText = query("status-text");
  dom.statusDot = document.querySelector(".status-dot");
  dom.registry = query("registry");
  dom.chain = query("chain");
  dom.chainEmpty = query("chain-empty");
  dom.waveform = query("waveform");
  dom.spectrum = query("spectrum");
  dom.transfer = query("transfer");
  dom.metrics = query("metrics");
  dom.diagnostics = query("diagnostics");
  dom.lint = query("lint");
  dom.source = query("source");
  dom.sourceSelect = query("source-select");
  dom.deleteSource = query("delete-source");
  dom.play = query("play");
  dom.ab = query("ab");
  dom.liveInput = query("live-input");
  dom.liveInputState = query("live-input-state");
  dom.recordInput = query("record-input");
  dom.recordSeconds = query("record-seconds");
  dom.signal = query("signal");
  dom.audioInputDevice = query("audio-input-device");
  dom.fileInput = query("file-input");
  dom.sampleRate = query("sample-rate");
  dom.blockFrames = query("block-frames");
  dom.channels = query("channels");
  dom.exportDialog = query("export-dialog");
  dom.exportFiles = query("export-files");
  dom.hardwareApp = query("hardware-app");
  dom.effectPositions = query("effect-positions");
  dom.reviewLocation = query("review-location");
  dom.reviewNotes = query("review-notes");
  dom.reviewSummary = query("review-summary");
  dom.bankFile = query("bank-file");
  dom.levelMatch = query("level-match");
  dom.levelMatchNote = query("level-match-note");
  dom.webhidState = query("webhid-state");
  dom.dfuFile = query("dfu-file");
  dom.dfuAck = query("dfu-ack");
  dom.dfuInstall = query("dfu-install");
  dom.dfuProgress = query("dfu-progress");
  dom.dfuDetail = query("dfu-detail");
  dom.enginePack = query("engine-pack-select");
  dom.enginePackTarget = query("engine-pack-target");
  dom.enginePackDetail = query("engine-pack-detail");
  dom.visualSelect = query("visual-effect-select");
  dom.visualName = query("visual-name");
  dom.visualEffectId = query("visual-effect-id");
  dom.visualBudget = query("visual-budget");
  dom.visualCost = query("visual-cost");
  dom.visualRecipes = query("visual-recipes");
  dom.visualPalette = query("visual-palette");
  dom.visualChain = query("visual-chain");
  dom.visualEmpty = query("visual-empty");

  document.querySelectorAll("[data-open-engine]").forEach((button) => {
    button.addEventListener("click", () => selectOpenProgram(
      Number(button.dataset.openEngine),
      state.activeEffectPosition,
    ));
  });
  query("load-defaults").addEventListener("click", loadFirmwareDefaults);
  dom.enginePack.addEventListener("change", renderEnginePackLibrary);
  query("load-engine-pack").addEventListener("click", () => {
    try {
      loadEnginePack();
    } catch (error) {
      setStatus(error.message, "error");
    }
  });
  query("import-bank").addEventListener("click", () => dom.bankFile.click());
  query("export-bank").addEventListener("click", exportEngineBank);
  dom.bankFile.addEventListener("change", () => {
    const file = dom.bankFile.files?.[0];
    if (!file) return;
    importEngineBank(file).catch((error) => {
      setStatus(`could not import bank: ${error.message}`, "error");
    }).finally(() => {
      dom.bankFile.value = "";
    });
  });
  query("previous-effect").addEventListener("click", () => stepOpenProgram(-1));
  query("next-effect").addEventListener("click", () => stepOpenProgram(1));
  document.querySelectorAll("[data-review-status]").forEach((button) => {
    button.addEventListener("click", () =>
      setReviewStatus(button.dataset.reviewStatus));
  });
  dom.reviewNotes.addEventListener("input", () => {
    state.program.review = normalizeReview(state.program.review);
    state.program.review.notes = dom.reviewNotes.value;
    persistWorkspace();
  });
  query("dfu-connect").addEventListener("click", () => {
    connectDfu().catch((error) => {
      dom.webhidState.textContent = "Disconnected";
      setStatus(error.message, "error");
    });
  });
  dom.dfuFile.addEventListener("change", updateDfuReady);
  dom.dfuAck.addEventListener("change", updateDfuReady);
  dom.dfuInstall.addEventListener("click", () => {
    installDfu().catch((error) => {
      setStatus(error.message, "error");
      dom.dfuDetail.textContent = `Install stopped: ${error.message}`;
      updateDfuReady();
    });
  });

  query("open-visual-designer").addEventListener("click", () => {
    query("visual-designer").scrollIntoView({
      behavior: "smooth",
      block: "start",
    });
  });
  query("new-visual-effect").addEventListener("click", () => {
    state.visualEffects.push(makeVisualEffect());
    state.activeVisualEffect = state.visualEffects.length - 1;
    persistWorkspace();
    renderVisualDesigner();
    dom.visualName.focus();
    dom.visualName.select();
  });
  dom.visualSelect.addEventListener("change", () => {
    state.activeVisualEffect = Number(dom.visualSelect.value);
    persistWorkspace();
    renderVisualDesigner();
  });
  dom.visualName.addEventListener("input", () => {
    currentVisualEffect().name = dom.visualName.value.slice(0, 48);
    const option = dom.visualSelect.options[state.activeVisualEffect];
    if (option) option.textContent = dom.visualName.value || "Untitled design";
    persistWorkspace();
  });
  dom.visualEffectId.addEventListener("change", () => {
    const spec = currentVisualEffect();
    spec.effect_id = parseNumber(dom.visualEffectId.value, spec.effect_id);
    dom.visualEffectId.value = formatHex(spec.effect_id);
    persistWorkspace();
  });
  query("build-visual-effect").addEventListener("click", () => {
    buildVisualEffect().catch((error) => setStatus(error.message, "error"));
  });
  query("toggle-source").addEventListener("click", (event) => {
    const drawer = query("source-drawer");
    drawer.hidden = !drawer.hidden;
    event.currentTarget.textContent = drawer.hidden
      ? "Advanced: show C"
      : "Advanced: hide C";
    if (!drawer.hidden) drawer.scrollIntoView({ behavior: "smooth" });
  });

  query("new-effect").addEventListener("click", () => {
    newEffect().catch((error) => setStatus(error.message, "error"));
  });
  query("compile").addEventListener("click", () => compileSources());
  query("verify").addEventListener("click", () => runVerify());
  query("export").addEventListener("click", () => exportSources());

  dom.deleteSource.addEventListener("click", () => {
    if (state.sources.length === 0) return;
    state.sources.splice(state.activeSource, 1);
    state.activeSource = Math.max(0, state.activeSource - 1);
    renderSourceList();
    persistWorkspace();
    compileSources();
  });

  dom.sourceSelect.addEventListener("change", () => {
    state.sources[state.activeSource].text = dom.source.value;
    state.activeSource = Number(dom.sourceSelect.value);
    dom.source.value = state.sources[state.activeSource].text;
  });

  dom.source.addEventListener("input", () => {
    if (state.sources.length > 0) {
      state.sources[state.activeSource].text = dom.source.value;
      persistWorkspace();
    }
  });

  dom.play.addEventListener("click", () => {
    if (state.playing) stopPlayback();
    else startPlayback(0);
  });

  dom.liveInput.addEventListener("click", () => {
    if (livePreview.active) {
      stopLivePreview().then(() => {
        setStatus("live input stopped", "ok");
        scheduleRender();
      });
    } else {
      startLivePreview().catch((error) => {
        setStatus(`live input unavailable: ${error.message}`, "error");
        updateLivePreviewState(`Could not start: ${error.message}`);
      });
    }
  });

  dom.recordInput.addEventListener("click", () => {
    recordInput().catch((error) => {
      setStatus(`could not record input: ${error.message}`, "error");
    });
  });

  dom.ab.addEventListener("click", () => {
    state.monitor = state.monitor === "wet" ? "dry" : "wet";
    applyMonitor();
  });

  dom.levelMatch.addEventListener("change", () => {
    state.levelMatch = dom.levelMatch.checked;
    persistWorkspace();
    applyMonitor();
  });

  dom.signal.addEventListener("change", () => {
    changeSignal().catch((error) => setStatus(error.message, "error"));
  });

  dom.audioInputDevice.addEventListener("change", () => {
    state.audioInputDeviceId = dom.audioInputDevice.value;
    persistWorkspace();
    if (livePreview.active) {
      stopLivePreview().then(() => startLivePreview()).catch((error) => {
        setStatus(`could not switch audio input: ${error.message}`, "error");
      });
    }
  });

  query("refresh-audio-inputs").addEventListener("click", () => {
    refreshAudioInputDevices().then(() => {
      setStatus(`audio input: ${selectedAudioInputLabel()}`, "ok");
    }).catch((error) => {
      setStatus(`could not list audio inputs: ${error.message}`, "error");
    });
  });

  dom.fileInput.addEventListener("change", async (event) => {
    const file = event.target.files?.[0];
    if (!file) return;
    setStatus(`decoding ${file.name}…`, "busy");
    try {
      state.userAudio = await decodeUserAudio(await file.arrayBuffer());
      await prepareInput();
      setStatus(`loaded ${file.name}`, "ok");
      scheduleRender();
    } catch (error) {
      setStatus(`could not decode ${file.name}`, "error");
    }
  });

  query("input-level").addEventListener("input", (event) => {
    state.inputLevelDb = Number(event.target.value);
    updateInputLevelControl();
    persistWorkspace();
    prepareInput().then(scheduleRender);
  });

  [dom.sampleRate, dom.blockFrames, dom.channels].forEach((select) => {
    select.addEventListener("change", () => {
      changeFormat().catch((error) => setStatus(error.message, "error"));
    });
  });

  query("program-name").addEventListener("input", (event) => {
    state.program.name = event.target.value || "Untitled";
    persistWorkspace();
    renderEngineBank();
  });
  query("program-id").addEventListener("change", (event) => {
    state.program.programId = parseNumber(event.target.value, 1);
    event.target.value = formatHex(state.program.programId);
    persistWorkspace();
  });

  dom.hardwareApp.addEventListener("change", () => {
    state.includeHardwareApp = dom.hardwareApp.checked;
    if (state.includeHardwareApp) {
      state.openEngines.forEach((engine) => {
        engine.effects.forEach(seedHardwarePreset);
      });
      renderEngineBank();
      renderChain();
    } else {
      removeHardwarePresets();
      renderEngineBank();
      renderChain();
    }
    persistWorkspace();
    setStatus(
      state.includeHardwareApp
        ? "extracting the hardware app presets…"
        : "rebuilding without the hardware app presets",
      "busy",
    );
    compileSources();
  });

  query("theme-toggle").addEventListener("click", () => {
    const root = document.documentElement;
    const dark = root.getAttribute("data-theme") === "dark";
    root.setAttribute("data-theme", dark ? "light" : "dark");
    drawAll();
  });

  window.addEventListener("beforeunload", () => {
    if (!livePreview.token) return;
    fetch("/api/live/stop", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ token: livePreview.token }),
      keepalive: true,
    }).catch(() => {});
  });

  document.addEventListener("keydown", (event) => {
    const typing = ["INPUT", "TEXTAREA", "SELECT", "BUTTON"].includes(
      document.activeElement?.tagName,
    );
    if (typing) return;
    if (event.code === "Space") {
      event.preventDefault();
      if (state.playing) stopPlayback();
      else startPlayback(0);
    }
    if (event.key.toLowerCase() === "b" && !dom.ab.disabled) {
      state.monitor = state.monitor === "wet" ? "dry" : "wet";
      applyMonitor();
    }
    if (event.key === "[") stepOpenProgram(-1);
    if (event.key === "]") stepOpenProgram(1);
    if (event.key.toLowerCase() === "k") setReviewStatus("keep");
    if (event.key.toLowerCase() === "t") setReviewStatus("tune");
    if (event.key.toLowerCase() === "r") setReviewStatus("replace");
  });

  new ResizeObserver(() => drawAll()).observe(document.body);
  window
    .matchMedia("(prefers-color-scheme: dark)")
    .addEventListener("change", () => drawAll());
}

async function start() {
  const restored = restoreWorkspace();
  bind();
  dom.signal.value = state.signal;
  dom.levelMatch.checked = state.levelMatch;
  updateInputLevelControl();
  query("program-name").value = state.program.name;
  renderEngineBank();
  OPTIONAL_ENGINE_PACKS.forEach((pack) => {
    const option = document.createElement("option");
    option.value = pack.id;
    option.textContent = pack.name;
    dom.enginePack.append(option);
  });
  renderEnginePackLibrary();
  renderSourceList();
  updateMetrics(null);
  updateLevelMatchNote();
  updateLivePreviewState();
  await refreshAudioInputDevices().catch(() => {});

  navigator.mediaDevices?.addEventListener?.("devicechange", () => {
    refreshAudioInputDevices().catch(() => {});
  });

  try {
    [state.session, state.visualCatalog] = await Promise.all([
      getJson("/api/session"),
      getJson("/api/visual-effects"),
    ]);
  } catch (error) {
    setStatus("cannot reach the editor server", "error");
    return;
  }
  state.pedalAudio = state.session.pedal_audio;
  await refreshAudioInputDevices(state.audioInputDeviceId).catch(() => {});
  renderVisualDesigner();
  fillSelect(dom.sampleRate, state.session.sample_rates, state.sampleRate);
  fillSelect(dom.blockFrames, state.session.block_frames, state.blockFrames);
  query("program-id").value = formatHex(state.program.programId);

  const hardware = state.session.hardware_app;
  if (hardware?.available) {
    query("hardware-app-toggle").hidden = false;
    query("hardware-app-note").textContent =
      `${hardware.presets.length} fixed-point presets lifted from ` +
      `${hardware.source} at ${hardware.sample_rate} Hz`;
    if (!restored) {
      state.openEngines.forEach((engine) => {
        engine.effects.forEach(seedHardwarePreset);
      });
      state.includeHardwareApp = true;
      renderEngineBank();
      renderChain();
      persistWorkspace();
    }
  }
  dom.hardwareApp.checked = state.includeHardwareApp;

  if (!state.session.compiler_available) {
    setStatus("no host C compiler found; previews unavailable", "error");
    return;
  }

  try {
    await refreshCatalog();
    await loadBundledGuitar();
    await prepareInput();
    setStatus(
      restored
        ? "review workspace restored · real clean guitar loaded"
        : "real clean guitar loaded · choose an effect to begin reviewing",
      "ok",
    );
    drawAll();
    scheduleRender();
  } catch (error) {
    setStatus(String(error.message), "error");
  }
}

start();
