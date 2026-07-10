import { firmwareCRC32, FW_CHUNK_SIZE, FW_MAX_SIZE } from './rw-ota.js';
import { RewairBleTransport } from './rw-ble-transport.js';

/* Rewair production API facade over HTTP or Web Bluetooth.
 * Point HTTP at a remote device with ?device=192.168.x.x (dev), else
 * same-origin. connectBluetooth() switches the active transport explicitly.
 * Exported as an ES module AND still assigned to window.RewairAPI (kept for
 * the dev console / parity with the pre-Vite build). Module scope means the
 * top-level consts/functions below no longer leak to globals on their own. */
const qs = new URLSearchParams(location.search);
const BASE = qs.get('device') ? `http://${qs.get('device')}` : '';

async function httpReq(path, opts) {
  const r = await fetch(BASE + path, opts);
  if (!r.ok) {
    let msg = `HTTP ${r.status}`;
    try { msg = (await r.json()).error || msg; } catch (e) { /* keep default */ }
    const err = new Error(msg);
    err.status = r.status;
    throw err;
  }
  if (r.status === 204) return null;           // No Content — nothing to parse
  const ct = r.headers.get('content-type') || '';
  return ct.includes('json') ? r.json() : null;
}

const httpTransport = { kind: 'http', request: httpReq };
let activeTransport = httpTransport;
let bleTransport = null;

function req(path, opts) {
  return activeTransport.request(path, opts);
}

/* POST JSON as text/plain: keeps the request CORS-"simple" (the device
 * cannot answer OPTIONS preflights). */
function post(path, obj) {
  return req(path, { method: 'POST', headers: { 'Content-Type': 'text/plain' },
                     body: JSON.stringify(obj || {}) });
}

/* Derive the browser zone's POSIX TZ string empirically (no tz table).
 * Samples UTC offsets through the year; if two offsets exist, binary-search
 * both transitions and express them as Mm.w.d/time rules. */
function derivePosixTZ() {
  const year = new Date().getFullYear();
  const offAt = (t) => -new Date(t).getTimezoneOffset(); // minutes east
  const jan = offAt(new Date(year, 0, 15).getTime());
  const jul = offAt(new Date(year, 6, 15).getTime());
  const zone = (Intl.DateTimeFormat().resolvedOptions().timeZone) || 'UTC';

  const fmtOff = (min) => {
    // POSIX sign is inverted: minutes east -> hours west
    const west = -min;
    const sign = west < 0 ? '-' : '';
    const a = Math.abs(west);
    const h = Math.floor(a / 60), m = a % 60;
    return sign + h + (m ? ':' + String(m).padStart(2, '0') : '');
  };
  const abbr = (dst) => {
    // POSIX needs a name; browsers don't expose one portably. Angle-bracket
    // numeric names are valid POSIX: <+01>, <-0430>, and parse fine device-side.
    const min = dst ? Math.max(jan, jul) : Math.min(jan, jul);
    const sign = min < 0 ? '-' : '+';
    const a = Math.abs(min);
    return `<${sign}${String(Math.floor(a / 60)).padStart(2, '0')}${a % 60 ? String(a % 60).padStart(2, '0') : ''}>`;
  };

  if (jan === jul) {
    return { tz_zone: zone, tz_posix: `${abbr(false)}${fmtOff(jan)}` };
  }

  const std = Math.min(jan, jul), dst = Math.max(jan, jul);
  // find the two instants where the offset changes, searching day pairs
  const findTransition = (fromMs, toMs) => {
    let lo = fromMs, hi = toMs;
    while (hi - lo > 60000) {
      const mid = lo + Math.floor((hi - lo) / 2);
      (offAt(mid) === offAt(fromMs)) ? (lo = mid) : (hi = mid);
    }
    return new Date(hi);
  };
  const y0 = new Date(year, 0, 1).getTime(), y1 = new Date(year + 1, 0, 1).getTime();
  const mid = new Date(year, 6, 1).getTime();
  const t1 = findTransition(y0, mid);   // offset(jan) -> offset(jul)
  const t2 = findTransition(mid, y1);   // offset(jul) -> offset(jan)
  const toDST = offAt(t1.getTime()) === dst ? t1 : t2;   // northern vs southern
  const toSTD = toDST === t1 ? t2 : t1;

  const mrule = (d, activeOffsetMin) => {
    // express the local wall-time instant as Mm.w.d/h[:mm]
    const local = new Date(d.getTime() + activeOffsetMin * 60000);
    const m = local.getUTCMonth() + 1, dom = local.getUTCDate(), dow = local.getUTCDay();
    const week = Math.floor((dom - 1) / 7) + 1;
    const lastWeek = dom + 7 > new Date(Date.UTC(local.getUTCFullYear(), m, 0)).getUTCDate();
    const w = lastWeek && week >= 4 ? 5 : week;
    const h = local.getUTCHours(), mm = local.getUTCMinutes();
    return `M${m}.${w}.${dow}` + (h === 2 && mm === 0 ? '' : `/${h}${mm ? ':' + String(mm).padStart(2, '0') : ''}`);
  };

  return {
    tz_zone: zone,
    tz_posix: `${abbr(false)}${fmtOff(std)}${abbr(true)}${fmtOff(dst)},` +
              `${mrule(toDST, std)},${mrule(toSTD, dst)}`
  };
}

let es = null;
let pollTimer = null;
let esFailures = 0;

