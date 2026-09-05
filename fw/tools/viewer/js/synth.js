// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * synth.js — a synthetic Giris, for developing and demonstrating every view
 * with no hardware attached.
 *
 * It is NOT a mock of the viewer's internals: it implements the wire protocol
 * and hands back real 64-byte reports, so protocol.js's parsers, the tag/seq
 * bookkeeping and the burst read loop are all genuinely exercised. Swap the
 * transport and nothing above it changes.
 *
 * The waveform is modelled on tools/../logs/giris-*.csv, captured off the real
 * board: rest around 1875..2065 counts, ~780 counts of travel, counts falling
 * as the magnet approaches, a ~13 ms descent and a damped bottom-out ring at a
 * few hundred hertz. That ring is deliberate — it is exactly the sort of detail
 * that survives an 8 kHz scan and is destroyed by a 1 kHz one.
 *
 * Everything here is generated. The UI says so.
 */

import {
  RPT_SIZE,
  PROTO_VERSION,
  NUM_KEYS,
  NUM_SLOTS,
  FRAMES_PER_RPT,
  FRAME_BYTES,
  STREAM_HDR,
  BURST_MAX,
  BURST_PER_RPT,
  SCAN_HZ_DEFAULT,
  SLOT_MAP_FALLBACK,
  SLOT_VBUS_DIV,
  SLOT_UNUSED,
  CMD,
  RSP,
} from './protocol.js';

const SCAN_HZ = SCAN_HZ_DEFAULT;
const FRAME_MS = 1000 / SCAN_HZ; /* 0.125 ms */

/* Measured resting counts and full travel, per key. */
const REST = [1875, 1947, 1941, 1940, 2063, 2058];
const DEPTH = [790, 802, 771, 812, 783, 796];

/* Press shaping, in milliseconds. */
const DOWN_MS = 13; /* rest -> bottomed */
const UP_MS = 26; /* bottomed -> rest, slower than the press */
const RING_HZ = 317; /* bottom-out ring: above 1 kHz Nyquist, on purpose */
const RING_TAU_MS = 3.1;
const RING_AMPL = 0.014; /* of full travel, so ~11 counts */

function hash2(a, b) {
  let h = (Math.imul(a | 0, 374761393) + Math.imul(b | 0, 668265263)) | 0;
  h = Math.imul(h ^ (h >>> 13), 1274126177) | 0;
  return ((h ^ (h >>> 16)) >>> 0) / 4294967296;
}

/* Triangular noise: cheap, deterministic, and close enough to the real ~1 LSB
 * of ADC + sensor noise that the spectrum plot has something honest to show. */
function noise(track, frame, rms) {
  return (hash2(track * 131 + 7, frame) + hash2(track * 977 + 13, frame ^ 0x5bd1) - 1) * rms * 1.7;
}

const smoothstep = (u) => (u <= 0 ? 0 : u >= 1 ? 1 : u * u * (3 - 2 * u));

class PressEvent {
  constructor(key, startFrame, holdFrames) {
    this.key = key;
    this.start = startFrame;
    this.down = Math.round(DOWN_MS / FRAME_MS);
    this.up = Math.round(UP_MS / FRAME_MS);
    /* null = still held: the user has the key down and has not let go */
    this.hold = holdFrames === null ? null : Math.max(1, holdFrames);
    /* small per-press variation so no two presses are identical */
    this.speed = 0.8 + hash2(key, startFrame) * 0.5;
    this.reach = 0.86 + hash2(key + 31, startFrame) * 0.14;
  }

  releaseAt(frame) {
    if (this.hold === null) this.hold = Math.max(1, frame - this.start - this.down);
  }

  get finished() {
    return this.hold !== null;
  }

  endFrame() {
    return this.hold === null ? Infinity : this.start + this.down + this.hold + this.up;
  }

  /* Fractional travel at a frame index: 0 at rest, ~1 bottomed out. */
  travel(frame) {
    const t = frame - this.start;
    if (t <= 0) return 0;
    const down = this.down * this.speed;
    if (t < down) return smoothstep(t / down) * this.reach;

    const ringMs = (t - down) * FRAME_MS;
    const ring =
      RING_AMPL * Math.exp(-ringMs / RING_TAU_MS) * Math.sin(2 * Math.PI * RING_HZ * (ringMs / 1000));

    if (this.hold === null) return this.reach + ring;
    if (t < down + this.hold) return this.reach + ring;

    const u = (t - down - this.hold) / (this.up * this.speed);
    if (u >= 1) return 0;
    return (1 - smoothstep(u)) * this.reach + ring * (1 - u);
  }
}

