// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * panels/spectrum.js — the noise spectrum of the burst in capture.js.
 *
 * A straight line on a log-log plot is the tell: slope 0 is white noise, slope
 * -1/2 in amplitude is 1/f. That decides whether narrowing the analog filter is
 * worth the transport delay, or buys ~5 % for nothing.
 */

import { el, select } from '../ui.js';
import { spectrum } from '../dsp.js';
import { KEYS_PRESS_DELTA } from '../protocol.js';
import { clear, cssVar, fit, font, label, placeholder } from '../chart.js';

const PAD_L = 44;
const PAD_R = 10;
const PAD_T = 12;
const PAD_B = 24;
const DB_FLOOR = -100;

export function spectrumPanel(app, root) {
  const store = app.capture;
  const canvas = el('canvas', { class: 'plot' });
  const note = el('span', { class: 'muted mono' });

  let region = 'quiet';
  const regionSel = select(
    [
      { value: 'quiet', label: 'quiet stretch only' },
      { value: 'all', label: 'whole capture' },
    ],
    region,
    (v) => {
      region = v;
      dirty = true;
    }
  );

  root.append(
    el('div', { class: 'row row--controls' }, el('label', { class: 'field' }, 'region ', regionSel), note),
    canvas,
    el(
      'p',
      { class: 'hint' },
      'Taken from the same burst as the two views above — one capture, three ways of looking at it. ' +
        'The spectrum runs to the Nyquist of the scan, 4 kHz, which is the whole reason for scanning ' +
        'at 8 kHz rather than filtering at 1 kHz and hoping.'
    )
  );

  const ctx = canvas.getContext('2d');
  let dirty = true;
  store.addEventListener('capture', () => (dirty = true));
  addEventListener('resize', () => (dirty = true));

  app.onFrame(() => {
    if (!dirty) return;
    dirty = false;
    const { dpr, W, H } = fit(canvas, 240);

    if (!store.ready) {
      placeholder(ctx, W, H, dpr, 'no burst captured yet');
      note.textContent = '';
      return;
    }
    /* A burst taken to look at a keypress has a keypress in it, and the FFT of
     * a step is a 1/f ramp that says nothing about the noise floor. Default to
     * the longest quiet stretch — before the press, or after it — and say which. */
    let src = store.samples;
    let what = 'whole capture';
    if (region === 'quiet') {
      const an = store.analyse(KEYS_PRESS_DELTA);
      if (an && an.press) {
        const head = store.samples.subarray(0, an.press.start);
        const tail = store.samples.subarray(Math.min(store.samples.length, an.press.end + 8));
        const pick = head.length >= tail.length ? head : tail;
        if (pick.length >= 512) {
          src = pick;
          what = pick === head ? 'quiet before the press' : 'quiet after the press';
        } else {
          what = 'whole capture (no quiet stretch ≥ 512 samples)';
        }
      } else {
        what = 'whole capture (no press found)';
      }
    }

    const sp = spectrum(src, store.fs);
    if (!sp) {
      placeholder(ctx, W, H, dpr, 'burst too short for a spectrum');
      return;
    }
    clear(ctx, W, H);

    const bins = sp.mag.length;
    let mx = 0;
    for (let i = 1; i < bins; i++) mx = Math.max(mx, sp.mag[i]);
    if (mx <= 0) {
      placeholder(ctx, W, H, dpr, 'flat capture — nothing to transform');
      return;
    }

    const fmin = sp.binHz;
    const fmax = store.fs / 2;
    const x0 = PAD_L * dpr;
    const x1 = W - PAD_R * dpr;
    const y0 = PAD_T * dpr;
    const y1 = H - PAD_B * dpr;
    const lx = (f) =>
      x0 + ((Math.log10(f) - Math.log10(fmin)) / (Math.log10(fmax) - Math.log10(fmin))) * (x1 - x0);
    const ly = (m) => {
      const db = 20 * Math.log10(Math.max(m, mx * 1e-6) / mx);
      return y0 + (Math.max(DB_FLOOR, db) / DB_FLOOR) * (y1 - y0);
    };

    ctx.lineWidth = dpr;
    font(ctx, dpr, 10);
    for (let d = 0; d >= DB_FLOOR; d -= 20) {
      const y = Math.round(y0 + (d / DB_FLOOR) * (y1 - y0)) + 0.5;
      ctx.strokeStyle = cssVar('--grid');
      ctx.beginPath();
      ctx.moveTo(x0, y);
      ctx.lineTo(x1, y);
      ctx.stroke();
      label(ctx, dpr, x0 - 6 * dpr, y, d + ' dB', cssVar('--ink-3'), { align: 'right', baseline: 'middle' });
    }
    for (let dec = Math.ceil(Math.log10(fmin)); dec <= Math.log10(fmax); dec++) {
      const f = Math.pow(10, dec);
      const x = Math.round(lx(f)) + 0.5;
      ctx.strokeStyle = cssVar('--grid');
      ctx.beginPath();
      ctx.moveTo(x, y0);
      ctx.lineTo(x, y1);
      ctx.stroke();
      label(ctx, dpr, x, y1 + 5 * dpr, f >= 1000 ? f / 1000 + ' kHz' : f + ' Hz', cssVar('--ink-3'), {
        align: 'center',
      });
    }
    ctx.strokeStyle = cssVar('--rule');
    ctx.strokeRect(Math.round(x0) + 0.5, Math.round(y0) + 0.5, Math.round(x1 - x0), Math.round(y1 - y0));

    ctx.strokeStyle = cssVar('--accent');
    ctx.lineWidth = 1.2 * dpr;
    ctx.beginPath();
    let first = true;
    for (let i = 1; i < bins; i++) {
      const x = lx(i * sp.binHz);
      const y = ly(sp.mag[i]);
      first ? ((first = false), ctx.moveTo(x, y)) : ctx.lineTo(x, y);
    }
    ctx.stroke();

    note.textContent =
      `${what} · ${sp.n} point FFT · ${sp.binHz.toFixed(2)} Hz bins · ` +
      `Nyquist ${(store.fs / 2 / 1000).toFixed(2)} kHz · slot ${store.slot} (${store.slotName})` +
      (store.synthetic ? ' · synthetic' : '');
  });
}
