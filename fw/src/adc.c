/*
 * adc.c — the scan engine.
 *
 * ADC1 converts a 4-deep ordinary sequence (PA3, PA2, PA1, PA0) on every TMR2
 * TRGO. DMA1_CHANNEL1 lands the results in a circular 8-halfword ring:
 *
 *   slot 0..3  SEL low  (bank A): key0, key2, key4, VBUS_B/2
 *   slot 4..7  SEL high (bank B): key1, key3, key5, J5 pin 10 (floating)
 *
 * The half-transfer interrupt flips SEL to bank B, the full-transfer interrupt
 * flips it back to bank A and publishes a frame. Flipping the instant the four
 * conversions finish gives the mux the whole remainder of the TMR2 period to
 * settle — ~55 us, against a TMUX1574 tTRAN of 350 ns max.
 *
 * TMR2 runs at 16 kHz so a full A+B frame lands at 8 kHz.
 *
 * Note the interleave: adjacent keys sit on opposite mux banks, so any mux
 * settling error shows up as an alternating pattern across neighbouring keys.
 */
#include "adc.h"
#include "board.h"
#include "clock.h"

#define SCAN_SLOTS       8
#define TMR2_PERIOD      13499u   /* 216 MHz / 13500 = 16 kHz -> 8 kHz frames */

static volatile uint16_t adc_ring[SCAN_SLOTS];

/* Seqlock: odd sequence means a write is in progress. */
static volatile uint32_t pub_seq;
static volatile uint32_t pub_frame;
static volatile uint16_t pub_slot[SCAN_SLOTS];

static volatile uint32_t frame_counter;
static volatile uint32_t phase_errors;
static bool adc_cal_timed_out;

bool adc_calibration_failed(void) { return adc_cal_timed_out; }

/* Physical scan slot -> key index, or a sentinel. Shipped to the host in
 * RSP_INFO so the viewer never hardcodes it. */
static const uint8_t slot_map[SCAN_SLOTS] = {
    0, 2, 4, SLOT_VBUS_DIV,
    1, 3, 5, SLOT_UNUSED,
};

const uint8_t *adc_slot_map(void) { return slot_map; }
uint32_t adc_phase_errors(void)   { return phase_errors; }

static void gpio_setup(void)
{
  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);

  gpio_init_type g;
  gpio_default_para_init(&g);
  g.gpio_mode = GPIO_MODE_ANALOG;
  g.gpio_pins = GPIO_PINS_0 | GPIO_PINS_1 | GPIO_PINS_2 | GPIO_PINS_3;
  gpio_init(GPIOA, &g);

  gpio_default_para_init(&g);
  g.gpio_pins           = MUX_SEL_PIN;
  g.gpio_mode           = GPIO_MODE_OUTPUT;
  g.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  g.gpio_pull           = GPIO_PULL_NONE;
  g.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(MUX_SEL_PORT, &g);

  gpio_bits_reset(MUX_SEL_PORT, MUX_SEL_PIN);   /* bank A */
}

static void dma_setup(void)
{
  crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
  dma_reset(DMA1_CHANNEL1);

  dma_init_type d;
  dma_default_para_init(&d);
  d.peripheral_base_addr  = (uint32_t)&(ADC1->odt);
  d.memory_base_addr      = (uint32_t)adc_ring;
  d.direction             = DMA_DIR_PERIPHERAL_TO_MEMORY;
  d.buffer_size           = SCAN_SLOTS;
  d.peripheral_inc_enable = FALSE;
  d.memory_inc_enable     = TRUE;
  d.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_HALFWORD;
  d.memory_data_width     = DMA_MEMORY_DATA_WIDTH_HALFWORD;
  d.loop_mode_enable      = TRUE;
  d.priority              = DMA_PRIORITY_VERY_HIGH;
  dma_init(DMA1_CHANNEL1, &d);

  dmamux_enable(DMA1, TRUE);
  dmamux_init(DMA1MUX_CHANNEL1, DMAMUX_DMAREQ_ID_ADC1);

  dma_interrupt_enable(DMA1_CHANNEL1, DMA_HDT_INT, TRUE);
  dma_interrupt_enable(DMA1_CHANNEL1, DMA_FDT_INT, TRUE);

  /* Above the USB handler: this ISR is ~30 cycles and must not be delayed by
   * the USB stack, whereas USB tolerates being late by a microframe. */
  nvic_irq_enable(DMA1_Channel1_IRQn, 1, 0);
}

