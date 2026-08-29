/*
 * reset.h — why the MCU last restarted.
 *
 * A board that keeps disappearing off a USB hub is ambiguous from the host
 * side: a re-enumeration, a port reset and a dead board all look alike, and the
 * frame counter only tells you THAT it restarted, never why. CRM_CTRLSTS knows.
 *
 * The distinction that matters here is **power-on reset versus anything else**.
 * A POR means VBUS actually went away — the hub cut the port, or the rail
 * browned out — and no amount of firmware can prevent it. Any other flag points
 * back at us.
 *
 * The flags are sticky: RM 4.3.21, "cleared by power reset or by writing the
 * RSTFC bit". So they must be read and cleared at boot, or every later boot
 * still reports the very first reason.
 */
#ifndef GIRIS_RESET_H
#define GIRIS_RESET_H

#include <stdint.h>

#define RESET_POR    0x01u   /* power-on / brown-out: VBUS or the 3.3 V rail went */
#define RESET_NRST   0x02u   /* the NRST pin was pulled — SW2, or a debugger */
#define RESET_SW     0x04u   /* NVIC_SystemReset(), i.e. our own CMD_BOOTLOADER */
#define RESET_WDT    0x08u
#define RESET_WWDT   0x10u
#define RESET_LOWPWR 0x20u

/* Call once, as early as possible, before anything else can reset the part. */
void reset_capture(void);

uint8_t reset_flags(void);

/* USB suspend accounting. A host that suspends us and a hub that cuts our power
 * are different faults with the same symptom, and this separates them: a
 * suspend that does NOT end in a reset shows up here, while one that does shows
 * up as RESET_POR on the next boot. */
void     reset_note_suspend(void);
void     reset_note_resume(void);
uint16_t reset_suspends(void);
uint16_t reset_resumes(void);

#endif