function otaPost(path, body, onUpload) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    xhr.open('POST', BASE + path, true);
    if (body != null) xhr.setRequestHeader('Content-Type', 'text/plain');
    if (onUpload) xhr.upload.onprogress = (e) => { if (e.lengthComputable) onUpload(e.loaded); };
    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        try { resolve(xhr.responseText ? JSON.parse(xhr.responseText) : null); }
        catch (e) { resolve(null); }
        return;
      }
      let message = `HTTP ${xhr.status}`;
      try { message = JSON.parse(xhr.responseText).error || message; } catch (e) { /* keep default */ }
      const err = new Error(message);
      err.status = xhr.status;
      reject(err);
    };
    xhr.onerror = () => reject(new Error('Device connection lost during firmware upload'));
    xhr.send(body);
  });
}

async function updateFirmware(file, onProgress) {
  if (activeTransport.kind !== 'http') throw new Error('Firmware update currently requires Wi-Fi');
  if (!file || file.size < 8) throw new Error('Choose a valid firmware .bin file');
  if (file.size > FW_MAX_SIZE) throw new Error('Firmware image exceeds the 464 KB app region');

  const bytes = new Uint8Array(await file.arrayBuffer());
  const crc = firmwareCRC32(bytes).toString(16).padStart(8, '0');
  const progress = typeof onProgress === 'function' ? onProgress : () => {};

  progress(0);
  await otaPost(`/api/update?op=begin&size=${file.size}&crc=${crc}`, null);
  for (let offset = 0; offset < file.size; offset += FW_CHUNK_SIZE) {
    const end = Math.min(offset + FW_CHUNK_SIZE, file.size);
    const chunk = file.slice(offset, end);
    await otaPost(`/api/update?offset=${offset}`, chunk, (loaded) => {
      progress(Math.min(99, Math.floor(((offset + loaded) / file.size) * 99)));
    });
    progress(Math.min(99, Math.floor((end / file.size) * 99)));
  }
  await otaPost('/api/update?op=commit', null);
  progress(100);
}

const RewairAPI = {
  capabilities: () => req('/api/capabilities'),
  status: () => req('/api/status'),
  scan: () => req('/api/scan'),
  networks: () => req('/api/networks'),
  join: (ssid, pass) => post('/api/join', { ssid, pass }),
  forget: (ssid) => post('/api/forget', { ssid }),
  priority: (order) => post('/api/priority', { order }),
  setSettings: (patch) => {
    const p = Object.assign({}, patch);
    /* rw-settings.js signals "auto-detect timezone" by sending a truthy
     * tz_zone (the browser's IANA zone name, e.g. "America/New_York")
     * alongside tz_offset/tz_dst — there is no tz_mode/tz_auto flag.
     * Fixed-offset picks send tz_zone: null. When auto, enrich the patch
     * with the empirically-derived POSIX TZ (tz_zone + tz_posix) so the
     * device can track DST transitions itself; tz_offset/tz_dst are kept
     * (they are accurate now, and the UI reads them back from status). */
    if (p.tz_zone) {
      Object.assign(p, derivePosixTZ());
    }
    return post('/api/settings', p);
  },
  mqtt: () => req('/api/mqtt'),
  setMQTT: (patch) => post('/api/mqtt', patch),
  setDisp: (mode) => post('/api/disp', { mode }),
  setTime: (epoch) => post('/api/time', { epoch }),
  update: updateFirmware,
  reset: () => post('/api/reset', {}),

  bluetoothAvailable: () => typeof navigator !== 'undefined' && !!navigator.bluetooth,
  transportKind: () => activeTransport.kind,
  async connectBluetooth() {
    if (es) { es.close(); es = null; }
    if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
    if (!bleTransport) bleTransport = new RewairBleTransport();
    const capabilities = await bleTransport.connect();
    activeTransport = bleTransport;
    return capabilities;
  },
  useHttp() {
    if (bleTransport) bleTransport.disconnect();
    activeTransport = httpTransport;
  },

  /* Live updates: SSE with transparent polling fallback. */
  subscribe(onStatus) {
    const startPolling = () => {
      if (pollTimer) return;
      const interval = activeTransport.kind === 'ble' ? 5000 : 2500;
      pollTimer = setInterval(() => this.status().then(onStatus).catch(() => {}), interval);
    };
    const stopPolling = () => { clearInterval(pollTimer); pollTimer = null; };

    /* Defensive: a second subscribe() call would otherwise overwrite `es`
     * and leak the prior EventSource (it keeps its connection open and
     * retrying forever). Auto-unsubscribe any live session first. */
    if (es) { es.close(); es = null; stopPolling(); }

    if (activeTransport.kind === 'ble') {
      startPolling();
      this.status().then(onStatus).catch(() => {});
      return () => stopPolling();
    }

    es = new EventSource(BASE + '/api/events');
    es.onmessage = (ev) => {
      esFailures = 0;
      stopPolling();
      try { onStatus(JSON.parse(ev.data)); } catch (e) { /* skip bad frame */ }
    };
    es.onerror = () => {
      esFailures += 1;
      if (esFailures >= 3) startPolling();   /* EventSource keeps retrying too */
    };
    this.status().then(onStatus).catch(() => {});
    return () => { if (es) es.close(); es = null; stopPolling(); };
  },
};

/* Kept as a global too: preserves the dev-console affordance (`RewairAPI.status()`
 * from devtools) that existed pre-Vite. */
window.RewairAPI = RewairAPI;

export { RewairAPI };
export default RewairAPI;
