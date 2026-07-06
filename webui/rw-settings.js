/* ============================================================
   rw-settings.js — Settings card.

   Two models, switchable via dev tweaks (ui.model):

   'direct' (default) — the control IS the value. Every setting
     is always live in the right slot, instant apply: text field
     for name, seg for choices, select for timezone. No edit
     modes, no disclosure. Buttons appear only for FLOWS
     (firmware update, factory reset).

   'rows' — disclosure grammar for comparison: value in right
     slot + chevron, whole row clickable, editor expands inline.

   Sub line is for status/explanation ONLY — never the value.
   ============================================================ */
import { h } from 'preact';
import { useState, useRef, useEffect } from 'preact/hooks';
import htm from 'htm';
import { RW } from './rw-lib.js';
import './rw-system.js'; // registers RW.FirmwareModal / RW.ResetModal

(function (RW) {
  'use strict';
  const html = htm.bind(h);

  function Seg({ value, options, onChange }) {
    return html`<span class="seg" onClick=${(e) => e.stopPropagation()}>
      ${options.map(([v, label]) => html`
        <button data-v=${v} key=${v} aria-pressed=${value === v} onClick=${() => onChange(v)}>${label}</button>`)}
    </span>`;
  }

  const TZ = [
    [-720, 'UTC−12'], [-660, 'UTC−11'], [-600, 'UTC−10'], [-540, 'UTC−9'], [-480, 'UTC−8'],
    [-420, 'UTC−7'], [-360, 'UTC−6'], [-300, 'UTC−5'], [-240, 'UTC−4'], [-180, 'UTC−3'],
    [-120, 'UTC−2'], [-60, 'UTC−1'], [0, 'UTC'], [60, 'UTC+1'], [120, 'UTC+2'], [180, 'UTC+3'],
    [240, 'UTC+4'], [300, 'UTC+5'], [330, 'UTC+5:30'], [360, 'UTC+6'], [420, 'UTC+7'], [480, 'UTC+8'],
    [540, 'UTC+9'], [600, 'UTC+10'], [660, 'UTC+11'], [720, 'UTC+12'], [780, 'UTC+13'], [840, 'UTC+14']
  ];

  const browserOff = () => -new Date().getTimezoneOffset();
  const browserZone = () => (Intl.DateTimeFormat().resolvedOptions().timeZone) || 'this browser';

  /* ---------- live controls (direct model) ---------- */

  /* quiet inline text field: looks like the value, edits in place.
     UNCONTROLLED while focused — the card re-renders every second
     (clock tick) and a controlled value prop would clobber typing.
     The DOM value is synced from st.name only when not editing. */
  function NameField({ st, onPatch }) {
    const ref = useRef();
    useEffect(() => {
      const el = ref.current;
      if (el && document.activeElement !== el) el.value = st.name;
    }, [st.name]);
    const commit = (el) => {
      const v = el.value.trim();
      if (v && v !== st.name) onPatch({ name: v }); else el.value = st.name;
    };
    return html`<input class="field quiet" ref=${ref} maxlength="24" autocomplete="off"
      aria-label="Monitor name"
      onBlur=${(e) => commit(e.target)}
      onKeyDown=${(e) => {
        if (e.key === 'Enter') e.target.blur();
        if (e.key === 'Escape') { e.target.value = st.name; e.target.blur(); }
      }} />`;
  }

  /* one select: browser-matched zone (auto, follows DST) or fixed offset */
  function TzSelect({ st, onPatch }) {
    const val = st.tz_zone ? 'auto' : (st.tz_offset == null ? '' : String(st.tz_offset));
    const change = (e) => {
      const v = e.target.value;
      if (v === 'auto') onPatch({ tz_offset: browserOff(), tz_dst: true, tz_zone: browserZone() });
      else if (v !== '') onPatch({ tz_offset: parseInt(v, 10), tz_dst: false, tz_zone: null });
    };
    return html`<span class="quiet-sel">
      <select class="field quiet" value=${val} onChange=${change} aria-label="Time zone">
        ${st.tz_offset == null && html`<option value="" disabled>Choose…</option>`}
        <option value="auto">Auto: ${browserZone()}</option>
        <optgroup label="Fixed offset">
          ${TZ.map(([v, label]) => html`<option value=${String(v)} key=${String(v)}>${label}</option>`)}
        </optgroup>
      </select>
      <span class="quiet-sel-chev" aria-hidden="true">▾</span>
    </span>`;
  }

  /* datetime composer — the one place a commit button is honest,
     because you compose a value before applying it */
  function ClockField({ status, onSetTime, onDone }) {
    const ref = useRef();
    useEffect(() => {
      const d = RW.deviceNow(status) || new Date();
      if (ref.current) ref.current.value =
        d.getFullYear() + '-' + RW.p2(d.getMonth() + 1) + '-' + RW.p2(d.getDate()) + 'T' + RW.p2(d.getHours()) + ':' + RW.p2(d.getMinutes());
    }, []);
    const apply = () => {
      const v = ref.current.value;
      if (v) onSetTime(Math.floor(new Date(v).getTime() / 1000));
      if (onDone) onDone();
    };
    return html`<div class="set-ed-row">
      <input class="field" type="datetime-local" ref=${ref} style="width:auto" />
      <button class="btn primary" onClick=${apply}>Set</button>
      ${onDone && html`<button class="btn" onClick=${onDone}>Cancel</button>`}
    </div>`;
  }

  /* ---------- disclosure editors ('rows' model) ---------- */

  function NameEditor({ st, onPatch, close }) {
    const ref = useRef();
    useEffect(() => { if (ref.current) { ref.current.focus(); ref.current.select(); } }, []);
    const save = () => { const v = ref.current.value.trim(); if (v) onPatch({ name: v }); close(); };
    return html`<div class="set-ed-row">
      <input class="field" ref=${ref} maxlength="24" autocomplete="off" value=${st.name} style="width:180px"
        onKeyDown=${(e) => { if (e.key === 'Enter') save(); if (e.key === 'Escape') close(); }} />
      <button class="btn primary" onClick=${save}>Save</button>
      <button class="btn" onClick=${close}>Cancel</button>
    </div>`;
  }

  function TzEditor({ st, onPatch, close }) {
    const sel = useRef();
    useEffect(() => { if (sel.current) sel.current.value = st.tz_offset == null ? '0' : String(st.tz_offset); }, []);
    const match = () => { onPatch({ tz_offset: browserOff(), tz_dst: true, tz_zone: browserZone() }); close(); };
    const manual = () => {
      const v = sel.current.value;
      if (v !== '') onPatch({ tz_offset: parseInt(v, 10), tz_dst: false, tz_zone: null });
      close();
    };
    return html`
      <div class="set-ed-row">
        <button class="btn primary" onClick=${match}>Match this browser</button>
        <span class="set-ed-hint">${browserZone()} · ${RW.fmtOffset(browserOff())} · follows DST</span>
      </div>
      <div class="set-ed-row">
        <span class="set-ed-or">or fixed offset</span>
        <select class="field" ref=${sel}>
          ${TZ.map(([v, label]) => html`<option value=${String(v)} key=${String(v)}>${label}</option>`)}
        </select>
        <button class="btn" onClick=${manual}>Set</button>
        <button class="btn" onClick=${close}>Cancel</button>
      </div>`;
  }

  /* ---------- the one row component ---------- */
  /* editable rows: whole row clickable + chevron ('rows' model) */
  function Row({ id, name, sub, ctrl, editor, below, open, setOpen }) {
    const editable = !!editor;
    const isOpen = editable && open === id;
    const close = () => setOpen(null);
    const toggle = () => setOpen(isOpen ? null : id);

    return html`
      <div class=${'set-row' + (editable ? ' rowable' : '') + (isOpen ? ' open' : '')}
        onClick=${editable ? toggle : null}
        tabindex=${editable ? '0' : null} role=${editable ? 'button' : null}
        onKeyDown=${editable ? (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); toggle(); } } : null}>
        <div class="set-left">
          <div class="set-name">${name}</div>
          ${sub && html`<div class="set-sub">${sub}</div>`}
        </div>
        <div class="set-ctrl" onClick=${(e) => e.stopPropagation()}>
          ${ctrl}
          ${editable && html`<span class="set-chev" aria-hidden="true">›</span>`}
        </div>
        ${isOpen && html`<div class="set-editor" onClick=${(e) => e.stopPropagation()}>${editor(close)}</div>`}
        ${below && html`<div class="set-editor" onClick=${(e) => e.stopPropagation()}>${below}</div>`}
      </div>`;
  }

  RW.Settings = function ({ status, onPatch, onSetTime, refresh, ui }) {
    ui = ui || { model: 'direct' };
    const direct = ui.model !== 'rows';
    const st = status.settings;
    const [open, setOpen] = useState(null);
    const [fwOpen, setFwOpen] = useState(false);
    const [resetOpen, setResetOpen] = useState(false);
    const now = RW.deviceNow(status);

    const tzSub = st.tz_offset == null ? 'Not set'
      : direct
        ? (st.tz_dst ? 'Currently ' + RW.fmtOffset(st.tz_offset) + ' · follows DST' : 'Fixed offset, no DST')
        : (st.tz_zone ? st.tz_zone + (st.tz_dst ? ' · follows DST' : '') : (st.tz_dst ? 'Follows DST' : 'Fixed offset'));

    const timeSub = status.time.valid && now
      ? (status.time.synced ? 'Synced via NTP' : 'Set manually')
      : 'Not set — ' + (st.time_mode === 'auto' ? 'no RTC, waiting for network time' : 'no RTC, set the clock');

    const clock = html`<span class="set-val">${status.time.valid && now
      ? RW.fmtDate(now) + ' · ' + RW.fmtClock(now, true) : '--:--'}</span>`;
    const timeSeg = html`<${Seg} value=${st.time_mode} options=${[['auto', 'Auto'], ['manual', 'Manual']]}
      onChange=${(v) => { onPatch({ time_mode: v }); if (!direct) setOpen(v === 'manual' ? 'time' : (open === 'time' ? null : open)); }} />`;

    const stopBtn = (fn) => (e) => { e.stopPropagation(); fn(); };

    return html`
      <h2 class="sec-label">Settings</h2>
      <section data-screen-label="Settings"><div class="card">

        <${Row} id="name" name="Monitor name" sub="Shown here and on the network"
          open=${open} setOpen=${setOpen}
          ctrl=${direct ? html`<${NameField} st=${st} onPatch=${onPatch} />` : html`<span class="set-val">${st.name}</span>`}
          editor=${direct ? null : (close) => html`<${NameEditor} st=${st} onPatch=${onPatch} close=${close} />`} />

        <${Row} id="units" name="Temperature unit" open=${open} setOpen=${setOpen}
          ctrl=${html`<${Seg} value=${st.units} options=${[['c', '°C'], ['f', '°F']]}
            onChange=${(v) => onPatch({ units: v })} />`} />

        <${Row} id="time" name="Time & date" sub=${timeSub} open=${open} setOpen=${setOpen}
          ctrl=${html`${clock}${timeSeg}`}
          below=${direct && st.time_mode === 'manual'
            ? html`<${ClockField} key="direct" status=${status} onSetTime=${onSetTime} />` : null}
          editor=${!direct && st.time_mode === 'manual'
            ? (close) => html`<${ClockField} key="rows" status=${status} onSetTime=${onSetTime} onDone=${close} />`
            : null} />

        <${Row} id="tz" name="Time zone" sub=${tzSub} open=${open} setOpen=${setOpen}
          ctrl=${direct ? html`<${TzSelect} st=${st} onPatch=${onPatch} />` : html`<span class="set-val">${RW.fmtOffset(st.tz_offset)}</span>`}
          editor=${direct ? null : (close) => html`<${TzEditor} st=${st} onPatch=${onPatch} close=${close} />`} />

        <${Row} id="fw" name="Firmware" open=${open} setOpen=${setOpen}
          ctrl=${html`<span class="set-val">${status.fw}</span>
            <button class="btn" onClick=${stopBtn(() => setFwOpen(true))}>Update…</button>`} />

        <${Row} id="reset" name="Factory reset" sub="Erase networks & settings, reboot to setup"
          open=${open} setOpen=${setOpen}
          ctrl=${html`<button class="btn" onClick=${stopBtn(() => setResetOpen(true))}>Reset…</button>`} />

      </div></section>

      ${fwOpen && html`<${RW.FirmwareModal} status=${status} refresh=${refresh} onClose=${() => setFwOpen(false)} />`}
      ${resetOpen && html`<${RW.ResetModal} refresh=${refresh} onClose=${() => setResetOpen(false)} />`}`;
  };
})(RW);
