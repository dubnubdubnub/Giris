#ifndef GIRIS_SK6812_H
#define GIRIS_SK6812_H

#include <stdint.h>

/*
 * Bit-banged SK6812 driver — bring-up only.
 *
 * This blocks with interrupts disabled for ~30 us per pixel, which is fine while
 * nothing else runs but is exactly what the architecture doc says not to ship.
 * The real driver is SPI + DMA at 3.375 MHz with 4-bit symbols (0b1000 / 0b1100
 * -> 296/889/593/593 ns, inside spec), which costs no CPU.
 */
void sk6812_init(void);

/* grb[] holds LED_COUNT entries of 0x00GGRRBB. Pixel 0 is the sacrificial
 * level-shifter; keep it dark or dim. */
void sk6812_write(const uint32_t *grb, uint32_t count);

#endif
