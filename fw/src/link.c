#include "board.h"
#include "clock.h"
#include "link.h"

static link_mode_t current_mode = LINK_MODE_OFF;

void link_init(void)
{
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_USART6_PERIPH_CLOCK, TRUE);

  /* PB12 /LM_ST already has R5 pulling it to +3.3V, and PB12 is OTGHS_ID in
   * MUX A — leave it a plain input and never mux it. PB10 /AP_FAULT has R6. */
  gpio_init_type gi;
  gpio_default_para_init(&gi);
  gi.gpio_pins = LINK_POWERED_PIN | PWR_FAULT_PIN;
  gi.gpio_mode = GPIO_MODE_INPUT;
  gi.gpio_pull = GPIO_PULL_NONE;
  gpio_init(GPIOB, &gi);

  /* PC13 / AP22653 EN, input, NO PULL. The fitted part is the active-high
   * variant, so a pull-up here would source 5 V out of J1. Reading it tells us
   * whether R10 is actually holding it down. */
  gi.gpio_pins = PWR_SOURCE_EN_PIN;
  gpio_init(PWR_SOURCE_EN_PORT, &gi);
}

uint8_t link_sense(void)
{
  uint8_t s = 0;
  if (gpio_input_data_bit_read(LINK_POWERED_PORT, LINK_POWERED_PIN)) s |= 1u;
  if (gpio_input_data_bit_read(PWR_FAULT_PORT, PWR_FAULT_PIN))       s |= 2u;
  if (gpio_input_data_bit_read(PWR_SOURCE_EN_PORT, PWR_SOURCE_EN_PIN)) s |= 4u;
  if (gpio_input_data_bit_read(LINK_PORT, LINK_PIN_PC6))               s |= 8u;
  if (gpio_input_data_bit_read(LINK_PORT, LINK_PIN_PC7))               s |= 16u;
  return s;
}

void link_hold(uint8_t pin_sel, bool drive_low)
{
  const uint16_t pin = pin_sel ? LINK_PIN_PC7 : LINK_PIN_PC6;

  usart_enable(USART6, FALSE);
  current_mode = LINK_MODE_OFF;

  gpio_init_type gi;
  gpio_default_para_init(&gi);
  gi.gpio_pins           = pin;
  gi.gpio_mode           = drive_low ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
  gi.gpio_out_type       = GPIO_OUTPUT_OPEN_DRAIN;
  gi.gpio_pull           = GPIO_PULL_NONE;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  if (drive_low) gpio_bits_reset(LINK_PORT, pin);
  gpio_init(LINK_PORT, &gi);
}

static void pins_release(void)
{
  gpio_init_type gi;
  gpio_default_para_init(&gi);
  gi.gpio_pins = LINK_PIN_PC6 | LINK_PIN_PC7;
  gi.gpio_mode = GPIO_MODE_INPUT;
  gi.gpio_pull = GPIO_PULL_NONE;
  gpio_init(LINK_PORT, &gi);
}

static void pin_af(uint16_t pin, gpio_output_type drive, gpio_pull_type pull, uint8_t mux)
{
  gpio_init_type gi;
  gpio_default_para_init(&gi);
  gi.gpio_pins           = pin;
  gi.gpio_mode           = GPIO_MODE_MUX;
  gi.gpio_out_type       = drive;
  gi.gpio_pull           = pull;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(LINK_PORT, &gi);

  gpio_pin_mux_config(LINK_PORT,
                      (pin == LINK_PIN_PC6) ? GPIO_PINS_SOURCE6 : GPIO_PINS_SOURCE7,
                      (gpio_mux_sel_type)mux);
}

