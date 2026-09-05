// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * panels/keypad.js — the board as it physically is.
 *
 * A 3 x 2 grid on the real 19.05 mm pitch, drawn to scale, each keycap filling
 * from the bottom with live travel. The point is that you can tell at a glance
 * which key is down and how far, without reading a number.
 *
 * Legends live in ../layout.js. Edit them there.
 */

import { el, select } from '../ui.js';
import {
  BOARDS,
  COLS,
  ROWS,
  COL_PCT,
  ROW_PCT,
  DEFAULT_BOARD,
  GRID_W_MM,
  GRID_H_MM,
  KEY_TO_SLOT_DOC,
  MUX_POS,
  PITCH_MM,
  REFDES,
  ROLE,
  legends,
} from '../layout.js';
import { KEY_VARS } from '../chart.js';
import { slotForKey } from '../protocol.js';

export function keypadPanel(app, root) {
  const boardSel = select(
    Object.entries(BOARDS).map(([k, b]) => ({ value: k, label: `${b.name} — ${b.note}` })),
    DEFAULT_BOARD,
    (v) => app.setBoard(v)
  );

  const hint = el('span', { class: 'muted' });
  const controls = el(
    'div',
    { class: 'row row--controls' },
    el('label', { class: 'field' }, 'legends ', boardSel),
    el('span', { class: 'legend-key' }, el('i', { class: 'swatch swatch--fn' }), 'reserved / function'),
    el('span', { class: 'legend-key' }, el('i', { class: 'swatch swatch--key' }), 'typing key'),
    hint
  );

  const grid = el('div', {
    class: 'keypad',
    style: {
      aspectRatio: `${GRID_W_MM} / ${GRID_H_MM}`,
      gridTemplateColumns: `repeat(${COLS}, ${COL_PCT})`,
      gridTemplateRows: `repeat(${ROWS}, ${ROW_PCT})`,
    },
  });

  const scale = el(
    'div',
    { class: 'keypad-scale' },
    el('span', {}, `${COLS} × ${ROWS} · ${PITCH_MM} mm pitch · drawn to scale`),
    el('span', { class: 'muted' }, 'key index = row-major reading order')
  );

  root.append(controls, el('div', { class: 'keypad-wrap' }, grid), scale);

  const caps = [];

  for (let k = 0; k < COLS * ROWS; k++) {
    const fill = el('i', { class: 'cap-fill' });
    const act = el('i', { class: 'cap-act', title: 'actuation threshold (150 counts)' });
    const label = el('div', { class: 'cap-label' });
    const sub = el('div', { class: 'cap-sub' });
    const counts = el('span', { class: 'cap-counts' }, '—');
    const travel = el('span', { class: 'cap-travel' }, '—');
    const slotTag = el('span', { class: 'cap-slot' });

    const cap = el(
      'div',
      {
        class: 'cap',
        'data-role': ROLE[k],
        style: { '--kc': `var(${KEY_VARS[k]})` },
        title: `key ${k} — ${REFDES[k]} · mux ${MUX_POS[k]}`,
      },
      fill,
      act,
      el(
        'div',
        { class: 'cap-face' },
        el('div', { class: 'cap-top' }, el('span', { class: 'cap-idx' }, 'k' + k), slotTag),
        el('div', { class: 'cap-mid' }, label, sub),
        el('div', { class: 'cap-read' }, counts, travel)
      )
    );

    /* Synthetic device: the caps are the input. Press one and watch every view
     * downstream respond, which is how these panels get reviewed with no board. */
    cap.addEventListener('pointerdown', (e) => {
      const t = app.active();
      if (!t || !t.transport.press) return;
      cap.setPointerCapture(e.pointerId);
      t.transport.press(k);
    });
    const up = () => {
      const t = app.active();
      if (t && t.transport.release) t.transport.release(k);
    };
    cap.addEventListener('pointerup', up);
    cap.addEventListener('pointercancel', up);
    cap.addEventListener('lostpointercapture', up);

    caps.push({ cap, fill, act, label, sub, counts, travel, slotTag });
    grid.append(cap);
  }

  function relabel() {
    boardSel.value = app.boardId();
    const legend = legends(app.boardId());
    const s = app.active();
    const map = s ? s.slotMap : null;
    caps.forEach((c, k) => {
      c.label.textContent = legend[k].label;
      c.sub.textContent = legend[k].sub || (ROLE[k] === 'fn' ? 'reserved' : '');
      const slot = map ? slotForKey(map, k) : KEY_TO_SLOT_DOC[k];
      c.slotTag.textContent = slot >= 0 ? 'slot ' + slot : '—';
      c.cap.title = `key ${k} — ${REFDES[k]} · mux ${MUX_POS[k]} · scan slot ${slot >= 0 ? slot : '?'}`;
    });
    hint.textContent = app.active()?.synthetic ? 'synthetic — click a keycap to press it' : '';
  }

  /* Physical keyboard shortcuts, built from whatever the legends say: a
   * single-character legend types that key, and 1..6 always work. */
  function keyFor(ch) {
    const legend = legends(app.boardId());
    if (/^[1-6]$/.test(ch)) return +ch - 1;
    const i = legend.findIndex((l) => l.label.length === 1 && l.label.toLowerCase() === ch);
    if (i >= 0) return i;
    if (ch === ' ') return legend.findIndex((l) => l.label === '␣');
    return -1;
  }
  const down = new Set();
  addEventListener('keydown', (e) => {
    const t = app.active();
    if (!t || !t.transport.press) return;
    if (e.repeat || e.metaKey || e.ctrlKey || e.altKey) return;
    if (/^(INPUT|SELECT|TEXTAREA|BUTTON)$/.test(document.activeElement?.tagName || '')) return;
    const k = keyFor(e.key.toLowerCase());
    if (k < 0) return;
    e.preventDefault();
    down.add(k);
    t.transport.press(k);
  });
  addEventListener('keyup', (e) => {
    const t = app.active();
    const k = keyFor(e.key.toLowerCase());
    if (k < 0 || !down.has(k)) return;
    down.delete(k);
    if (t && t.transport.release) t.transport.release(k);
  });

  app.on('session', relabel);
  app.on('board', relabel);
  app.on('info', relabel);
  relabel();

  app.onFrame(() => {
    const s = app.active();
    if (!s) return;
    const kt = s.keys;
    for (let k = 0; k < caps.length; k++) {
      const c = caps[k];
      if (k >= s.numKeys || !kt.seen) {
        c.cap.dataset.state = 'idle';
        continue;
      }
      const tr = kt.travel[k];
      c.fill.style.height = (tr * 100).toFixed(2) + '%';
      c.act.style.bottom = (kt.actuationFrac(k) * 100).toFixed(2) + '%';
      c.counts.textContent = kt.value[k];
      c.travel.textContent = Math.round(tr * 100) + '%';
      c.cap.dataset.state = kt.pressed[k] ? 'down' : 'up';
    }
  });
}
