/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
/* TinyUSB configuration — AT32F405 OTG_HS (rhport 1) device only. */
#ifndef _TUSB_CONFIG_H_
#define _TUSB_CONFIG_H_

#define CFG_TUSB_MCU              OPT_MCU_AT32F402_405
#define CFG_TUSB_OS               OPT_OS_NONE
#define CFG_TUSB_DEBUG            0

/* rhport 1 is OTG_HS (0x40040000, 4 KB FIFO). rhport 0 is the FS core on J2,
 * which belongs to the ROM bootloader, not to us. */
#define BOARD_TUD_RHPORT          1
#define BOARD_TUD_MAX_SPEED       OPT_MODE_HIGH_SPEED

#define CFG_TUD_ENABLED           1
#define CFG_TUH_ENABLED           0
#define CFG_TUD_MAX_SPEED         BOARD_TUD_MAX_SPEED

#define CFG_TUSB_MEM_SECTION
#define CFG_TUSB_MEM_ALIGN        __attribute__((aligned(4)))

#define CFG_TUD_ENDPOINT0_SIZE    64

/* The board does not wire PB13 as a VBUS sense, so the core must not look for
 * VBUS — see usb.c, which sets GCCFG.VBUSIG by hand. */
#define CFG_TUD_VBUS_DETECT_HW    0

#define CFG_TUD_HID               2
#define CFG_TUD_CDC               0
#define CFG_TUD_MSC               0
#define CFG_TUD_MIDI              0
#define CFG_TUD_VENDOR            1
/* The XUSB (Xbox) interface rides TinyUSB's vendor class. Without this,
 * vendord_open is absent from usbd's driver table, nothing claims interface 2,
 * and the TU_ASSERT inside SET_CONFIGURATION fails — the WHOLE device stops
 * enumerating, keyboard and raw HID included. */

/* XUSB is interrupt-only ([MS-XUSBI] Table 16). These default to 0, and with
 * them off tud_vendor_int_write() and friends do not exist, so xusb.c fails to
 * link. */
#define CFG_TUD_VENDOR_EP_INT_IN          1
#define CFG_TUD_VENDOR_EP_INT_OUT         1

/* Default 64. vendord_open asserts the descriptor's packet size fits the
 * buffer, and ours is 32 ([MS-XUSBI] Tables 48/49). */
#define CFG_TUD_VENDOR_EP_INT_IN_BUFSIZE  32
#define CFG_TUD_VENDOR_EP_INT_OUT_BUFSIZE 32

/* No bulk endpoints here. Zeroing the FIFO sizes drops the stream buffers, but
 * that un-gates a raw endpoint buffer sized from CFG_TUD_VENDOR_EPSIZE — which
 * defaults to the 512-byte high-speed bulk maximum. All three move together, or
 * we carry 1 KB of dead DMA buffer. */
#define CFG_TUD_VENDOR_EPSIZE     32
#define CFG_TUD_VENDOR_RX_BUFSIZE 0
#define CFG_TUD_VENDOR_TX_BUFSIZE 0

#define CFG_TUD_HID_EP_BUFSIZE    64

#endif
