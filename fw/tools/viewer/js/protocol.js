// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * protocol.js — a literal mirror of fw/include/protocol.h.
 *
 * Every offset in this file is quoted from the header in the comment beside it.
 * If the two ever disagree, the header wins: it is what the firmware compiles
 * against, this is only what the browser believes.
 *
 * Transport: 64-byte reports, NO report IDs, so WebHID uses reportId 0 in both
 * directions. Vendor usage page 0xFF60 / usage 0x61.
 */

export const RPT_SIZE = 64;
export const PROTO_VERSION = 7;

export const HDR_LEN = 4; /* [0]=msg [1]=tag [2..3]=seq */
export const NUM_SLOTS = 10; /* PROTO_NUM_SLOTS: 2 mux banks x 5 conversions */
export const NUM_KEYS = 6; /* PROTO_NUM_KEYS */
export const SEQ_LEN = 5; /* PROTO_SEQ_LEN */

export const STREAM_HDR = 8; /* PROTO_STREAM_HDR */
export const FRAME_BYTES = 16; /* PROTO_FRAME_BYTES: u32 frame + 6 x u16 */
export const FRAMES_PER_RPT = 3; /* PROTO_FRAMES_PER_RPT */

export const BURST_MAX = 8192; /* PROTO_BURST_MAX */
export const BURST_PER_RPT = 28; /* PROTO_BURST_PER_RPT */

export const SCAN_HZ_DEFAULT = 8000; /* ADC_SCAN_HZ, until RSP_INFO says otherwise */

/* keys.h — counts away from the resting baseline, with hysteresis. */
export const KEYS_PRESS_DELTA = 150;
export const KEYS_RELEASE_DELTA = 80;

export const CMD = {
  INFO: 0x01,
  STREAM_SET: 0x02, /* [2]=on/off, [3]=decimation 1..255 */
  SNAPSHOT: 0x03,
  BURST_START: 0x04, /* [2]=slot, [3..4]=count LE */
  BURST_STATUS: 0x05,
  BURST_READ: 0x06, /* [2..3]=sample offset LE */
  BURST_ABORT: 0x07,
  SEQ_SET: 0x08,
  LINK_TEST: 0x09,
  LINK_PROBE: 0x0a,
  LINK_HOLD: 0x0b,
  PEER: 0x0c,
  SPLIT: 0x0d,
  POWER: 0x0e,
  KEYS: 0x0f,
  BOOTLOADER: 0x7e,
};

export const RSP = {
  INFO: 0x81,
  STREAM: 0x82,
  SNAPSHOT: 0x83,
  BURST_STATUS: 0x85,
  BURST_DATA: 0x86,
  SEQ: 0x88,
  LINK: 0x89,
  PROBE: 0x8a,
  PEER: 0x8b,
  SPLIT: 0x8c,
  ERROR: 0xee,
};

/* slot_map[] sentinels. Anything below NUM_KEYS is a key index. */
export const SLOT_VBUS_DIV = 0xfe;
export const SLOT_UNUSED = 0xff;
export const SLOT_DUMMY = 0xfd; /* discarded: absorbs the VBUS charge step */

/* The map the dev board actually ships (adc.c). Only ever used as a fallback
 * when nothing has answered CMD_INFO yet — the wire value always wins, which is
 * the whole reason the firmware sends it. */
export const SLOT_MAP_FALLBACK = [
  0, 2, 4, SLOT_VBUS_DIV, SLOT_DUMMY, 1, 3, 5, SLOT_UNUSED, SLOT_DUMMY,
];

export function slotLabel(m) {
  if (m === SLOT_VBUS_DIV) return 'VBUS ÷2';
  if (m === SLOT_UNUSED) return 'floating';
  if (m === SLOT_DUMMY) return 'dummy';
  return 'key ' + m;
}

export function slotIsKey(m) {
  return m < NUM_KEYS;
}

/* slot index that carries a given key, or -1. */
export function slotForKey(slotMap, key) {
  return slotMap.indexOf(key);
}

/* ---------------------------------------------------------------- parsers */

/* RSP_INFO body, offsets straight from protocol.h:
 *   [4] version  [5] num_keys  [6] num_slots  [7] adc_bits
 *   [8..9] scan_hz LE
 *   [10..19] slot_map[PROTO_NUM_SLOTS]      <- 10 entries, not 8
 *   [20..23] counts_per_gauss_q8 LE
 *   [24..27] firmware build id LE
 *   [28..32] ordinary-sequence channel order, rank 1..5
 *   [33..44] 96-bit factory UID, low word first
 *   [45..46] uid_tag LE   [47] link sense   [48] reset cause
 *   [49..50] suspends LE  [51..52] resumes LE
 *   [53] power state  [54] remote wakeup granted
 *   [55..56] wake attempts LE   [57..58] wake grants LE
 *   [59] keyboard output enabled  [60] own key bitmap
 *   [61] peer key bitmap  [62] merging peer keys
 */
