// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * app.js — wiring. Owns the device list, the header, and the animation frame.
 *
 * No build step and no dependencies: this is loaded as
 * <script type="module">, and serve.sh is a plain python http.server. That
 * constraint is deliberate — the page has to run on a Windows handheld with
 * nothing installed on it.
 */

import { $, el, saveBlob, fmtHz } from './ui.js';
import { HidTransport, browserNote, knownDevices, pickDevices, webHidSupported } from './hid.js';
import { SynthTransport } from './synth.js';
import { Session } from './session.js';
import { CaptureStore } from './capture.js';
import { DEFAULT_BOARD, boardForInfo } from './layout.js';
import { keypadPanel } from './panels/keypad.js';
import { tracePanel } from './panels/trace.js';
import { comparePanel } from './panels/compare.js';
import { burstZoomPanel } from './panels/burstzoom.js';
import { spectrumPanel } from './panels/spectrum.js';
import { histogramPanel } from './panels/histogram.js';
import { slotsPanel } from './panels/slots.js';

/* ------------------------------------------------------------------- app */

const listeners = new Map();
const frameFns = [];
const sessions = [];
let activeId = null;
let board = DEFAULT_BOARD;

const app = {
  capture: new CaptureStore(),
  sessions,
  active: () => sessions.find((s) => s.id === activeId) || null,
  boardId: () => board,
  setBoard(id) {
    board = id;
    emit('board');
  },
  on(evt, fn) {
    if (!listeners.has(evt)) listeners.set(evt, []);
    listeners.get(evt).push(fn);
  },
  onFrame(fn) {
    frameFns.push(fn);
  },
};

function emit(evt, detail) {
  for (const fn of listeners.get(evt) || []) {
    try {
      fn(detail);
    } catch (e) {
      console.error('[' + evt + ']', e);
    }
  }
}

/* --------------------------------------------------------------- header */

const dot = $('#dot');
const statusEl = $('#status');
const tabs = $('#tabs');
const banner = $('#banner');

const connectBtn = $('#connect');
const synthBtn = $('#synth');
const streamBtn = $('#stream');
const decimInput = $('#decim');
const recordBtn = $('#record');
const saveBtn = $('#save');
const bootBtn = $('#boot');

function setStatus(text, live) {
  statusEl.textContent = text;
  dot.classList.toggle('live', !!live);
}

function refreshHeader() {
  const s = app.active();
  const on = !!s && !s.gone;
  streamBtn.disabled = !on;
  recordBtn.disabled = !on;
  bootBtn.disabled = !on || s.synthetic;
  bootBtn.title = s && s.synthetic ? 'the synthetic device has no bootloader' : 'Jump to the ROM bootloader for reflashing';
  saveBtn.disabled = !on || s.recording || !s.recorded.length;
  streamBtn.classList.toggle('on', !!s && s.streamOn);
  streamBtn.textContent = s && s.streamOn ? 'Streaming' : 'Stream';
  recordBtn.classList.toggle('on', !!s && s.recording);
  recordBtn.textContent = s && s.recording ? 'Stop' : 'Record';
  if (s) decimInput.value = s.decim;
  renderTabs();
}

function renderTabs() {
  tabs.innerHTML = '';
  tabs.hidden = sessions.length === 0;
  for (const s of sessions) {
    const tab = el(
      'button',
      {
        class: 'tab' + (s.id === activeId ? ' on' : ''),
        onclick: () => setActive(s.id),
        title: s.info ? `build 0x${s.info.build.toString(16)} · uid ${s.info.uidHex}` : s.name,
      },
      el('span', { class: 'tab-dot' + (s.streamOn ? ' live' : '') }),
      s.synthetic ? 'synthetic' : s.name,
      el('i', {
        class: 'tab-x',
        title: 'disconnect',
        onclick: (e) => {
          e.stopPropagation();
          s.close();
        },
      }, '×')
    );
    tabs.append(tab);
  }
}

function setActive(id) {
  activeId = id;
  const s = app.active();
  if (s && s.info) board = boardForInfo(s.info);
  emit('session');
  emit('board');
  refreshHeader();
}

/* ------------------------------------------------------------- sessions */

