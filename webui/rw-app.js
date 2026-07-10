/* ============================================================
   rw-app.js — App root: live status (SSE via RewairAPI.subscribe,
   polling fallback inside the adapter), theme, layout, modal routing.
   Exposes window.RewairUI for the dev tweaks harness.
   ============================================================ */
import { h, render } from 'preact';
import { useState, useEffect, useRef } from 'preact/hooks';
import htm from 'htm';
import { RW } from './rw-lib.js';
import RewairAPI from './rw-api.js';
import './rw-widgets.js';
import './rw-network.js';
import './rw-settings.js'; // also pulls in rw-system.js (Firmware/Reset modals)

(function (RW) {
  'use strict';
  const html = htm.bind(h);

  RW.HISTORY_MAX = 48;

  /* module-level hooks for the dev tweaks harness; assigned synchronously
     below so window.RewairUI is live the instant the harness mounts. */
  let setThemeExternal = null;
  let pollExternal = null;
  let setSettingsUIExternal = null;
  window.RewairUI = {
    setTheme: (t) => setThemeExternal && setThemeExternal(t),
    refresh: () => pollExternal && pollExternal(),
    setSettingsUI: (ui) => setSettingsUIExternal && setSettingsUIExternal(ui)
  };

  function TopBar({ status, transport, onBluetooth }) {
    const w = status.wifi;
    const sta = w.mode === 'sta';
    const dotCol = sta ? RW.accent(RW.rssiQual(w.rssi)) : RW.accent('amber');
    const unstable = sta && w.drops > 0;
    return html`
      <header class="topbar">
        <h1>${status.name}</h1>
        <span class="brand">Rewair</span>
        <span class="spacer"></span>
        ${transport === 'ble'
          ? html`<span class="transport-chip">Bluetooth</span>`
          : RewairAPI.bluetoothAvailable() && html`
            <button class="transport-button" onClick=${onBluetooth}>Bluetooth</button>`}
        <span class="wifi-chip">
          <span class=${'dot' + (sta ? '' : ' ap')} style=${'background:' + dotCol}></span>
          <span>${sta ? w.ssid : 'AP mode'}</span>
          ${unstable && html`<span class="chip-warn" style=${'background:' + RW.accent('amber')}
            data-tip=${'Unstable: ' + w.drops + ' reconnects in the last 24 h'}>!</span>`}
        </span>
      </header>`;
  }

  function App() {
    const [status, setStatus] = useState(null);
    const [theme, setTheme] = useState('auto');
    const [modal, setModal] = useState(null); // 'manager' | 'details' | 'connect'
    const [settingsUI, setSettingsUI] = useState({ model: 'direct' });
    const [bump, setBump] = useState(0);
    const [cycle, setCycle] = useState(0);
    const [transport, setTransport] = useState(RewairAPI.transportKind());
    const [connectingBle, setConnectingBle] = useState(false);
    const [connectError, setConnectError] = useState(null);
    const [, setTick] = useState(0);
    const history = useRef({});

    setThemeExternal = setTheme;
    setSettingsUIExternal = setSettingsUI;

    /* theme application */
    useEffect(() => {
      const mq = matchMedia('(prefers-color-scheme: dark)');
      const apply = () => {
        const dark = theme === 'dark' || (theme === 'auto' && mq.matches);
        document.documentElement.setAttribute('data-dark', dark ? '1' : '0');
        setTick((n) => n + 1); // re-read accent() vars after theme flip
      };
      apply();
      mq.addEventListener('change', apply);
      return () => mq.removeEventListener('change', apply);
    }, [theme]);

    /* status ingest: shared by both the live subscription and one-shot refresh */
    const onStatus = (st) => {
      RW.setEpoch(st.time.valid ? st.time.epoch : null);
      RW.SENSORS.forEach((s) => {
        const hk = history.current[s.key] || (history.current[s.key] = []);
        hk.push(st.sens[s.key]);
        if (hk.length > RW.HISTORY_MAX) hk.shift();
      });
      setStatus(st);
    };
    /* one-shot refresh, used by modals after mutations (join/forget/settings/etc.) */
    const poll = () => RewairAPI.status().then(onStatus);
    pollExternal = poll;
    useEffect(() => {
      const unsubscribe = RewairAPI.subscribe(onStatus);
      const sec = setInterval(() => setTick((n) => n + 1), 1000);     // live clock
      const cyc = setInterval(() => setCycle((c) => c + 1), 2000);  // matrix sensor cycle
      return () => { unsubscribe(); clearInterval(sec); clearInterval(cyc); };
    }, [transport]);

    const connectBluetooth = async () => {
      setConnectingBle(true);
      setConnectError(null);
      try {
        await RewairAPI.connectBluetooth();
        setTransport('ble');
        onStatus(await RewairAPI.status());
      } catch (error) {
        setConnectError(error.message || String(error));
      } finally {
        setConnectingBle(false);
      }
    };

    /* auto timezone is set explicitly by the user (Settings → Time zone) */

    if (!status) return html`
      <main class="connect-gate">
        <div class="connect-mark">Rewair</div>
        <h1>Connect to your monitor</h1>
        <p>${RewairAPI.httpAvailable()
          ? 'Trying the configured local HTTP device. You can connect over Bluetooth instead.'
          : 'Connect over Bluetooth to view live readings and device status.'}</p>
        <button class="btn primary connect-ble" disabled=${connectingBle || !RewairAPI.bluetoothAvailable()}
          onClick=${connectBluetooth}>${connectingBle ? 'Connecting…' : 'Connect over Bluetooth'}</button>
        ${!RewairAPI.bluetoothAvailable() && html`
          <div class="connect-error">Web Bluetooth is not available in this browser.</div>`}
        ${connectError && html`<div class="connect-error">${connectError}</div>`}
      </main>`;

    const patch = (p) => RewairAPI.setSettings(p).then(poll);
    const setDisp = (m) => RewairAPI.setDisp(m).then(poll);
    const setTime = (e) => RewairAPI.setTime(e).then(poll);

    return html`<div id="rw-root">
      <div class="wrap">
        <${TopBar} status=${status} transport=${transport} onBluetooth=${connectBluetooth} />
        ${connectError && html`<div class="transport-error">${connectError}</div>`}
        <${RW.ScoreHero} status=${status} />
        <${RW.Sensors} status=${status} history=${history.current} />
        ${transport === 'ble' ? html`
          <div class="transport-note">Bluetooth status is live. Settings and network onboarding stay locked
            until authenticated BLE sessions are implemented.</div>` : html`
          <${RW.DisplaySelector} status=${status} cycle=${cycle} onPick=${setDisp} />
          <${RW.NetworkSection} status=${status}
            onManage=${() => setModal('manager')} onDetails=${() => setModal('details')} />
          <${RW.Settings} status=${status} onPatch=${patch} onSetTime=${setTime} refresh=${poll} ui=${settingsUI} />`}
        <footer class="foot"><span>${status.fw}</span> · ${transport === 'ble' ? 'Bluetooth' : 'local bridge'}</footer>
      </div>
      ${modal === 'manager' && html`<${RW.Manager} bump=${bump} refresh=${poll}
        onClose=${() => setModal(null)} onAdd=${() => setModal('connect')} onDetails=${() => setModal('details')} />`}
      ${modal === 'details' && html`<${RW.DetailsModal} status=${status} onClose=${() => setModal(null)} />`}
      ${modal === 'connect' && html`<${RW.ConnectModal} refresh=${poll} onClose=${() => setModal(null)} />`}
    </div>`;
  }

  render(html`<${App} />`, document.getElementById('app'));
})(RW);
