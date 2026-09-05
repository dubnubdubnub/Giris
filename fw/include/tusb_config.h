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
#define CFG_TUD_VENDOR            0

#define CFG_TUD_HID_EP_BUFSIZE    64

#endif
