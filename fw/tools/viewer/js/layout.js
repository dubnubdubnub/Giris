// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * layout.js — the physical key layout, and the legends printed on it.
 *
 * ==========================================================================
 *  EDIT THIS FILE, NOT THE PANEL. These legends will change.
 * ==========================================================================
 *
 * Geometry, verified against the KiCad netlist and PCB: a 3 x 2 grid on a
 * 19.05 mm pitch. Key index is simply row-major reading order.
 *
 *     top row      key0 (SW3)   key1 (SW4)   key2 (SW5)
 *     bottom row   key3 (SW6)   key4 (SW7)   key5 (SW8)
 *
 * Electrically the two mux banks interleave, so neighbouring keys never share a
 * bank — a mux settling error shows up as an alternating pattern rather than a
 * plausible-looking one:
 *
 *     key0 = S1A -> slot 0      key1 = S1B -> slot 5
 *     key2 = S2A -> slot 1      key3 = S2B -> slot 6
 *     key4 = S3A -> slot 2      key5 = S3B -> slot 7
 *
 * The slot numbers above are documentation only. At runtime the viewer uses the
 * slot_map[] that RSP_INFO carries, so a rewire needs a firmware flash and not a
 * viewer edit. Streamed frames are already key-indexed by the firmware.
 */

export const PITCH_MM = 19.05;
export const CAP_MM = 18.0; /* keycap across flats; the rest is the gap */
export const COLS = 3;
export const ROWS = 2;

/* Reference designator per key index, for cross-referencing the schematic. */
export const REFDES = ['SW3', 'SW4', 'SW5', 'SW6', 'SW7', 'SW8'];

/* Documented mux position per key index. Display only — see the note above. */
export const MUX_POS = ['S1A', 'S1B', 'S2A', 'S2B', 'S3A', 'S3B'];
export const KEY_TO_SLOT_DOC = [0, 5, 1, 6, 2, 7];

/*
 * Roles. Two keys are reserved for function on BOTH boards and are drawn
 * differently from the four typing keys, because pressing them does not type.
 *   'fn'  — reserved / function key
 *   'key' — an ordinary typing key
 */
export const ROLE = ['fn', 'key', 'fn', 'key', 'key', 'key'];

/*
 * Board legends. `label` is what gets printed big on the keycap, `sub` is the
 * small caption underneath. Add a board by adding an entry; the picker in the
 * key-layout panel is generated from this object.
 */
export const BOARDS = {
  1: {
    name: 'board 1',
    note: 'WASD half',
    legends: [
      { label: 'KVM', sub: 'switch' },
      { label: 'W', sub: '' },
      { label: 'Fn', sub: 'layer' },
      { label: 'A', sub: '' },
      { label: 'S', sub: '' },
      { label: 'D', sub: '' },
    ],
  },
  2: {
    name: 'board 2',
    note: 'phrase half',
    legends: [
      { label: 'KVM', sub: 'switch' },
      { label: 'what', sub: '' },
      { label: 'Fn', sub: 'layer' },
      { label: 'the', sub: '' },
      { label: 'sigma', sub: '' },
      { label: '␣', sub: 'space' },
    ],
  },
};

export const DEFAULT_BOARD = '1';

/*
 * Optional: map a board's 16-bit uid_tag (RSP_INFO [45..46]) to a legend set so
 * two halves plugged in at once each pick up their own labels automatically.
 * Fill in once the tags are known — until then the panel's picker decides.
 *
 *   export const BOARD_BY_UID_TAG = { 0x1A2B: '1', 0x3C4D: '2' };
 */
export const BOARD_BY_UID_TAG = {};

export function boardForInfo(info) {
  if (info && BOARD_BY_UID_TAG[info.uidTag]) return BOARD_BY_UID_TAG[info.uidTag];
  return DEFAULT_BOARD;
}

export function legends(boardId) {
  return (BOARDS[boardId] || BOARDS[DEFAULT_BOARD]).legends;
}

/* Row-major reading order is the key index; nothing clever needed. */
export function gridPos(key) {
  return { row: Math.floor(key / COLS), col: key % COLS };
}

/* Percentages for a CSS grid that reproduces the real pitch exactly.
 * width  = COLS*CAP + (COLS-1)*gap ; height = ROWS*CAP + (ROWS-1)*gap */
const GAP_MM = PITCH_MM - CAP_MM;
export const GRID_W_MM = COLS * CAP_MM + (COLS - 1) * GAP_MM;
export const GRID_H_MM = ROWS * CAP_MM + (ROWS - 1) * GAP_MM;
export const COL_PCT = ((CAP_MM / GRID_W_MM) * 100).toFixed(4) + '%';
export const ROW_PCT = ((CAP_MM / GRID_H_MM) * 100).toFixed(4) + '%';
