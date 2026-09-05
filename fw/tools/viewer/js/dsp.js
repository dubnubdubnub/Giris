// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * dsp.js — buffers and maths. No DOM in here.
 */

/* A fixed-capacity ring of frames: one timestamp track plus N value tracks.
 * Written at up to 8 kHz, so nothing in the write path allocates. */
export class Ring {
  constructor(capacity, tracks) {
    this.cap = capacity;
    this.tracks = tracks;
    this.frame = new Float64Array(capacity); /* device frame index */
    this.host = new Float64Array(capacity); /* performance.now() at arrival */
    this.v = [];
    for (let i = 0; i < tracks; i++) this.v.push(new Uint16Array(capacity));
    this.n = 0;
    this.w = 0;
  }

  push(frameIndex, hostMs, values) {
    const w = this.w;
    this.frame[w] = frameIndex;
    this.host[w] = hostMs;
    for (let k = 0; k < this.tracks; k++) this.v[k][w] = values[k];
    this.w = (w + 1) % this.cap;
    if (this.n < this.cap) this.n++;
  }

  clear() {
    this.n = 0;
    this.w = 0;
  }

  /* Index of the i-th most recent sample (0 = newest). */
  at(i) {
    return (this.w - 1 - i + this.cap * 2) % this.cap;
  }

  /* Absolute ring index of the oldest sample in a window of `count`. */
  start(count) {
    return (this.w - count + this.cap * 2) % this.cap;
  }

  latest(track) {
    return this.n ? this.v[track][this.at(0)] : 0;
  }
}

/*
 * Oscilloscope column reduction: for each of `cols` pixel columns spanning the
 * newest `count` samples, the min and max of every sample that falls in it.
 * Nothing is thrown away — a 12 us glitch still paints a full-height column.
 *
 * out is a Float32Array of length cols*2 per track, laid out [min,max,min,max...]
 */
export function envelope(ring, track, count, cols, out) {
  const n = Math.min(count, ring.n);
  if (n <= 0 || cols <= 0) return 0;
  const src = ring.v[track];
  const cap = ring.cap;
  const start = (ring.w - n + cap * 2) % cap;
  let si = 0;
  for (let c = 0; c < cols; c++) {
    /* half-open sample range for this column */
    const a = Math.floor((c * n) / cols);
    const b = Math.max(a + 1, Math.floor(((c + 1) * n) / cols));
    let lo = 65535;
    let hi = 0;
    for (si = a; si < b; si++) {
      const v = src[(start + si) % cap];
      if (v < lo) lo = v;
      if (v > hi) hi = v;
    }
    out[c * 2] = lo;
    out[c * 2 + 1] = hi;
  }
  return n;
}

/* Min/max of the newest `count` samples on one track, in one pass. */
export function windowRange(ring, track, count) {
  const n = Math.min(count, ring.n);
  const src = ring.v[track];
  const cap = ring.cap;
  const start = (ring.w - n + cap * 2) % cap;
  let lo = 65535;
  let hi = 0;
  for (let i = 0; i < n; i++) {
    const v = src[(start + i) % cap];
    if (v < lo) lo = v;
    if (v > hi) hi = v;
  }
  return n ? { lo, hi, n } : { lo: 0, hi: 0, n: 0 };
}

/* ------------------------------------------------------------ resampling */

/*
 * What a slower poller SEES. Not a filter: a poller takes whatever the sensor
 * reads at its own sample instants and holds it until the next one, which is
 * precisely why it is late. `n` is the decimation factor, `phase` the offset of
 * its first sample within one of its own periods (0..n-1) — a real 1 kHz poller
 * has no phase relationship to the 8 kHz scan, so the phase is arbitrary and
 * the honest thing is to show that it matters.
 */
export function decimate(samples, n, phase = 0) {
  const p = ((phase % n) + n) % n;
  const out = [];
  for (let i = p; i < samples.length; i += n) out.push({ i, v: samples[i] });
  return out;
}

/*
 * First sample index at which |v - baseline| >= threshold, or -1.
 * Direction-agnostic, exactly like keys.c: a TMR2615 moves up or down with the
 * magnet depending on which pole faces it.
 */
export function crossIndex(samples, baseline, threshold, from = 0) {
  for (let i = from; i < samples.length; i++) {
    if (Math.abs(samples[i] - baseline) >= threshold) return i;
  }
  return -1;
}

/* Same, but only looking at the instants a poller of decimation n/phase p
 * actually samples. Returns the index into the ORIGINAL array. */
export function crossIndexDecimated(samples, n, phase, baseline, threshold, from = 0) {
  const p = ((phase % n) + n) % n;
  let i = p;
  while (i < from) i += n;
  for (; i < samples.length; i += n) {
    if (Math.abs(samples[i] - baseline) >= threshold) return i;
  }
  return -1;
}

/* Worst and mean added latency over every possible phase of a slower poller,
 * in samples of the fast clock. ref is the 8 kHz detection index. */