export class SynthTransport {
  constructor(opts = {}) {
    this.kind = 'synth';
    this.boardId = opts.board || '1';
    this.name = 'Giris (synthetic)';
    this.id = 'synth:' + (SynthTransport._n = (SynthTransport._n || 0) + 1);
    this.synthetic = true;
    this.onreport = null;
    this.ondisconnect = null;

    this.t0 = performance.now();
    this.seq = 0;
    this.events = [];
    this.held = new Map(); /* key -> PressEvent while a pointer is down */
    this.autoDemo = true;
    this.nextAuto = 40; /* frames */

    this.streamOn = false;
    this.decim = 1;
    this.lastStreamed = 0;
    this.streamGap = false;

    this.burstState = 0;
    this.burstSlot = 0;
    this.burstWant = 0;
    this.burstStart = 0;
    this.burstBuf = new Uint16Array(BURST_MAX);
    this.burstFilled = 0;

    this._buf = new Uint8Array(RPT_SIZE);
    this._view = new DataView(this._buf.buffer);
    this._timer = null;
  }

  /* ------------------------------------------------------------ lifecycle */

  async open() {
    this._timer = setInterval(() => this._service(), 4);
  }

  async close() {
    clearInterval(this._timer);
    this._timer = null;
  }

  frameNow() {
    return Math.floor((performance.now() - this.t0) / FRAME_MS);
  }

  /* ---------------------------------------------------------------- model */

  press(key, holdMs = null) {
    if (key < 0 || key >= NUM_KEYS) return;
    const ev = new PressEvent(key, this.frameNow(), holdMs === null ? null : Math.round(holdMs / FRAME_MS));
    this.events.push(ev);
    if (holdMs === null) {
      const prev = this.held.get(key);
      if (prev) prev.releaseAt(this.frameNow());
      this.held.set(key, ev);
    }
    this._prune();
  }

  release(key) {
    const ev = this.held.get(key);
    if (ev) {
      ev.releaseAt(this.frameNow());
      this.held.delete(key);
    }
  }

  /* Schedule a press that starts `delayMs` from now — used so a burst capture
   * of a given key always contains a keypress to look at. */
  schedulePress(key, delayMs, holdMs = 70) {
    const start = this.frameNow() + Math.round(delayMs / FRAME_MS);
    this.events.push(new PressEvent(key, start, Math.round(holdMs / FRAME_MS)));
  }

  _prune() {
    const cutoff = this.frameNow() - SCAN_HZ * 4;
    this.events = this.events.filter((e) => !e.finished || e.endFrame() > cutoff);
  }

  _autoDemo(frame) {
    if (!this.autoDemo) return;
    if (frame < this.nextAuto) return;
    /* a rolling roll across the four typing keys, with the odd Fn tap */
    const order = [1, 3, 4, 5, 4, 3, 1, 2];
    const k = order[Math.floor(hash2(frame, 99) * order.length) % order.length];
    this.events.push(new PressEvent(k, frame + 2, Math.round((40 + hash2(frame, 5) * 90) / FRAME_MS)));
    this.nextAuto = frame + Math.round((260 + hash2(frame, 17) * 520) / FRAME_MS);
  }

  keyValue(key, frame) {
    let travel = 0;
    for (const e of this.events) {
      if (e.key !== key) continue;
      const t = e.travel(frame);
      if (t > travel) travel = t;
    }
    const drift = 1.6 * Math.sin((frame / SCAN_HZ) * 2 * Math.PI * 0.07 + key * 1.7);
    return Math.max(0, Math.min(4095, Math.round(REST[key] - travel * DEPTH[key] + drift + noise(key, frame, 1.1))));
  }

  slotValue(slot, frame) {
    const m = SLOT_MAP_FALLBACK[slot];
    if (m < NUM_KEYS) return this.keyValue(m, frame);
    if (m === SLOT_VBUS_DIV) return Math.round(3166 + noise(40, frame, 0.8));
    if (m === SLOT_UNUSED) {
      /* a floating header pin: mains pickup and a lot more noise */
      const hum = 26 * Math.sin((frame / SCAN_HZ) * 2 * Math.PI * 60);
      return Math.max(0, Math.min(4095, Math.round(2048 + hum + noise(41, frame, 9))));
    }
    /* the discarded dummy sits wherever the previous conversion left the cap */
    return Math.round(1900 + noise(42 + slot, frame, 2.5));
  }

