/* ============================================================
   rw-system.js — Firmware update + Factory reset modals
   ============================================================ */
import { h } from 'preact';
import { useState, useRef } from 'preact/hooks';
import htm from 'htm';
import { RW } from './rw-lib.js';
import RewairAPI from './rw-api.js';

(function (RW) {
  'use strict';
  const html = htm.bind(h);

  RW.FirmwareModal = function ({ status, onClose, refresh }) {
    const [phase, setPhase] = useState('pick'); // pick | flashing | done
    const [file, setFile] = useState(null);
    const [pct, setPct] = useState(0);
    const [err, setErr] = useState(null);
    const inputRef = useRef();

    const choose = (e) => { const f = e.target.files && e.target.files[0]; if (f) setFile(f); };
    const install = () => {
      setErr(null);
      setPhase('flashing'); setPct(0);
      RewairAPI.update(file, setPct).then(() => {
        setPhase('done');
        setTimeout(() => { onClose(); refresh(); }, 1800);
      }, (e) => {
        setErr(e.status === 501 ? 'Firmware update not supported by this device' : e.message);
        setPhase('pick');
      });
    };
    const dismissable = phase === 'pick';

    return html`
      <div class="overlay" onClick=${(e) => dismissable && e.target.classList.contains('overlay') && onClose()}>
        <div class="modal" role="dialog" aria-label="Update firmware">
          <div class="modal-head"><h2>Update firmware</h2>
            ${dismissable && html`<button class="modal-x" onClick=${onClose}>✕</button>`}</div>
          <div class="modal-body">
            ${phase === 'pick' && html`
              <div>
                <label class="fw-drop">
                  <input type="file" accept=".bin" ref=${inputRef} onChange=${choose} hidden />
                  <span class="fw-drop-ic">⤓</span>
                  <span class="fw-drop-main">${file ? file.name : 'Choose a firmware file'}</span>
                  <span class="fw-drop-sub">${file ? RW.fmtBytes(file.size) : '.bin image for EMW3165'}</span>
                </label>
                <div class="fw-current">Installed: <strong>${status.fw}</strong></div>
                ${err && html`<span class="modal-err">${err}</span>`}
                <div class="join-actions">
                  <button class="btn" onClick=${onClose}>Cancel</button>
                  <button class="btn primary" disabled=${!file} onClick=${install}>Install update</button>
                </div>
              </div>`}
            ${phase === 'flashing' && html`
              <div class="fw-flash">
                <div class="fw-pct">${pct}<span>%</span></div>
                <div class="progress"><div class="progress-fill" style=${'width:' + pct + '%'}></div></div>
                <div class="fw-note">Installing firmware… Keep the device powered. It will reboot automatically when the update finishes.</div>
              </div>`}
            ${phase === 'done' && html`
              <div class="modal-status"><span class="big-ok">✓</span>
                <span>Update complete — rebooting…</span></div>`}
          </div>
        </div>
      </div>`;
  };

  RW.ResetModal = function ({ onClose, refresh }) {
    const [phase, setPhase] = useState('confirm'); // confirm | resetting
    const doReset = () => {
      setPhase('resetting');
      RewairAPI.reset().then(() => { setTimeout(() => { onClose(); refresh(); }, 400); });
    };
    return html`
      <div class="overlay" onClick=${(e) => phase === 'confirm' && e.target.classList.contains('overlay') && onClose()}>
        <div class="modal" role="dialog" aria-label="Factory reset">
          <div class="modal-head"><h2>Factory reset</h2>
            ${phase === 'confirm' && html`<button class="modal-x" onClick=${onClose}>✕</button>`}</div>
          <div class="modal-body">
            ${phase === 'confirm' && html`
              <div>
                <p class="reset-warn">This erases all saved Wi-Fi networks and settings, then reboots the device into setup (AP) mode. You'll need to reconnect it to your network.</p>
                <div class="join-actions">
                  <button class="btn" onClick=${onClose}>Cancel</button>
                  <button class="btn danger" onClick=${doReset}>Reset device</button>
                </div>
              </div>`}
            ${phase === 'resetting' && html`
              <div class="modal-status"><span class="spinner"></span><span>Resetting…</span></div>`}
          </div>
        </div>
      </div>`;
  };
})(RW);
