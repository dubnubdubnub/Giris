// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * capture.js — one burst capture, shared by three views.
 *
 * CMD_BURST_START fills device SRAM with up to 8192 consecutive samples of ONE
 * slot at the full scan rate, then CMD_BURST_READ pages them back 28 at a time.
 * That single array is what the noise spectrum, the per-sample zoom and the
 * polling-rate comparison all read, so a capture is taken once and broadcast.
 */

import { CMD, BURST_MAX, BURST_PER_RPT, SLOT_UNUSED, slotLabel } from './protocol.js';
import { restingBaseline, findPress } from './dsp.js';

export class CaptureStore extends EventTarget {
  constructor() {
    super();
    this.samples = null;
    this.periodNs = 125000;
    this.slot = 0;
    this.slotName = '';
    this.source = null; /* session name at capture time */
    this.synthetic = false;
    this.busy = false;
    this.status = 'no capture yet';
    this.at = 0;
  }

  get fs() {
    return 1e9 / this.periodNs;
  }
  get periodUs() {
    return this.periodNs / 1000;
  }
  get ready() {
    return !!(this.samples && this.samples.length);
  }

  /* Milliseconds from the start of the capture for sample i. */
  tMs(i) {
    return (i * this.periodNs) / 1e6;
  }

  _say(text) {
    this.status = text;
    this.dispatchEvent(new CustomEvent('status'));
  }

  /*
   * Run a capture. onProgress gets a human string; the whole thing is one
   * await so callers can just disable a button around it.
   */
  async run(session, slot, count, opts = {}) {
    if (this.busy) return null;
    if (!session || session.gone) {
      this._say('no device');
      return null;
    }
    this.busy = true;
    this.dispatchEvent(new CustomEvent('busy'));
    try {
      const want = Math.max(64, Math.min(BURST_MAX, count | 0));
      this._say(opts.prompt || 'capturing…');
      await session.request(CMD.BURST_START, [slot, want & 0xff, (want >> 8) & 0xff]);

      let st = null;
      const deadline = performance.now() + 15000;
      for (;;) {
        await new Promise((r) => setTimeout(r, 40));
        st = await session.request(CMD.BURST_STATUS);
        if (!st) break;
        if (st.state === 2) break;
        if (performance.now() > deadline) break;
        const pct = Math.round((st.captured / want) * 100);
        this._say(`${opts.prompt || 'capturing'} ${pct}%  (${st.captured}/${want})`);
      }
      if (!st || st.state !== 2) {
        this._say('capture did not complete — aborting');
        try {
          await session.request(CMD.BURST_ABORT);
        } catch {
          /* nothing useful to do */
        }
        return null;
      }

      const out = new Uint16Array(st.captured);
      let got = 0;
      for (let off = 0; off < st.captured; off += BURST_PER_RPT) {
        const chunk = await session.request(CMD.BURST_READ, [off & 0xff, (off >> 8) & 0xff]);
        if (!chunk || !chunk.count) break;
        out.set(chunk.samples, chunk.off);
        got = Math.max(got, chunk.off + chunk.count);
        if (off % (BURST_PER_RPT * 24) === 0) this._say(`reading back ${Math.round((got / st.captured) * 100)}%`);
      }

      this.samples = got === out.length ? out : out.subarray(0, got);
      this.periodNs = st.periodNs || 125000;
      this.slot = st.slot;
      this.slotName = slotLabel(session.slotMap[st.slot] ?? SLOT_UNUSED);
      this.source = session.name;
      this.synthetic = session.synthetic;
      this.at = Date.now();
      const kHz = this.fs / 1000;
      this._say(`${this.samples.length} samples · ${kHz.toFixed(2)} kHz · ${this.periodUs.toFixed(0)} µs apart · slot ${this.slot} (${this.slotName})`);
      this.dispatchEvent(new CustomEvent('capture'));
      return this.samples;
    } catch (e) {
      this._say('capture failed: ' + e.message);
      return null;
    } finally {
      this.busy = false;
      this.dispatchEvent(new CustomEvent('busy'));
    }
  }

  /* Analysis shared by the zoom and comparison views. */
  analyse(threshold) {
    if (!this.ready) return null;
    const s = this.samples;
    const baseline = restingBaseline(s);
    const press = findPress(s, baseline, threshold);
    return { baseline, press };
  }

  csv() {
    const lines = ['n,t_us,counts'];
    for (let i = 0; i < this.samples.length; i++) {
      lines.push(`${i},${(i * this.periodNs) / 1000},${this.samples[i]}`);
    }
    return lines.join('\n') + '\n';
  }
}
