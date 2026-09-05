// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Isaac Chiu
/*
 * hid.js — the WebHID transport.
 *
 * A transport is deliberately dumb: it moves 64 raw bytes each way and knows
 * nothing about tags, sequence numbers or message shapes. That lives in
 * session.js, which is why the synthetic device in synth.js can stand in for
 * this one exactly — it speaks the same wire format, so the real parsers are
 * exercised even with no hardware in the room.
 */

import { RPT_SIZE } from './protocol.js';

/* Only this collection is the protocol interface. The board also exposes a real
 * keyboard interface and a System Control interface (which exists purely to earn
 * wake capability); either of those answers nothing and looks like a dead board. */
export const USAGE_PAGE = 0xff60;
export const USAGE = 0x61;
export const FILTERS = [{ usagePage: USAGE_PAGE, usage: USAGE }];

export function webHidSupported() {
  return typeof navigator !== 'undefined' && 'hid' in navigator;
}

/* Chromium ships WebHID; Firefox and Safari have both declined to. Say so
 * rather than failing mysteriously. */
export function browserNote() {
  if (webHidSupported()) return null;
  const ua = navigator.userAgent;
  if (/Firefox\//.test(ua)) return 'Firefox has no WebHID. Use Chrome, Edge or another Chromium browser.';
  if (/Safari\//.test(ua) && !/Chrome\//.test(ua))
    return 'Safari has no WebHID. Use Chrome, Edge or another Chromium browser.';
  return 'WebHID is unavailable. Use a Chromium browser over http://localhost or https://.';
}

function isProtocolInterface(d) {
  return d.collections.some((c) => c.usagePage === USAGE_PAGE && (c.usage === USAGE || c.usage === undefined));
}

/* Pick the HIDDevices that really carry our collection. requestDevice() hands
 * back one HIDDevice per matching interface, and two physical boards may be
 * plugged in at once, so this returns a list rather than a single device. */
function protocolInterfaces(devices) {
  const exact = devices.filter(isProtocolInterface);
  if (exact.length) return exact;
  const page = devices.filter((d) => d.collections.some((c) => c.usagePage === USAGE_PAGE));
  return page.length ? page : devices.slice(0, 1);
}

/* Devices the user has already granted us — lets a reload reattach silently. */
export async function knownDevices() {
  if (!webHidSupported()) return [];
  try {
    return protocolInterfaces(await navigator.hid.getDevices());
  } catch {
    return [];
  }
}

/* Shows the chooser. Returns [] if the user cancelled. */
export async function pickDevices() {
  if (!webHidSupported()) throw new Error(browserNote());
  const devices = await navigator.hid.requestDevice({ filters: FILTERS });
  return protocolInterfaces(devices);
}

export class HidTransport {
  constructor(device) {
    this.kind = 'hid';
    this.device = device;
    /* Two identical boards report the same product name, so identity has to come
     * from somewhere else. Fixed at construction: a getter that counted up would
     * hand out a different id on every read. */
    this.id = 'hid:' + (HidTransport._n = (HidTransport._n || 0) + 1);
    this.onreport = null;
    this.ondisconnect = null;
    this._out = new Uint8Array(RPT_SIZE);
    this._onInput = (e) => {
      if (this.onreport) this.onreport(e.data);
    };
    this._onGone = (e) => {
      if (e.device === this.device && this.ondisconnect) this.ondisconnect();
    };
  }

  get name() {
    return this.device.productName || 'Giris';
  }

  get synthetic() {
    return false;
  }

  async open() {
    if (!this.device.opened) await this.device.open();
    this.device.addEventListener('inputreport', this._onInput);
    navigator.hid.addEventListener('disconnect', this._onGone);
  }

  async close() {
    this.device.removeEventListener('inputreport', this._onInput);
    navigator.hid.removeEventListener('disconnect', this._onGone);
    try {
      if (this.device.opened) await this.device.close();
    } catch {
      /* the device may already be gone; that is what close means */
    }
  }

  /* No report IDs anywhere in this protocol, so reportId is 0 both ways. */
  write(bytes) {
    this._out.set(bytes);
    return this.device.sendReport(0, this._out);
  }
}
