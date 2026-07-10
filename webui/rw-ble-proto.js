export const BLE_PROTOCOL_VERSION = 1;
export const BLE_PAYLOAD_MAX = 32;
export const BLE_WIRE_MAX = 50;

export const BLE_FRAME = Object.freeze({ REQUEST: 1, RESPONSE: 2, EVENT: 3, ACK: 4 });
export const BLE_FLAG = Object.freeze({ FIRST: 0x01, MORE: 0x02 });
export const BLE_OP = Object.freeze({
  CAPABILITIES: 1,
  STATUS: 2,
  SCAN: 3,
  NETWORKS: 4,
  JOIN: 5,
  FORGET: 6,
  PRIORITY: 7,
  SETTINGS: 8,
  TIME: 9,
  DISPLAY: 10,
  RESET: 11,
  UPDATE: 12,
});

function crc32(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc = (crc ^ byte) >>> 0;
    for (let bit = 0; bit < 8; bit += 1) {
      crc = ((crc >>> 1) ^ ((crc & 1) ? 0xedb88320 : 0)) >>> 0;
    }
  }
  return (crc ^ 0xffffffff) >>> 0;
}

function cobsEncode(input) {
  const output = new Uint8Array(input.length + Math.ceil(input.length / 254) + 1);
  let read = 0;
  let write = 1;
  let codeIndex = 0;
  let code = 1;

  while (read < input.length) {
    if (input[read] === 0) {
      output[codeIndex] = code;
      code = 1;
      codeIndex = write;
      write += 1;
      read += 1;
    } else {
      output[write] = input[read];
      write += 1;
      read += 1;
      code += 1;
      if (code === 0xff) {
        output[codeIndex] = code;
        code = 1;
        codeIndex = write;
        write += 1;
      }
    }
  }
  output[codeIndex] = code;
  return output.slice(0, write);
}

function cobsDecode(input) {
  const output = new Uint8Array(input.length);
  let read = 0;
  let write = 0;

  while (read < input.length) {
    const code = input[read];
    read += 1;
    if (code === 0 || read + code - 1 > input.length) throw new Error('bad COBS frame');
    for (let i = 1; i < code; i += 1) output[write++] = input[read++];
    if (code !== 0xff && read < input.length) output[write++] = 0;
  }
  return output.slice(0, write);
}

export function encodeBleFrame(frame) {
  const payload = frame.payload || new Uint8Array();
  if (payload.length > BLE_PAYLOAD_MAX) throw new Error('BLE frame payload too large');

  const raw = new Uint8Array(12 + payload.length + 4);
  const view = new DataView(raw.buffer);
  raw[0] = BLE_PROTOCOL_VERSION;
  raw[1] = frame.type;
  raw[2] = frame.flags || 0;
  raw[3] = frame.operation;
  view.setUint16(4, frame.requestId, true);
  view.setUint16(6, frame.sequence || 0, true);
  view.setUint16(8, frame.status || 0, true);
  view.setUint16(10, payload.length, true);
  raw.set(payload, 12);
  view.setUint32(12 + payload.length, crc32(raw.subarray(0, 12 + payload.length)), true);

  const encoded = cobsEncode(raw);
  const wire = new Uint8Array(encoded.length + 1);
  wire.set(encoded);
  return wire;
}

export function decodeBleFrame(encoded) {
  const raw = cobsDecode(encoded);
  if (raw.length < 16 || raw[0] !== BLE_PROTOCOL_VERSION) throw new Error('bad BLE frame header');
  const view = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
  const payloadLength = view.getUint16(10, true);
  if (payloadLength > BLE_PAYLOAD_MAX || raw.length !== 12 + payloadLength + 4) {
    throw new Error('bad BLE frame length');
  }
  const expected = view.getUint32(12 + payloadLength, true);
  const actual = crc32(raw.subarray(0, 12 + payloadLength));
  if (actual !== expected) throw new Error('bad BLE frame CRC');
  return {
    type: raw[1],
    flags: raw[2],
    operation: raw[3],
    requestId: view.getUint16(4, true),
    sequence: view.getUint16(6, true),
    status: view.getUint16(8, true),
    payload: raw.slice(12, 12 + payloadLength),
  };
}
