/*
 * console.c — a minimal blocking UART console for bring-up.
 *
 * This exists because the board's SWD lines are only on J9, a Tag-Connect
 * footprint. USART1 comes out on plain J6 header pins, so a USB-serial bridge
 * (the Pi Debug Probe's UART side works) gets us visibility with three wires.
 */
#include "board.h"
#include "console.h"

static volatile uint32_t stage_counter;

void console_init(void)
{
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_USART1_PERIPH_CLOCK, TRUE);

  gpio_init_type g;
  gpio_default_para_init(&g);
  g.gpio_pins           = GPIO_PINS_9 | GPIO_PINS_10;
  g.gpio_mode           = GPIO_MODE_MUX;
  g.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_pull           = GPIO_PULL_UP;
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(GPIOA, &g);

  gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE9,  GPIO_MUX_7);
  gpio_pin_mux_config(GPIOA, GPIO_PINS_SOURCE10, GPIO_MUX_7);

  /* USART1 is on APB2 = 216 MHz, so 115200 divides exactly (1875). */
  usart_init(USART1, 115200, USART_DATA_8BITS, USART_STOP_1_BIT);
  usart_transmitter_enable(USART1, TRUE);
  usart_receiver_enable(USART1, TRUE);
  usart_enable(USART1, TRUE);
}

static void putc_blocking(char c)
{
  /* Bounded so a missing console can never wedge the firmware. */
  uint32_t guard = 200000;
  while (usart_flag_get(USART1, USART_TDBE_FLAG) == RESET && guard--) {
  }
  usart_data_transmit(USART1, (uint16_t)c);
}

void console_puts(const char *s)
{
  while (*s) {
    if (*s == '\n') putc_blocking('\r');
    putc_blocking(*s++);
  }
}

void console_hex(uint32_t v)
{
  static const char d[] = "0123456789abcdef";
  char buf[11];
  buf[0] = '0'; buf[1] = 'x';
  for (int i = 0; i < 8; i++) buf[2 + i] = d[(v >> ((7 - i) * 4)) & 0xF];
  buf[10] = 0;
  console_puts(buf);
}

void console_dec(uint32_t v)
{
  char buf[11];
  int i = 10;
  buf[i] = 0;
  do { buf[--i] = (char)('0' + (v % 10u)); v /= 10u; } while (v && i);
  console_puts(&buf[i]);
}

void console_stage(const char *name)
{
  stage_counter++;
  console_puts("[");
  console_dec(stage_counter);
  console_puts("] ");
  console_puts(name);
  console_puts("\n");
}

uint32_t console_last_stage(void) { return stage_counter; }