static void adc_setup(void)
{
  crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);

  /* adc_reset() clears ADCCOM->cctrl, so the divider must be set AFTER it.
   * Backwards and you silently run at AHB/2 = 108 MHz, ~4x over the 28 MHz
   * limit, giving plausible-looking garbage. */
  adc_reset(ADC1);
  adc_clock_div_set(ADC_DIV_8);              /* 216/8 = 27 MHz, under fADC 28 MHz */

  adc_base_config_type b;
  adc_base_default_para_init(&b);
  b.sequence_mode           = TRUE;          /* required for >1 channel */
  b.repeat_mode             = FALSE;         /* one sequence per trigger */
  b.data_align              = ADC_RIGHT_ALIGNMENT;
  b.ordinary_channel_length = 4;
  adc_base_config(ADC1, &b);

  /* Netlist order. The odd one out (VBUS_B/2, ~2.5 V) goes LAST so the three
   * sensors — which sit near each other in voltage — are not preceded by it:
   * each conversion carries ~0.39% of (previous slot - this slot) as charge
   * redistribution off the ADC sample cap. */
  adc_ordinary_channel_set(ADC1, ADC_CHANNEL_3, 1, ADC_SAMPLETIME_28_5);
  adc_ordinary_channel_set(ADC1, ADC_CHANNEL_2, 2, ADC_SAMPLETIME_28_5);
  adc_ordinary_channel_set(ADC1, ADC_CHANNEL_1, 3, ADC_SAMPLETIME_28_5);
  adc_ordinary_channel_set(ADC1, ADC_CHANNEL_0, 4, ADC_SAMPLETIME_28_5);

  adc_ordinary_conversion_trigger_set(ADC1, ADC12_ORDINARY_TRIG_TMR2TRGOUT, TRUE);
  adc_dma_mode_enable(ADC1, TRUE);

  adc_enable(ADC1, TRUE);
  board_delay_us(5);                          /* tSTAB = 42/fADC = 1.56 us */

  /* Bounded: an unbounded spin here is exactly what stops the firmware from
   * ever reaching usb_init(), and then the board looks dead with no clue why. */
  uint32_t guard = 1000000;
  adc_calibration_init(ADC1);
  while (adc_calibration_init_status_get(ADC1) && guard--) {
  }
  guard = 1000000;
  adc_calibration_start(ADC1);
  while (adc_calibration_status_get(ADC1) && guard--) {
  }
  adc_cal_timed_out = (guard == 0);
}

static void timer_setup(void)
{
  crm_periph_clock_enable(CRM_TMR2_PERIPH_CLOCK, TRUE);
  tmr_reset(TMR2);

  /* TMR2 is on APB1 (/2), so its clock is 2 x PCLK1 = 216 MHz. */
  tmr_base_init(TMR2, TMR2_PERIOD, 0);
  tmr_cnt_dir_set(TMR2, TMR_COUNT_UP);
  tmr_primary_mode_select(TMR2, TMR_PRIMARY_SEL_OVERFLOW);   /* overflow -> TRGO */
  tmr_flag_clear(TMR2, TMR_OVF_FLAG);
}

void adc_scan_init(void)
{
  gpio_setup();
  dma_setup();
  adc_setup();
  timer_setup();

  dma_channel_enable(DMA1_CHANNEL1, TRUE);
  gpio_bits_reset(MUX_SEL_PORT, MUX_SEL_PIN);
  tmr_counter_enable(TMR2, TRUE);
}

bool adc_read_frame(adc_frame_t *out)
{
  for (int attempt = 0; attempt < 4; attempt++) {
    const uint32_t s0 = pub_seq;
    if (s0 & 1u) continue;                    /* writer mid-update */

    __DMB();
    out->frame = pub_frame;
    for (int i = 0; i < SCAN_SLOTS; i++) out->slot[i] = pub_slot[i];
    __DMB();

    if (pub_seq == s0) return true;
  }
  return false;
}

void DMA1_Channel1_IRQHandler(void)
{
  if (dma_interrupt_flag_get(DMA1_HDT1_FLAG) != RESET) {
    dma_flag_clear(DMA1_HDT1_FLAG);
    MUX_SEL_PORT->scr = MUX_SEL_PIN;          /* bank A done -> select bank B */
  }

  if (dma_interrupt_flag_get(DMA1_FDT1_FLAG) != RESET) {
    dma_flag_clear(DMA1_FDT1_FLAG);
    MUX_SEL_PORT->clr = MUX_SEL_PIN;          /* bank B done -> back to bank A */

    /* This ADC has no overrun flag, so a missed DMA transfer would rotate the
     * ring forever with nothing to show for it. Slot 3 is the VBUS divider
     * (~2.5 V) and slot 7 is a floating header pin: if slot 7 ever reads higher
     * than slot 3, the phase has slipped. */
    if (adc_ring[7] > adc_ring[3]) phase_errors++;

    pub_seq++;                                /* odd: writing */
    __DMB();
    pub_frame = ++frame_counter;
    for (int i = 0; i < SCAN_SLOTS; i++) pub_slot[i] = adc_ring[i];
    __DMB();
    pub_seq++;                                /* even: stable */
  }
}
