import assert from 'node:assert/strict';
import {
  BLE_FLAG, BLE_FRAME, BLE_PAYLOAD_MAX, decodeBleFrame, encodeBleFrame,
} from '../rw-ble-proto.js';

const payload = Uint8Array.from({ length: BLE_PAYLOAD_MAX }, (_, i) => i & 0xff);
const wire = encodeBleFrame({
  type: BLE_FRAME.RESPONSE,
  flags: BLE_FLAG.FIRST | BLE_FLAG.MORE,
  operation: 2,
  requestId: 0x1234,
  sequence: 7,
  status: 200,
  payload,
});
assert.equal(wire.at(-1), 0);
const decoded = decodeBleFrame(wire.slice(0, -1));
assert.equal(decoded.requestId, 0x1234);
assert.equal(decoded.sequence, 7);
assert.equal(decoded.status, 200);
assert.deepEqual(decoded.payload, payload);

const corrupt = wire.slice(0, -1);
corrupt[Math.floor(corrupt.length / 2)] ^= 0x40;
assert.throws(() => decodeBleFrame(corrupt));

console.log('test_ble OK');
