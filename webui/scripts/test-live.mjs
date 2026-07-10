import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const eventSources = [];
const intervals = new Map();
const requests = [];
let nextInterval = 1;

globalThis.window = {};
globalThis.location = {
  protocol: 'http:', hostname: '192.168.1.242', search: '',
};
Object.defineProperty(globalThis, 'navigator', {
  configurable: true, value: { bluetooth: null },
});
globalThis.setInterval = (fn, delay) => {
  const id = nextInterval++;
  intervals.set(id, { fn, delay });
  return id;
};
globalThis.clearInterval = (id) => intervals.delete(id);
globalThis.fetch = async (url) => {
  requests.push(url);
  return {
    ok: true,
    status: 200,
    headers: { get: () => 'application/json' },
    json: async () => ({ source: 'poll' }),
  };
};
globalThis.EventSource = class FakeEventSource {
  constructor(url) {
    this.url = url;
    this.closed = false;
    eventSources.push(this);
  }

  close() { this.closed = true; }
};

const { default: RewairAPI } = await import('../rw-api.js');
const statuses = [];
const firstStop = RewairAPI.subscribe((status) => statuses.push(status));

assert.equal(eventSources.length, 1);
assert.equal(eventSources[0].url, '/api/events');
assert.equal(requests.length, 0, 'SSE provides the initial status without an extra HTTP poll');
eventSources[0].onmessage({ data: '{"source":"sse"}' });
assert.deepEqual(statuses, [{ source: 'sse' }]);

const secondStop = RewairAPI.subscribe(() => {});
assert.equal(eventSources.length, 2);
assert.equal(eventSources[0].closed, true, 'a replacement subscription closes the prior stream');
assert.equal(eventSources[1].closed, false);
firstStop();
assert.equal(eventSources[1].closed, false, 'stale cleanup cannot close the active stream');
secondStop();
assert.equal(eventSources[1].closed, true);

const fallbackStop = RewairAPI.subscribe((status) => statuses.push(status));
const fallbackSource = eventSources.at(-1);
fallbackSource.onerror();
fallbackSource.onerror();
fallbackSource.onerror();
await Promise.resolve();
assert.equal(requests.length, 1, 'polling starts only after repeated SSE failures');
assert.equal(intervals.size, 1);
assert.equal(fallbackSource.closed, true, 'failed SSE stops retrying once polling takes over');
fallbackStop();
assert.equal(intervals.size, 0);

const settingsSource = await readFile(new URL('../rw-settings.js', import.meta.url), 'utf8');
const mqttEffectStart = settingsSource.indexOf('useEffect(() => {', settingsSource.indexOf('const [mqtt, setMqtt]'));
const mqttEffectEnd = settingsSource.indexOf('const tzSub', mqttEffectStart);
const mqttEffect = settingsSource.slice(mqttEffectStart, mqttEffectEnd);
assert.match(mqttEffect, /RewairAPI\.mqtt\(\)/);
assert.doesNotMatch(mqttEffect, /setInterval/, 'MQTT configuration loads once per Settings mount');

console.log('test_live OK');
