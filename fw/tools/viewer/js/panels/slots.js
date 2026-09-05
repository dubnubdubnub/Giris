// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * panels/slots.js — the raw scan, unmapped, plus everything RSP_INFO carries.
 *
 * PROTO_NUM_SLOTS is 10, not 8: two mux banks of five conversions, and the
 * fifth of each is a dummy that exists only to absorb the charge step the VBUS
 * divider leaves on the ADC sampling cap. The table is in hardware scan order
 * because that is the order the mux and the DMA ring see, which is the only
 * order in which a phase slip is legible.
 */

import { el, fmtHz } from '../ui.js';
import { CMD, POWER_STATE, SLOT_VBUS_DIV, SLOT_UNUSED, SLOT_DUMMY, resetCause, slotLabel } from '../protocol.js';

export function slotsPanel(app, root) {
  const tbody = el('tbody');
  const infoGrid = el('dl', { class: 'kv' });
  const health = el('div', { class: 'row row--controls' });
  const healthText = el('span', { class: 'muted mono' });
  health.append(healthText);

  root.append(
    el(
      'div',
      { class: 'grid-info' },
      el(
        'div',
        { class: 'table-wrap' },
        el(
          'table',
          {},
          el(
            'thead',
            {},
            el(
              'tr',
              {},
              el('th', {}, 'slot'),
              el('th', {}, 'mux bank'),
              el('th', {}, 'adc ch'),
              el('th', {}, 'maps to'),
              el('th', { class: 'num' }, 'counts'),
              el('th', { class: 'num' }, 'volts')
            )
          ),
          tbody
        )
      ),
      el('div', { class: 'info-box' }, el('h3', {}, 'RSP_INFO'), infoGrid)
    ),
    health,
    el(
      'p',
      { class: 'hint' },
      'The VBUS ÷2 slot and the floating header slot are the phase check: if the floating one ever ' +
        'reads higher than the VBUS one, the DMA ring has slipped against the mux SEL line. The ' +
        'firmware counts that too — phase errors, below.'
    )
  );

  const cells = [];

  function rebuild() {
    const s = app.active();
    tbody.innerHTML = '';
    cells.length = 0;
    if (!s || !s.info) return;
    const seq = s.info.sequence || [];
    s.info.slotMap.forEach((m, i) => {
      const perBank = Math.max(1, Math.ceil(s.info.numSlots / 2));
      const bank = i < perBank ? 'A (SEL low)' : 'B (SEL high)';
      const ch = seq.length ? seq[i % seq.length] : '?';
      const counts = el('td', { class: 'num' }, '—');
      const volts = el('td', { class: 'num' }, '—');
      const cls = m === SLOT_DUMMY ? 'dim' : m === SLOT_UNUSED ? 'dim' : '';
      tbody.append(
        el(
          'tr',
          { class: cls },
          el('td', {}, String(i)),
          el('td', { class: 'muted' }, bank),
          el('td', { class: 'muted' }, 'IN' + ch),
          el('td', { class: m === SLOT_VBUS_DIV ? 'warnish' : '' }, slotLabel(m)),
          counts,
          volts
        )
      );
      cells.push({ counts, volts });
    });
    renderInfo(s.info, s);
  }

  function renderInfo(info, s) {
    infoGrid.innerHTML = '';
    const rows = [
      ['protocol', 'v' + info.version],
      ['build id', '0x' + info.build.toString(16).padStart(8, '0')],
      ['scan rate', fmtHz(info.scanHz)],
      ['keys / slots', `${info.numKeys} / ${info.numSlots}`],
      ['adc', info.adcBits + ' bit'],
      ['counts / gauss', info.countsPerGauss.toFixed(3)],
      ['uid', info.uidHex],
      ['uid tag', '0x' + info.uidTag.toString(16).padStart(4, '0')],
      ['seq order', info.sequence.map((c) => 'IN' + c).join(' → ')],
      ['last reset', resetCause(info.resetFlags)],
      ['power', POWER_STATE[info.power] || info.power],
      ['suspends / resumes', `${info.suspends} / ${info.resumes}`],
      ['remote wake', `${info.wakeGrants} granted of ${info.wakeAttempts} tried${info.remoteWakeupEn ? '' : ' · not enabled'}`],
      ['kbd output', info.keysEnabled ? 'ENABLED' : 'off (default)'],
      ['pressed bitmap', '0b' + info.keysOwn.toString(2).padStart(6, '0') + (info.keysMerge ? ' +peer' : '')],
      ['link sense', '0b' + info.sense.toString(2).padStart(5, '0')],
      ['transport', s.synthetic ? 'synthetic' : 'WebHID · 0xFF60/0x61'],
    ];
    for (const [k, v] of rows) {
      infoGrid.append(el('dt', {}, k), el('dd', {}, String(v)));
    }
  }

  app.on('session', rebuild);
  app.on('info', rebuild);
  rebuild();

  /* The snapshot path is debug, not hot path: poll it slowly and separately. */
  setInterval(async () => {
    const s = app.active();
    if (!s || s.gone || !s.info) return;
    try {
      const snap = await s.request(CMD.SNAPSHOT);
      if (!snap) return;
      if (cells.length !== snap.slots.length) rebuild();
      snap.slots.forEach((v, i) => {
        const c = cells[i];
        if (!c) return;
        c.counts.textContent = v;
        c.volts.textContent = ((v * 3.3) / 4096).toFixed(3);
      });
      healthText.textContent =
        `frame ${snap.frame.toLocaleString()} · phase errors ${snap.phaseErrors} · tx dropped ${snap.txDropped} · ` +
        `read failures ${snap.readFailures} · seqlock ${snap.seqLsb}${snap.seqLsb & 1 ? ' (ODD — writer stuck)' : ''}` +
        (snap.readFailed ? ' · FRAME READ FAILED' : '');
      healthText.className = 'mono ' + (snap.readFailed || snap.seqLsb & 1 ? 'err' : 'muted');
    } catch {
      /* a timeout here is not worth shouting about; the header shows the link */
    }
  }, 500);
}
