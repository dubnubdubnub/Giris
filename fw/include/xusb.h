/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
/*
 * xusb.h — the Xbox 360 controller interface, which is how analog travel is
 * going to reach games.
 *
 * Why this exists at all: no HID report descriptor can ever make a device
 * visible to XInput. Windows' inbox xusb22.sys binds by COMPATIBLE ID, and the
 * shipped INF matches exactly five — MS_COMP_XUSB10, MS_COMP_XUSB20, and three
 * Microsoft VID/PID pairs. It contains no class/subclass/protocol rule at all.
 * So a vendor interface of 0xFF/0x5D/0x01 is necessary but nowhere near
 * sufficient: the compatible ID has to be delivered too, via the MS OS 1.0
 * descriptor chain in usb_descriptors.c. That is the whole trick, and it is why
 * a keyboard with our own VID can present itself to XInput without asking
 * anyone for a driver.
 *
 * Written from Microsoft's [MS-XUSBI] specification. Both existing open
 * implementations of this interface are copyleft, and this firmware is
 * Apache-2.0, so nothing here is adapted from either — the spec's own IPR notice
 * permits copying the documentation in order to build implementations, which is
 * exactly what was done. Table numbers are cited where a value comes from one.
 *
 * The interface is ALWAYS present once built in. There is no runtime enable of
 * the interface itself: that would mean mutating the configuration descriptor
 * while usbd may be reading it, forcing a re-enumeration, and burning a second
 * bcdDevice identity to avoid a stale devnode. What xusb_set_enabled() gates is
 * whether reports are transmitted. Gated off, the pad reads dead centre rather
 * than being absent — and it is off at boot, for the same reason keys.c gates
 * keyboard output: a board that shoves a virtual stick into whatever game has
 * focus is a bad afternoon.
 */
#ifndef GIRIS_XUSB_H
#define GIRIS_XUSB_H

#include <stdint.h>
#include <stdbool.h>

void xusb_init(void);

/* From the main loop, via usb_task(). Sends at most one report and never
 * blocks or spins. */
void xusb_service(void);

/* Drop any pending state. Call on mount, unmount and suspend — a producer that
 * outlives its host is the failure that cost this project a USB controller
 * once already. */
void xusb_quiesce(void);

/* Arm the interrupt OUT endpoint. Must be called after the host has configured
 * us: TinyUSB's vendor class arms its bulk OUT automatically but NOT the
 * interrupt one, so without this every rumble and LED write from xusb22.sys
 * NAKs forever — and the LED write is the clearest proof the driver bound. */
void xusb_arm_out(void);

void xusb_set_enabled(bool on);
bool xusb_enabled(void);

/* Sticks are int8 here and scaled to the wire's int16. That is the test
 * interface's unit, not the eventual one — the travel pipeline will feed
 * calibrated depth straight to full scale. */
void xusb_set_state(int8_t lx, int8_t ly, int8_t rx, int8_t ry,
                    uint8_t lt, uint8_t rt, uint16_t buttons);

/* Called from the descriptor code when Windows asks for string index 0xEE. */
void xusb_note_msos_string(void);

/* Packed for RSP_INFO[63]. Bits 4 and 5 are the two that matter most when this
 * does not work: they separate "Windows never asked" from "Windows asked and
 * did not like the answer". */
uint8_t xusb_status(void);

#endif
