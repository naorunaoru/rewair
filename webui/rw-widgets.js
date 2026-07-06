/* ============================================================
   rw-widgets.js — ScoreHero, Sensors, DisplaySelector
   ============================================================ */
import { h } from 'preact';
import htm from 'htm';
import { RW } from './rw-lib.js';

(function (RW) {
  'use strict';
  const html = htm.bind(h);

  const SENSORS = [
    { key: 'temp', label: 'Temperature', unit: (u) => (u === 'f' ? '°F' : '°C'),
      fmt: (v, u) => (u === 'f' ? v * 9 / 5 + 32 : v).toFixed(1), bar: '°' },
    { key: 'humid', label: 'Humidity', unit: () => '%', fmt: (v) => Math.round(v), bar: 'RH' },
    { key: 'co2', label: 'CO\u2082', unit: () => 'ppm', fmt: (v) => Math.round(v), bar: 'CO\u2082' },
    { key: 'voc', label: 'VOC', unit: () => 'ppb', fmt: (v) => Math.round(v), bar: 'VOC' },
    { key: 'dust', label: 'Dust', unit: () => '\u00b5g/m\u00b3', fmt: (v) => v.toFixed(1), bar: 'PM' },
    { key: 'light', label: 'Light', unit: () => 'lx', fmt: (v) => Math.round(v), aux: true }
  ];
  RW.SENSORS = SENSORS;

  /* ---- score hero: numeral, dot, 5 index bars ---- */
  RW.ScoreHero = function ({ status }) {
    const sc = status.score;
    const word = sc.color === 'green' ? 'Good' : sc.color === 'amber' ? 'Fair' : 'Poor';
    return html`
      <section data-screen-label="Score">
        <div class="card score-hero">
          <div class="score-left">
            <span class="score-cap">
              <span class="sdot" style=${'background:' + RW.accent(sc.color)}></span>Air score</span>
            <span class="score-num-row">
              <span class="score-num">${sc.value}</span>
              <span class="score-denom">/ 100</span>
            </span>
            <span class=${'score-word t-' + sc.color}>${word}</span>
          </div>
          <div class="bars" aria-label="Per-sensor index bars">
            ${SENSORS.filter((s) => !s.aux).map((s) => {
              const idx = sc.indices[s.key];
              const col = RW.indexColor(idx);
              return html`
                <div class="bar-col" key=${s.key}>
                  <div class="bar-track">
                    <div class="bar-fill" style=${'height:' + ((5 - idx) / 5 * 100) + '%;background:' + RW.accent(col)}></div>
                  </div>
                  <span class="bar-lab">${s.bar}</span>
                </div>`;
            })}
          </div>
        </div>
      </section>`;
  };

  /* ---- one sensor widget (value + dot + sparkline) ---- */
  function Spark({ data }) {
    if (!data || data.length < 2) return html`<svg class="spark" viewBox="0 0 100 26" preserveAspectRatio="none"><polyline points=""></polyline></svg>`;
    const min = Math.min(...data), max = Math.max(...data), span = (max - min) || 1;
    const pts = data.map((v, i) => {
      const x = (i / (RW.HISTORY_MAX - 1)) * 100;
      const y = 23 - ((v - min) / span) * 20;
      return x.toFixed(1) + ',' + y.toFixed(1);
    }).join(' ');
    return html`<svg class="spark" viewBox="0 0 100 26" preserveAspectRatio="none"><polyline points=${pts}></polyline></svg>`;
  }

  RW.Sensors = function ({ status, history }) {
    return html`
      <section data-screen-label="Sensors">
        <div class="grid-sensors">
          ${SENSORS.map((s) => {
            const v = status.sens[s.key];
            const idx = s.aux ? 0 : status.score.indices[s.key];
            return html`
              <div class=${'card widget' + (s.aux ? ' aux' : '')} key=${s.key}>
                <div class="w-top">
                  <span class="w-label">${s.label}</span>
                  ${s.aux
                    ? html`<span class="w-aux-tag">aux</span>`
                    : idx > 0
                      ? html`<span class="w-dot" style=${'background:' + RW.accent(RW.indexColor(idx))}></span>`
                      : null}
                </div>
                <div class="w-val-row">
                  <span class="w-val">${s.fmt(v, status.settings.units)}</span>
                  <span class="w-unit">${s.unit(status.settings.units)}</span>
                </div>
                <${Spark} data=${history[s.key]} />
              </div>`;
          })}
        </div>
      </section>`;
  };

  /* ---- display-mode selector with live LED previews ---- */
  RW.DisplaySelector = function ({ status, cycle, onPick }) {
    const mode = status.settings.disp_mode;
    const now = RW.deviceNow(status);
    const cyc = [
      status.sens.temp.toFixed(0) + '°',
      String(Math.round(status.sens.humid)) + '%',
      String(Math.round(status.sens.co2))
    ];
    const previews = {
      score: String(status.score.value),
      clock: status.time.valid && now ? RW.fmtClock(now, false) : '--:--',
      sensors: cyc[cycle % cyc.length]
    };
    return html`
      <h2 class="sec-label">Device display</h2>
      <section data-screen-label="Display mode">
        <div class="grid-modes">
          ${['score', 'clock', 'sensors'].map((m) => html`
            <button class="mode-card" key=${m} aria-pressed=${mode === m} onClick=${() => onPick(m)}>
              <span class="matrix"><${RW.Matrix} text=${previews[m]} /></span>
              <span class="mode-name">${m[0].toUpperCase() + m.slice(1)}</span>
            </button>`)}
        </div>
      </section>`;
  };
})(RW);