  /* ------------------------------------------------------------- reporting */

  _begin(msg, tag) {
    this._buf.fill(0);
    this._buf[0] = msg;
    this._buf[1] = tag;
    this._view.setUint16(2, this.seq & 0xffff, true);
    this.seq = (this.seq + 1) & 0xffff;
    return this._view;
  }

  _emit() {
    if (this.onreport) this.onreport(this._view);
  }

  _sendInfo(tag) {
    const d = this._begin(RSP.INFO, tag);
    d.setUint8(4, PROTO_VERSION);
    d.setUint8(5, NUM_KEYS);
    d.setUint8(6, NUM_SLOTS);
    d.setUint8(7, 12);
    d.setUint16(8, SCAN_HZ, true);
    for (let i = 0; i < NUM_SLOTS; i++) d.setUint8(10 + i, SLOT_MAP_FALLBACK[i]);
    d.setUint32(20, 1573, true); /* 6.144 counts/Gs * 256 */
    d.setUint32(24, 0x53594e54, true); /* "SYNT" — obviously not a real build */
    const seqOrder = [3, 2, 1, 0, 3];
    for (let i = 0; i < 5; i++) d.setUint8(28 + i, seqOrder[i]);
    const b = this.boardId === '2' ? 2 : 1;
    d.setUint32(33, 0x5e00_0000 + b, true);
    d.setUint32(37, 0x54_4e59_53 & 0xffffffff, true);
    d.setUint32(41, 0x0000_53 + b * 0x100, true);
    d.setUint16(45, 0x5e00 + b, true); /* uid_tag */
    d.setUint8(47, 0x1b); /* link sense: a healthy pair */
    d.setUint8(48, 0x04); /* last reset: software — i.e. after a DFU flash */
    d.setUint16(49, 0, true);
    d.setUint16(51, 0, true);
    d.setUint8(53, 0); /* run */
    d.setUint8(54, 1); /* remote wakeup granted */
    d.setUint16(55, 0, true);
    d.setUint16(57, 0, true);
    d.setUint8(59, 0); /* keyboard output off at boot, as the firmware does */
    let own = 0;
    const f = this.frameNow();
    for (let k = 0; k < NUM_KEYS; k++) if (Math.abs(this.keyValue(k, f) - REST[k]) >= 150) own |= 1 << k;
    d.setUint8(60, own);
    d.setUint8(61, 0);
    d.setUint8(62, 0);
    this._emit();
  }

  _sendSnapshot(tag) {
    const d = this._begin(RSP.SNAPSHOT, tag);
    const f = this.frameNow();
    d.setUint32(4, f, true);
    for (let i = 0; i < NUM_SLOTS; i++) d.setUint16(8 + 2 * i, this.slotValue(i, f), true);
    d.setUint32(48, 0, true); /* phase errors */
    d.setUint32(52, 0, true); /* tx dropped */
    d.setUint8(56, 0);
    d.setUint32(57, 0, true);
    d.setUint8(61, 2); /* even seqlock: no write in progress */
    this._emit();
  }

  _sendBurstStatus(tag) {
    this._serviceBurst();
    const d = this._begin(RSP.BURST_STATUS, tag);
    d.setUint8(4, this.burstState);
    d.setUint8(5, this.burstSlot);
    d.setUint16(6, this.burstFilled, true);
    d.setUint32(8, Math.round(1e9 / SCAN_HZ), true); /* 125000 ns */
    this._emit();
  }

  _sendBurstData(tag, off) {
    const d = this._begin(RSP.BURST_DATA, tag);
    let n = 0;
    if (off < this.burstFilled) n = Math.min(BURST_PER_RPT, this.burstFilled - off);
    d.setUint16(4, off, true);
    d.setUint8(6, n);
    for (let i = 0; i < n; i++) d.setUint16(7 + 2 * i, this.burstBuf[off + i], true);
    this._emit();
  }

  _sendError(tag, cmd) {
    const d = this._begin(RSP.ERROR, tag);
    d.setUint8(4, cmd);
    this._emit();
  }

  /* ------------------------------------------------------------- commands */

  write(bytes) {
    const cmd = bytes[0];
    const tag = bytes[1];
    /* A real device answers from its own main loop; a microtask is the closest
     * thing to "not synchronously inside the caller's sendReport". */
    Promise.resolve().then(() => this._dispatch(cmd, tag, bytes));
    return Promise.resolve();
  }

