/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
#include <string.h>

#include "tusb.h"
#include "usb_descriptors.h"
#include "uid.h"
#include "xusb.h"

/* ---------------------------------------------------------------- report */
/* [MS-XUSBI] section 3.2.5.1.1, Table 53. Report ID 0x00, 20 bytes, all
 * multi-byte fields little-endian.
 *
 *   [0]      bReportID      = 0x00
 *   [1]      bSize          = 0x14 (20)
 *   [2..3]   bmButtons      u16   (Table 54)
 *   [4]      bLeftTrigger   u8    0..255
 *   [5]      bRightTrigger  u8
 *   [6..7]   wThumbLeftX    i16   SIGNED, despite the w prefix
 *   [8..9]   wThumbLeftY    i16
 *   [10..11] wThumbRightX   i16
 *   [12..13] wThumbRightY   i16
 *   [14..19] reserved, zero
 *
 * Signedness matters and is easy to get wrong. Table 53's min/max is a
 * two's-complement int16 range. Section 2.2.2.1.2 does say thumbstick values are
 * "recentered around 0x8000" — but that describes the DRIVER's XUSB-to-HID
 * translation on the host side, not the wire. Do not add 0x8000 here. */
#define XUSB_REPORT_LEN   20

enum {
  R_ID = 0, R_SIZE = 1, R_BTN = 2, R_LT = 4, R_RT = 5,
  R_LX = 6, R_LY = 8, R_RX = 10, R_RY = 12,
};

static uint8_t  report[XUSB_REPORT_LEN];
static bool     enabled;
static bool     dirty;
static uint32_t in_sent;
static uint32_t out_reports;
static bool     saw_msos_string;
static bool     saw_compat_id;

/* ------------------------------------------------- extended compatible id */
/* [MS-XUSBI] section 2.2.4.6, Table 19. 40 bytes (0x28).
 *
 * bFirstInterfaceNumber at offset 16 is the field that decides whether this
 * works. It scopes the compatible ID onto one function of a composite device,
 * and if it names the wrong interface the ID lands on the wrong child PDO and
 * nothing binds — with no error anywhere. Table 19 prints 0x00 because it
 * describes a single-function pad; ours must be ITF_NUM_XUSB. */
static const uint8_t msos_compat_id[40] = {
  0x28, 0x00, 0x00, 0x00,         /* dwLength = 40                            */
  0x00, 0x01,                     /* bcdVersion = 0x0100                      */
  0x04, 0x00,                     /* wIndex = 0x0004, extended compatible ID  */
  0x01,                           /* bCount = one function section            */
  0, 0, 0, 0, 0, 0, 0,            /* reserved[7]                              */

  ITF_NUM_XUSB,                   /* bFirstInterfaceNumber                    */
  0x01,                           /* reserved / bNumInterfaces — 1 either way */
  'X', 'U', 'S', 'B', '1', '0', 0, 0,   /* compatibleID, Table 19 literal     */
  0, 0, 0, 0, 0, 0, 0, 0,         /* subCompatibleID — none                   */
  0, 0, 0, 0, 0, 0,               /* reserved[6]                              */
};

/* ------------------------------------------------------------------- api */

void xusb_init(void)
{
  memset(report, 0, sizeof(report));
  report[R_ID]   = 0x00;
  report[R_SIZE] = XUSB_REPORT_LEN;
  enabled = false;
  dirty   = false;
  in_sent = out_reports = 0;
  saw_msos_string = saw_compat_id = false;
}

void xusb_quiesce(void)
{
  dirty = false;
  memset(&report[R_BTN], 0, XUSB_REPORT_LEN - R_BTN);
}

void xusb_arm_out(void)
{
  /* vendord_open arms the bulk OUT endpoint but not the interrupt one, so
   * without this the driver's LED and rumble writes NAK forever. */
  if (tud_vendor_mounted()) tud_vendor_int_read_xfer();
}

void xusb_set_enabled(bool on)
{
  if (on == enabled) return;
  enabled = on;
  if (!on) {
    /* Leave the host holding a centred pad, not the last deflection. */
    memset(&report[R_BTN], 0, XUSB_REPORT_LEN - R_BTN);
    dirty = true;
  }
}

bool xusb_enabled(void) { return enabled; }

void xusb_note_msos_string(void) { saw_msos_string = true; }

/* int8 test unit to the wire's int16. 127 * 258 = 32766, full scale to within
 * one part in 16k; -128 and -127 both clamp to INT16_MIN so the negative end
 * reaches the rail too. */
static int16_t scale_axis(int8_t v)
{
  return (v <= -127) ? (int16_t)-32768 : (int16_t)((int32_t)v * 258);
}

