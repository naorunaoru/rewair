import assert from 'node:assert/strict';
import { RW } from '../rw-lib.js';

const realNow = Date.now;
let now = 100000;
Date.now = () => now;

try {
  const manual = { settings: { time_mode: 'manual', tz_offset: 0 } };

  RW.setEpoch(1000);
  assert.equal(RW.deviceNow(manual).getTime(), 1000000);

  now += 5000;
  RW.setEpoch(1000);
  assert.equal(RW.deviceNow(manual).getTime(), 1005000,
    'a repeated status epoch must not reset the running clock');

  RW.setEpoch(1010);
  assert.equal(RW.deviceNow(manual).getTime(), 1010000,
    'a changed device epoch must replace the local clock anchor');

  RW.setEpoch(null);
  assert.equal(RW.deviceNow(manual), null);
} finally {
  Date.now = realNow;
}

console.log('time tracking tests passed');
