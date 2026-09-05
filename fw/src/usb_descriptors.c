/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
/*
 * usb_descriptors.c — two HID interfaces, high speed, bInterval = 1.
 *
 *   ITF_NUM_HID  vendor raw HID, usage page 0xFF60. Telemetry and config.
 *   ITF_NUM_KBD  boot-protocol keyboard.
 *
 * The keyboard lives on its own interface rather than sharing the vendor one,
 * and that separation is load-bearing: macOS evaluates Input Monitoring per
 * IOHIDDevice, which is per USB interface. Keyboard usages on the vendor
 * interface would make the browser viewer demand permission to open it.
 * Anything added later — a gamepad, an NKRO report — gets its own interface for
 * the same reason.
 *
 *   ITF_NUM_XUSB vendor class 0xFF/0x5D/0x01 — the Xbox 360 controller
 *                interface, which is how analog travel reaches XInput.
 *
 * String index 0xEE is served, and must stay served: it is where Windows asks
 * for the MS OS 1.0 descriptor that carries the XUSB10 compatible ID, which is
 * the ONLY thing that makes the inbox xusb22.sys bind to a non-Microsoft VID.
 * It used to return NULL and STALL.
 *
 * That history is why bcdDevice matters here more than it looks. Windows caches
 * the outcome of the 0xEE probe as osvc under
 * HKLM\SYSTEM\CurrentControlSet\Control\usbflags\<vid><pid><bcdDevice> and
 * never asks again — so a machine that met this board while 0xEE stalled would
 * keep serving "no MS OS descriptor" from cache no matter how correct the
 * firmware became.
 *
 * BUMP bcdDevice ON ANY CHANGE TO THE USB-VISIBLE DESCRIPTOR SET.
 */
#include "tusb.h"
#include "protocol.h"
#include "uid.h"

#define USB_VID   0x1209   /* pid.codes — open-source range */

/* 0x6415 is REQUESTED, not yet allocated: pid.codes PR #1271.
 *
 * Deliberately not 0x0001. That is pid.codes' private-testing PID and carries an
 * explicit prohibition — "MUST NOT be used on any device that will be
 * redistributed, sold, or manufactured" — which is a rule this project would be
 * breaking the moment a board leaves the bench. Using the number we have
 * publicly asked for breaks no rule; it is at worst a claim staked early, and it
 * is staked in the open: the PR names it, this repo is public, and anyone
 * hitting a collision can see exactly who is using it and why.
 *
 * Checked clear at submission against both the merged tree and all 77 open PRs
 * (98 PIDs are claimed by unmerged PRs and invisible in master). Allocation goes
 * by PR order and ours is filed, so the exposure is small and shrinks with time.
 * If it is ever assigned elsewhere, this is a one-line change and a reflash.
 *
 * pid.codes had merged one commit in the 129 days before this was filed, so
 * expect to be on this number for a while. */
#define USB_PID   0x6415
/* 2.00, not 2.10.
 *
 * Declaring 2.01 or higher obliges the device to answer GET_DESCRIPTOR(BOS), and
 * TinyUSB's tud_descriptor_bos_cb is a weak stub returning NULL, so the request
 * is STALLED (usbd.c:1344, TU_VERIFY(desc_bos != 0)). macOS does not care.
 * Windows asks every device that claims >= 2.01 and is entitled not to forgive
 * the stall. Claim 2.10 again only in the same commit that adds a real BOS
 * descriptor — which is what a Microsoft OS 2.0 descriptor set would need. */
#define USB_BCD   0x0200

/* ---------------------------------------------------------------- device */
static const tusb_desc_device_t desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = USB_BCD,
    .bDeviceClass       = 0x00,
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = USB_VID,
    .idProduct          = USB_PID,
    /* Bump on ANY change to what USB can see — interfaces, endpoints, strings,
     * MS OS descriptors. Windows caches per <vid><pid><bcdDevice>, so a stale
     * entry otherwise serves old strings and a remembered "no MS OS descriptor".
     * 0x0100 -> 0x0101: manufacturer and product strings changed.
     * 0x0101 -> 0x0102: XUSB interface added, plus MS OS 1.0 descriptors.
     *   That bump is NOT cosmetic. Index 0xEE used to STALL, and Windows
     *   recorded osvc = 0 against 120964150101 and never asks again. A
     *   flawless descriptor set at the old revision would do nothing. */
    .bcdDevice          = 0x0102,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

const uint8_t *tud_descriptor_device_cb(void) { return (const uint8_t *)&desc_device; }

/* ------------------------------------------------------------ hid report */
/* Hand-rolled rather than TUD_HID_REPORT_DESC_GENERIC_INOUT, because that macro
 * emits vendor page 0xFF00 — the one vendor page named in Chromium's static HID
 * blocklist. 0xFF60 / usage 0x61 is the QMK raw-HID convention and is claimable. */
