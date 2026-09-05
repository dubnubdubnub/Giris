// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * panels/compare.js — (c) what slower polling misses.
 *
 * One burst-captured keypress, drawn three times over itself:
 *
 *   8 kHz    every sample the board actually took, 125 us apart
 *   1 kHz    the same press as a 1 kHz poller would have seen it
 *   125 Hz   ditto at 125 Hz
 *
 * The slower traces are not filtered versions of the fast one. They are the
 * fast one sampled at the slower instants and held — which is exactly what a
 * poller does, and exactly why it is late. A poller has no phase relationship
 * to the key, so the phase control matters: at 1 kHz the added latency is
 * anywhere from 0 to 1000 us depending on where the press lands between two
 * polls, and the honest number to quote is the worst case, not the lucky one.
 *
 * The annotation is the point of the panel: the moment |value - rest| crosses
 * the actuation threshold, and how long each slower rate takes to notice.
 */

import { el, select, stat, button, fmtUs } from '../ui.js';
import { KEYS_PRESS_DELTA, slotForKey, NUM_KEYS } from '../protocol.js';
import { crossIndex, crossIndexDecimated, decimate, phaseSweep, restingBaseline } from '../dsp.js';
import {
  Plot,
  bandX,
  clear,
  cssVar,
  dots,
  fit,
  frame,
  gridX,
  gridY,
  label,
  line,
  markerX,
  placeholder,
  ruleY,
  stepLine,
} from '../chart.js';
import { legends } from '../layout.js';

const PAD_L = 58;
const PAD_R = 14;
const PAD_T = 12;
const RAIL_H = 78;
const AXIS_H = 24;
const CANVAS_H = 420;

const RATE_CHOICES = [4000, 2000, 1000, 500, 250, 125, 60];

