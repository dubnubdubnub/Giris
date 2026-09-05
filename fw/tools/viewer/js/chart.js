// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * chart.js — canvas primitives shared by every plot.
 *
 * All drawing works in device pixels; `fit()` returns the dpr so callers can
 * scale line widths and type. Colours come out of the CSS custom properties so
 * the palette lives in exactly one place (style.css).
 */

const varCache = new Map();

export function cssVar(name) {
  if (!varCache.has(name)) {
    varCache.set(name, getComputedStyle(document.documentElement).getPropertyValue(name).trim());
  }
  return varCache.get(name);
}

export function invalidatePalette() {
  varCache.clear();
}

export const KEY_VARS = ['--k0', '--k1', '--k2', '--k3', '--k4', '--k5'];
export const keyColor = (k) => cssVar(KEY_VARS[k % KEY_VARS.length]);

/* Resize the backing store to the element's CSS box at device resolution.
 * cssHeight is authoritative — the element's own height is set to match. */
export function fit(canvas, cssHeight) {
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(canvas.getBoundingClientRect().width));
  const h = Math.max(1, Math.round(cssHeight));
  canvas.style.height = h + 'px';
  const bw = Math.round(w * dpr);
  const bh = Math.round(h * dpr);
  if (canvas.width !== bw || canvas.height !== bh) {
    canvas.width = bw;
    canvas.height = bh;
  }
  return { dpr, W: bw, H: bh, w, h };
}

export function clear(ctx, W, H, color = cssVar('--plot')) {
  ctx.fillStyle = color;
  ctx.fillRect(0, 0, W, H);
}

/*
 * A plot box: pixel rectangle plus value<->pixel mapping in both axes.
 * Everything else in this file draws into one of these.
 */
export class Plot {
  constructor(ctx, box, dpr) {
    this.ctx = ctx;
    this.dpr = dpr;
    Object.assign(this, box); /* x, y, w, h in device px */
    this.x0 = 0;
    this.x1 = 1;
    this.y0 = 0;
    this.y1 = 1;
  }
  domain(x0, x1) {
    this.x0 = x0;
    this.x1 = x1;
    return this;
  }
  range(y0, y1) {
    this.y0 = y0;
    this.y1 = y1;
    return this;
  }
  px(v) {
    return this.x + ((v - this.x0) / (this.x1 - this.x0)) * this.w;
  }
  py(v) {
    return this.y + this.h - ((v - this.y0) / (this.y1 - this.y0)) * this.h;
  }
  ux(px) {
    return this.x0 + ((px - this.x) / this.w) * (this.x1 - this.x0);
  }
  clip() {
    this.ctx.save();
    this.ctx.beginPath();
    this.ctx.rect(this.x, this.y, this.w, this.h);
    this.ctx.clip();
  }
  unclip() {
    this.ctx.restore();
  }
}

export function font(ctx, dpr, size = 10, weight = '') {
  ctx.font = `${weight} ${size * dpr}px ui-monospace, "SF Mono", Menlo, monospace`.trim();
}

/* "Nice" tick step for a span, in 1/2/5 x 10^n. */
export function niceStep(span, target = 6) {
  if (!(span > 0)) return 1;
  const raw = span / target;
  const mag = Math.pow(10, Math.floor(Math.log10(raw)));
  const norm = raw / mag;
  const step = norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10;
  return step * mag;
}

export function ticks(lo, hi, target = 6) {
  const step = niceStep(hi - lo, target);
  const out = [];
  for (let t = Math.ceil(lo / step) * step; t <= hi + step * 1e-9; t += step) out.push(t);
  return out;
}

/* Horizontal gridlines + left-hand labels. */
export function gridY(plot, fmt, opts = {}) {
  const { ctx, dpr } = plot;
  const vals = ticks(plot.y0, plot.y1, opts.count || 5);
  ctx.lineWidth = dpr;
  font(ctx, dpr, 10);
  ctx.textBaseline = 'middle';
  ctx.textAlign = 'right';
  for (const v of vals) {
    const y = Math.round(plot.py(v)) + 0.5;
    if (y < plot.y - 1 || y > plot.y + plot.h + 1) continue;
    ctx.strokeStyle = opts.grid || cssVar('--grid');
    ctx.beginPath();
    ctx.moveTo(plot.x, y);
    ctx.lineTo(plot.x + plot.w, y);
    ctx.stroke();
    ctx.fillStyle = cssVar('--ink-3');
    ctx.fillText(fmt(v), plot.x - 6 * dpr, y);
  }
  ctx.textAlign = 'left';
}