async function attach(transport, { autoStream = true } = {}) {
  const s = new Session(transport);
  sessions.push(s);
  s.addEventListener('info', () => {
    if (s.id === activeId) {
      board = boardForInfo(s.info);
      emit('board');
    }
    emit('info');
    refreshHeader();
  });
  s.addEventListener('stream', refreshHeader);
  s.addEventListener('gone', () => {
    const i = sessions.indexOf(s);
    if (i >= 0) sessions.splice(i, 1);
    if (activeId === s.id) setActive(sessions.length ? sessions[0].id : null);
    else refreshHeader();
    if (!sessions.length) setStatus('disconnected');
  });

  try {
    await s.open();
  } catch (e) {
    const i = sessions.indexOf(s);
    if (i >= 0) sessions.splice(i, 1);
    setStatus('connect failed: ' + e.message);
    return null;
  }
  setActive(s.id);
  if (autoStream) {
    try {
      await s.setStream(true, Math.max(1, +decimInput.value || 1));
    } catch (e) {
      setStatus('stream failed: ' + e.message);
    }
  }
  return s;
}

connectBtn.onclick = async () => {
  if (!webHidSupported()) {
    setStatus(browserNote());
    return;
  }
  try {
    const devices = await pickDevices();
    if (!devices.length) return;
    for (const d of devices) {
      if (sessions.some((s) => s.transport.device === d)) continue;
      await attach(new HidTransport(d));
    }
  } catch (e) {
    setStatus('connect failed: ' + e.message);
  }
};

synthBtn.onclick = async () => {
  const t = new SynthTransport({ board });
  await attach(t);
};

streamBtn.onclick = async () => {
  const s = app.active();
  if (!s) return;
  try {
    await s.setStream(!s.streamOn, Math.max(1, +decimInput.value || 1));
  } catch (e) {
    setStatus('stream failed: ' + e.message);
  }
  refreshHeader();
};

decimInput.onchange = async () => {
  const s = app.active();
  const d = Math.max(1, Math.min(255, +decimInput.value || 1));
  decimInput.value = d;
  if (s && s.streamOn) await s.setStream(true, d);
};

recordBtn.onclick = () => {
  const s = app.active();
  if (!s) return;
  s.recording ? s.stopRecording() : s.startRecording();
  refreshHeader();
};

saveBtn.onclick = async () => {
  const s = app.active();
  if (!s || !s.recorded.length) return;
  await saveBlob(new Blob([s.csv()], { type: 'text/csv' }), `giris-${Date.now()}.csv`);
};

bootBtn.onclick = async () => {
  const s = app.active();
  if (!s) return;
  if (!confirm('Jump to the ROM bootloader? The device will disappear and reappear as DFU on J2.')) return;
  try {
    await s.bootloader();
  } catch {
    /* the device stops answering the moment it jumps; that is success */
  }
  setStatus('rebooting to DFU…');
};

/* --------------------------------------------------------------- panels */

keypadPanel(app, $('#panel-keypad .body'));
tracePanel(app, $('#panel-trace .body'));
comparePanel(app, $('#panel-compare .body'));
burstZoomPanel(app, $('#panel-burst .body'));
spectrumPanel(app, $('#panel-spectrum .body'));
histogramPanel(app, $('#panel-histogram .body'));
slotsPanel(app, $('#panel-slots .body'));

/* ------------------------------------------------------------ the frame */

function loop() {
  requestAnimationFrame(loop);
  for (const s of sessions) s.tick();
  for (const fn of frameFns) {
    try {
      fn();
    } catch (e) {
      console.error(e);
    }
  }
  const s = app.active();
  if (s && !s.gone) {
    const rec = s.recording ? ` · REC ${s.recorded.length.toLocaleString()}` : '';
    setStatus(
      `${s.reportRate.toFixed(0)} rpt/s · ${fmtHz(s.frameRate)} frames · ` +
        `seq gaps ${s.seqGaps} · frame gaps ${s.frameGaps}${rec}`,
      s.streamOn
    );
  }
}
requestAnimationFrame(loop);

/* ----------------------------------------------------------- start-up */

(async function start() {
  if (!webHidSupported()) {
    connectBtn.disabled = true;
    banner.hidden = false;
    banner.textContent = browserNote() + ' Everything else works — start the synthetic board.';
    setStatus('WebHID unavailable');
  } else {
    setStatus('not connected — Connect a board, or start the synthetic one');
    /* Reattach silently to anything the user already granted us. A failure here
     * is not news — the board may simply be unplugged — so it must not leave a
     * scary message where "not connected" belongs. */
    try {
      let any = false;
      for (const d of await knownDevices()) if (await attach(new HidTransport(d))) any = true;
      if (!any && !sessions.length) setStatus('not connected — Connect a board, or start the synthetic one');
    } catch {
      setStatus('not connected — Connect a board, or start the synthetic one');
    }
  }
  refreshHeader();
  emit('session');
})();

/* Handy for poking at things from the console. */
window.giris = app;
