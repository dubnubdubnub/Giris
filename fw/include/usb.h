/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
#ifndef GIRIS_USB_H
#define GIRIS_USB_H

#include <stdint.h>

/* Clock/PHY bring-up for OTG_HS, then tud_init(1). Call after board_clock_init(). */
void usb_init(void);

/* Pump TinyUSB and the telemetry stream. Call from the main loop. */
void usb_task(void);

/* Jump to the Artery ROM bootloader (DFU on the FS port J2) without touching
 * the BOOT0/NRST buttons. */
void usb_jump_to_bootloader(void);

/* Call FIRST in main(), before clocks. Jumps to the ROM bootloader if the
 * previous run asked for it. */
void usb_bootloader_check(void);

#define GIRIS_BOOT_MAGIC  0xB007DF00u

#endif
