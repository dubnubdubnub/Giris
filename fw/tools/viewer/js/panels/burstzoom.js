// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * panels/burstzoom.js — (b) one keypress, every sample.
 *
 * CMD_BURST_START fills device SRAM at the full scan rate, so a burst is the
 * only place the 8 kHz record exists intact: no decimation, no USB pacing, no
 * 60 Hz redraw in the way. 8192 samples is 1.024 s of wall clock; a real
 * keypress is 20-50 ms, i.e. 160-400 samples, which is few enough to draw every
 * one of them as a dot 125 us from its neighbour.
 *
 * Wheel to zoom about the cursor, drag to pan, double-click to fit.
 */

import { el, button, saveBlob, fmtUs } from '../ui.js';
import { KEYS_PRESS_DELTA, slotLabel, slotForKey, BURST_MAX } from '../protocol.js';
import {
  Plot,
  clear,
  cssVar,
  fit,
  gridX,
  gridY,
  frame,
  label,
  line,
  dots,
  ruleY,
  markerX,
  placeholder,
} from '../chart.js';

const PAD_L = 56;
const PAD_R = 12;
const PAD_T = 10;
const PAD_B = 40;

export function burstZoomPanel(app, root) {
  const store = app.capture;

  const slotSel = el('select');
  const countInput = el('input', { type: 'number', value: 4096, min: 256, max: BURST_MAX, step: 256 });
  const captureBtn = button('Capture', () => run());
  const saveBtn = button('Save samples', () => save(), { disabled: true });
  const fitAll = button('Fit all', () => {
    if (!store.ready) return;
    setView(0, store.samples.length - 1);
  });
  const fitPress = button('Fit press', () => zoomToPress());
  const fitEdge = button('Fit edge', () => zoomToEdge());
  const status = el('span', { class: 'muted mono' }, store.status);
  const cursorOut = el('span', { class: 'muted mono cursor-out' });

  root.append(
    el(
      'div',
      { class: 'row row--controls' },
      el('label', { class: 'field' }, 'slot ', slotSel),
      el('label', { class: 'field' }, 'samples ', countInput),
      captureBtn,
      saveBtn,
      el('span', { class: 'sep' }),
      fitAll,
      fitPress,
      fitEdge,
      status
    ),
    el('canvas', { class: 'plot', id: 'burstzoom-canvas' }),
    el(
      'div',
      { class: 'row row--foot' },
      el(
        'p',
        { class: 'hint' },
        'Every dot is one ADC frame, 125 µs after the last. Zoom in until the dots separate and ' +
          'the trace stops being a line and starts being a measurement: the descent, the bottom-out ' +
          'ring, and the exact frame at which |value − rest| first passes the actuation threshold.'
      ),
      cursorOut
    )
  );

  const canvas = root.querySelector('#burstzoom-canvas');
  const ctx = canvas.getContext('2d');

  let a = 0;
  let b = 1;
  let cursor = null;
  let dirty = true;
  let lastPlot = null;

  function setView(na, nb) {
    const n = store.ready ? store.samples.length : 1;
    const minSpan = 8;
    let lo = Math.max(0, Math.min(na, nb));
    let hi = Math.min(n - 1, Math.max(na, nb));
    if (hi - lo < minSpan) hi = lo + minSpan;
    if (hi > n - 1) {
      hi = n - 1;
      lo = Math.max(0, hi - minSpan);
    }
    a = lo;
    b = hi;
    dirty = true;
  }

  function zoomToPress() {
    if (!store.ready) return;
    const an = store.analyse(KEYS_PRESS_DELTA);
    if (!an || !an.press) {
      setView(0, store.samples.length - 1);
      return;
    }
    const p = an.press;
    const pad = Math.max(24, Math.round((p.end - p.start) * 0.25));
    setView(p.start - pad, p.end + pad);
  }

  /* The default after a capture. A whole press is 20-50 ms of descent plus
   * however long a finger rested on the key, which zooms out far enough that
   * the samples merge back into a line — and a line is what this panel exists
   * to stop drawing. The actuation edge is the part worth 125 us of resolution. */
  function zoomToEdge() {
    if (!store.ready) return;
    const an = store.analyse(KEYS_PRESS_DELTA);
    if (!an || !an.press) {
      setView(0, store.samples.length - 1);
      return;
    }
    const spanSamples = Math.round(0.032 * store.fs); /* ~32 ms */
    const lead = Math.round(0.004 * store.fs);
    const a0 = Math.min(an.press.start, an.press.cross - lead);
    setView(a0 - lead, a0 + spanSamples);
  }

  function rebuildSlots() {
    const s = app.active();
    const map = s ? s.slotMap : null;
    slotSel.innerHTML = '';
    if (!map) {
      slotSel.append(el('option', { value: '0' }, 'slot 0'));
      return;
    }
    map.forEach((m, i) => slotSel.append(el('option', { value: String(i) }, `${i} — ${slotLabel(m)}`)));
    /* Default to a typing key rather than slot 0, which is the KVM key. */
    const preferred = slotForKey(map, 1);
    slotSel.value = String(preferred >= 0 ? preferred : 0);
  }

  async function run() {
    const s = app.active();
    if (!s) return;
    captureBtn.disabled = true;
    const want = Math.max(256, Math.min(BURST_MAX, +countInput.value || 4096));
    const ms = Math.round((want / s.scanHz) * 1000);
    await store.run(s, +slotSel.value, want, { prompt: `capturing ${ms} ms — press the key now,` });
    captureBtn.disabled = false;
  }

  async function save() {
    if (!store.ready) return;
    await saveBlob(new Blob([store.csv()], { type: 'text/csv' }), `giris-burst-${Date.now()}.csv`);
  }

  store.addEventListener('status', () => (status.textContent = store.status));
  store.addEventListener('capture', () => {
    saveBtn.disabled = false;
    zoomToEdge();
  });
  app.on('session', () => {
    rebuildSlots();
    captureBtn.disabled = !app.active();
  });
  app.on('info', rebuildSlots);
  rebuildSlots();
  captureBtn.disabled = !app.active();

  /* --------------------------------------------------------- interaction */

  canvas.addEventListener('wheel', (e) => {
    if (!store.ready) return;
    e.preventDefault();
    const r = canvas.getBoundingClientRect();
    const frac = Math.max(0, Math.min(1, (e.clientX - r.left - PAD_L) / (r.width - PAD_L - PAD_R)));
    const at = a + frac * (b - a);
    const k = Math.exp(e.deltaY * 0.0016);
    setView(at - (at - a) * k, at + (b - at) * k);
  }, { passive: false });

  let drag = null;
  canvas.addEventListener('pointerdown', (e) => {
    if (!store.ready) return;
    canvas.setPointerCapture(e.pointerId);
    drag = { x: e.clientX, a, b };
    canvas.classList.add('grabbing');
  });
  canvas.addEventListener('pointermove', (e) => {
    const r = canvas.getBoundingClientRect();
    cursor = { x: e.clientX - r.left, y: e.clientY - r.top };
    dirty = true;
    if (!drag) return;
    const w = r.width - PAD_L - PAD_R;
    const perPx = (drag.b - drag.a) / Math.max(1, w);
    const d = (e.clientX - drag.x) * perPx;
    setView(drag.a - d, drag.b - d);
  });
  const endDrag = () => {
    drag = null;
    canvas.classList.remove('grabbing');
  };
  canvas.addEventListener('pointerup', endDrag);
  canvas.addEventListener('pointercancel', endDrag);
  canvas.addEventListener('pointerleave', () => {
    cursor = null;
    dirty = true;
  });
  canvas.addEventListener('dblclick', () => setView(0, store.ready ? store.samples.length - 1 : 1));
  addEventListener('resize', () => (dirty = true));

  /* --------------------------------------------------------------- draw */

  app.onFrame(() => {
    if (!dirty) return;
    dirty = false;
    const { dpr, W, H } = fit(canvas, 300);

    if (!store.ready) {
      placeholder(ctx, W, H, dpr, 'no burst captured yet — press Capture');
      cursorOut.textContent = '';
      return;
    }
    clear(ctx, W, H);

    const s = store.samples;
    const usPerSample = store.periodUs;
    const i0 = Math.max(0, Math.floor(a));
    const i1 = Math.min(s.length - 1, Math.ceil(b));
    let lo = Infinity;
    let hi = -Infinity;
    for (let i = i0; i <= i1; i++) {
      if (s[i] < lo) lo = s[i];
      if (s[i] > hi) hi = s[i];
    }
    if (hi - lo < 20) {
      const m = (hi + lo) / 2;
      lo = m - 10;
      hi = m + 10;
    }
    const pad = (hi - lo) * 0.1;

    const p = new Plot(
      ctx,
      { x: PAD_L * dpr, y: PAD_T * dpr, w: W - (PAD_L + PAD_R) * dpr, h: H - (PAD_T + PAD_B) * dpr },
      dpr
    )
      .domain((a * usPerSample) / 1000, (b * usPerSample) / 1000)
      .range(lo - pad, hi + pad);
    lastPlot = p;

    const spanMs = p.x1 - p.x0;
    gridY(p, (v) => Math.round(v));
    gridX(p, (t) => (spanMs < 4 ? t.toFixed(2) : spanMs < 40 ? t.toFixed(1) : t.toFixed(0)) + ' ms');
    frame(p);

    const an = store.analyse(KEYS_PRESS_DELTA);
    const baseline = an.baseline;
    p.clip();
    ruleY(p, baseline, cssVar('--ink-3'), `rest ${baseline}`);
    /* Direction-agnostic, like keys.c: draw both sides of the threshold band. */
    ruleY(p, baseline - KEYS_PRESS_DELTA, cssVar('--warn'), `actuation −${KEYS_PRESS_DELTA}`);
    if (hi > baseline + KEYS_PRESS_DELTA * 0.5) ruleY(p, baseline + KEYS_PRESS_DELTA, cssVar('--warn'), null);

    const count = i1 - i0 + 1;
    line(p, count, (i) => ((i0 + i) * usPerSample) / 1000, (i) => s[i0 + i], cssVar('--accent'), 1.3, 0.95);

    const pxPerSample = p.w / dpr / Math.max(1, b - a);
    if (pxPerSample >= 3.5) {
      const pts = [];
      for (let i = i0; i <= i1; i++) pts.push({ t: (i * usPerSample) / 1000, v: s[i] });
      dots(p, pts, cssVar('--accent'), Math.min(3.2, 1.4 + pxPerSample * 0.12), cssVar('--plot'));
    }

    if (an.press) {
      const tc = (an.press.cross * usPerSample) / 1000;
      if (tc >= p.x0 && tc <= p.x1) markerX(p, tc, cssVar('--warn'), 'actuates', 0);
    }
    p.unclip();

    /* Footer facts, in the plot so a screenshot carries them. */
    const bits = [
      `${count.toLocaleString()} samples in view`,
      `${usPerSample.toFixed(0)} µs/sample`,
      `${pxPerSample.toFixed(1)} px/sample`,
      `${spanMs.toFixed(spanMs < 4 ? 2 : 1)} ms span`,
    ];
    if (an.press) {
      bits.push(`press ${fmtUs((an.press.end - an.press.start) * usPerSample)}`);
      bits.push(`depth ${an.press.depth} counts`);
    }
    label(ctx, dpr, PAD_L * dpr, H - 12 * dpr, bits.join('   ·   '), cssVar('--ink-3'), { size: 10 });

    if (cursor && lastPlot) {
      const cx = cursor.x * dpr;
      if (cx >= p.x && cx <= p.x + p.w) {
        ctx.save();
        ctx.strokeStyle = cssVar('--ink-3');
        ctx.globalAlpha = 0.5;
        ctx.lineWidth = dpr;
        ctx.beginPath();
        ctx.moveTo(Math.round(cx) + 0.5, p.y);
        ctx.lineTo(Math.round(cx) + 0.5, p.y + p.h);
        ctx.stroke();
        ctx.restore();
        const tms = p.ux(cx);
        const idx = Math.round((tms * 1000) / usPerSample);
        if (idx >= 0 && idx < s.length) {
          cursorOut.textContent = `n=${idx}  t=${tms.toFixed(3)} ms  ${s[idx]} counts  Δrest ${s[idx] - baseline}`;
        }
      }
    } else {
      cursorOut.textContent = '';
    }
  });
}
