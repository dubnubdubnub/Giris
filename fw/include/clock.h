/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
#ifndef GIRIS_CLOCK_H
#define GIRIS_CLOCK_H

#include <stdint.h>

#include <stdbool.h>

/* Enable HEXT and wait, bounded. False means the 12 MHz crystal never started —
 * which also means USB HS can never work, since that crystal is the HS PHY's
 * only legal reference. */
bool board_clock_try_hext(void);

/* 12 MHz HEXT -> 216 MHz SCLK, 48 MHz for OTG_FS. False if any step timed out. */
bool board_clock_init(void);

/* DWT cycle counter, used as the timebase for bit-banged timing. */
void board_delay_cycles_init(void);
void board_delay_us(uint32_t us);

#endif