  _dispatch(cmd, tag, b) {
    switch (cmd) {
      case CMD.INFO:
        this._sendInfo(tag);
        break;
      case CMD.STREAM_SET:
        this.streamOn = b[2] !== 0;
        this.decim = b[3] || 1;
        this.lastStreamed = 0;
        this.streamGap = false;
        this._sendInfo(tag); /* firmware answers STREAM_SET with RSP_INFO */
        break;
      case CMD.SNAPSHOT:
        this._sendSnapshot(tag);
        break;
      case CMD.BURST_START: {
        this.burstSlot = b[2] < NUM_SLOTS ? b[2] : 0;
        let want = b[3] | (b[4] << 8);
        if (want === 0 || want > BURST_MAX) want = BURST_MAX;
        this.burstWant = want;
        this.burstFilled = 0;
        this.burstStart = this.frameNow();
        this.burstState = 1;
        /* Guarantee the capture has a keypress in it — this is a demo device,
         * and an empty burst teaches nobody anything. Real presses still land. */
        const key = SLOT_MAP_FALLBACK[this.burstSlot];
        if (key < NUM_KEYS) {
          const win = want * FRAME_MS;
          this.schedulePress(key, Math.min(win * 0.28, 240), 90);
        }
        this._sendBurstStatus(tag);
        break;
      }
      case CMD.BURST_STATUS:
        this._sendBurstStatus(tag);
        break;
      case CMD.BURST_READ:
        this._sendBurstData(tag, b[2] | (b[3] << 8));
        break;
      case CMD.BURST_ABORT:
        this.burstState = 0;
        this.burstFilled = 0;
        this._sendBurstStatus(tag);
        break;
      case CMD.BOOTLOADER: {
        /* The firmware acknowledges with an RSP_INFO carrying only the version
         * and then vanishes. Reproduce both halves of that. */
        const d = this._begin(RSP.INFO, tag);
        d.setUint8(4, PROTO_VERSION);
        this._emit();
        setTimeout(() => this.ondisconnect && this.ondisconnect(), 400);
        break;
      }
      default:
        this._sendError(tag, cmd);
    }
  }

  /* -------------------------------------------------------------- service */

  _serviceBurst() {
    if (this.burstState !== 1) return;
    const now = this.frameNow();
    const target = Math.min(this.burstWant, Math.max(0, now - this.burstStart));
    for (let i = this.burstFilled; i < target; i++) {
      this.burstBuf[i] = this.slotValue(this.burstSlot, this.burstStart + i);
    }
    this.burstFilled = target;
    if (this.burstFilled >= this.burstWant) this.burstState = 2;
  }

  _service() {
    const now = this.frameNow();
    this._autoDemo(now);
    this._serviceBurst();
    if (!this.streamOn) {
      this.lastStreamed = now;
      return;
    }
    if (!this.lastStreamed) this.lastStreamed = now - this.decim;

    /* Never try to make up an unbounded backlog: a backgrounded tab would come
     * back and emit minutes of frames. Skip forward and flag the gap, which is
     * exactly what the firmware's tx ring does when the host stops polling. */
    const maxFrames = 4096;
    if (now - this.lastStreamed > maxFrames * this.decim) {
      this.lastStreamed = now - maxFrames * this.decim;
      this.streamGap = true;
    }

    let d = null;
    let n = 0;
    let emitted = 0;
    for (let f = this.lastStreamed + this.decim; f <= now; f += this.decim) {
      if (!d) d = this._begin(RSP.STREAM, 0);
      const base = STREAM_HDR + n * FRAME_BYTES;
      d.setUint32(base, f, true);
      for (let k = 0; k < NUM_KEYS; k++) d.setUint16(base + 4 + 2 * k, this.keyValue(k, f), true);
      n++;
      this.lastStreamed = f;
      if (n === FRAMES_PER_RPT) {
        d.setUint8(4, n);
        d.setUint8(5, this.streamGap ? 1 : 0);
        d.setUint16(6, this.decim, true);
        this.streamGap = false;
        this._emit();
        emitted += n;
        d = null;
        n = 0;
      }
    }
    if (d) {
      d.setUint8(4, n);
      d.setUint8(5, this.streamGap ? 1 : 0);
      d.setUint16(6, this.decim, true);
      this.streamGap = false;
      this._emit();
      emitted += n;
    }
    if (emitted) this._prune();
  }
}
