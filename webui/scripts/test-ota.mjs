import assert from 'node:assert/strict';
import { firmwareCRC32, FW_CHUNK_SIZE, FW_MAX_SIZE } from '../rw-ota.js';

assert.equal(firmwareCRC32(new TextEncoder().encode('123456789')), 0xcbf43926);
assert.equal(firmwareCRC32(new Uint8Array()), 0);
assert.equal(FW_CHUNK_SIZE, 16384);
assert.equal(FW_MAX_SIZE, 475136);
console.log('test-ota OK');
