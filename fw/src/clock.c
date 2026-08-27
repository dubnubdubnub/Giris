/*
 * clock.c — bring the AT32F405RBT7-7 up to 216 MHz from the 12 MHz crystal.
 *
 * 12 MHz HEXT -> PLL (MS=1, NS=72) -> VCO 864 MHz -> FP /4 -> SCLK 216 MHz
 *                                                 -> FU /18 -> 48 MHz for OTG_FS
 *
 * The VCO must land inside 500-1000 MHz and the post-MS input inside 2-16 MHz;
 * 12/1/72 gives 12 MHz and 864 MHz, both legal.
 *
 * The 12 MHz crystal is not a free choice: it is the OTG_HS PHY's only legal
 * reference (CRM->otghs_bit.usbhs_phy12_sel has exactly one valid value) and it
 * is specified at +-50 ppm total.
 */
#include "board.h"
#include "clock.h"

bool board_clock_try_hext(void)
{
  crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, TRUE);

  /* Bounded. An unbounded spin here is invisible from outside: no console, no
   * USB, no LEDs — the board just looks dead. */
  uint32_t guard = 4000000;
  while (crm_flag_get(CRM_HEXT_STABLE_FLAG) != SET && guard) guard--;
  return guard != 0;
}

bool board_clock_init(void)
{
  /* 216 MHz needs the LDO at 1.3 V and six flash wait states, and both must be
   * in place BEFORE the switch to PLL. */
  pwc_ldo_output_voltage_set(PWC_LDO_OUTPUT_1V3);
  flash_psr_set(FLASH_WAIT_CYCLE_6);

  if (!board_clock_try_hext()) return FALSE;

  crm_pll_config(CRM_PLL_SOURCE_HEXT, 72, 1, CRM_PLL_FP_4);
  crm_pllu_div_set(CRM_PLL_FU_18);            /* 864 / 18 = 48 MHz for OTG_FS */

  crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
  {
    uint32_t guard = 4000000;
    while (crm_flag_get(CRM_PLL_STABLE_FLAG) != SET && guard) guard--;
    if (!guard) return FALSE;
  }

  /* AHB /1 = 216 MHz.  APB2 /1 = 216 MHz (so USART6 and the ADC clock come off
   * the fast bus).  APB1 must be /2 = 108 MHz — it cannot exceed 120 MHz. */
  crm_ahb_div_set(CRM_AHB_DIV_1);
  crm_apb2_div_set(CRM_APB2_DIV_1);
  crm_apb1_div_set(CRM_APB1_DIV_2);

  /* Auto-step is required across a large sysclk jump and must be turned back off. */
  crm_auto_step_mode_enable(CRM_AUTO_STEP_MODE_ENABLE);
  crm_sysclk_switch(CRM_SCLK_PLL);
  {
    uint32_t guard = 4000000;
    while (crm_sysclk_switch_status_get() != CRM_SCLK_PLL && guard) guard--;
    if (!guard) return FALSE;
  }
  crm_auto_step_mode_enable(CRM_AUTO_STEP_MODE_DISABLE);

  system_core_clock_update();
  return TRUE;
}

void board_delay_cycles_init(void)
{
  /* DWT cycle counter — the timebase for the SK6812 bit-bang and for the
   * deadline watermarks the architecture doc calls for. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

void board_delay_us(uint32_t us)
{
  const uint32_t start  = DWT->CYCCNT;
  const uint32_t cycles = us * (system_core_clock / 1000000U);
  while ((DWT->CYCCNT - start) < cycles) {
  }
}
