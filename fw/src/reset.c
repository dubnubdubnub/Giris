/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
#include "board.h"
#include "reset.h"

static uint8_t  flags;
static uint16_t suspends, resumes;

void reset_capture(void)
{
  uint8_t f = 0;
  if (crm_flag_get(CRM_POR_RESET_FLAG)      == SET) f |= RESET_POR;
  if (crm_flag_get(CRM_NRST_RESET_FLAG)     == SET) f |= RESET_NRST;
  if (crm_flag_get(CRM_SW_RESET_FLAG)       == SET) f |= RESET_SW;
  if (crm_flag_get(CRM_WDT_RESET_FLAG)      == SET) f |= RESET_WDT;
  if (crm_flag_get(CRM_WWDT_RESET_FLAG)     == SET) f |= RESET_WWDT;
  if (crm_flag_get(CRM_LOWPOWER_RESET_FLAG) == SET) f |= RESET_LOWPWR;
  flags = f;

  /* Sticky until cleared. Without this every boot reports the reason for the
   * first one, which is worse than reporting nothing at all. */
  crm_flag_clear(CRM_ALL_RESET_FLAG);
}

uint8_t  reset_flags(void)    { return flags; }
void     reset_note_suspend(void) { suspends++; }
void     reset_note_resume(void)  { resumes++; }
uint16_t reset_suspends(void) { return suspends; }
uint16_t reset_resumes(void)  { return resumes; }