void link_configure_mux(link_mode_t mode, uint32_t baud, uint8_t mux)
{
  usart_enable(USART6, FALSE);
  pins_release();
  current_mode = mode;
  if (mode == LINK_MODE_OFF) return;

  const bool half = (mode == LINK_MODE_HD_PC6) || (mode == LINK_MODE_HD_PC7) ||
                    (mode == LINK_MODE_HD_PC6_PP);
  const bool swap = (mode == LINK_MODE_HD_PC7) || (mode == LINK_MODE_FD_SWAP);
  const gpio_output_type hdrive = (mode == LINK_MODE_HD_PC6_PP) ? GPIO_OUTPUT_PUSH_PULL
                                                                : GPIO_OUTPUT_OPEN_DRAIN;

  if (half) {
    /* One wire, wired-AND. Open drain plus the board's 10k (and a second 10k
     * once a peer is attached); the internal pull-up would only fight it, so
     * leave it off and let the fitted resistor set the rise. */
    pin_af(swap ? LINK_PIN_PC7 : LINK_PIN_PC6, hdrive, GPIO_PULL_NONE, mux);
  } else {
    pin_af(LINK_PIN_PC6, GPIO_OUTPUT_PUSH_PULL, GPIO_PULL_NONE, mux);
    pin_af(LINK_PIN_PC7, GPIO_OUTPUT_PUSH_PULL, GPIO_PULL_NONE, mux);
  }

  usart_init(USART6, baud, USART_DATA_8BITS, USART_STOP_1_BIT);
  usart_parity_selection_config(USART6, USART_PARITY_NONE);
  usart_transmit_receive_pin_swap(USART6, swap ? TRUE : FALSE);
  usart_single_line_halfduplex_select(USART6, half ? TRUE : FALSE);
  usart_transmitter_enable(USART6, TRUE);
  usart_receiver_enable(USART6, TRUE);
  usart_enable(USART6, TRUE);

  /* Drain anything the reconfiguration shook loose. */
  (void)usart_data_receive(USART6);
  usart_flag_clear(USART6, USART_ROERR_FLAG | USART_FERR_FLAG |
                           USART_NERR_FLAG  | USART_PERR_FLAG);
}

void link_configure(link_mode_t mode, uint32_t baud)
{
  link_configure_mux(mode, baud, LINK_MUX_USART6);
}

/* --------------------------------------------------------------- probing */

static uint8_t probe_pad(uint16_t pin)
{
  gpio_init_type gi;
  gpio_default_para_init(&gi);
  gi.gpio_pins           = pin;
  gi.gpio_mode           = GPIO_MODE_OUTPUT;
  gi.gpio_out_type       = GPIO_OUTPUT_OPEN_DRAIN;   /* never fight the pull-up,
                                                      * and never fight a peer */
  gi.gpio_pull           = GPIO_PULL_NONE;
  gi.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(LINK_PORT, &gi);

  uint8_t r = 0;
  gpio_bits_reset(LINK_PORT, pin);                   /* drive low */
  board_delay_us(20);
  if ((LINK_PORT->idt & pin) == 0) r |= 1u;
  gpio_bits_set(LINK_PORT, pin);                     /* release to the 10k */
  board_delay_us(20);
  if ((LINK_PORT->idt & pin) != 0) r |= 2u;

  gi.gpio_mode = GPIO_MODE_INPUT;
  gpio_init(LINK_PORT, &gi);
  return r;
}

static uint8_t probe_sense(gpio_pull_type pull)
{
  gpio_init_type gi;
  gpio_default_para_init(&gi);
  gi.gpio_pins = LINK_POWERED_PIN | PWR_FAULT_PIN;
  gi.gpio_mode = GPIO_MODE_INPUT;
  gi.gpio_pull = pull;
  gpio_init(GPIOB, &gi);
  board_delay_us(200);
  const uint8_t s = link_sense();

  gi.gpio_pull = GPIO_PULL_NONE;
  gpio_init(GPIOB, &gi);
  return s;
}

void link_probe(link_mode_t mode, uint32_t baud, link_probe_t *out)
{
  for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;

  link_configure(LINK_MODE_OFF, 0);
  out->pads     = (uint8_t)(probe_pad(LINK_PIN_PC6) | (probe_pad(LINK_PIN_PC7) << 2));
  out->sense_pu = probe_sense(GPIO_PULL_UP);
  out->sense_pd = probe_sense(GPIO_PULL_DOWN);

  if (mode == LINK_MODE_OFF) return;

  const bool swap = (mode == LINK_MODE_HD_PC7) || (mode == LINK_MODE_FD_SWAP);
  const uint16_t txpin = swap ? LINK_PIN_PC7 : LINK_PIN_PC6;
  const uint16_t n = 4000;
  out->mux_samples = n;

  for (uint8_t mux = 0; mux < 16; mux++) {
    link_configure_mux(mode, baud, mux);
    uint16_t lows = 0;
    for (uint16_t i = 0; i < n; i++) {
      if (usart_flag_get(USART6, USART_TDBE_FLAG) == SET) usart_data_transmit(USART6, 0x00);
      if ((LINK_PORT->idt & txpin) == 0) lows++;
    }
    out->mux_low[mux] = lows;
    /* Let the shift register empty so the next index starts from an idle line. */
    board_delay_us(200);
  }

  link_configure(LINK_MODE_OFF, 0);
}

