// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * panels/histogram.js — is it really 8 kHz, and does the host keep up?
 *
 * Two different questions, so two histograms:
 *
 *  device frame period   the gap between consecutive streamed frame indices,
 *                        in units of the 125 us scan tick. The frame index is
 *                        the firmware's own counter, so this is immune to how
 *                        fast the browser is: a bar anywhere but at 1 tick (at
 *                        decim 1) is a frame that existed and never arrived.
 *
 *  host inter-arrival    wall-clock milliseconds between input reports as the
 *                        page saw them. This one IS the browser's timing, and
 *                        it will be lumpy, because the OS hands over several
 *                        microframes' worth at a time. Two clocks, both worth
 *                        having: one says nothing was lost, the other says when
 *                        it turned up.
 *
 * Counts are drawn on a log scale — the interesting bars are the tiny ones.
 */

import { el, button, fmtUs } from '../ui.js';
import { clear, cssVar, fit, font, label, placeholder, roundRect } from '../chart.js';

const PAD_L = 46;
const PAD_R = 10;
const PAD_T = 10;
const PAD_B = 26;

export function histogramPanel(app, root) {
  const resetBtn = button('Reset', () => {
    const s = app.active();
    if (!s) return;
    s.periodHist.reset();
    s.arrivalHist.reset();
  });
  const note = el('span', { class: 'muted mono' });

  const cPeriod = el('canvas', { class: 'plot' });
  const cArrival = el('canvas', { class: 'plot' });
  const sPeriod = el('div', { class: 'histo-stats mono' });
  const sArrival = el('div', { class: 'histo-stats mono' });

  root.append(
    el('div', { class: 'row row--controls' }, resetBtn, note),
    el(
      'div',
      { class: 'grid-2' },
      el(
        'div',
        { class: 'histo' },
        el('h3', {}, 'device frame period'),
        cPeriod,
        sPeriod,
        el('p', { class: 'hint' }, 'From the firmware’s own frame counter. Anything past the first bar is a frame that never reached the host.')
      ),
      el(
        'div',
        { class: 'histo' },
        el('h3', {}, 'host report inter-arrival'),
        cArrival,
        sArrival,
        el('p', { class: 'hint' }, 'Wall clock in the browser. Lumpy is normal — the OS delivers microframes in batches.')
      )
    )
  );

  const ctxP = cPeriod.getContext('2d');
  const ctxA = cArrival.getContext('2d');

  function drawHist(canvas, ctx, hist, opts) {
    const { dpr, W, H } = fit(canvas, 190);
    if (!hist || !hist.total) {
      placeholder(ctx, W, H, dpr, opts.empty);
      return;
    }
    clear(ctx, W, H);
    const x0 = PAD_L * dpr;
    const x1 = W - PAD_R * dpr;
    const y0 = PAD_T * dpr;
    const y1 = H - PAD_B * dpr;

    let maxCount = 1;
    for (const c of hist.bins) if (c > maxCount) maxCount = c;
    const top = Math.pow(10, Math.ceil(Math.log10(maxCount + 1)));
    const ly = (c) => y1 - (Math.log10(c + 1) / Math.log10(top + 1)) * (y1 - y0);

    ctx.lineWidth = dpr;
    for (let d = 0; Math.pow(10, d) <= top; d++) {
      const c = Math.pow(10, d);
      const y = Math.round(ly(c)) + 0.5;
      ctx.strokeStyle = cssVar('--grid');
      ctx.beginPath();
      ctx.moveTo(x0, y);
      ctx.lineTo(x1, y);
      ctx.stroke();
      label(ctx, dpr, x0 - 6 * dpr, y, c >= 1000 ? c / 1000 + 'k' : String(c), cssVar('--ink-3'), {
        align: 'right',
        baseline: 'middle',
      });
    }

    const n = hist.bins.length;
    const bw = (x1 - x0) / n;
    for (let i = 0; i < n; i++) {
      const c = hist.bins[i];
      if (!c) continue;
      const h = y1 - ly(c);
      const x = x0 + i * bw;
      ctx.fillStyle = i === opts.expectedBin ? cssVar('--accent') : cssVar('--warn');
      roundRect(ctx, x + bw * 0.12, y1 - h, Math.max(1.5 * dpr, bw * 0.76), h, Math.min(2 * dpr, bw * 0.3));
      ctx.fill();
    }

    ctx.strokeStyle = cssVar('--rule');
    ctx.strokeRect(Math.round(x0) + 0.5, Math.round(y0) + 0.5, Math.round(x1 - x0), Math.round(y1 - y0));

    font(ctx, dpr, 10);
    const ticks = opts.xticks;
    for (const t of ticks) {
      const frac = (t.v - hist.lo) / (hist.hi - hist.lo);
      if (frac < 0 || frac > 1) continue;
      /* Nudge the end labels inboard so they cannot run off the canvas. */
      const x = x0 + Math.min(0.965, Math.max(0.035, frac)) * (x1 - x0);
      label(ctx, dpr, x, y1 + 5 * dpr, t.label, cssVar('--ink-3'), { align: 'center' });
    }
    if (opts.unit) label(ctx, dpr, x1, y1 + 5 * dpr, opts.unit, cssVar('--ink-3'), { align: 'right' });
  }

  let last = 0;
  app.onFrame(() => {
    /* Histograms move slowly; redrawing them 60 times a second is a waste. */
    const now = performance.now();
    if (now - last < 250) return;
    last = now;

    const s = app.active();
    if (!s) {
      drawHist(cPeriod, ctxP, null, { empty: 'no device' });
      drawHist(cArrival, ctxA, null, { empty: 'no device' });
      sPeriod.textContent = '';
      sArrival.textContent = '';
      note.textContent = '';
      return;
    }

    const tickUs = 1e6 / s.scanHz;
    drawHist(cPeriod, ctxP, s.periodHist, {
      empty: 'start the stream',
      expectedBin: Math.max(0, s.decim - 1),
      unit: '',
      xticks: [1, 4, 8, 16, 24, 32].map((v) => ({ v, label: fmtUs(v * tickUs) })),
    });
    drawHist(cArrival, ctxA, s.arrivalHist, {
      empty: 'start the stream',
      expectedBin: -1,
      unit: '',
      xticks: [0, 2, 4, 6, 8].map((v) => ({ v, label: v ? v + ' ms' : '0' })),
    });

    const ph = s.periodHist;
    const expected = s.decim;
    const onTime = ph.bins[Math.max(0, expected - 1)] || 0;
    sPeriod.textContent = ph.total
      ? `n ${ph.total.toLocaleString()} · on time ${((onTime / ph.total) * 100).toFixed(3)} % · ` +
        `mean ${fmtUs(ph.mean * tickUs)} · p99 ${fmtUs(ph.quantile(0.99) * tickUs)} · max ${fmtUs(ph.max * tickUs)}`
      : '—';

    const ah = s.arrivalHist;
    sArrival.textContent = ah.total
      ? `n ${ah.total.toLocaleString()} · mean ${ah.mean.toFixed(3)} ms · sd ${ah.sd.toFixed(3)} ms · ` +
        `p99 ${ah.quantile(0.99).toFixed(3)} ms · over 8 ms ${ah.over}`
      : '—';

    note.textContent =
      `expecting ${fmtUs(expected * tickUs)} between frames` +
      (s.decim > 1 ? ` (decim ${s.decim})` : '') +
      ` · seq gaps ${s.seqGaps} · dropped-frame flags ${s.frameGaps}` +
      (s.synthetic ? ' · synthetic timing is generated, not measured' : '');
  });
}
