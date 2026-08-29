#include <string.h>

#include "board.h"
#include "adc.h"
#include "peer.h"
#include "split.h"
#include "sk6812.h"
#include "power.h"
#include "tusb.h"

static power_state_t state = POWER_RUN;
static bool     wakeup_en;
static uint16_t attempts, grants;
static uint16_t baseline[PROTO_NUM_SLOTS];
static bool     baseline_valid;
static bool     press_pending;
static uint8_t  settle;

/* The peer's host being awake is the whole reason SERVING exists. Both halves
 * run one image, so this is asked from either side with the same answer. */
static bool peer_needs_us(void)
{
  peer_status_t ps;
  peer_status(&ps);
  if (ps.state != PEER_RUNNING) return false;

  split_stats_t ss;
  split_stats(&ss);
  if (ss.stale) return false;                 /* link nominally up, nothing arriving */
  /* _AWAKE, not _HOST. A peer whose own host is asleep still reports _HOST —
   * it is still enumerated — and serving it would mean both halves burning
   * full scan current for two sleeping machines. */
  return (ss.peer_flags & SPLIT_F_AWAKE) != 0u;
}

static void leds_off(void)
{
  uint32_t off[LED_COUNT];
  memset(off, 0, sizeof(off));
  sk6812_write(off, LED_COUNT);
}

static void enter(power_state_t s)
{
  if (s == state) return;

  const bool was_slow = (state == POWER_SUSPENDED);
  const bool now_slow = (s == POWER_SUSPENDED);
  state = s;

  if (now_slow != was_slow) {
    adc_set_low_power(now_slow);
    /* The bank flip and the DMA ring have to be put back in step after the
     * period changes under them, or the first frames at the new rate come back
     * with the two mux banks swapped. */
    adc_resync();
    baseline_valid = false;                   /* re-take it at the new rate */
    /* And not from the very next frame. adc_resync() realigns the mux bank with
     * the DMA ring, and the frame straddling that realignment can carry one
     * bank's readings in the other's slots — which as a baseline makes every
     * subsequent frame look like a huge deflection. Measured: exactly one
     * spurious wake attempt per rate change, with nobody near the keyboard.
     * A false wake is the worst failure this code can produce, so discard a few
     * frames and start from a settled one. */
    settle = 4u;
  }
}

void power_init(void)
{
  state = POWER_RUN;
  wakeup_en = false;
  attempts = grants = 0;
  baseline_valid = false;
  press_pending = false;
}

void power_on_suspend(bool remote_wakeup_en)
{
  wakeup_en = remote_wakeup_en;
  press_pending = false;
  settle = 4u;
  split_set_awake(false);
  leds_off();
  enter(peer_needs_us() ? POWER_SERVING : POWER_SUSPENDED);
}

void power_on_resume(void)
{
  press_pending = false;
  split_set_awake(true);
  enter(POWER_RUN);
}

bool power_may_idle(void)
{
  return state == POWER_SUSPENDED && !press_pending;
}

void power_task(void)
{
  if (state == POWER_RUN) return;

  /* The peer's host can wake or sleep while ours stays asleep, so this is not a
   * decision taken once at suspend time. Re-evaluated every pass; enter() is a
   * no-op unless the answer actually changed. */
  enter(peer_needs_us() ? POWER_SERVING : POWER_SUSPENDED);

  adc_frame_t f;
  if (!adc_read_frame(&f)) return;

  if (settle) { settle--; return; }

  if (!baseline_valid) {
    for (uint32_t i = 0; i < PROTO_NUM_SLOTS; i++) baseline[i] = f.slot[i];
    baseline_valid = true;
    return;
  }

  bool moved = false;
  for (uint32_t i = 0; i < PROTO_NUM_SLOTS; i++) {
    const int32_t d = (int32_t)f.slot[i] - (int32_t)baseline[i];
    if (d > (int32_t)POWER_WAKE_DELTA || d < -(int32_t)POWER_WAKE_DELTA) { moved = true; break; }
  }
  if (!moved) return;

  press_pending = true;
  attempts++;

  /* Only the host can decide to come back. If it never granted remote wakeup
   * there is nothing to do but keep the count, which is exactly the number to
   * look at when someone reports that their keyboard will not wake the machine:
   * attempts climbing with grants at zero means the host refused, not that the
   * press was missed. */
  if (wakeup_en && tud_remote_wakeup()) grants++;

  /* Re-baseline so a key held down does not re-trigger every frame. */
  baseline_valid = false;
}

void power_status(power_status_t *out)
{
  out->state            = (uint8_t)state;
  out->remote_wakeup_en = wakeup_en ? 1u : 0u;
  out->wake_attempts    = attempts;
  out->wake_grants      = grants;
}