export function parseInfo(d) {
  const slotMap = [];
  for (let i = 0; i < NUM_SLOTS; i++) slotMap.push(d.getUint8(10 + i));
  const sequence = [];
  for (let i = 0; i < SEQ_LEN; i++) sequence.push(d.getUint8(28 + i));
  const uid = [d.getUint32(33, true), d.getUint32(37, true), d.getUint32(41, true)];
  return {
    version: d.getUint8(4),
    numKeys: d.getUint8(5),
    numSlots: d.getUint8(6),
    adcBits: d.getUint8(7),
    scanHz: d.getUint16(8, true) || SCAN_HZ_DEFAULT,
    slotMap,
    countsPerGauss: d.getUint32(20, true) / 256,
    build: d.getUint32(24, true),
    sequence,
    uid,
    uidHex: uid
      .slice()
      .reverse()
      .map((w) => w.toString(16).padStart(8, '0'))
      .join(''),
    uidTag: d.getUint16(45, true),
    sense: d.getUint8(47),
    resetFlags: d.getUint8(48),
    suspends: d.getUint16(49, true),
    resumes: d.getUint16(51, true),
    power: d.getUint8(53),
    remoteWakeupEn: d.getUint8(54),
    wakeAttempts: d.getUint16(55, true),
    wakeGrants: d.getUint16(57, true),
    keysEnabled: d.getUint8(59),
    keysOwn: d.getUint8(60),
    keysPeer: d.getUint8(61),
    keysMerge: d.getUint8(62),
  };
}

/* RSP_SNAPSHOT body:
 *   [4..7] u32 frame_index
 *   [8..27] u16 slot[PROTO_NUM_SLOTS]        <- 10 slots = 20 bytes, not 8
 *   [48..51] phase_errors LE                 <- fixed offset, NOT 8+2*8
 *   [52..55] tx_dropped LE
 *   [56] flags (bit0 = frame read failed)
 *   [57..60] adc_read_failures LE
 *   [61] low byte of the raw seqlock counter (odd = writer stuck)
 */
export function parseSnapshot(d) {
  const slots = [];
  for (let i = 0; i < NUM_SLOTS; i++) slots.push(d.getUint16(8 + 2 * i, true));
  return {
    frame: d.getUint32(4, true),
    slots,
    phaseErrors: d.getUint32(48, true),
    txDropped: d.getUint32(52, true),
    flags: d.getUint8(56),
    readFailed: (d.getUint8(56) & 1) !== 0,
    readFailures: d.getUint32(57, true),
    seqLsb: d.getUint8(61),
  };
}

/* RSP_STREAM header: [4] frame_count, [5] flags bit0 = frames dropped,
 * [6..7] decimation in force. Frames follow at PROTO_STREAM_HDR, 16 bytes each:
 * u32 frame index then u16 key[0..5]. */
export function parseStreamHeader(d) {
  return {
    count: d.getUint8(4),
    dropped: (d.getUint8(5) & 1) !== 0,
    decim: d.getUint16(6, true),
  };
}

/* RSP_BURST_STATUS: [4] state (0 idle, 1 running, 2 done), [5] slot,
 * [6..7] captured LE, [8..11] sample_period_ns LE */
export function parseBurstStatus(d) {
  return {
    state: d.getUint8(4),
    slot: d.getUint8(5),
    captured: d.getUint16(6, true),
    periodNs: d.getUint32(8, true),
  };
}

/* RSP_BURST_DATA: [4..5] offset LE, [6] count, then count x u16 LE from [7]. */
export function parseBurstData(d) {
  const off = d.getUint16(4, true);
  const n = Math.min(d.getUint8(6), BURST_PER_RPT);
  const out = new Uint16Array(n);
  for (let i = 0; i < n; i++) out[i] = d.getUint16(7 + 2 * i, true);
  return { off, count: n, samples: out };
}

export function parseError(d) {
  return { error: d.getUint8(4) };
}

export function parse(msg, d) {
  switch (msg) {
    case RSP.INFO:
      return parseInfo(d);
    case RSP.SNAPSHOT:
      return parseSnapshot(d);
    case RSP.BURST_STATUS:
      return parseBurstStatus(d);
    case RSP.BURST_DATA:
      return parseBurstData(d);
    case RSP.ERROR:
      return parseError(d);
    default:
      return null;
  }
}

/* Human-readable decoding of the two RSP_INFO bitfields worth surfacing. */
export function resetCause(flags) {
  const names = ['power-on/BOR', 'NRST pin', 'software', 'watchdog', 'window wdt', 'low-power'];
  const hits = names.filter((_, i) => flags & (1 << i));
  return hits.length ? hits.join(' + ') : 'none reported';
}

export const POWER_STATE = ['run', 'suspended (500 Hz)', 'serving peer'];