static void put16(uint32_t off, uint16_t v)
{
  report[off]     = (uint8_t)(v & 0xFFu);
  report[off + 1] = (uint8_t)(v >> 8);
}

void xusb_set_state(int8_t lx, int8_t ly, int8_t rx, int8_t ry,
                    uint8_t lt, uint8_t rt, uint16_t buttons)
{
  uint8_t prev[XUSB_REPORT_LEN];
  memcpy(prev, report, sizeof(prev));

  put16(R_BTN, buttons);
  report[R_LT] = lt;
  report[R_RT] = rt;
  put16(R_LX, (uint16_t)scale_axis(lx));
  put16(R_LY, (uint16_t)scale_axis(ly));
  put16(R_RX, (uint16_t)scale_axis(rx));
  put16(R_RY, (uint16_t)scale_axis(ry));

  if (memcmp(prev, report, sizeof(prev)) != 0) dirty = true;
}

void xusb_service(void)
{
  /* Section 1.3.4: "If the state does not change between IN requests, the
   * device SHOULD negatively acknowledge (NAK) the IN request." Leaving the
   * endpoint unarmed produces exactly that, for free. So this deliberately does
   * NOT keep a transfer permanently queued the way a HID interface would. */
  if (!enabled || !dirty) return;
  if (!tud_vendor_mounted()) return;
  if (tud_vendor_int_write_available() == 0u) return;   /* busy; never spin */

  if (tud_vendor_int_write(report, XUSB_REPORT_LEN) == XUSB_REPORT_LEN) {
    dirty = false;
  }
}

uint8_t xusb_status(void)
{
  uint8_t s = 0;
  if (enabled)              s |= 0x01u;
  if (tud_vendor_mounted()) s |= 0x02u;
  if (in_sent)              s |= 0x04u;
  if (out_reports)          s |= 0x08u;
  if (saw_msos_string)      s |= 0x10u;
  if (saw_compat_id)        s |= 0x20u;
  s |= (uint8_t)((out_reports & 3u) << 6);
  return s;
}

/* ------------------------------------------------------ tinyusb callbacks */

void tud_vendor_int_tx_cb(uint8_t itf, uint32_t sent_bytes)
{
  (void)itf; (void)sent_bytes;
  in_sent++;
}

void tud_vendor_int_rx_cb(uint8_t itf, const uint8_t *buffer, uint32_t bufsize)
{
  (void)itf; (void)buffer; (void)bufsize;
  /* Rumble (id 0x00), LED / player slot (0x01), master rumble level (0x02) —
   * sections 3.2.5.2 and Tables 61-63. This board has no motors and no LED
   * quadrant, so they are counted and dropped. Counting them is the point: only
   * a bound xusb22.sys sends these, so out_reports > 0 is the single strongest
   * proof the whole chain works. Re-arm, or the next one never arrives. */
  out_reports++;
  tud_vendor_int_read_xfer();
}

bool tud_vendor_control_xfer_cb(uint8_t rhport, uint8_t stage,
                                tusb_control_request_t const *request)
{
  /* This sees EVERY vendor-type control request aimed at the whole device, so
   * it must decline anything it does not recognise rather than blanket-ACK.
   * usbd answers the standard interface requests itself when we return false. */
  if (stage != CONTROL_STAGE_SETUP) return true;

  /* Extended Compatible ID. Windows asks twice — once for the 16-byte header,
   * then for all 40 — so hand over 40 and let usbd clamp to wLength. */
  if ((request->bmRequestType == 0xC0u || request->bmRequestType == 0xC1u) &&
      request->bRequest == MSOS_VENDOR_CODE && request->wIndex == 0x0004u) {
    saw_compat_id = true;
    return tud_control_xfer(rhport, request, (void *)(uintptr_t)msos_compat_id,
                            sizeof(msos_compat_id));
  }

  /* GET_DEVICE_ID, section 2.2.8.5: "Each XUSB device MUST have a unique serial
   * ID". Table 29 makes it the one vendor request valid for a wired device in
   * the Configured state. Four bytes off the factory UID. */
  if (request->bmRequestType == 0xC0u && request->bRequest == 0x01u) {
    static uint8_t devid[4];
    memcpy(devid, uid_bytes(), sizeof(devid));
    return tud_control_xfer(rhport, request, devid, sizeof(devid));
  }

  /* Everything else, including SET_CONTROL (Table 29 says STALL in every state)
   * and GET_CAPABILITIES (section 2.2.8.7 opens "XUSB v1.10 NOT USED", prints
   * its opcode as literal 0x??, and Table 29 marks it STALL everywhere — we
   * declare bcdXUSB 0x0100, so it does not apply). */
  return false;
}
