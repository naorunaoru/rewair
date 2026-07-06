/* ============================================================
   rw-lib.js — Rewair Preact app: shared helpers + primitives.
   Imported first by the other rw-*.js modules; exports the RW
   namespace object (helpers + Signal/NetworkRow/Matrix primitives).
   ============================================================ */
import { h } from 'preact';
import { useEffect, useRef } from 'preact/hooks';
import htm from 'htm';

export const RW = {};
(function (RW) {
  'use strict';
  const html = htm.bind(h);

  /* ---- formatting ---- */
  const p2 = (n) => (n < 10 ? '0' : '') + n;
  RW.p2 = p2;
  RW.fmtOffset = (min) => {
    if (min == null) return 'Not set';
    if (min === 0) return 'UTC';
    const a = Math.abs(min);
    return 'UTC' + (min < 0 ? '−' : '+') + p2(Math.floor(a / 60)) + ':' + p2(a % 60);
  };
  RW.fmtClock = (d, secs) => p2(d.getHours()) + ':' + p2(d.getMinutes()) + (secs ? ':' + p2(d.getSeconds()) : '');
  RW.fmtDate = (d) => {
    const days = ['Sun', 'Mon', 'Tue', 'Wed', 'Thu', 'Fri', 'Sat'];
    const mons = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
    return days[d.getDay()] + ' ' + d.getDate() + ' ' + mons[d.getMonth()] + ' ' + d.getFullYear();
  };
  RW.fmtDuration = (s) => {
    if (s < 60) return s + 's';
    const m = Math.floor(s / 60) % 60, hh = Math.floor(s / 3600) % 24, d = Math.floor(s / 86400);
    if (d > 0) return d + 'd ' + hh + 'h';
    if (hh > 0) return hh + 'h ' + m + 'm';
    return m + 'm';
  };
  RW.fmtBytes = (b) => (b < 1024 ? b + ' B' : b < 1048576 ? (b / 1024).toFixed(1) + ' KB' : (b / 1048576).toFixed(1) + ' MB');

  /* ---- signal / score color helpers ---- */
  RW.rssiBars = (r) => (r >= -55 ? 4 : r >= -65 ? 3 : r >= -75 ? 2 : 1);
  RW.rssiQual = (r) => (r >= -60 ? 'green' : r >= -75 ? 'amber' : 'purple');
  RW.indexColor = (i) => (i === 0 ? 'green' : i <= 2 ? 'amber' : 'purple');
  RW.accent = (name) => getComputedStyle(document.documentElement).getPropertyValue('--' + name).trim();

  /* ---- time: device epoch tracking (no RTC) ---- */
  let epochBase = null, epochAt = 0;
  RW.setEpoch = (epoch) => {
    if (epoch == null) { epochBase = null; } else { epochBase = epoch; epochAt = Date.now(); }
  };
  RW.deviceNow = (status) => {
    if (epochBase === null) return null;
    const epoch = epochBase + (Date.now() - epochAt) / 1000;
    // manual clock: the stored value already IS device-local wall time
    if (status && status.settings.time_mode === 'manual') return new Date(epoch * 1000);
    // NTP/UTC: render in the device's configured time zone offset
    const tz = status ? status.settings.tz_offset : null;
    const offs = tz == null ? -new Date().getTimezoneOffset() : tz;
    return new Date((epoch + offs * 60) * 1000 + new Date().getTimezoneOffset() * 60000);
  };

  /* ============================================================
     Shared network primitives — ONE definition, used by the
     Network section AND the saved-networks manager.
     ============================================================ */
  RW.netStatus = (net, opts) => {
    opts = opts || {};
    if (net.connected) {
      let s = 'Connected' + (net.connected_s != null ? ' ' + RW.fmtDuration(net.connected_s) : '');
      if (!opts.noDbm && net.rssi != null) s += ' · ' + net.rssi + ' dBm';
      if (net.drops > 0) s += ' · ' + net.drops + ' drops / 24 h';
      return s;
    }
    if (net.in_range) return 'In range' + (net.sec === 'open' ? ' · open' : '');
    return 'Out of range' + (net.sec === 'open' ? ' · open' : '');
  };

  RW.Signal = function ({ net, showDbm }) {
    const live = net.in_range || net.connected;
    const bars = live && net.rssi != null ? RW.rssiBars(net.rssi) : 0;
    const col = `var(--${RW.rssiQual(net.rssi != null ? net.rssi : -100)})`;
    const barsEl = html`
      <span class="rbars sig" style="height:18px">
        ${[1, 2, 3, 4].map((i) => html`<span style=${i <= bars ? `background:${col}` : ''}></span>`)}
      </span>`;
    if (showDbm && net.rssi != null) {
      return html`<div class="wifi-sig">${barsEl}<span class="rssi-small">${net.rssi} dBm</span></div>`;
    }
    return barsEl;
  };

  RW.NetworkRow = function ({ net, size = 'md', lead, trailing, badge, showDbm, cls }) {
    const big = size === 'lg';
    return html`
      <div class=${'nrow' + (net.in_range || net.connected ? '' : ' dim') + (cls ? ' ' + cls : '')} style=${big ? 'border:0;padding:6px 2px' : ''}>
        ${lead}
        <${RW.Signal} net=${net} showDbm=${showDbm} />
        <div class="nmain">
          <div class="nname" style=${big ? 'font-size:16px;font-weight:650' : ''}>
            <span class="ssid-txt">${net.ssid}</span>${badge}
          </div>
          <div class="nsub">${RW.netStatus(net, { noDbm: showDbm })}</div>
        </div>
        ${trailing}
      </div>`;
  };

  /* ============================================================
     LED dot-matrix preview (faceplate mimic)
     ============================================================ */
  const GLYPHS = {
    '0': ['111', '101', '101', '101', '111'], '1': ['010', '110', '010', '010', '111'],
    '2': ['111', '001', '111', '100', '111'], '3': ['111', '001', '011', '001', '111'],
    '4': ['101', '101', '111', '001', '001'], '5': ['111', '100', '111', '001', '111'],
    '6': ['111', '100', '111', '101', '111'], '7': ['111', '001', '001', '010', '010'],
    '8': ['111', '101', '111', '101', '111'], '9': ['111', '101', '111', '001', '111'],
    ':': ['0', '1', '0', '1', '0'], '°': ['11', '11', '00', '00', '00'],
    '-': ['000', '000', '111', '000', '000'],
    '%': ['101', '001', '010', '100', '101'], ' ': ['0', '0', '0', '0', '0']
  };
  function dot(ctx, cx, cy, color, cell, gap) {
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.arc(cx * (cell + gap) + cell / 2, cy * (cell + gap) + cell / 2, cell / 2, 0, Math.PI * 2);
    ctx.fill();
  }
  function drawMatrix(canvas, text) {
    const rows = 5, cell = 4, gap = 2, pad = 1;
    let cols = 0; const glyphs = [];
    for (let i = 0; i < text.length; i++) {
      const g = GLYPHS[text[i]] || GLYPHS[' '];
      glyphs.push(g);
      cols += g[0].length + (i < text.length - 1 ? 1 : 0);
    }
    const w = (cols + pad * 2) * (cell + gap) - gap;
    const hgt = (rows + pad * 2) * (cell + gap) - gap;
    const dpr = window.devicePixelRatio || 1;
    canvas.width = w * dpr; canvas.height = hgt * dpr;
    canvas.style.width = w + 'px'; canvas.style.height = hgt + 'px';
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, hgt);
    const led = RW.accent('led') || '#eee';
    const off = 'rgba(255,255,255,0.07)';
    for (let r = -pad; r < rows + pad; r++)
      for (let c = -pad; c < cols + pad; c++) dot(ctx, c + pad, r + pad, off, cell, gap);
    let x0 = pad;
    glyphs.forEach((g) => {
      for (let r = 0; r < rows; r++)
        for (let c = 0; c < g[r].length; c++)
          if (g[r][c] === '1') dot(ctx, x0 + c, r + pad, led, cell, gap);
      x0 += g[0].length + 1;
    });
  }
  RW.Matrix = function ({ text }) {
    const ref = useRef();
    useEffect(() => { drawMatrix(ref.current, text); }, [text]);
    return html`<canvas ref=${ref}></canvas>`;
  };
})(RW);

export default RW;
