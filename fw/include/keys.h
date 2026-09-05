/* SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Isaac Chiu
 */
/*
 * keys.h — analog readings to keycodes.
 *
 * The minimum that makes this a keyboard rather than a telemetry board, and
 * deliberately no more: a per-key baseline, a threshold with hysteresis, and a
 * 6KRO boot report. Everything that makes an analog keyboard interesting —
 * calibrated travel in centi-mm, adjustable actuation, rapid trigger, per-key
 * curves — belongs to the travel pipeline that does not exist yet, and none of
 * it changes the shape of what is here.
 *
 * Direction-agnostic on purpose. A TMR2615 output moves up or down as the
 * magnet approaches depending on the pole facing it, and with no calibration
 * data there is nothing that knows which way round each key is fitted. So the
 * test is |value - baseline|, which is correct either way.
 *
 * Output is gated OFF by default, and that is not timidity: this interface can
 * type into whatever machine it is plugged into, the thresholds below are
 * guesses until the travel pipeline calibrates them, and a board that spews
 * keystrokes into someone's session while they are debugging it is a genuinely
 * bad afternoon. The interface still enumerates while gated, which is all that
 * is needed to earn wake capability from the host.
 */
#ifndef GIRIS_KEYS_H
#define GIRIS_KEYS_H

#include <stdint.h>
#include <stdbool.h>

#define KEYS_PER_HALF   6      /* fitted on this dev board */

/* Counts away from baseline. Hysteresis, so a key resting near the actuation
 * point does not chatter — press and release thresholds must never be equal. */
#define KEYS_PRESS_DELTA    150u
#define KEYS_RELEASE_DELTA   80u

typedef struct {
  uint8_t  enabled;              /* is output actually allowed */
  uint8_t  own_pressed;          /* bitmap of this half's keys */
  uint8_t  peer_pressed;         /* bitmap of the peer's, when we send them */
  uint8_t  sending_peer;         /* peer keys are being merged into our report */
  uint16_t reports;              /* keyboard reports sent */
  uint16_t baseline[KEYS_PER_HALF];
} keys_status_t;

void keys_init(void);

/* From the main loop. Reads the scan, decides what is pressed, and sends a
 * boot-keyboard report when the set changes. */
void keys_task(void);

/* Off by default. Turning it on is a deliberate act. */
void keys_set_enabled(bool on);

/* Re-take the resting baseline. Nothing may be touching the keys. */
void keys_calibrate(void);

void keys_status(keys_status_t *out);

#endif
