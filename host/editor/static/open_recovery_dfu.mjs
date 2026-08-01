/* Browser transport for the bounded Open Recover A/B update protocol. */

export const PACKET_MAGIC = 0x5846584e;
export const PROTOCOL_VERSION = 2;
export const PACKET_SIZE = 64;
export const PAYLOAD_SIZE = 32;
export const OPEN_RECOVERY_VENDOR_ID = 0x9527;
export const OPEN_RECOVERY_PRODUCT_ID = 0xc157;

export const COMMAND = Object.freeze({
  GET_INFO: 1,
  BEGIN_IMAGE: 2,
  ERASE_SLOT: 3,
  WRITE_CHUNK: 4,
  FINALIZE_IMAGE: 6,
  SET_PENDING: 7,
  REBOOT: 8,
});

const STATUS = Object.freeze([
  "ok", "bad magic", "bad version", "bad command", "bad length",
  "bad header CRC", "bad payload CRC", "bad session", "bad sequence",
  "bad slot", "range denied", "backend error", "invalid state",
  "image invalid", "active slot", "not finalized", "bad flags",
  "write order", "full flash disabled",
]);

const crcTable = (() => {
  const table = new Uint32Array(256);
  for (let byte = 0; byte < 256; byte += 1) {
    let value = byte;
    for (let bit = 0; bit < 8; bit += 1) {
      value = (value & 1) !== 0
        ? (value >>> 1) ^ 0xedb88320
        : value >>> 1;
    }
    table[byte] = value >>> 0;
  }
  return table;
})();

export function crc32(bytes) {
  let value = 0xffffffff;
  for (const byte of bytes) {
    value = crcTable[(value ^ byte) & 0xff] ^ (value >>> 8);
  }
  return (value ^ 0xffffffff) >>> 0;
}

export function encodePacket({
  command,
  flags = 0,
  session = 0,
  sequence = 0,
  offset = 0,
  status = 0,
  payload = new Uint8Array(0),
}) {
  const body = payload instanceof Uint8Array
    ? payload
    : new Uint8Array(payload);
  if (body.byteLength > PAYLOAD_SIZE) {
    throw new Error(`recovery payload exceeds ${PAYLOAD_SIZE} bytes`);
  }
  const packet = new Uint8Array(PACKET_SIZE);
  const view = new DataView(packet.buffer);
  view.setUint32(0x00, PACKET_MAGIC, true);
  view.setUint8(0x04, PROTOCOL_VERSION);
  view.setUint8(0x05, command);
  view.setUint16(0x06, flags, true);
  view.setUint32(0x08, session, true);
  view.setUint32(0x0c, sequence, true);
  view.setUint32(0x10, offset, true);
  view.setUint16(0x14, body.byteLength, true);
  view.setUint16(0x16, status, true);
  view.setUint32(0x18, crc32(body), true);
  packet.set(body, 0x20);
  view.setUint32(0x1c, crc32(packet.subarray(0, 0x1c)), true);
  return packet;
}