static uint8_t collect_errors(void)
{
  uint8_t e = 0;
  if (usart_flag_get(USART6, USART_PERR_FLAG)  == SET) e |= 1u;
  if (usart_flag_get(USART6, USART_FERR_FLAG)  == SET) e |= 2u;
  if (usart_flag_get(USART6, USART_NERR_FLAG)  == SET) e |= 4u;
  if (usart_flag_get(USART6, USART_ROERR_FLAG) == SET) e |= 8u;
  return e;
}

void link_selftest(link_mode_t mode, uint32_t baud, uint16_t nbytes,
                   link_role_t role, uint16_t timeout_ms, link_test_t *out)
{
  for (uint32_t i = 0; i < sizeof(*out); i++) ((uint8_t *)out)[i] = 0;
  out->mode = (uint8_t)mode;
  out->baud = baud;

  if (mode == LINK_MODE_OFF || nbytes == 0) return;
  link_configure(mode, baud);

  if (role == LINK_ROLE_TX) {
    for (uint16_t i = 0; i < nbytes; i++) {
      uint32_t guard = system_core_clock / 100u;      /* 10 ms, bounded */
      while (usart_flag_get(USART6, USART_TDBE_FLAG) == RESET && guard) guard--;
      if (!guard) { out->timeouts++; break; }
      usart_data_transmit(USART6, (uint8_t)i);
      out->sent++;
    }
    uint32_t guard = system_core_clock / 100u;
    while (usart_flag_get(USART6, USART_TDC_FLAG) == RESET && guard) guard--;
    goto done;
  }

  if (role == LINK_ROLE_RX) {
    /* Turn the transmitter OFF while listening. In single-wire half-duplex the
     * TX pin is the bus, and if this silicon gates the receiver on TEN — which
     * would also explain why it never echoes itself — a listener with TEN=1 is
     * deaf. Costs nothing in full duplex, where the two are independent. */
    usart_transmitter_enable(USART6, FALSE);
    usart_receiver_enable(USART6, TRUE);
    (void)usart_data_receive(USART6);
    usart_flag_clear(USART6, USART_ROERR_FLAG | USART_FERR_FLAG |
                             USART_NERR_FLAG  | USART_PERR_FLAG);

    /* Blocks the main loop for up to timeout_ms. USB transfers keep running out
     * of the DWC2 ISR; only tud_task() stalls, and nothing needs it meanwhile. */
    const uint32_t limit = (system_core_clock / 1000u) * (uint32_t)timeout_ms;
    const uint32_t t0 = DWT->CYCCNT;
    while (out->received < nbytes && (DWT->CYCCNT - t0) < limit) {
      if (usart_flag_get(USART6, USART_RDBF_FLAG) != SET) continue;
      const uint8_t rx = (uint8_t)usart_data_receive(USART6);
      const uint8_t want = (uint8_t)out->received;
      out->received++;
      if (rx != want) {
        out->mismatched++;
        if (out->mismatched == 1) { out->first_bad_tx = want; out->first_bad_rx = rx; }
      }
    }
    if (out->received < nbytes) out->timeouts = (uint16_t)(nbytes - out->received);
    goto done;
  }

  /* Two full byte periods is a generous ceiling at any baud on this ladder and
   * still bounded: 17 us at 9 Mbaud, 174 us at 115200. */
  const uint32_t per_byte  = (system_core_clock / baud) * 10u;
  const uint32_t deadline  = per_byte * 2u + 1000u;

  for (uint16_t i = 0; i < nbytes; i++) {
    const uint8_t tx = (uint8_t)i;

    while (usart_flag_get(USART6, USART_TDBE_FLAG) == RESET) {
    }
    usart_data_transmit(USART6, tx);
    out->sent++;

    const uint32_t t0 = DWT->CYCCNT;
    bool got = false;
    while ((DWT->CYCCNT - t0) < deadline) {
      if (usart_flag_get(USART6, USART_RDBF_FLAG) == SET) { got = true; break; }
    }

    if (!got) {
      out->timeouts++;
      if (out->timeouts == 1) { out->first_bad_tx = tx; out->first_bad_rx = 0; }
      continue;
    }

    const uint8_t rx = (uint8_t)usart_data_receive(USART6);
    out->received++;
    if (rx != tx) {
      out->mismatched++;
      if (out->mismatched == 1) { out->first_bad_tx = tx; out->first_bad_rx = rx; }
    }
  }

done:
  out->err_flags = collect_errors();
  out->sts   = USART6->sts;
  out->ctrl1 = USART6->ctrl1;
  out->ctrl2 = USART6->ctrl2;
  out->ctrl3 = USART6->ctrl3;
  usart_flag_clear(USART6, USART_ROERR_FLAG | USART_FERR_FLAG |
                           USART_NERR_FLAG  | USART_PERR_FLAG);
}
