import {
  BLE_FLAG, BLE_FRAME, BLE_OP, BLE_PAYLOAD_MAX, BLE_WIRE_MAX,
  decodeBleFrame, encodeBleFrame,
} from './rw-ble-proto.js';

const SERVICE_UUID = '2f2dfff0-2e85-649d-3545-3586428f5da3';
const NOTIFY_UUID = '2f2dfff4-2e85-649d-3545-3586428f5da3';
const WRITE_UUID = '2f2dfff5-2e85-649d-3545-3586428f5da3';
const GATT_CHUNK = 20;
const REQUEST_BODY_MAX = 1024;
const REQUEST_TIMEOUT_MS = 30000;

const ROUTES = new Map([
  ['/api/capabilities', BLE_OP.CAPABILITIES],
  ['/api/status', BLE_OP.STATUS],
  ['/api/scan', BLE_OP.SCAN],
  ['/api/networks', BLE_OP.NETWORKS],
]);

function concat(chunks) {
  const size = chunks.reduce((sum, chunk) => sum + chunk.length, 0);
  const joined = new Uint8Array(size);
  let offset = 0;
  for (const chunk of chunks) {
    joined.set(chunk, offset);
    offset += chunk.length;
  }
  return joined;
}

export class RewairBleTransport {
  constructor() {
    this.kind = 'ble';
    this.device = null;
    this.server = null;
    this.notify = null;
    this.write = null;
    this.nextRequestId = 1;
    this.pending = new Map();
    this.rxEncoded = [];
    this.writeChain = Promise.resolve();
  }

  get connected() { return !!(this.device && this.device.gatt && this.device.gatt.connected); }

  async connect() {
    if (!navigator.bluetooth) throw new Error('Web Bluetooth is not available in this browser');
    this.device = await navigator.bluetooth.requestDevice({ filters: [{ services: [SERVICE_UUID] }] });
    this.device.addEventListener('gattserverdisconnected', () => this.handleDisconnect());
    this.server = await this.device.gatt.connect();
    const service = await this.server.getPrimaryService(SERVICE_UUID);
    this.notify = await service.getCharacteristic(NOTIFY_UUID);
    this.write = await service.getCharacteristic(WRITE_UUID);
    this.notify.addEventListener('characteristicvaluechanged', (event) => this.handleNotification(event));
    await this.notify.startNotifications();
    /* CoreBluetooth can resolve startNotifications before this CC2540
     * firmware has actually committed the CCCD change. Avoid losing the
     * first response on a freshly established connection. */
    await new Promise((resolve) => setTimeout(resolve, 1000));
    return this.request('/api/capabilities');
  }

  disconnect() {
    if (this.device && this.device.gatt && this.device.gatt.connected) this.device.gatt.disconnect();
    this.handleDisconnect();
  }

  handleDisconnect() {
    for (const pending of this.pending.values()) {
      clearTimeout(pending.timer);
      pending.reject(new Error('Bluetooth device disconnected'));
    }
    this.pending.clear();
    this.rxEncoded = [];
  }

  handleNotification(event) {
    const value = event.target.value;
    const bytes = new Uint8Array(value.buffer, value.byteOffset, value.byteLength);
    for (const byte of bytes) {
      if (byte === 0) {
        if (this.rxEncoded.length) {
          try { this.handleFrame(decodeBleFrame(Uint8Array.from(this.rxEncoded))); }
          catch (error) { /* delimiter resynchronizes the next frame */ }
        }
        this.rxEncoded = [];
      } else if (this.rxEncoded.length < BLE_WIRE_MAX) {
        this.rxEncoded.push(byte);
      } else {
        this.rxEncoded = [];
      }
    }
  }

  handleFrame(frame) {
    if (frame.type !== BLE_FRAME.RESPONSE) return;
    const pending = this.pending.get(frame.requestId);
    if (!pending || frame.operation !== pending.operation) return;

    if (frame.flags & BLE_FLAG.FIRST) {
      pending.sequence = 0;
      pending.chunks = [];
      pending.status = frame.status;
    }
    if (frame.sequence !== pending.sequence || frame.status !== pending.status) {
      clearTimeout(pending.timer);
      this.pending.delete(frame.requestId);
      pending.reject(new Error('Bluetooth response fragment mismatch'));
      return;
    }
    pending.sequence += 1;
    pending.chunks.push(frame.payload);
    if (frame.flags & BLE_FLAG.MORE) {
      this.writeFrame({
        type: BLE_FRAME.ACK,
        flags: 0,
        operation: frame.operation,
        requestId: frame.requestId,
        sequence: frame.sequence,
        status: 0,
        payload: new Uint8Array(),
      }).catch((error) => {
        clearTimeout(pending.timer);
        this.pending.delete(frame.requestId);
        pending.reject(error);
      });
      return;
    }

    clearTimeout(pending.timer);
    this.pending.delete(frame.requestId);
    const text = new TextDecoder().decode(concat(pending.chunks));
    let body = null;
    if (text) {
      try { body = JSON.parse(text); }
      catch (error) { pending.reject(new Error('Device returned invalid JSON')); return; }
    }
    if (pending.status < 200 || pending.status >= 300) {
      const error = new Error((body && body.error) || `Device error ${pending.status}`);
      error.status = pending.status;
      pending.reject(error);
      return;
    }
    pending.resolve(body);
  }

  async writeFrameNow(frame) {
    const wire = encodeBleFrame(frame);
    for (let offset = 0; offset < wire.length; offset += GATT_CHUNK) {
      const chunk = wire.slice(offset, Math.min(offset + GATT_CHUNK, wire.length));
      if (this.write.writeValueWithResponse) await this.write.writeValueWithResponse(chunk);
      else await this.write.writeValue(chunk);
    }
  }

  writeFrame(frame) {
    const write = this.writeChain.then(() => this.writeFrameNow(frame));
    this.writeChain = write.catch(() => {});
    return write;
  }

  async request(path, options = {}) {
    if (!this.connected || !this.write) throw new Error('Bluetooth device is not connected');
    const operation = ROUTES.get(path);
    if (!operation) throw new Error(`${path} is not available over Bluetooth yet`);
    const method = options.method || 'GET';
    if (method !== 'GET') throw new Error(`${path} is read-only over Bluetooth`);

    const payload = options.body == null ? new Uint8Array() : new TextEncoder().encode(options.body);
    if (payload.length > REQUEST_BODY_MAX) throw new Error('Bluetooth request body is too large');
    const requestId = this.nextRequestId;
    this.nextRequestId = (this.nextRequestId % 0xffff) + 1;

    const response = new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(requestId);
        reject(new Error('Bluetooth request timed out'));
      }, REQUEST_TIMEOUT_MS);
      this.pending.set(requestId, { operation, resolve, reject, timer, sequence: 0, status: 0, chunks: [] });
    });

    try {
      let offset = 0;
      let sequence = 0;
      do {
        const take = Math.min(BLE_PAYLOAD_MAX, payload.length - offset);
        let flags = sequence === 0 ? BLE_FLAG.FIRST : 0;
        if (offset + take < payload.length) flags |= BLE_FLAG.MORE;
        await this.writeFrame({
          type: BLE_FRAME.REQUEST, flags, operation, requestId, sequence,
          status: 0, payload: payload.slice(offset, offset + take),
        });
        offset += take;
        sequence += 1;
      } while (offset < payload.length);
    } catch (error) {
      const pending = this.pending.get(requestId);
      if (pending) clearTimeout(pending.timer);
      this.pending.delete(requestId);
      throw error;
    }

    return response;
  }
}
