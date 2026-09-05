/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
/*
 * usb_descriptors.c — one raw-HID interface, high speed, bInterval = 1.
 *
 * Deliberately no keyboard interface yet: with no keyboard usages anywhere in
 * the descriptor, macOS does not require Input Monitoring permission to open
 * this device, so the browser viewer just works.
 */
#include "tusb.h"
#include "protocol.h"
#include "uid.h"

#define USB_VID   0x1209   /* pid.codes — open-source range, fine for a dev board */
#define USB_PID   0x0001   /* placeholder; claim a real one before anything ships */
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
    .bcdDevice          = 0x0100,
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

#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_INOUT_DESC_LEN \
                                               + TUD_HID_DESC_LEN)

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
};

const uint8_t *tud_descriptor_configuration_cb(uint8_t index)
{
  (void)index;
  return desc_configuration;
}

/* --------------------------------------------------------------- strings */
static const char *string_desc_arr[] = {
    (const char[]){0x09, 0x04},   /* 0: en-US */
    "Giris",                      /* 1: manufacturer */
    "Giris osu pad (telemetry)",  /* 2: product      */
    NULL,                         /* 3: serial — filled from the 96-bit UID */
};

static uint16_t _desc_str[33];

const uint16_t *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  (void)langid;
  size_t chr_count = 0;

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