/* Vertical gridlines + bottom labels. */
export function gridX(plot, fmt, opts = {}) {
  const { ctx, dpr } = plot;
  const vals = ticks(plot.x0, plot.x1, opts.count || 8);
  ctx.lineWidth = dpr;
  font(ctx, dpr, 10);
  ctx.textBaseline = 'top';
  ctx.textAlign = 'center';
  for (const v of vals) {
    const x = Math.round(plot.px(v)) + 0.5;
    if (x < plot.x - 1 || x > plot.x + plot.w + 1) continue;
    ctx.strokeStyle = opts.grid || cssVar('--grid');
    ctx.beginPath();
    ctx.moveTo(x, plot.y);
    ctx.lineTo(x, plot.y + plot.h);
    ctx.stroke();
    ctx.fillStyle = cssVar('--ink-3');
    ctx.fillText(fmt(v), x, plot.y + plot.h + 5 * dpr);
  }
  ctx.textAlign = 'left';
}

export function frame(plot, color) {
  const { ctx, dpr } = plot;
  ctx.strokeStyle = color || cssVar('--rule');
  ctx.lineWidth = dpr;
  ctx.strokeRect(
    Math.round(plot.x) + 0.5,
    Math.round(plot.y) + 0.5,
    Math.round(plot.w),
    Math.round(plot.h)
  );
}

/* A dashed horizontal rule with a right-aligned caption. */
export function ruleY(plot, v, color, label) {
  const { ctx, dpr } = plot;
  const y = Math.round(plot.py(v)) + 0.5;
  ctx.save();
  ctx.setLineDash([4 * dpr, 4 * dpr]);
  ctx.strokeStyle = color;
  ctx.lineWidth = dpr;
  ctx.beginPath();
  ctx.moveTo(plot.x, y);
  ctx.lineTo(plot.x + plot.w, y);
  ctx.stroke();
  ctx.restore();
  if (label) {
    font(ctx, dpr, 10, '600');
    ctx.fillStyle = color;
    ctx.textAlign = 'right';
    ctx.textBaseline = 'bottom';
    ctx.fillText(label, plot.x + plot.w - 4 * dpr, y - 3 * dpr);
    ctx.textAlign = 'left';
  }
}

/* A vertical marker with a small flag at the top. */
export function markerX(plot, v, color, label, tier = 0) {
  const { ctx, dpr } = plot;
  const x = Math.round(plot.px(v)) + 0.5;
  ctx.strokeStyle = color;
  ctx.lineWidth = 1.5 * dpr;
  ctx.beginPath();
  ctx.moveTo(x, plot.y);
  ctx.lineTo(x, plot.y + plot.h);
  ctx.stroke();
  if (!label) return;
  font(ctx, dpr, 10, '600');
  const pad = 4 * dpr;
  const tw = ctx.measureText(label).width;
  const bh = 14 * dpr;
  const by = plot.y + 3 * dpr + tier * (bh + 3 * dpr);
  let bx = x + 2 * dpr;
  if (bx + tw + pad * 2 > plot.x + plot.w) bx = x - tw - pad * 2 - 2 * dpr;
  ctx.fillStyle = color;
  roundRect(ctx, bx, by, tw + pad * 2, bh, 3 * dpr);
  ctx.fill();
  ctx.fillStyle = cssVar('--paper');
  ctx.textBaseline = 'middle';
  ctx.fillText(label, bx + pad, by + bh / 2);
}

export function roundRect(ctx, x, y, w, h, r) {
  const rr = Math.min(r, w / 2, h / 2);
  ctx.beginPath();
  ctx.moveTo(x + rr, y);
  ctx.arcTo(x + w, y, x + w, y + h, rr);
  ctx.arcTo(x + w, y + h, x, y + h, rr);
  ctx.arcTo(x, y + h, x, y, rr);
  ctx.arcTo(x, y, x + w, y, rr);
  ctx.closePath();
}

