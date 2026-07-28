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

const state = {
  session: null,
  sources: [],
  activeSource: 0,
  catalog: [],
  program: { name: "Untitled", programId: 1, nodes: [] },
  sampleRate: 48000,
  blockFrames: 64,
  channels: 2,
  signal: "pluck",
  inputLevelDb: -6,
  input: null,
  inputDigest: null,
  output: null,
  ramp: null,
  rampDigest: null,
  rampOutput: null,
  report: null,
  build: null,
  monitor: "wet",
  playing: false,
  userAudio: null,
  includeHardwareApp: false,
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

async function postRender(request, payload) {
  const header = new TextEncoder().encode(JSON.stringify(request));
  const body = new Uint8Array(4 + header.length + (payload?.byteLength ?? 0));
  new DataView(body.buffer).setUint32(0, header.length, true);
  body.set(header, 4);
  if (payload) body.set(new Uint8Array(payload), 4 + header.length);

  const response = await fetch("/api/render", {
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
  const mono =
    captured && state.userAudio !== null
      ? state.userAudio
      : normalize(generateSignal(captured ? "pluck" : state.signal));
  const gain = Math.pow(10, state.inputLevelDb / 20);
  state.input = interleave(mono, state.channels, gain);
  state.inputDigest = await digestOf(state.input.buffer);
  state.ramp = buildRamp();
  state.rampDigest = await digestOf(state.ramp.buffer);
}

async function decodeUserAudio(arrayBuffer) {
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
  return normalize(mono);
}

async function captureMicrophone(seconds = 2.0) {
  const stream = await navigator.mediaDevices.getUserMedia({ audio: true });
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
        const mono = new Float32Array(captured);
        let offset = 0;
        for (const chunk of chunks) {
          mono.set(chunk, offset);
          offset += chunk.length;
        }
        resolve(normalize(mono));
      }
    };
    source.connect(capture);
    capture.connect(context.destination);
  });
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
    gain.gain.value = state.monitor === name ? 1 : 0;
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
    gain.gain.value = state.monitor === name ? 1 : 0;
  });
  dom.ab.textContent =
    state.monitor === "wet" ? "Hear dry" : "Hear processed";
}

/* ------------------------------------------------------------------ */
/* Charts                                                              */
/* ------------------------------------------------------------------ */

function chartContext(canvas) {
  const ratio = window.devicePixelRatio || 1;
  const width = canvas.clientWidth;
  const height = Number(canvas.getAttribute("height"));
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
    card.className = "effect-card";

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
  scheduleRender();
}

function renderChain() {
  dom.chain.replaceChildren();
  dom.chainEmpty.hidden = state.program.nodes.length > 0;

  state.program.nodes.forEach((node, index) => {
    const effect = effectFor(node);
    const card = document.createElement("article");
    card.className = "node";

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

function parameterControl(node, parameter) {
  const wrapper = document.createElement("div");
  wrapper.className = "parameter";

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
    value.textContent = `${current.toFixed(3)} ${parameter.unit}`;
  };
  show();

  slider.addEventListener("input", () => {
    node.values[parameter.parameter_id] = Number(slider.value);
    show();
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
  scheduleRender();
}

function removeNode(index) {
  state.program.nodes.splice(index, 1);
  renderChain();
  scheduleRender();
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
  renderSourceList();
  setStatus(`created ${template.file_name}`, "ok");
  await compileSources();
}

async function compileSources() {
  if (state.sources.length > 0) {
    state.sources[state.activeSource].text = dom.source.value;
  }
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
  await prepareInput();
  scheduleRender();
}

async function changeSignal() {
  state.signal = dom.signal.value;
  if (state.signal === "file") {
    dom.fileInput.click();
    return;
  }
  if (state.signal === "mic") {
    setStatus("recording 2 s from the microphone…", "busy");
    try {
      state.userAudio = await captureMicrophone();
      setStatus("captured microphone input", "ok");
    } catch (error) {
      setStatus(`microphone unavailable: ${error.message}`, "error");
      return;
    }
  }
  await prepareInput();
  scheduleRender();
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
  dom.signal = query("signal");
  dom.fileInput = query("file-input");
  dom.sampleRate = query("sample-rate");
  dom.blockFrames = query("block-frames");
  dom.channels = query("channels");
  dom.exportDialog = query("export-dialog");
  dom.exportFiles = query("export-files");
  dom.hardwareApp = query("hardware-app");

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
    }
  });

  dom.play.addEventListener("click", () => {
    if (state.playing) stopPlayback();
    else startPlayback(0);
  });

  dom.ab.addEventListener("click", () => {
    state.monitor = state.monitor === "wet" ? "dry" : "wet";
    applyMonitor();
  });

  dom.signal.addEventListener("change", () => {
    changeSignal().catch((error) => setStatus(error.message, "error"));
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
    query("input-level-value").textContent =
      `${state.inputLevelDb >= 0 ? "" : "−"}` +
      `${Math.abs(state.inputLevelDb)} dB`;
    prepareInput().then(scheduleRender);
  });

  [dom.sampleRate, dom.blockFrames, dom.channels].forEach((select) => {
    select.addEventListener("change", () => {
      changeFormat().catch((error) => setStatus(error.message, "error"));
    });
  });

  query("program-name").addEventListener("input", (event) => {
    state.program.name = event.target.value || "Untitled";
  });
  query("program-id").addEventListener("change", (event) => {
    state.program.programId = parseNumber(event.target.value, 1);
    event.target.value = formatHex(state.program.programId);
  });

  dom.hardwareApp.addEventListener("change", () => {
    state.includeHardwareApp = dom.hardwareApp.checked;
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

  document.addEventListener("keydown", (event) => {
    const typing = ["INPUT", "TEXTAREA", "SELECT"].includes(
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
  });

  new ResizeObserver(() => drawAll()).observe(document.body);
  window
    .matchMedia("(prefers-color-scheme: dark)")
    .addEventListener("change", () => drawAll());
}

async function start() {
  bind();
  renderSourceList();
  updateMetrics(null);

  try {
    state.session = await getJson("/api/session");
  } catch (error) {
    setStatus("cannot reach the editor server", "error");
    return;
  }
  fillSelect(dom.sampleRate, state.session.sample_rates, state.sampleRate);
  fillSelect(dom.blockFrames, state.session.block_frames, state.blockFrames);
  query("program-id").value = formatHex(state.program.programId);

  const hardware = state.session.hardware_app;
  if (hardware?.available) {
    query("hardware-app-toggle").hidden = false;
    query("hardware-app-note").textContent =
      `${hardware.presets.length} fixed-point presets lifted from ` +
      `${hardware.source} at ${hardware.sample_rate} Hz`;
  }

  if (!state.session.compiler_available) {
    setStatus("no host C compiler found; previews unavailable", "error");
    return;
  }

  try {
    await refreshCatalog();
    await prepareInput();
    setStatus("registry loaded — add an effect to hear it", "ok");
    drawAll();
    scheduleRender();
  } catch (error) {
    setStatus(String(error.message), "error");
  }
}

start();