export function phaseSweep(samples, n, baseline, threshold, ref) {
  let worst = 0;
  let sum = 0;
  let count = 0;
  for (let p = 0; p < n; p++) {
    const i = crossIndexDecimated(samples, n, p, baseline, threshold);
    if (i < 0) continue;
    const d = i - ref;
    if (d > worst) worst = d;
    sum += d;
    count++;
  }
  return { worst, mean: count ? sum / count : 0, phases: count };
}

/*
 * Resting baseline of a burst: the median of the first quiet stretch. Median,
 * not mean, so a press that starts early does not drag it.
 */
export function restingBaseline(samples, span = 256) {
  const n = Math.min(span, samples.length);
  if (!n) return 0;
  const a = Array.from(samples.slice(0, n)).sort((x, y) => x - y);
  return a[a.length >> 1];
}

/*
 * Locate a keypress inside a burst: the first crossing, then walk back to where
 * the signal actually left the baseline (within 3 counts) so the plot starts on
 * the flat, and forward to the far side of the event.
 */
export function findPress(samples, baseline, threshold) {
  const cross = crossIndex(samples, baseline, threshold);
  if (cross < 0) return null;
  let a = cross;
  while (a > 0 && Math.abs(samples[a] - baseline) > 3) a--;
  let peak = cross;
  let best = 0;
  for (let i = cross; i < samples.length; i++) {
    const d = Math.abs(samples[i] - baseline);
    if (d > best) {
      best = d;
      peak = i;
    }
    /* stop once it has clearly come back to rest */
    if (i > cross + 16 && d < threshold * 0.25) break;
  }
  let b = peak;
  while (b < samples.length - 1 && Math.abs(samples[b] - baseline) > 3) b++;
  return { cross, start: a, peak, end: b, depth: best };
}

/* ------------------------------------------------------------- histogram */

export class Histogram {
  constructor(bins, lo, hi) {
    this.bins = new Uint32Array(bins);
    this.lo = lo;
    this.hi = hi;
    this.total = 0;
    this.under = 0;
    this.over = 0;
    this.min = Infinity;
    this.max = -Infinity;
    this.sum = 0;
    this.sum2 = 0;
  }
  add(x) {
    this.total++;
    this.sum += x;
    this.sum2 += x * x;
    if (x < this.min) this.min = x;
    if (x > this.max) this.max = x;
    const b = Math.floor(((x - this.lo) / (this.hi - this.lo)) * this.bins.length);
    if (b < 0) this.under++;
    else if (b >= this.bins.length) this.over++;
    else this.bins[b]++;
  }
  reset() {
    this.bins.fill(0);
    this.total = this.under = this.over = 0;
    this.min = Infinity;
    this.max = -Infinity;
    this.sum = this.sum2 = 0;
  }
  get mean() {
    return this.total ? this.sum / this.total : 0;
  }
  get sd() {
    if (this.total < 2) return 0;
    const m = this.mean;
    return Math.sqrt(Math.max(0, this.sum2 / this.total - m * m));
  }
  /* value below which `q` of the mass sits, by bin interpolation */
  quantile(q) {
    if (!this.total) return 0;
    const want = q * this.total;
    let acc = this.under;
    if (acc >= want) return this.lo;
    for (let i = 0; i < this.bins.length; i++) {
      if (acc + this.bins[i] >= want) {
        const frac = this.bins[i] ? (want - acc) / this.bins[i] : 0;
        return this.lo + ((i + frac) / this.bins.length) * (this.hi - this.lo);
      }
      acc += this.bins[i];
    }
    return this.hi;
  }
}

/* ------------------------------------------------------------------- fft */

/* In-place iterative radix-2. re/im must be power-of-two length. */
export function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) {
      const tr = re[i];
      re[i] = re[j];
      re[j] = tr;
      const ti = im[i];
      im[i] = im[j];
      im[j] = ti;
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    const wr = Math.cos(ang);
    const wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1;
      let ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const ur = re[i + k];
        const ui = im[i + k];
        const vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
        const vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
        re[i + k] = ur + vr;
        im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr;
        im[i + k + len / 2] = ui - vi;
        const ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

/* Hann-windowed, mean-removed magnitude spectrum of the largest power-of-two
 * prefix of `samples`. Returns { mag, binHz, n }. */
export function spectrum(samples, fs) {
  let n = 1;
  while (n * 2 <= samples.length) n *= 2;
  if (n < 64) return null;
  let mean = 0;
  for (let i = 0; i < n; i++) mean += samples[i];
  mean /= n;
  const re = new Float64Array(n);
  const im = new Float64Array(n);
  for (let i = 0; i < n; i++) {
    const w = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (n - 1));
    re[i] = (samples[i] - mean) * w;
  }
  fft(re, im);
  const bins = n >> 1;
  const mag = new Float64Array(bins);
  for (let i = 1; i < bins; i++) mag[i] = Math.hypot(re[i], im[i]) / n;
  return { mag, binHz: fs / n, n };
}
