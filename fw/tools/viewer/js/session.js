// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * session.js — one connected board.
 *
 * Owns the tag/response bookkeeping, the sequence-gap accounting, the sample
 * ring, and the per-key travel tracker. Transport-agnostic: it is handed either
 * a HidTransport or a SynthTransport and cannot tell the difference.
 *
 * More than one of these can exist at once — two halves of a split plugged into
 * the same machine is the normal case, not an exotic one.
 */

import {
  RPT_SIZE,
  NUM_KEYS,
  STREAM_HDR,
  FRAME_BYTES,
  SCAN_HZ_DEFAULT,
  KEYS_PRESS_DELTA,
  KEYS_RELEASE_DELTA,
  SLOT_MAP_FALLBACK,
  CMD,
  RSP,
  parse,
  parseStreamHeader,
} from './protocol.js';
import { Ring, Histogram } from './dsp.js';

/* 8 s of history at the full 8 kHz scan rate. 6 tracks x 64 Ki x 2 B ~ 786 KB,
 * which is nothing, and it means the envelope trace can show a long window
 * without ever having thrown a sample away. */
const RING_CAP = 65536;

/* Per-key travel, derived host-side. keys.c is direction-agnostic on purpose —
 * a TMR2615 output moves either way depending on which pole faces it — so this
 * is too: rest is learned, and travel is |v - rest| normalised by the deepest
 * excursion ever seen on that key. */
const SPAN_FLOOR = 500;

class KeyTracker {
  constructor(n) {
    this.n = n;
    this.rest = new Float64Array(n);
    this.span = new Float64Array(n).fill(SPAN_FLOOR);
    this.value = new Uint16Array(n);
    this.travel = new Float64Array(n);
    this.pressed = new Uint8Array(n);
    this.seen = false;
  }

  update(values) {
    if (!this.seen) {
      for (let k = 0; k < this.n; k++) this.rest[k] = values[k];
      this.seen = true;
    }
    for (let k = 0; k < this.n; k++) {
      const v = values[k];
      this.value[k] = v;
      const d = v - this.rest[k];
      const mag = Math.abs(d);
      /* Only track the baseline while the key is demonstrably not moving, or a
       * long hold would quietly redefine "rest" as "bottomed out". */
      if (mag < KEYS_RELEASE_DELTA) this.rest[k] += d * 0.0004;
      if (mag > this.span[k]) this.span[k] = mag;
      this.travel[k] = Math.min(1, mag / this.span[k]);
      if (this.pressed[k]) {
        if (mag < KEYS_RELEASE_DELTA) this.pressed[k] = 0;
      } else if (mag >= KEYS_PRESS_DELTA) {
        this.pressed[k] = 1;
      }
    }
  }

  /* Where actuation sits on the 0..1 travel bar, per key. */
  actuationFrac(k) {
    return Math.min(0.95, KEYS_PRESS_DELTA / Math.max(this.span[k], 1));
  }
}

export class Session extends EventTarget {
  constructor(transport) {
    super();
    this.transport = transport;
    this.id = transport.id;
    this.name = transport.name;
    this.synthetic = !!transport.synthetic;
    this.info = null;
    this.gone = false;

    this.ring = new Ring(RING_CAP, NUM_KEYS);
    this.keys = new KeyTracker(NUM_KEYS);

    this._tag = 1;
    this._pending = new Map();
    this._out = new Uint8Array(RPT_SIZE);

    /* health */
    this.lastSeq = null;
    this.seqGaps = 0;
    this.frameGaps = 0;
    this.reports = 0;
    this.frames = 0;
    this.streamOn = false;
    this.decim = 1;
    this._rptWindow = 0;
    this._rptAt = performance.now();
    this.reportRate = 0;
    this.frameRate = 0;
    this._frmWindow = 0;

    /* timing, for the histogram panel */
    this.lastFrameIdx = null;
    this.lastHostMs = null;
    /* device frame period, in units of the 125 us scan tick — one bin per tick */
    this.periodHist = new Histogram(32, 0.5, 32.5);
    /* host inter-arrival between input reports, in ms */
    this.arrivalHist = new Histogram(80, 0, 8);

    /* CSV recorder */
    this.recording = false;
    this.recorded = [];

    transport.onreport = (d) => this._onReport(d);
    transport.ondisconnect = () => this._onGone();
  }

  get slotMap() {
    return this.info ? this.info.slotMap : SLOT_MAP_FALLBACK;
  }
  get scanHz() {
    return this.info ? this.info.scanHz : SCAN_HZ_DEFAULT;
  }
  get numKeys() {
    return this.info ? Math.min(this.info.numKeys, NUM_KEYS) : NUM_KEYS;
  }

  async open() {
    await this.transport.open();
    this.info = await this.request(CMD.INFO);
    this.dispatchEvent(new CustomEvent('info'));
    return this.info;
  }

  async close() {
    try {
      if (this.streamOn) await this.setStream(false, this.decim);
    } catch {
      /* going away anyway */
    }
    await this.transport.close();
    this._onGone();
  }

