// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * panels/trace.js — (a) the envelope live trace.
 *
 * The board scans at 8 kHz. A screen redraws at 60 Hz and is maybe 1200 columns
 * wide, so a two-second window holds ~16000 samples for ~1100 columns: about 14
 * samples per column, and at the widest window over 130. The old trace dealt
 * with that by asking the firmware to throw samples away (`decim`), which is
 * fine for a slow wander and useless for anything fast — a 250 us spike simply
 * was not there to draw.
 *
 * This streams at full rate and reduces per COLUMN instead: every sample in a
 * column contributes to that column's min and max, and the column is drawn as
 * the vertical span between them. Nothing is discarded visually. A one-sample
 * excursion paints a full-height column, which is exactly what an oscilloscope
 * does and for exactly the same reason.
 */

import { el, select, fmtHz } from '../ui.js';
import { envelope } from '../dsp.js';
import { KEY_VARS, Plot, clear, cssVar, fit, font, gridX, gridY, envelopeBand, label, placeholder, frame } from '../chart.js';
import { legends, ROLE } from '../layout.js';

const WINDOWS = [0.25, 0.5, 1, 2, 4, 8];
const PAD_L = 52;
const PAD_R = 10;
const PAD_T = 8;
const PAD_B = 22;

export function tracePanel(app, root) {
  let windowSec = 1;
  let mode = 'stack';
  const visible = new Set([0, 1, 2, 3, 4, 5]);

  const readout = el('span', { class: 'muted mono' });
  const chips = el('div', { class: 'chips' });

  const controls = el(
    'div',
    { class: 'row row--controls' },
    el(
      'label',
      { class: 'field' },
      'window ',
      select(
        WINDOWS.map((w) => ({ value: w, label: w < 1 ? w * 1000 + ' ms' : w + ' s' })),
        windowSec,
        (v) => (windowSec = +v)
      )
    ),
    el(
      'label',
      { class: 'field' },
      'layout ',
      select(
        [
          { value: 'stack', label: 'stacked lanes' },
          { value: 'overlay', label: 'overlay' },
        ],
        mode,
        (v) => (mode = v)
      )
    ),
    chips,
    readout
  );

  const canvas = el('canvas', { class: 'plot' });
  root.append(controls, canvas);

  for (let k = 0; k < 6; k++) {
    const chip = el(
      'button',
      {
        class: 'chip on',
        style: { '--kc': `var(${KEY_VARS[k]})` },
        onclick: () => {
          visible.has(k) ? visible.delete(k) : visible.add(k);
          chip.classList.toggle('on', visible.has(k));
        },
      },
      'k' + k
    );
    chips.append(chip);
  }

  const ctx = canvas.getContext('2d');
  /* One column buffer per key, reused across frames: the reduction runs 60
   * times a second over tens of thousands of samples and must not allocate. */
  const bufs = [];
  function colBuf(k, cols) {
    if (!bufs[k] || bufs[k].length < cols * 2) bufs[k] = new Float32Array(cols * 2);
    return bufs[k];
  }

  function relabelChips() {
    const legend = legends(app.boardId());
    Array.from(chips.children).forEach((chip, k) => {
      chip.textContent = `k${k} ${legend[k].label}`;
      chip.dataset.role = ROLE[k];
    });
  }
  app.on('board', relabelChips);
  app.on('session', relabelChips);
  relabelChips();

  app.onFrame(() => {
    const s = app.active();
    const { dpr, W, H } = fit(canvas, 320);

    if (!s || s.ring.n === 0) {
      placeholder(ctx, W, H, dpr, s ? 'no samples yet — press Stream' : 'no device — connect one, or start the synthetic board');
      return;
    }

    clear(ctx, W, H);

    const scan = s.scanHz;
    /* The ring holds STREAMED frames, which is the scan rate only at decim 1.
     * Sizing the window off the scan rate regardless would silently show eight
     * seconds of history under a "1 s" label the moment decimation went up. */
    const streamHz = scan / Math.max(1, s.decim);
    const want = Math.max(2, Math.round(windowSec * streamHz));
    const cols = Math.max(1, Math.min(Math.floor(W / dpr), 4000));

    const keys = [];
    for (let k = 0; k < s.numKeys; k++) if (visible.has(k)) keys.push(k);
    if (!keys.length) {
      placeholder(ctx, W, H, dpr, 'every trace is hidden');
      return;
    }

    const px = (v) => v * dpr;
    const plotX = px(PAD_L);
    const plotW = W - px(PAD_L) - px(PAD_R);
    const totalH = H - px(PAD_T) - px(PAD_B);

    /* Reduce once per key; keep each column array so the draw pass is cheap. */
    const data = [];
    let n = 0;
    for (const k of keys) {
      const arr = colBuf(k, cols);
      n = envelope(s.ring, k, want, cols, arr);
      let lo = Infinity;
      let hi = -Infinity;
      for (let c = 0; c < cols; c++) {
        if (arr[c * 2] < lo) lo = arr[c * 2];
        if (arr[c * 2 + 1] > hi) hi = arr[c * 2 + 1];
      }
      data.push({ k, arr, lo, hi });
    }

    /* Measured from the device's own frame counter, so a decim change mid-window
     * or a dropped run shows up as the axis it really is. */
    const newest = s.ring.frame[s.ring.at(0)];
    const oldest = s.ring.frame[s.ring.start(n)];
    const spanSec = newest > oldest ? (newest - oldest) / scan : n / streamHz;
    const perCol = n / cols;

    const timeFmt = (t) => (spanSec >= 1 ? t.toFixed(2) + 's' : (t * 1000).toFixed(0) + 'ms');

    if (mode === 'overlay') {
      let lo = Infinity;
      let hi = -Infinity;
      for (const d of data) {
        lo = Math.min(lo, d.lo);
        hi = Math.max(hi, d.hi);
      }
      if (hi - lo < 40) {
        const m = (hi + lo) / 2;
        lo = m - 20;
        hi = m + 20;
      }
      const pad = (hi - lo) * 0.08;
      const p = new Plot(ctx, { x: plotX, y: px(PAD_T), w: plotW, h: totalH }, dpr)
        .domain(-spanSec, 0)
        .range(lo - pad, hi + pad);
      gridY(p, (v) => Math.round(v));
      gridX(p, timeFmt);
      frame(p);
      for (const d of data) envelopeBand(p, cols, d.arr, cssVar(KEY_VARS[d.k]), 0.95);
    } else {
      const laneGap = px(6);
      const laneH = (totalH - laneGap * (data.length - 1)) / data.length;
      const legend = legends(app.boardId());
      data.forEach((d, i) => {
        let lo = d.lo;
        let hi = d.hi;
        if (hi - lo < 30) {
          const m = (hi + lo) / 2;
          lo = m - 15;
          hi = m + 15;
        }
        const pad = (hi - lo) * 0.14;
        const y = px(PAD_T) + i * (laneH + laneGap);
        const p = new Plot(ctx, { x: plotX, y, w: plotW, h: laneH }, dpr)
          .domain(-spanSec, 0)
          .range(lo - pad, hi + pad);
        ctx.fillStyle = cssVar('--plot-lane');
        ctx.fillRect(p.x, p.y, p.w, p.h);
        gridY(p, (v) => Math.round(v), { count: 2 });
        if (i === data.length - 1) gridX(p, timeFmt);
        else gridX(p, () => '', { count: 8 });
        envelopeBand(p, cols, d.arr, cssVar(KEY_VARS[d.k]), 0.95);
        label(ctx, dpr, p.x + 6 * dpr, p.y + 4 * dpr, `k${d.k} ${legend[d.k].label}`, cssVar(KEY_VARS[d.k]), {
          size: 10,
          weight: '600',
        });
        label(
          ctx,
          dpr,
          p.x + p.w - 6 * dpr,
          p.y + 4 * dpr,
          `pp ${Math.round(d.hi - d.lo)}`,
          cssVar('--ink-3'),
          { size: 10, align: 'right' }
        );
        frame(p);
      });
    }

    font(ctx, dpr, 10);
    readout.textContent =
      `${n.toLocaleString()} samples over ${spanSec.toFixed(2)} s · ${perCol.toFixed(1)}/column · ` +
      `stream ${fmtHz(streamHz)}${s.decim > 1 ? ` (decim ${s.decim})` : ' full rate'}`;
  });
}