export function decodePacket(data) {
  const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
  if (bytes.byteLength !== PACKET_SIZE) {
    throw new Error(`recovery report is ${bytes.byteLength} bytes`);
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  const length = view.getUint16(0x14, true);
  if (view.getUint32(0x00, true) !== PACKET_MAGIC) {
    throw new Error("device did not answer with Open Recover packet magic");
  }
  if (view.getUint8(0x04) !== PROTOCOL_VERSION) {
    throw new Error("device uses an incompatible recovery protocol");
  }
  if (length > PAYLOAD_SIZE) throw new Error("bad recovery payload length");
  if (view.getUint32(0x1c, true) !== crc32(bytes.subarray(0, 0x1c))) {
    throw new Error("bad recovery response header CRC");
  }
  const payload = bytes.slice(0x20, 0x20 + length);
  if (view.getUint32(0x18, true) !== crc32(payload)) {
    throw new Error("bad recovery response payload CRC");
  }
  return {
    command: view.getUint8(0x05),
    flags: view.getUint16(0x06, true),
    session: view.getUint32(0x08, true),
    sequence: view.getUint32(0x0c, true),
    offset: view.getUint32(0x10, true),
    status: view.getUint16(0x16, true),
    payload,
  };
}

export function decodeInfo(payload) {
  if (payload.byteLength !== 32) throw new Error("GET_INFO payload is invalid");
  const view = new DataView(
    payload.buffer,
    payload.byteOffset,
    payload.byteLength,
  );
  return {
    flashSize: view.getUint32(0, true),
    slotSize: view.getUint32(4, true),
    slotAOffset: view.getUint32(8, true),
    slotBOffset: view.getUint32(12, true),
    manifestSize: view.getUint32(16, true),
    confirmedSlot: view.getUint8(20),
    pendingSlot: view.getUint8(21),
    selectedSlot: view.getUint8(22),
    updatePhase: view.getUint8(23),
    capabilities: view.getUint32(24, true),
    maxChunkSize: view.getUint32(28, true),
  };
}

export async function validateSlotImage(data, info) {
  const bytes = data instanceof Uint8Array ? data : new Uint8Array(data);
  if (bytes.byteLength < info.manifestSize + 8 ||
      bytes.byteLength > info.slotSize) {
    throw new Error("slot image size is outside the device's A/B partition");
  }
  const view = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (view.getUint32(0, true) !== 0x4e45504f) {
    throw new Error("not an NCR-2 open slot image (bad OPEN manifest magic)");
  }
  if (view.getUint16(4, true) !== 1 ||
      view.getUint16(6, true) !== info.manifestSize) {
    throw new Error("slot manifest version or header size is incompatible");
  }
  const imageSize = view.getUint32(8, true);
  if (info.manifestSize + imageSize !== bytes.byteLength) {
    throw new Error("slot file length does not match its manifest");
  }
  if (view.getUint32(20, true) !== 0x3252434e) {
    throw new Error("slot image is for a different board");
  }
  const headerCrc = view.getUint32(64, true);
  if (headerCrc !== crc32(bytes.subarray(0, 64))) {
    throw new Error("slot manifest CRC is invalid");
  }
  const payload = bytes.subarray(info.manifestSize);
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", payload));
  const expected = bytes.subarray(32, 64);
  if (!digest.every((byte, index) => byte === expected[index])) {
    throw new Error("slot payload SHA-256 does not match its manifest");
  }
  return { imageSize, semanticVersion: view.getUint32(24, true) };
}

export class OpenRecoveryDfu {
  constructor(device, timeoutMs = 30000) {
    this.device = device;
    this.timeoutMs = timeoutMs;
    this.session = 0;
    this.sequence = 0;
    this.flags = 0;
    this.pending = null;
    this.onInputReport = this.onInputReport.bind(this);
  }

  static supported() {
    return typeof navigator !== "undefined" && "hid" in navigator;
  }

  static async request() {
    if (!this.supported()) {
      throw new Error("WebHID is unavailable; use Chrome or Edge on localhost");
    }
    const devices = await navigator.hid.requestDevice({
      filters: [{
        vendorId: OPEN_RECOVERY_VENDOR_ID,
        productId: OPEN_RECOVERY_PRODUCT_ID,
      }],
    });
    if (devices.length !== 1) throw new Error("no recovery device selected");
    const device = devices[0];
    if (!String(device.productName).includes("Open Recover")) {
      throw new Error("selected device is not identified as Open Recover");
    }
    const client = new OpenRecoveryDfu(device);
    await client.open();
    return client;
  }

  async open() {
    if (!this.device.opened) await this.device.open();
    this.device.addEventListener("inputreport", this.onInputReport);
  }

  close() {
    this.device.removeEventListener("inputreport", this.onInputReport);
    if (this.pending) {
      clearTimeout(this.pending.timer);
      this.pending.reject(new Error("recovery device closed"));
      this.pending = null;
    }
  }

  onInputReport(event) {
    if (!this.pending) return;
    const bytes = new Uint8Array(
      event.data.buffer,
      event.data.byteOffset,
      event.data.byteLength,
    );
    try {
      const response = decodePacket(bytes);
      const pending = this.pending;
      if (response.command !== pending.command ||
          response.sequence !== pending.sequence) return;
      this.pending = null;
      clearTimeout(pending.timer);
      pending.resolve(response);
    } catch (error) {
      const pending = this.pending;
      this.pending = null;
      clearTimeout(pending.timer);
      pending.reject(error);
    }
  }

  async exchange(request, begin = false) {
    if (this.pending) throw new Error("another recovery request is pending");
    const packet = encodePacket(request);
    const responsePromise = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending = null;
        reject(new Error("timed out waiting for Open Recover"));
      }, this.timeoutMs);
      this.pending = {
        resolve,
        reject,
        timer,
        command: request.command,
        sequence: request.sequence ?? 0,
      };
    });
    try {
      await this.device.sendReport(0, packet);
    } catch (error) {
      if (this.pending) {
        clearTimeout(this.pending.timer);
        this.pending = null;
      }
      throw error;
    }
    const response = await responsePromise;
    if (response.flags !== (request.flags ?? 0)) {
      throw new Error("recovery response slot does not match request");
    }
    if (!begin && response.session !== (request.session ?? 0)) {
      throw new Error("recovery response session does not match request");
    }
    if (response.status !== 0) {
      throw new Error(
        `recovery command ${request.command} failed: ` +
        `${STATUS[response.status] ?? `status ${response.status}`}`,
      );
    }
    return response;
  }

  async getInfo() {
    const response = await this.exchange({ command: COMMAND.GET_INFO });
    return decodeInfo(response.payload);
  }

  async begin(slot, size) {
    const response = await this.exchange({
      command: COMMAND.BEGIN_IMAGE,
      flags: slot,
      offset: size,
    }, true);
    if (response.session === 0) throw new Error("device returned zero session");
    this.flags = slot;
    this.session = response.session;
    this.sequence = 1;
  }

  async sessionCommand(command, offset = 0, payload = new Uint8Array(0)) {
    const response = await this.exchange({
      command,
      flags: this.flags,
      session: this.session,
      sequence: this.sequence,
      offset,
      payload,
    });
    this.sequence += 1;
    return response;
  }

  async install(bytes, info, progress = () => {}) {
    const target = info.confirmedSlot === 0 ? 1 :
      info.confirmedSlot === 1 ? 0 : -1;
    if (target < 0) throw new Error("device reports no valid confirmed slot");
    if (target === info.selectedSlot) {
      throw new Error("inactive slot is currently selected; boot or roll back first");
    }
    await validateSlotImage(bytes, info);
    progress("begin", 0, bytes.byteLength);
    await this.begin(target, bytes.byteLength);
    progress("erase", 0, bytes.byteLength);
    await this.sessionCommand(COMMAND.ERASE_SLOT);
    const chunkSize = Math.min(PAYLOAD_SIZE, info.maxChunkSize || PAYLOAD_SIZE);
    for (let offset = 0; offset < bytes.byteLength; offset += chunkSize) {
      const chunk = bytes.slice(offset, offset + chunkSize);
      await this.sessionCommand(COMMAND.WRITE_CHUNK, offset, chunk);
      progress("write", offset + chunk.byteLength, bytes.byteLength);
    }
    progress("verify", bytes.byteLength, bytes.byteLength);
    await this.sessionCommand(COMMAND.FINALIZE_IMAGE);
    await this.sessionCommand(COMMAND.SET_PENDING);
    progress("reboot", bytes.byteLength, bytes.byteLength);
    try {
      await this.sessionCommand(COMMAND.REBOOT);
    } catch (error) {
      if (this.device.opened &&
          !["NetworkError", "NotFoundError"].includes(error?.name)) {
        throw error;
      }
    }
    return target;
  }
}
