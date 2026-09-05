// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * ui.js — the only DOM sugar in the project. Deliberately tiny; there is no
 * framework here and there is not going to be one.
 */

export const $ = (sel, root = document) => root.querySelector(sel);
export const $$ = (sel, root = document) => Array.from(root.querySelectorAll(sel));

export function el(tag, attrs = {}, ...kids) {
  const n = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (v === null || v === undefined || v === false) continue;
    if (k === 'class') n.className = v;
    else if (k === 'text') n.textContent = v;
    else if (k === 'html') n.innerHTML = v;
    else if (k === 'style' && typeof v === 'object') {
      /* Custom properties need setProperty(); plain assignment silently drops
       * anything starting with `--`, which is how every per-key colour is fed
       * to the stylesheet. */
      for (const [p, val] of Object.entries(v)) {
        if (p.startsWith('--')) n.style.setProperty(p, val);
        else n.style[p] = val;
      }
    }
    else if (k.startsWith('on') && typeof v === 'function') n.addEventListener(k.slice(2), v);
    else n.setAttribute(k, v === true ? '' : v);
  }
  for (const kid of kids.flat()) {
    if (kid === null || kid === undefined || kid === false) continue;
    n.append(kid.nodeType ? kid : document.createTextNode(String(kid)));
  }
  return n;
}

/* <label>text <control></label> — the pattern the header uses everywhere. */
export function field(text, control) {
  return el('label', { class: 'field' }, text, control);
}

export function select(options, value, onchange) {
  const s = el('select');
  for (const o of options) {
    const opt = el('option', { value: String(o.value) }, o.label);
    if (String(o.value) === String(value)) opt.selected = true;
    s.append(opt);
  }
  s.addEventListener('change', () => onchange(s.value));
  return s;
}

export function button(label, onclick, attrs = {}) {
  return el('button', { ...attrs, onclick }, label);
}

/* A labelled number readout. Returns the element; write through .set(). */
export function stat(label, hint) {
  const v = el('div', { class: 'stat-v' }, '—');
  const box = el(
    'div',
    { class: 'stat' },
    el('div', { class: 'stat-k' }, label),
    v,
    hint ? el('div', { class: 'stat-h' }, hint) : null
  );
  box.set = (text, tone) => {
    v.textContent = text;
    box.dataset.tone = tone || '';
  };
  box.setHint = (text) => {
    const h = box.querySelector('.stat-h');
    if (h) h.textContent = text;
  };
  return box;
}

export function fmtUs(us) {
  if (!isFinite(us)) return '—';
  const a = Math.abs(us);
  if (a >= 10000) return (us / 1000).toFixed(2) + ' ms';
  if (a >= 1000) return (us / 1000).toFixed(3) + ' ms';
  return Math.round(us) + ' µs';
}

export function fmtHz(hz) {
  if (hz >= 1e6) return (hz / 1e6).toFixed(2) + ' MHz';
  if (hz >= 1000) return (hz / 1000).toFixed(hz % 1000 ? 2 : 0) + ' kHz';
  return hz.toFixed(0) + ' Hz';
}

export function fmtCount(n) {
  return n.toLocaleString('en-US');
}

/* Save a Blob, preferring the real file picker when the browser has one. */
export async function saveBlob(blob, name) {
  if (window.showSaveFilePicker) {
    try {
      const h = await window.showSaveFilePicker({ suggestedName: name });
      const w = await h.createWritable();
      await w.write(blob);
      await w.close();
      return true;
    } catch (e) {
      if (e.name === 'AbortError') return false;
      /* fall through to the anchor path */
    }
  }
  const a = el('a', { href: URL.createObjectURL(blob), download: name });
  a.click();
  setTimeout(() => URL.revokeObjectURL(a.href), 5000);
  return true;
}
