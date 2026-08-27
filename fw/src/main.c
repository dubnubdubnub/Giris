/*
 * main.c — Giris firmware, first light.
 *
 * What this does today: clock to 216 MHz, prove the SK6812 chain works, and put a
 * square wave on the scope pin so the clock tree can be checked against a probe.
 * That is deliberately all of it — this is the bring-up skeleton the architecture
 * doc calls Phase 1, not the keyboard.
 *
 * See ../../hw/TMR2615F_osu_pad/firmware_architecture.md for what comes next.
 */
#include "board.h"
#include "clock.h"
#include "sk6812.h"

static uint32_t leds[LED_COUNT];

/* 8-bit hue -> 0x00GGRRBB, dim enough that the whole chain is safe on any port. */
static uint32_t hue(uint8_t h, uint8_t v)
{
  uint8_t r, g, b;
  const uint8_t seg = h / 43u;
  const uint8_t f   = (uint8_t)((h - (seg * 43u)) * 6u);

  switch (seg) {
    case 0:  r = 255;            g = f;              b = 0;   break;
    case 1:  r = (uint8_t)(255 - f); g = 255;        b = 0;   break;
    case 2:  r = 0;              g = 255;            b = f;   break;
    case 3:  r = 0;              g = (uint8_t)(255 - f); b = 255; break;
    case 4:  r = f;              g = 0;              b = 255; break;
    default: r = 255;            g = 0;              b = (uint8_t)(255 - f); break;
  }

  r = (uint8_t)((r * v) / 255u);
  g = (uint8_t)((g * v) / 255u);
  b = (uint8_t)((b * v) / 255u);

  return ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
}

static void scope_pin_init(void)
{
  crm_periph_clock_enable(CRM_GPIOD_PERIPH_CLOCK, TRUE);

  gpio_init_type gi;
  gpio_default_para_init(&gi);
  gi.gpio_pins           = SCOPE_PIN;
  gi.gpio_mode           = GPIO_MODE_OUTPUT;
  gi.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gi.gpio_pull           = GPIO_PULL_NONE;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(SCOPE_PORT, &gi);
}

int main(void)
{
  board_clock_init();
  board_delay_cycles_init();

  /* PC13 (AP22653 enable) is deliberately left untouched. It is Hi-Z at reset and
   * R27's 10k pulldown holds the 5 V source OFF. Driving it before link
   * arbitration would push VBUS out of J1 into whatever is plugged in. */

  scope_pin_init();
  sk6812_init();

  uint8_t phase = 0;

  for (;;) {
    /* Pixel 0 is the level shifter — keep it dark. */
    leds[0] = 0;
    for (uint32_t i = LED_FIRST_KEY_PIXEL; i < LED_COUNT; i++) {
      leds[i] = hue((uint8_t)(phase + i * 36u), 24);   /* low value: 6 LEDs, any port */
    }
    sk6812_write(leds, LED_COUNT);

    /* Heartbeat on PD2 (J6 pin 17): one pulse per frame. Scope it to confirm the
     * 216 MHz clock tree — the pulse should be 100 us wide, not 100 us * (216/f). */
    SCOPE_PORT->scr = SCOPE_PIN;
    board_delay_us(100);
    SCOPE_PORT->clr = SCOPE_PIN;

    board_delay_us(20000);
    phase++;
  }
}
