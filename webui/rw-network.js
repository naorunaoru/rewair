/* ============================================================
   rw-network.js — NetworkSection, Manager, Details + Connect modals
   ============================================================ */
import { h } from 'preact';
import { useState, useEffect, useRef } from 'preact/hooks';
import htm from 'htm';
import { RW } from './rw-lib.js';
import RewairAPI from './rw-api.js';

(function (RW) {
  'use strict';
  const html = htm.bind(h);

  /* ---- compact Network section (uses shared NetworkRow) ---- */
  RW.NetworkSection = function ({ status, onManage, onDetails }) {
    const w = status.wifi;
    const head = html`<h2 class="sec-label sec-label-row">Network
      ${(w.saved_count || 0) > 0 && html`<button class="sec-act" onClick=${onManage}>Manage →</button>`}</h2>`;
    if (w.mode !== 'sta') {
      return html`
        ${head}
        <section data-screen-label="WiFi"><div class="card">
          <div class="ap-state">
            <span class="ap-icon"><span class="dot" style=${'background:' + RW.accent('amber')}></span></span>
            <div class="wifi-main">
              <div class="wifi-ssid">${w.ap_ssid}</div>
              <div class="wifi-sub">Open access point · ${w.ap_ip}</div>
            </div>
            <button class="btn primary" onClick=${onManage}>Connect to WiFi…</button>
          </div>
        </div></section>`;
    }
    const net = { ssid: w.ssid, rssi: w.rssi, in_range: true, connected: true, connected_s: w.connected_s, drops: w.drops };
    return html`
      ${head}
      <section data-screen-label="WiFi"><div class="card">
        <${RW.NetworkRow} net=${net} size="lg" showDbm=${true}
          trailing=${html`<button class="btn" onClick=${onDetails}>Details</button>`} />
      </div></section>`;
  };

  /* ---- network details modal ---- */
  RW.DetailsModal = function ({ status, onClose }) {
    const w = status.wifi;
    const rows = [
      ['Network', w.ssid], ['Signal', w.rssi != null ? w.rssi + ' dBm' : '–'],
      ['IP address', w.ip], ['Gateway', w.gw], ['DNS', w.dns], ['MAC address', w.mac]
    ];
    return html`
      <div class="overlay" onClick=${(e) => e.target.classList.contains('overlay') && onClose()}>
        <div class="modal" role="dialog" aria-label="Network details">
          <div class="modal-head"><h2>Network details</h2>
            <button class="modal-x" onClick=${onClose}>✕</button></div>
          <div class="modal-body"><div class="kv-list">
            ${rows.map(([k, v]) => html`<div class="kv-row" key=${k}><span class="kv-key">${k}</span><span class="kv-val">${v || '–'}</span></div>`)}
          </div></div>
        </div>
      </div>`;
  };

  /* ---- saved-networks manager ---- */
  function RowMenu({ net, isPriority, onConnect, onPriority, onForget }) {
    const ref = useRef();
    useEffect(() => {
      const r = ref.current.getBoundingClientRect();
      const m = ref.current.closest('.modal').getBoundingClientRect();
      if (r.bottom > m.bottom - 8) ref.current.classList.add('up');
    }, []);
    return html`
      <div class="pop" ref=${ref}>
        ${!net.connected && net.in_range && html`<button onClick=${onConnect}>Connect</button>`}
        ${!isPriority && html`<button onClick=${onPriority}>★ Make priority</button>`}
        ${net.connected && html`<button onClick=${onConnect}>Details</button>`}
        <div class="sep"></div>
        <button class="warn" onClick=${onForget}>Forget network</button>
      </div>`;
  }

  RW.Manager = function ({ bump, onClose, onAdd, onDetails, refresh }) {
    const [nets, setNets] = useState([]);
    const [loading, setLoading] = useState(true);
    const [menu, setMenu] = useState(null);
    const [dragSsid, setDragSsid] = useState(null);
    const listRef = useRef();
    const netsRef = useRef([]);
    netsRef.current = nets;

    const load = (scanFirst) => {
      setLoading(true);
      const scan = scanFirst ? RewairAPI.scan().catch(() => null) : Promise.resolve();
      return scan.then(() => RewairAPI.networks()).then(setNets).finally(() => setLoading(false));
    };
    useEffect(() => { if (!dragSsid) load(true); }, [bump]);

    const multi = nets.length > 1;
    const act = (fn) => { setMenu(null); fn().then(() => { load(false); refresh(); }); };
    const persistOrder = (order) => RewairAPI.priority(order).then(() => { load(false); refresh(); });

    /* pointer-based drag: live reorder of the in-memory list as you move */
    const startDrag = (e, ssid) => {
      e.preventDefault();
      setMenu(null);
      setDragSsid(ssid);
      const move = (ev) => {
        const y = ev.touches ? ev.touches[0].clientY : ev.clientY;
        const rows = Array.prototype.slice.call(listRef.current.querySelectorAll('.nrow'));
        // insertion index among the OTHER rows (exclude the dragged row's own
        // midpoint, otherwise moving past your own centre swaps immediately)
        let insert = 0;
        rows.forEach((r) => {
          const name = r.querySelector('.ssid-txt');
          if (name && name.textContent === ssid) return;
          const b = r.getBoundingClientRect();
          if (y > b.top + b.height / 2) insert++;
        });
        setNets((prev) => {
          const from = prev.findIndex((n) => n.ssid === ssid);
          if (from < 0) return prev;
          const dragged = prev[from];
          const without = prev.filter((n) => n.ssid !== ssid);
          const at = Math.max(0, Math.min(without.length, insert));
          const next = without.slice();
          next.splice(at, 0, dragged);
          for (let i = 0; i < next.length; i++) if (next[i].ssid !== prev[i].ssid) return next;
          return prev;
        });
      };
      const up = () => {
        document.removeEventListener('pointermove', move);
        document.removeEventListener('pointerup', up);
        setDragSsid(null);
        persistOrder(netsRef.current.map((n) => n.ssid));
      };
      document.addEventListener('pointermove', move);
      document.addEventListener('pointerup', up);
    };

    const kebab = (net, isPriority) => html`
      <button class="kebab" aria-label="Actions" onClick=${(e) => { e.stopPropagation(); setMenu(menu === net.ssid ? null : net.ssid); }}>⋯</button>
      ${menu === net.ssid && html`<${RowMenu} net=${net} isPriority=${isPriority}
          onConnect=${() => net.connected ? (setMenu(null), onDetails()) : act(() => RewairAPI.join(net.ssid, ''))}
          onPriority=${() => act(() => persistOrder([net.ssid].concat(netsRef.current.filter((n) => n.ssid !== net.ssid).map((n) => n.ssid))))}
          onForget=${() => act(() => RewairAPI.forget(net.ssid))} />`}`;

    const badges = (n, i) => html`
      ${n.connected && html`<span class="badge on">on</span>`}
      ${i === 0 && multi && html`<span class="badge prio">1st</span>`}`;

    return html`
      <div class="overlay" onClick=${(e) => {
        if (e.target.classList.contains('overlay')) onClose();
        else if (!e.target.closest('.pop') && !e.target.closest('.kebab')) setMenu(null);
      }}>
        <div class="modal" role="dialog" aria-label="Saved networks">
          <div class="modal-head"><h2>Saved networks</h2>
            <button class="modal-x" onClick=${onClose}>✕</button></div>
          <div class="modal-body">
            ${loading
              ? html`<div class="modal-status"><span class="spinner"></span><span>Scanning for saved networks…</span></div>`
              : nets.length === 0
              ? html`<div class="mgr-empty">No saved networks yet.</div>`
              : html`
                <div class="grp">${multi ? 'Priority order · drag to reorder' : 'Saved network'}</div>
                <div class=${'net-list' + (dragSsid ? ' dragging-list' : '')} ref=${listRef}>
                  ${nets.map((n, i) => html`
                    <${RW.NetworkRow} net=${n} key=${n.ssid}
                      cls=${dragSsid === n.ssid ? 'dragging' : ''}
                      badge=${badges(n, i)}
                      lead=${multi && html`<span class="drag-h" title="Drag to reorder"
                        onPointerDown=${(e) => startDrag(e, n.ssid)}>⋮⋮</span>`}
                      trailing=${kebab(n, i === 0)} />`)}
                </div>`}
            <button class="add-row" onClick=${onAdd}><span class="plus">+</span>Add a network…</button>
          </div>
        </div>
      </div>`;
  };

  /* ---- scan → join modal (Add a network / AP-mode connect) ---- */
  RW.ConnectModal = function ({ onClose, refresh }) {
    const [phase, setPhase] = useState('scanning'); // scanning|list|join|joining|done
    const [nets, setNets] = useState([]);
    const [picked, setPicked] = useState(null);
    const [err, setErr] = useState(null);
    const passRef = useRef();

    const scan = () => {
      setPhase('scanning');
      RewairAPI.scan().then((list) => { setNets(list); setPhase('list'); });
    };
    useEffect(() => { scan(); }, []);

    const pick = (n) => { setPicked(n); setErr(null); setPhase('join'); };
    const join = () => {
      setPhase('joining');
      RewairAPI.join(picked.ssid, picked.sec === 'open' ? '' : (passRef.current ? passRef.current.value : '')).then(
        () => { setPhase('done'); setTimeout(() => { onClose(); refresh(); }, 1500); },
        (e) => { setErr(e.message); setPhase('join'); }
      );
    };

    return html`
      <div class="overlay" onClick=${(e) => e.target.classList.contains('overlay') && onClose()}>
        <div class="modal" role="dialog" aria-label="Connect to WiFi">
          <div class="modal-head"><h2>Connect to WiFi</h2>
            <button class="modal-x" onClick=${onClose}>✕</button></div>
          <div class="modal-body">
            ${phase === 'scanning' && html`<div class="modal-status"><span class="spinner"></span><span>Scanning for networks…</span></div>`}
            ${phase === 'list' && html`
              <div><div class="net-list">
                ${nets.map((n) => html`
                  <button class="net-item" key=${n.ssid} onClick=${() => pick(n)}>
                    <${RW.Signal} net=${{ rssi: n.rssi, in_range: true }} />
                    <span class="net-ssid">${n.ssid}</span>
                    <span class="net-sec">${n.sec}</span>
                  </button>`)}
              </div>
              <div style="padding-top:12px;text-align:center"><button class="btn" onClick=${scan}>Rescan</button></div></div>`}
            ${phase === 'join' && html`
              <div class="join-form">
                <span class="join-ssid">${picked.ssid}</span>
                ${picked.sec !== 'open' && html`<input class="field" ref=${passRef} type="password" placeholder="Password"
                  autocomplete="off" onKeyDown=${(e) => e.key === 'Enter' && join()} />`}
                ${err && html`<span class="modal-err">${err}</span>`}
                <div class="join-actions">
                  <button class="btn" onClick=${() => setPhase('list')}>Back</button>
                  <button class="btn primary" onClick=${join}>Join</button>
                </div>
              </div>`}
            ${phase === 'joining' && html`<div class="modal-status"><span class="spinner"></span><span>Joining <strong>${picked.ssid}</strong>…</span></div>`}
            ${phase === 'done' && html`<div class="modal-status"><span class="big-ok">✓</span><span>Connected. Switching to station mode…</span></div>`}
          </div>
        </div>
      </div>`;
  };
})(RW);
