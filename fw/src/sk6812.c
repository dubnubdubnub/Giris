/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
/*
 * sk6812.c — bit-banged SK6812MINI-E driver, bring-up only.
 *
 * SK6812MINI-E timing:  T0H 300 ns, T1H 600 ns, bit period 1.25 us, reset > 80 us.
 * At 216 MHz one cycle is 4.63 ns, so those are 65 / 130 / 259 cycles.
 *
 * The chain is 7 pixels. Pixel 0 (U12) is the sacrificial level-shifter sitting
 * on the 1N4148-dropped ~4.3 V rail: PB9 at 3.3 V clears its VIH, and its 4.3 V
 * DOUT then clears the 5 V VIH of pixels 1..6. Keep pixel 0 dark or dim.
 */
#include "board.h"
#include "clock.h"
#include "sk6812.h"

#define CYC_PER_NS_NUM  (BOARD_SCLK_HZ / 1000000U)   /* cycles per microsecond */

#define T0H_CYCLES  ((300U  * CYC_PER_NS_NUM) / 1000U)   /*  65 @216 MHz */
#define T1H_CYCLES  ((600U  * CYC_PER_NS_NUM) / 1000U)   /* 130 */
#define BIT_CYCLES  ((1200U * CYC_PER_NS_NUM) / 1000U)   /* 259 */

void sk6812_init(void)
{
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);

  gpio_init_type gi;
  gpio_default_para_init(&gi);
  gi.gpio_pins           = LED_DATA_PIN;
  gi.gpio_mode           = GPIO_MODE_OUTPUT;
  gi.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gi.gpio_pull           = GPIO_PULL_NONE;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(LED_DATA_PORT, &gi);

  LED_DATA_PORT->clr = LED_DATA_PIN;
  board_delay_us(100);            /* latch/reset */
}

/* One 24-bit pixel, MSB first, GRB order. Must run with interrupts off. */
static void sk6812_write_pixel(uint32_t grb)
{
  for (int32_t bit = 23; bit >= 0; bit--) {
    const uint32_t high = (grb & (1UL << bit)) ? T1H_CYCLES : T0H_CYCLES;
    const uint32_t t0   = DWT->CYCCNT;

    LED_DATA_PORT->scr = LED_DATA_PIN;
    while ((DWT->CYCCNT - t0) < high) {
    }
    LED_DATA_PORT->clr = LED_DATA_PIN;
    while ((DWT->CYCCNT - t0) < BIT_CYCLES) {
    }
  }
}

void sk6812_write(const uint32_t *grb, uint32_t count)
{
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();

  for (uint32_t i = 0; i < count; i++) {
    sk6812_write_pixel(grb[i]);
  }

  __set_PRIMASK(primask);

  LED_DATA_PORT->clr = LED_DATA_PIN;
  board_delay_us(100);            /* > 80 us reset so the chain latches */
}