/* Translucent fill of an x-interval, for "this is the latency you added". */
export function bandX(plot, a, b, color, alpha = 0.14) {
  const { ctx } = plot;
  const x0 = plot.px(Math.min(a, b));
  const x1 = plot.px(Math.max(a, b));
  ctx.save();
  ctx.globalAlpha = alpha;
  ctx.fillStyle = color;
  ctx.fillRect(x0, plot.y, Math.max(1, x1 - x0), plot.h);
  ctx.restore();
}

/* Polyline over (x,y) pairs produced by a generator function. */
export function line(plot, count, fx, fy, color, width = 1.4, alpha = 1) {
  const { ctx, dpr } = plot;
  if (count <= 0) return;
  ctx.save();
  ctx.globalAlpha = alpha;
  ctx.strokeStyle = color;
  ctx.lineWidth = width * dpr;
  ctx.lineJoin = 'round';
  ctx.beginPath();
  for (let i = 0; i < count; i++) {
    const x = plot.px(fx(i));
    const y = plot.py(fy(i));
    i ? ctx.lineTo(x, y) : ctx.moveTo(x, y);
  }
  ctx.stroke();
  ctx.restore();
}

/* Zero-order hold: what a sample-and-hold poller actually presents downstream. */
export function stepLine(plot, points, color, width = 1.6, alpha = 1) {
  const { ctx, dpr } = plot;
  if (!points.length) return;
  ctx.save();
  ctx.globalAlpha = alpha;
  ctx.strokeStyle = color;
  ctx.lineWidth = width * dpr;
  ctx.lineJoin = 'miter';
  ctx.beginPath();
  ctx.moveTo(plot.px(points[0].t), plot.py(points[0].v));
  for (let i = 1; i < points.length; i++) {
    const x = plot.px(points[i].t);
    ctx.lineTo(x, plot.py(points[i - 1].v));
    ctx.lineTo(x, plot.py(points[i].v));
  }
  ctx.lineTo(plot.x + plot.w, plot.py(points[points.length - 1].v));
  ctx.stroke();
  ctx.restore();
}

export function dots(plot, points, color, r = 2.4, ring = null) {
  const { ctx, dpr } = plot;
  for (const p of points) {
    const x = plot.px(p.t);
    const y = plot.py(p.v);
    if (x < plot.x - 8 || x > plot.x + plot.w + 8) continue;
    ctx.beginPath();
    ctx.arc(x, y, r * dpr, 0, Math.PI * 2);
    ctx.fillStyle = color;
    ctx.fill();
    if (ring) {
      ctx.strokeStyle = ring;
      ctx.lineWidth = 1.2 * dpr;
      ctx.stroke();
    }
  }
}

/* Min/max envelope: one vertical span per pixel column. */
export function envelopeBand(plot, cols, data, color, alpha = 1) {
  const { ctx, dpr } = plot;
  ctx.save();
  ctx.globalAlpha = alpha;
  ctx.strokeStyle = color;
  ctx.lineWidth = Math.max(1, dpr);
  ctx.beginPath();
  const step = plot.w / cols;
  for (let c = 0; c < cols; c++) {
    const lo = data[c * 2];
    const hi = data[c * 2 + 1];
    if (hi < lo) continue;
    const x = Math.round(plot.x + c * step) + 0.5;
    let ya = plot.py(hi);
    let yb = plot.py(lo);
    if (yb - ya < dpr) {
      const m = (ya + yb) / 2;
      ya = m - dpr / 2;
      yb = m + dpr / 2;
    }
    ctx.moveTo(x, ya);
    ctx.lineTo(x, yb);
  }
  ctx.stroke();
  ctx.restore();
}

export function label(ctx, dpr, x, y, text, color, opts = {}) {
  font(ctx, dpr, opts.size || 10, opts.weight || '');
  ctx.fillStyle = color;
  ctx.textAlign = opts.align || 'left';
  ctx.textBaseline = opts.baseline || 'top';
  ctx.fillText(text, x, y);
  ctx.textAlign = 'left';
  ctx.textBaseline = 'alphabetic';
}

/* Empty-state: a centred hint instead of a blank rectangle. */
export function placeholder(ctx, W, H, dpr, text) {
  clear(ctx, W, H);
  font(ctx, dpr, 11);
  ctx.fillStyle = cssVar('--ink-3');
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  ctx.fillText(text, W / 2, H / 2);
  ctx.textAlign = 'left';
  ctx.textBaseline = 'alphabetic';
}