  _onGone() {
    if (this.gone) return;
    this.gone = true;
    for (const [, cb] of this._pending) cb(null, new Error('device went away'));
    this._pending.clear();
    this.dispatchEvent(new CustomEvent('gone'));
  }

  /* ---------------------------------------------------------------- send */

  send(cmd, args = []) {
    const t = (this._tag++ & 0x7f) || 1;
    this._out.fill(0);
    this._out[0] = cmd;
    this._out[1] = t;
    for (let i = 0; i < args.length; i++) this._out[2 + i] = args[i] & 0xff;
    return { tag: t, promise: this.transport.write(this._out) };
  }

  request(cmd, args = [], timeoutMs = 2000) {
    if (this.gone) return Promise.reject(new Error('device went away'));
    const { tag, promise } = this.send(cmd, args);
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this._pending.delete(tag);
        reject(new Error('timeout waiting for a reply to cmd 0x' + cmd.toString(16)));
      }, timeoutMs);
      this._pending.set(tag, (v, err) => {
        clearTimeout(timer);
        err ? reject(err) : resolve(v);
      });
      promise.catch((e) => {
        clearTimeout(timer);
        this._pending.delete(tag);
        reject(e);
      });
    });
  }

  /* ------------------------------------------------------------- receive */

  _onReport(d) {
    const msg = d.getUint8(0);
    const tag = d.getUint8(1);
    const seq = d.getUint16(2, true);

    /* One u16 counter across every IN report of every type, so a single gap
     * catches a lost stream frame, burst chunk or response alike. */
    if (this.lastSeq !== null && ((this.lastSeq + 1) & 0xffff) !== seq) this.seqGaps++;
    this.lastSeq = seq;
    this.reports++;
    this._rptWindow++;

    if (msg === RSP.STREAM) {
      this._onStream(d);
      return;
    }

    const parsed = parse(msg, d);
    if (msg === RSP.INFO && parsed && parsed.numKeys > 0 && parsed.numSlots > 0) {
      /* CMD_BOOTLOADER also answers RSP_INFO, but with only the version set;
       * adopting that would zero the slot map. Only a complete one counts. */
      this.info = parsed;
      this.dispatchEvent(new CustomEvent('info'));
    }
    const cb = this._pending.get(tag);
    if (cb) {
      this._pending.delete(tag);
      cb(parsed);
    }
  }

  _onStream(d) {
    const h = parseStreamHeader(d);
    if (h.dropped) this.frameGaps++;
    this.decim = h.decim || this.decim;
    const now = performance.now();
    const scan = this.scanHz;

    if (this.lastHostMs !== null) {
      const dt = now - this.lastHostMs;
      if (dt >= 0 && dt < 8) this.arrivalHist.add(dt);
    }
    this.lastHostMs = now;

    const vals = this._vals || (this._vals = new Uint16Array(NUM_KEYS));
    for (let i = 0; i < h.count; i++) {
      const base = STREAM_HDR + i * FRAME_BYTES;
      const frame = d.getUint32(base, true);
      if (this.lastFrameIdx !== null && frame <= this.lastFrameIdx) continue;
      if (this.lastFrameIdx !== null) {
        const gap = frame - this.lastFrameIdx;
        if (gap > 0 && gap < 33) this.periodHist.add(gap);
      }
      this.lastFrameIdx = frame;

      for (let k = 0; k < NUM_KEYS; k++) vals[k] = d.getUint16(base + 4 + 2 * k, true);
      this.ring.push(frame, now, vals);
      this.keys.update(vals);
      this.frames++;
      this._frmWindow++;

      if (this.recording) {
        let row = (frame / scan).toFixed(6);
        for (let k = 0; k < this.numKeys; k++) row += ',' + vals[k];
        this.recorded.push(row);
      }
    }
  }

  /* Called once per animation frame by the app; keeps the rate readouts sane
   * without adding work to the 8 kHz path. */
  tick() {
    const now = performance.now();
    const dt = now - this._rptAt;
    if (dt >= 500) {
      this.reportRate = (this._rptWindow / dt) * 1000;
      this.frameRate = (this._frmWindow / dt) * 1000;
      this._rptWindow = 0;
      this._frmWindow = 0;
      this._rptAt = now;
    }
  }

  /* ------------------------------------------------------------- control */

  async setStream(on, decim = 1) {
    const d = Math.max(1, Math.min(255, decim | 0));
    await this.request(CMD.STREAM_SET, [on ? 1 : 0, d]);
    this.streamOn = !!on;
    this.decim = d;
    if (on) {
      this.lastFrameIdx = null;
      this.lastHostMs = null;
    }
    this.dispatchEvent(new CustomEvent('stream'));
  }

  snapshot() {
    return this.request(CMD.SNAPSHOT);
  }

  bootloader() {
    return this.request(CMD.BOOTLOADER, [], 3000);
  }

  /* ------------------------------------------------------------ recorder */

  startRecording() {
    this.recorded = [];
    this.recording = true;
  }
  stopRecording() {
    this.recording = false;
  }
  csv() {
    const cols = ['t_s'];
    for (let k = 0; k < this.numKeys; k++) cols.push('key' + k);
    return cols.join(',') + '\n' + this.recorded.join('\n') + '\n';
  }
}