export function comparePanel(app, root) {
  const store = app.capture;

  let keyIdx = 1; /* a typing key, not the KVM key */
  let rateA = 1000;
  let rateB = 125;
  let phase = 0.5;
  let threshold = KEYS_PRESS_DELTA;
  let dirty = true;

  const keySel = el('select');
  const rateASel = select(
    RATE_CHOICES.map((r) => ({ value: r, label: r >= 1000 ? r / 1000 + ' kHz' : r + ' Hz' })),
    rateA,
    (v) => {
      rateA = +v;
      dirty = true;
    }
  );
  const rateBSel = select(
    RATE_CHOICES.map((r) => ({ value: r, label: r >= 1000 ? r / 1000 + ' kHz' : r + ' Hz' })),
    rateB,
    (v) => {
      rateB = +v;
      dirty = true;
    }
  );
  const thrInput = el('input', { type: 'number', value: threshold, min: 10, max: 2000, step: 10 });
  thrInput.addEventListener('input', () => {
    threshold = Math.max(10, +thrInput.value || KEYS_PRESS_DELTA);
    dirty = true;
  });
  const phaseInput = el('input', { type: 'range', class: 'slider', min: 0, max: 1000, value: 500 });
  phaseInput.addEventListener('input', () => {
    phase = +phaseInput.value / 1000;
    dirty = true;
  });
  const worstBtn = button('Show worst case', () => {
    worstPhase();
  });
  const capBtn = button('Capture a keypress', () => runCapture());
  const status = el('span', { class: 'muted mono' });

  const tiles = {
    fast: stat('8 kHz detects at', 'reference'),
    a: stat('1 kHz is late by', ''),
    b: stat('125 Hz is late by', ''),
    missA: stat('travel missed · 1 kHz', ''),
    missB: stat('travel missed · 125 Hz', ''),
  };

  const verdict = el('p', { class: 'verdict' });

  root.append(
    el(
      'div',
      { class: 'row row--controls' },
      el('label', { class: 'field' }, 'key ', keySel),
      capBtn,
      el('span', { class: 'sep' }),
      el('label', { class: 'field' }, 'compare ', rateASel),
      el('label', { class: 'field' }, 'and ', rateBSel),
      el('label', { class: 'field' }, 'actuation ', thrInput, el('span', { class: 'unit' }, 'counts')),
      status
    ),
    el('canvas', { class: 'plot', id: 'compare-canvas' }),
    el(
      'div',
      { class: 'row row--controls row--phase' },
      el('label', { class: 'field field--grow' }, 'poll phase ', phaseInput),
      el('span', { class: 'muted mono', id: 'phase-out' }),
      worstBtn
    ),
    el('div', { class: 'stats' }, tiles.fast, tiles.a, tiles.b, tiles.missA, tiles.missB),
    verdict
  );

  const canvas = root.querySelector('#compare-canvas');
  const ctx = canvas.getContext('2d');
  const phaseOut = root.querySelector('#phase-out');

  function rebuildKeys() {
    const legend = legends(app.boardId());
    const prev = keySel.value;
    keySel.innerHTML = '';
    for (let k = 0; k < NUM_KEYS; k++) {
      keySel.append(el('option', { value: String(k) }, `key ${k} — ${legend[k].label}`));
    }
    keySel.value = prev || String(keyIdx);
    keyIdx = +keySel.value;
  }
  keySel.addEventListener('change', () => (keyIdx = +keySel.value));
  app.on('board', rebuildKeys);
  app.on('session', () => {
    rebuildKeys();
    capBtn.disabled = !app.active();
    dirty = true;
  });
  app.on('info', rebuildKeys);
  rebuildKeys();
  capBtn.disabled = !app.active();

  async function runCapture() {
    const s = app.active();
    if (!s) return;
    const slot = slotForKey(s.slotMap, keyIdx);
    if (slot < 0) {
      status.textContent = `key ${keyIdx} is not in this board's slot map`;
      return;
    }
    capBtn.disabled = true;
    await store.run(s, slot, 8192, { prompt: 'capturing 1.02 s — press the key now,' });
    capBtn.disabled = false;
    dirty = true;
  }

  store.addEventListener('capture', () => (dirty = true));
  store.addEventListener('status', () => {
    status.textContent = store.status;
  });
  addEventListener('resize', () => (dirty = true));

  /* ------------------------------------------------------------ analysis */

  function analyse() {
    if (!store.ready) return null;
    const s = store.samples;
    const fs = store.fs;
    const usPerSample = store.periodUs;
    const baseline = restingBaseline(s);
    const ref = crossIndex(s, baseline, threshold);
    if (ref < 0) return { empty: true, baseline };

    const nA = Math.max(2, Math.round(fs / rateA));
    const nB = Math.max(2, Math.round(fs / rateB));
    /* One shared epoch, not one phase each: both pollers are started at the
     * same instant and then divide it. When the rates divide (1 kHz and 125 Hz
     * do), that makes the slower poller's instants a strict subset of the
     * faster one's — so the slower rate can never be the earlier to notice,
     * which is the whole claim the panel is making. Independent phases would
     * occasionally show 125 Hz beating 1 kHz, which is true of two unrelated
     * hosts and useless as a demonstration. */
    const nMax = Math.max(nA, nB);
    const offset = Math.min(nMax - 1, Math.floor(phase * nMax));
    const pA = offset % nA;
    const pB = offset % nB;
    const iA = crossIndexDecimated(s, nA, pA, baseline, threshold);
    const iB = crossIndexDecimated(s, nB, pB, baseline, threshold);
    const swA = phaseSweep(s, nA, baseline, threshold, ref);
    const swB = phaseSweep(s, nB, baseline, threshold, ref);

    /* Deepest the key has already travelled by the time each rate notices. */
    const depth = (i) => (i >= 0 ? Math.abs(s[i] - baseline) : 0);
    const full = Math.max(1, Math.max(...Array.from(s.slice(ref, Math.min(s.length, ref + 800))).map((v) => Math.abs(v - baseline))));

    return {
      s,
      fs,
      usPerSample,
      baseline,
      ref,
      nA,
      nB,
      nMax,
      offset,
      pA,
      pB,
      iA,
      iB,
      swA,
      swB,
      depth,
      full,
      delayA: iA >= 0 ? (iA - ref) * usPerSample : NaN,
      delayB: iB >= 0 ? (iB - ref) * usPerSample : NaN,
      worstA: swA.worst * usPerSample,
      worstB: swB.worst * usPerSample,
      meanA: swA.mean * usPerSample,
      meanB: swB.mean * usPerSample,
    };
  }

  function worstPhase() {
    const an = analyse();
    if (!an || an.empty) return;
    /* The shared epoch offset that makes the slower rate as late as it gets. */
    let best = -1;
    let bestO = 0;
    for (let o = 0; o < an.nMax; o++) {
      const i = crossIndexDecimated(an.s, an.nB, o % an.nB, an.baseline, threshold);
      if (i < 0) continue;
      if (i - an.ref > best) {
        best = i - an.ref;
        bestO = o;
      }
    }
    phase = Math.min(0.999, (bestO + 0.5) / an.nMax);
    phaseInput.value = Math.round(phase * 1000);
    dirty = true;
  }

  const rateLabel = (r) => (r >= 1000 ? r / 1000 + ' kHz' : r + ' Hz');

  /* ---------------------------------------------------------------- draw */

  app.onFrame(() => {
    if (!dirty) return;
    dirty = false;
    const { dpr, W, H } = fit(canvas, CANVAS_H);

    const an = analyse();
    if (!an) {
      placeholder(ctx, W, H, dpr, 'capture a keypress to compare polling rates');
      verdict.textContent = '';
      for (const t of Object.values(tiles)) t.set('—');
      return;
    }
    if (an.empty) {
      placeholder(
        ctx,
        W,
        H,
        dpr,
        `no crossing of ±${threshold} counts in this capture — capture again and press the key`
      );
      verdict.textContent = '';
      for (const t of Object.values(tiles)) t.set('—');
      return;
    }

    clear(ctx, W, H);

    const { s, usPerSample, baseline, ref } = an;
    const tOf = (i) => ((i - ref) * usPerSample) / 1000; /* ms, 0 = 8 kHz detection */

    const rightMs = Math.max(14, (isFinite(an.delayB) ? an.delayB / 1000 : 0) + 6);
    const leftMs = -Math.max(3, rightMs * 0.22);
    const i0 = Math.max(0, ref + Math.floor((leftMs * 1000) / usPerSample));
    const i1 = Math.min(s.length - 1, ref + Math.ceil((rightMs * 1000) / usPerSample));

    let lo = Infinity;
    let hi = -Infinity;
    for (let i = i0; i <= i1; i++) {
      if (s[i] < lo) lo = s[i];
      if (s[i] > hi) hi = s[i];
    }
    const pad = Math.max(12, (hi - lo) * 0.12);

    const plotH = H - PAD_T * dpr - (RAIL_H + AXIS_H) * dpr;
    const p = new Plot(
      ctx,
      { x: PAD_L * dpr, y: PAD_T * dpr, w: W - (PAD_L + PAD_R) * dpr, h: plotH },
      dpr
    )
      .domain(leftMs, rightMs)
      .range(lo - pad, hi + pad);

    gridY(p, (v) => Math.round(v));
    gridX(p, (t) => t.toFixed(rightMs < 6 ? 2 : 1));
    frame(p);

    p.clip();

    /* The latency each slower rate adds, as area rather than as a number. */
    if (isFinite(an.delayB)) bandX(p, 0, an.delayB / 1000, cssVar('--k2'), 0.07);
    if (isFinite(an.delayA)) bandX(p, 0, an.delayA / 1000, cssVar('--k1'), 0.13);

    ruleY(p, baseline, cssVar('--ink-3'), `rest ${baseline}`);
    const dir = s[an.ref] < baseline ? -1 : 1;
    ruleY(p, baseline + dir * threshold, cssVar('--warn'), `actuation ${dir < 0 ? '−' : '+'}${threshold} counts`);

    /* 8 kHz: the record itself. */
    const n = i1 - i0 + 1;
    line(p, n, (i) => tOf(i0 + i), (i) => s[i0 + i], cssVar('--accent'), 1.4, 0.9);
    const pxPerSample = p.w / dpr / Math.max(1, i1 - i0);
    if (pxPerSample >= 2.2) {
      const pts = [];
      for (let i = i0; i <= i1; i++) pts.push({ t: tOf(i), v: s[i] });
      dots(p, pts, cssVar('--accent'), Math.min(2.6, 1.2 + pxPerSample * 0.14));
    }

    /* The slower pollers: sample-and-hold, plus the instants themselves. */
    const seriesFor = (nDec, ph) =>
      decimate(s, nDec, ph)
        .filter((d) => d.i >= i0 - nDec && d.i <= i1 + nDec)
        .map((d) => ({ t: tOf(d.i), v: d.v, i: d.i }));

    const sa = seriesFor(an.nB, an.pB);
    stepLine(p, sa, cssVar('--k2'), 1.7, 0.95);
    dots(p, sa, cssVar('--k2'), 4, cssVar('--plot'));

    const sb = seriesFor(an.nA, an.pA);
    stepLine(p, sb, cssVar('--k1'), 1.6, 0.95);
    dots(p, sb, cssVar('--k1'), 2.8, cssVar('--plot'));

    markerX(p, 0, cssVar('--accent'), '8 kHz', 0);
    if (isFinite(an.delayA)) markerX(p, an.delayA / 1000, cssVar('--k1'), `${rateLabel(rateA)} +${fmtUs(an.delayA)}`, 1);
    if (isFinite(an.delayB)) markerX(p, an.delayB / 1000, cssVar('--k2'), `${rateLabel(rateB)} +${fmtUs(an.delayB)}`, 2);
    p.unclip();

    /* ------------------------------------------------------------- rail */

    const railY = p.y + p.h + 10 * dpr;
    const lanes = [
      { name: '8 kHz', nDec: 1, ph: 0, color: cssVar('--accent'), hit: ref },
      { name: rateLabel(rateA), nDec: an.nA, ph: an.pA, color: cssVar('--k1'), hit: an.iA },
      { name: rateLabel(rateB), nDec: an.nB, ph: an.pB, color: cssVar('--k2'), hit: an.iB },
    ];
    const laneH = (RAIL_H * dpr) / lanes.length;
    lanes.forEach((ln, li) => {
      const y = railY + li * laneH;
      const mid = y + laneH * 0.55;
      label(ctx, dpr, p.x - 8 * dpr, mid, ln.name, ln.color, { size: 10, weight: '600', align: 'right', baseline: 'middle' });
      ctx.strokeStyle = cssVar('--grid');
      ctx.lineWidth = dpr;
      ctx.beginPath();
      ctx.moveTo(p.x, Math.round(mid) + 0.5);
      ctx.lineTo(p.x + p.w, Math.round(mid) + 0.5);
      ctx.stroke();

      const step = ln.nDec;
      const first = Math.ceil((i0 - ln.ph) / step) * step + ln.ph;
      for (let i = first; i <= i1; i += step) {
        const x = Math.round(p.px(tOf(i))) + 0.5;
        if (x < p.x || x > p.x + p.w) continue;
        const isHit = i === ln.hit;
        const h = isHit ? laneH * 0.42 : laneH * 0.2;
        ctx.strokeStyle = isHit ? ln.color : cssVar('--rule');
        ctx.lineWidth = (isHit ? 2 : 1) * dpr;
        ctx.beginPath();
        ctx.moveTo(x, mid - h);
        ctx.lineTo(x, mid + h);
        ctx.stroke();
        if (isHit) {
          ctx.fillStyle = ln.color;
          ctx.beginPath();
          ctx.arc(x, mid, 3 * dpr, 0, Math.PI * 2);
          ctx.fill();
        }
      }
    });

    label(
      ctx,
      dpr,
      p.x,
      H - 12 * dpr,
      'poll instants — the filled tick is the first sample at which that rate could have known',
      cssVar('--ink-3'),
      { size: 10 }
    );
    label(ctx, dpr, p.x + p.w, H - 12 * dpr, 'ms from the 8 kHz detection', cssVar('--ink-3'), {
      size: 10,
      align: 'right',
    });

    /* -------------------------------------------------------------- text */

    phaseOut.textContent =
      `epoch offset ${fmtUs(an.offset * usPerSample)} · ` +
      `${rateLabel(rateA)} polls at +${fmtUs(an.pA * usPerSample)} · ` +
      `${rateLabel(rateB)} polls at +${fmtUs(an.pB * usPerSample)}`;

    tiles.fast.set('t = 0', 'ok');
    tiles.fast.setHint(`reference · never worse than one ${fmtUs(usPerSample)} frame`);

    tiles.a.querySelector('.stat-k').textContent = `${rateLabel(rateA)} is late by`;
    tiles.b.querySelector('.stat-k').textContent = `${rateLabel(rateB)} is late by`;
    tiles.missA.querySelector('.stat-k').textContent = `travel missed · ${rateLabel(rateA)}`;
    tiles.missB.querySelector('.stat-k').textContent = `travel missed · ${rateLabel(rateB)}`;

    tiles.a.set('+' + fmtUs(an.delayA), 'warn');
    tiles.a.setHint(`worst case +${fmtUs(an.worstA)} · mean +${fmtUs(an.meanA)}`);
    tiles.b.set('+' + fmtUs(an.delayB), 'stop');
    tiles.b.setHint(`worst case +${fmtUs(an.worstB)} · mean +${fmtUs(an.meanB)}`);

    const missA = an.depth(an.iA) - an.depth(ref);
    const missB = an.depth(an.iB) - an.depth(ref);
    tiles.missA.set(`${Math.round(missA)} counts`, 'warn');
    tiles.missA.setHint(`${((missA / an.full) * 100).toFixed(1)} % of full travel further down`);
    tiles.missB.set(`${Math.round(missB)} counts`, 'stop');
    tiles.missB.setHint(`${((missB / an.full) * 100).toFixed(1)} % of full travel further down`);

    verdict.innerHTML =
      `At this phase the ${rateLabel(rateB)} poller learns about the press <b>${fmtUs(an.delayB)}</b> after the board does, ` +
      `and by then the key is already <b>${Math.round(missB)} counts</b> further down — ` +
      `<b>${((missB / an.full) * 100).toFixed(0)}%</b> of its travel. ` +
      `Worst case over every phase: <b>${fmtUs(an.worstB)}</b> at ${rateLabel(rateB)}, ` +
      `<b>${fmtUs(an.worstA)}</b> at ${rateLabel(rateA)}, ` +
      `<b>${fmtUs(store.periodUs)}</b> at 8 kHz. ` +
      (store.synthetic ? '<span class="tagline">Synthetic capture — the waveform is generated.</span>' : '');
  });
}