static const uint8_t desc_hid_report[] = {
    0x06, 0x60, 0xFF,              /* Usage Page (Vendor 0xFF60)            */
    0x09, 0x61,                    /* Usage (0x61)                          */
    0xA1, 0x01,                    /* Collection (Application)              */
    0x09, 0x62,                    /*   Usage (0x62)  - IN                  */
    0x15, 0x00,                    /*   Logical Minimum (0)                 */
    0x26, 0xFF, 0x00,              /*   Logical Maximum (255)               */
    0x95, PROTO_REPORT_SIZE,       /*   Report Count (64)                   */
    0x75, 0x08,                    /*   Report Size (8)                     */
    0x81, 0x02,                    /*   Input (Data,Var,Abs)                */
    0x09, 0x63,                    /*   Usage (0x63)  - OUT                 */
    0x15, 0x00,                    /*   Logical Minimum (0)                 */
    0x26, 0xFF, 0x00,              /*   Logical Maximum (255)               */
    0x95, PROTO_REPORT_SIZE,       /*   Report Count (64)                   */
    0x75, 0x08,                    /*   Report Size (8)                     */
    0x91, 0x02,                    /*   Output (Data,Var,Abs)               */
    0xC0                           /* End Collection                        */
};

#include "usb_descriptors.h"

/* Interface 1: a real HID keyboard.
 *
 * This is what the board is for, and it is also the only thing that makes it
 * wake-capable. A vendor-defined HID is not a wake source: Windows exposed no
 * MSPower_DeviceWakeEnable for it, powercfg refused to arm it under either
 * name, and the host therefore never sent SET_FEATURE(DEVICE_REMOTE_WAKEUP) —
 * measured as 65 detected presses against 0 grants while genuinely suspended.
 * A keyboard is the canonical wake source on every OS.
 *
 * The cost, and it is real: keyboard usages are what make macOS demand Input
 * Monitoring before any process may open the device, so the telemetry tooling
 * now needs that permission granted once to whatever runs it. That was the
 * reason this interface was kept out until now (see main.c), but it buys a
 * convenience that has to be spent the moment the keyboard exists at all.
 *
 * Boot protocol, 6KRO — which is exactly the six keys this board has fitted.
 * NKRO is a later concern and belongs with the keymap layer. */
static const uint8_t desc_hid_report_kbd[] = {
    TUD_HID_REPORT_DESC_KEYBOARD()
};

const uint8_t *tud_hid_descriptor_report_cb(uint8_t instance)
{
  return (instance == ITF_NUM_KBD) ? desc_hid_report_kbd : desc_hid_report;
}

/* --------------------------------------------------------- configuration */
#define EP_HID_OUT     0x01
#define EP_HID_IN      0x81
#define EP_KBD_IN      0x82

/* interface 9 + XUSB device descriptor 17 + two endpoints 7 each = 40 (0x28) */
#define XUSB_DESC_LEN     (9 + 17 + 7 + 7)

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN \
                                               + TUD_HID_DESC_LEN \
                                               + XUSB_DESC_LEN)

