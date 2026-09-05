/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
/*
 * power.h — what this half does when its own host goes to sleep.
 *
 * Why an analog keyboard cannot do the normal thing
 * -------------------------------------------------
 * A mechanical keyboard suspends by stopping everything and arming a GPIO
 * interrupt: the switch shorts a pin, an edge fires, the MCU wakes from deep
 * sleep having drawn microamps. **None of that is available here.** A TMR2615
 * is a ratiometric Hall sensor — it has no contact, produces no edge, and its
 * output at rest is a mid-scale voltage indistinguishable from its output under
 * a finger except by measuring it. Detecting a press REQUIRES running the mux
 * and the ADC.
 *
 * This part has no standalone analog comparator either (checked: the only
 * comparators in the reference manual are the timers' output-compare units), so
 * the one hardware trick that could have helped is absent. What it does have is
 * the ADC's voltage monitor — thresholds plus the VMOR interrupt — which does
 * not remove the need to convert but does let the CPU sleep between
 * conversions instead of inspecting every sample. That is the refinement worth
 * building next; this is the version that works today.
 *
 * So suspend here means **scan slower**, not **stop**: 500 Hz instead of 8 kHz,
 * a 16x cut in mux and ADC duty, with a press still seen inside 2 ms.
 *
 * This is honest about what it is: it does NOT reach the 2.5 mA that USB 2.0
 * §7.2.3 allows a suspended device. Getting there needs the PLL stopped and the
 * OTG PHY in low-power mode, which is a larger piece of work. What it does is
 * remove the two largest consumers that were running for no reason at all — a
 * full-rate scan nobody was reading, and the LEDs.
 *
 * The split makes suspend a three-state problem, not two
 * ------------------------------------------------------
 * In dual-host topology (b) each half has its own host, and **they sleep
 * independently.** If half A's host sleeps while half B's host is wide awake,
 * A's keys must keep reaching B — the person is still typing, just on the other
 * machine. A half that powered down on suspend would silently drop half the
 * keyboard.
 *
 * So "my host suspended" and "I should stop working" are different questions,
 * and the peer's own SPLIT_F_HOST bit is what separates them.
 */
#ifndef GIRIS_POWER_H
#define GIRIS_POWER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
  POWER_RUN       = 0,  /* our host is awake */
  POWER_SUSPENDED = 1,  /* host asleep and nobody else needs us: 500 Hz scan */
  POWER_SERVING   = 2,  /* host asleep, but the PEER's host is awake and our
                         * keys still have somewhere to go — stay at full rate.
                         * Costs current we would rather not spend, and is the
                         * correct answer anyway: the alternative is half a
                         * keyboard going dead on a machine somebody is using. */
} power_state_t;

typedef struct {
  uint8_t  state;
  uint8_t  remote_wakeup_en;  /* the host granted SET_FEATURE(REMOTE_WAKEUP) */
  uint16_t wake_attempts;     /* presses seen while suspended */
  uint16_t wake_grants;       /* tud_remote_wakeup() accepted */
} power_status_t;

void power_init(void);

/* From the main loop. Detects a press while suspended, asks the host to wake,
 * and re-evaluates SUSPENDED vs SERVING as the peer comes and goes. */
void power_task(void);

void power_on_suspend(bool remote_wakeup_en);
void power_on_resume(void);

void power_status(power_status_t *out);

/* True while the main loop may sleep in WFI — suspended, not serving a peer,
 * and no press pending. */
bool power_may_idle(void);

/* Raw counts away from the resting baseline that counts as "somebody touched a
 * key". Provisional, like everything else upstream of the travel pipeline: it
 * is deliberately coarse because a false wake is a far worse failure than a
 * slightly firm press, and there is no per-key calibration to be precise with
 * yet. */
#define POWER_WAKE_DELTA  120u

#endif
