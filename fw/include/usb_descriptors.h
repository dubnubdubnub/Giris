/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
#ifndef GIRIS_USB_DESCRIPTORS_H
#define GIRIS_USB_DESCRIPTORS_H

/* Interface numbers double as TinyUSB HID instance indices — which holds ONLY
 * because both HID interfaces come first and are contiguous from 0. hidd_open()
 * assigns instances by first-free ep_in slot in configuration-descriptor order,
 * so anything inserted ahead of ITF_NUM_KBD silently renumbers the keyboard:
 * tud_hid_n_report(2, ...) would then index past CFG_TUD_HID and be rejected by
 * TU_VERIFY, and the keyboard would go quiet with no compile error. XUSB is not
 * HID and goes LAST for that reason. */
enum { ITF_NUM_HID = 0, ITF_NUM_KBD = 1, ITF_NUM_XUSB = 2, ITF_NUM_TOTAL };

#define EP_XUSB_OUT       0x03
#define EP_XUSB_IN        0x83

/* MS OS 1.0 vendor code, carried in the index-0xEE string descriptor and used as
 * the bRequest for the Extended Compatible ID.
 *
 * NOT 0x00 or 0x01. usbd funnels every vendor-type control request into the one
 * tud_vendor_control_xfer_cb, so this shares a bRequest namespace with
 * [MS-XUSBI] Table 29's own vendor requests — SET_CONTROL (0x41/0x00),
 * GET_DEVICE_ID (0xC0/0x01) and SET_BIND_INFO (0x40/0x01). GET_DEVICE_ID would
 * collide on direction and value both. */
#define MSOS_VENDOR_CODE  0x20

#endif