static const uint8_t desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    /* The trailing 1 is bInterval. At high speed the interval is
     * 2^(bInterval-1) microframes, so 1 = one 125 us microframe = 8 kHz. */
    TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE,
                             sizeof(desc_hid_report), EP_HID_OUT, EP_HID_IN,
                             CFG_TUD_HID_EP_BUFSIZE, 1),

    /* bInterval 1 = one 125 us microframe. The whole point of the board is that
     * a keypress is not waiting on a poll, so the keyboard gets the same 8 kHz
     * the telemetry interface does. HID_ITF_PROTOCOL_KEYBOARD is what makes the
     * host treat it as a boot keyboard — and as a wake source. */
    TUD_HID_DESCRIPTOR(ITF_NUM_KBD, 0, HID_ITF_PROTOCOL_KEYBOARD,
                       sizeof(desc_hid_report_kbd), EP_KBD_IN,
                       CFG_TUD_HID_EP_BUFSIZE, 1),

    /* ---- XUSB game controller.  [MS-XUSBI] section 3.2.1.1.1, Table 46. ----
     *
     * Written from Microsoft's published specification rather than adapted from
     * either of the two open implementations of it, both of which are copyleft.
     * Every value below is traceable to a numbered table; the three that differ
     * from the tables are marked, and differ only because the tables describe a
     * single-function Microsoft pad rather than a composite device.
     *
     * bInterfaceNumber is 2, not Table 46's 0, because ours is the third
     * interface. iInterface is 0, not 1: Table 46's 1 would resolve to our
     * manufacturer string, and Table 15 marks the field optional. */
    9, TUSB_DESC_INTERFACE, ITF_NUM_XUSB, 0, 2, 0xFF, 0x5D, 0x01, 0x00,

    /* XUSB Interface Device Descriptor — section 2.2.4.5 Table 17, with the
     * wired values of section 3.2.1.1.2 Table 47. Type 0x21 is the wired device;
     * 0x22 would be a wireless adapter, which has a different report layout.
     * bLength is 0x11 (17): Table 47's offsets run 0..16 with a field at 16.
     *
     * Emitted inline because 2.2.4.5 requires it between the interface and the
     * endpoints and says it "cannot be retrieved separately" — so a
     * GET_DESCRIPTOR for type 0x21 is never serviced. TinyUSB's descriptor walk
     * skips unknown types, so it passes through untouched.
     *
     * The two wReports words are spelled as byte pairs rather than u16 literals
     * so the substitution stays visible: the high byte of each IS an endpoint
     * address and must track EP_XUSB_IN / EP_XUSB_OUT. Byte order comes from
     * USB 2.0 section 8.1 — [MS-XUSBI] never states descriptor endianness. */
    0x11, 0x21, 0x00, 0x01, 0x01,
    0x25, EP_XUSB_IN,  0x14, 0x00, 0x00, 0x00, 0x00,
    0x13, EP_XUSB_OUT, 0x08, 0x00, 0x00,

    /* Sections 3.2.1.1.3 / 3.2.1.1.4, Tables 48 and 49. Both say the endpoint
     * address "is determined by the individual chip design", so EP3 is fine.
     *
     * bInterval is the one field that must NOT be copied numerically. Table 16
     * defines it in MILLISECONDS, which is the full-speed encoding, and the
     * spec covers low and full speed only. At high speed the field is an
     * exponent: 2^(bInterval-1) microframes. So 5 = 2 ms, which is Table 10's
     * stated minimum for interrupt IN, and 6 = 4 ms for OUT. Copying Table 48's
     * 4 and Table 49's 8 literally would give 1 ms and 16 ms. */
    7, TUSB_DESC_ENDPOINT, EP_XUSB_IN,  TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(32), 5,
    7, TUSB_DESC_ENDPOINT, EP_XUSB_OUT, TUSB_XFER_INTERRUPT, U16_TO_U8S_LE(32), 6,
};

/* wTotalLength in the config descriptor is CONFIG_TOTAL_LEN, but the bytes the
 * host actually receives are sizeof(desc_configuration). If those ever disagree
 * the host reads past the end or truncates an interface, and the failure looks
 * like "the device enumerates but one function is missing" — which is a long
 * afternoon. The XUSB block is hand-written bytes with no macro to keep them in
 * step, so this is not hypothetical. */
_Static_assert(sizeof(desc_configuration) == CONFIG_TOTAL_LEN,
               "config descriptor length does not match wTotalLength");

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
  (void)index;
  return desc_configuration;
}

/* --------------------------------------------------------------- strings */
static const char *string_desc_arr[] = {
    (const char[]){0x09, 0x04},   /* 0: en-US */
    "Bohe Labs",                  /* 1: manufacturer */
    "Giris",                      /* 2: product      */
    NULL,                         /* 3: serial — filled from the 96-bit UID */
};

static uint16_t _desc_str[33];

/* [MS-XUSBI] section 2.2.4.6, Table 18. Fixed 18 bytes, always at index 0xEE.
 * The MSFT100 signature bytes are given literally by the table.
 *
 * Cannot go through the _desc_str widening path below: that one measures with
 * strlen and builds a length from it, and this is fixed-width UTF-16 with a
 * vendor code and a pad byte rather than a NUL-terminated string. */
static const uint16_t msos_string_desc[9] = {
    (uint16_t)((TUSB_DESC_STRING << 8) | 0x12),   /* bLength 18, type STRING */
    'M', 'S', 'F', 'T', '1', '0', '0',
    (uint16_t)MSOS_VENDOR_CODE,                   /* low byte code, high byte pad */
};

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void)langid;
  size_t chr_count = 0;

  /* Before the bounds check below, which would otherwise reject 0xEE and STALL.
   * usbd takes the length from byte 0 of whatever we return and never consults
   * string_desc_arr, so indices 0..3 are unaffected.
   *
   * Served unconditionally: this probe happens during enumeration, before any
   * driver is loaded, against the whole device. A bug here does not degrade XUSB
   * — it breaks enumeration of the keyboard and the viewer interface too. */
  if (index == 0xEEu) return msos_string_desc;

  if (index == 0) {
    memcpy(&_desc_str[1], string_desc_arr[0], 2);
    chr_count = 1;
  } else {
    if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;

    const char *str = (index == 3) ? uid_serial() : string_desc_arr[index];
    chr_count = strlen(str);
    if (chr_count > 31) chr_count = 31;

    for (size_t i = 0; i < chr_count; i++) _desc_str[1 + i] = str[i];
  }

  _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
  return _desc_str;
}
