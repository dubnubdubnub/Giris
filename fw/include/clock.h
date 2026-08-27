#ifndef GIRIS_CLOCK_H
#define GIRIS_CLOCK_H

#include <stdint.h>

/* 12 MHz HEXT -> 216 MHz SCLK, 48 MHz for OTG_FS. Call first, before anything. */
void board_clock_init(void);

/* DWT cycle counter, used as the timebase for bit-banged timing. */
void board_delay_cycles_init(void);
void board_delay_us(uint32_t us);

#endif
